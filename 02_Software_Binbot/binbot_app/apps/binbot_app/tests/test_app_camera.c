#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "libs/lodepng/lodepng.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

enum { WIDTH = 640, HEIGHT = 480, FRAME_BYTES = WIDTH * HEIGHT * 3 / 2, BUFFER_COUNT = 4 };
static uint8_t *buffers[BUFFER_COUNT];
static bool queued[BUFFER_COUNT], ready[BUFFER_COUNT], streaming;
static int camera_fd = -1;
static unsigned generation, mapped, streams_closed, buffers_released, mappings_protected;
static atomic_uint dequeued;
static atomic_uint hand_results;
static atomic_int hand_label;
static unsigned detector_token;
static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

binbot_hand_detector_t *__wrap_binbot_hand_detector_create(const binbot_hand_detector_config_t *config)
{
    assert(config->width == WIDTH && config->height == HEIGHT);
    return (binbot_hand_detector_t *)&detector_token;
}

void __wrap_binbot_hand_detector_destroy(binbot_hand_detector_t *detector) { (void)detector; }
bool __wrap_binbot_hand_detector_is_available(const binbot_hand_detector_t *detector) { return detector != NULL; }

binbot_hand_submit_status_t __wrap_binbot_hand_detector_submit_nv12(
    binbot_hand_detector_t *detector, const uint8_t *y, uint32_t ys, const uint8_t *uv, uint32_t uvs)
{
    assert(detector && y && uv && ys == WIDTH && uvs == WIDTH);
    return BINBOT_HAND_SUBMIT_ACCEPTED;
}

bool __wrap_binbot_hand_detector_latest(binbot_hand_detector_t *detector,
                                        uint64_t after, binbot_hand_result_t *result)
{
    (void)after; assert(detector);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    *result = (binbot_hand_result_t){.sequence = atomic_fetch_add(&hand_results, 1) + 1,
        .monotonic_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000,
        .gesture = {.label = atomic_load(&hand_label), .score = 0.99f}};
    return true;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    if(!strcmp(path, "/tmp/binbot-fake-camera")) {
        camera_fd = __real_open("/dev/null", O_RDWR);
        return camera_fd;
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = flags & O_CREAT ? va_arg(ap, int) : 0;
    va_end(ap);
    return __real_open(path, flags, mode);
}

int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    if(fd != camera_fd) return __real_ioctl(fd, request, arg);
    pthread_mutex_lock(&device_mutex);
    int result = 0;
    if(request == VIDIOC_QUERYCAP) {
        ((struct v4l2_capability *)arg)->capabilities =
            V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
    } else if(request == VIDIOC_S_FMT) {
        struct v4l2_pix_format_mplane *f = &((struct v4l2_format *)arg)->fmt.pix_mp;
        f->num_planes = 1; f->plane_fmt[0].bytesperline = WIDTH;
    } else if(request == VIDIOC_REQBUFS) {
        if(((struct v4l2_requestbuffers *)arg)->count == 0) {
            assert(!streaming && !mapped);
            ++buffers_released;
        }
        memset(queued, 0, sizeof(queued)); memset(ready, 0, sizeof(ready));
    } else if(request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        b->m.planes[0].length = FRAME_BYTES;
        b->m.planes[0].m.mem_offset = b->index * FRAME_BYTES;
    } else if(request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg; assert(b->index < BUFFER_COUNT && !queued[b->index]);
        queued[b->index] = true;
    } else if(request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        unsigned i = 0;
        while(i < BUFFER_COUNT && !ready[i]) ++i;
        if(i == BUFFER_COUNT) { errno = EAGAIN; result = -1; }
        else { b->index = i; ready[i] = false; queued[i] = false; atomic_fetch_add(&dequeued, 1); }
    } else if(request == VIDIOC_STREAMON) streaming = true;
    else if(request == VIDIOC_STREAMOFF) {
        streaming = false; ++streams_closed;
        for(unsigned i = 0; i < BUFFER_COUNT; ++i) assert(queued[i]);
    } else assert(!"unexpected camera ioctl");
    pthread_mutex_unlock(&device_mutex);
    return result;
}

void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if(fd != camera_fd) return __real_mmap(address, length, prot, flags, fd, offset);
    assert(length == FRAME_BYTES && offset / FRAME_BYTES < BUFFER_COUNT);
    ++mapped; return buffers[offset / FRAME_BYTES];
}

int __real_munmap(void *, size_t);
int __real_madvise(void *, size_t, int);
int __wrap_madvise(void *address, size_t length, int advice)
{
    for(unsigned i = 0; i < BUFFER_COUNT; ++i)
        if(address == buffers[i]) {
            assert(length == FRAME_BYTES && advice == MADV_DONTFORK);
            ++mappings_protected;
            return 0;
        }
    return __real_madvise(address, length, advice);
}

int __wrap_munmap(void *address, size_t length)
{
    for(unsigned i = 0; i < BUFFER_COUNT; ++i)
        if(address == buffers[i]) { assert(mapped > 0); --mapped; return 0; }
    return __real_munmap(address, length);
}

int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if(count != 1 || fds[0].fd != camera_fd) return __real_poll(fds, count, timeout);
    usleep(10000);
    pthread_mutex_lock(&device_mutex);
    assert(streaming);
    ++generation;
    for(unsigned i = 0; i < BUFFER_COUNT; ++i) {
        if(!queued[i]) continue;
        memset(buffers[i], 32 + generation % 64, WIDTH * HEIGHT);
        for(unsigned y = 0; y < HEIGHT; ++y)
            memset(buffers[i] + y * WIDTH + WIDTH / 2, 192 + generation % 32, WIDTH / 2);
        memset(buffers[i] + WIDTH * HEIGHT, 128, WIDTH * HEIGHT / 2);
        ready[i] = true;
    }
    fds[0].revents = POLLIN;
    pthread_mutex_unlock(&device_mutex);
    return 1;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void advance(unsigned milliseconds)
{
    while(milliseconds) {
        unsigned step = milliseconds > 33 ? 33 : milliseconds;
        lv_tick_inc(step); lv_timer_handler(); usleep(3000); milliseconds -= step;
    }
    lv_obj_update_layout(lv_screen_active());
}

static lv_obj_t *canvas(lv_obj_t *root)
{
    if(lv_obj_check_type(root, &lv_canvas_class)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = canvas(lv_obj_get_child(root, i));
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void check_frame(lv_obj_t *image, bool mirrored)
{
    lv_draw_buf_t *frame = lv_canvas_get_draw_buf(image);
    assert(frame && frame->header.w == 680 && frame->header.h == 510);
    assert(frame->header.cf == LV_COLOR_FORMAT_XRGB8888 && lv_image_get_scale_x(image) == 256);
    const uint32_t *pixels = (const uint32_t *)frame->data;
    assert((pixels[0] & 0xffffff) != 0);
    assert((pixels[0] > pixels[679]) == mirrored);
    for(unsigned y = 0; y < 510; ++y)
        for(unsigned x = 0; x < 680; ++x)
            assert(pixels[y * 680 + x] == pixels[x < 340 ? 0 : 679]);
}

static void check_photo(const char *photos, bool mirrored)
{
    DIR *directory = opendir(photos); assert(directory);
    struct dirent *entry; char name[128] = {0}, photo[512];
    while((entry = readdir(directory)))
        if(!strncmp(entry->d_name, "shot-", 5) && strcmp(entry->d_name, name) > 0)
            snprintf(name, sizeof(name), "%.127s", entry->d_name);
    closedir(directory); assert(name[0]);
    snprintf(photo, sizeof(photo), "%s/%s", photos, name);
    unsigned char *decoded; unsigned width, height;
    char source[520]; snprintf(source, sizeof(source), "A:%s", photo);
    assert(!lodepng_decode32_file(&decoded, &width, &height, source));
    lv_draw_buf_t *photo_frame = (lv_draw_buf_t *)decoded;
    const uint8_t *rgb = photo_frame->data;
    assert(width == 640 && height == 480 && rgb[0] > 0);
    assert((rgb[0] > rgb[(width - 1) * 4]) == mirrored);
    for(unsigned y = 0; y < height; ++y)
        for(unsigned x = 0; x < width; ++x)
            assert(!memcmp(rgb + (y * width + x) * 4, rgb + (x < width / 2 ? 0 : width - 1) * 4, 4));
    lv_draw_buf_destroy(photo_frame);
}

int main(void)
{
    char photos[] = "/tmp/binbot-camera-test-XXXXXX"; assert(mkdtemp(photos));
    setenv("BINBOT_PHOTO_ROOT", photos, 1);
    setenv("BINBOT_INITIAL_ROUTE", "camera", 1);
    setenv("BINBOT_CAMERA_DEVICE", "/tmp/binbot-fake-camera", 1);
    setenv("BINBOT_FACE_DETECTION", "0", 1);
    setenv("BINBOT_HAND_DETECTION", "0", 1);
    setenv("BINBOT_CUP_DETECTION", "0", 1);
    setenv("BINBOT_CAMERA_MIRROR", "1", 1);
    for(unsigned i = 0; i < BUFFER_COUNT; ++i) { buffers[i] = malloc(FRAME_BYTES); assert(buffers[i]); }
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    advance(1000);
    lv_obj_t *image = canvas(app->current.root); assert(image); check_frame(image, true);
    for(unsigned i = 0; i < 20; ++i) { advance(33); check_frame(image, true); }

    app->current.set_paused(app->current.context, true);
    usleep(30000);
    unsigned before = atomic_load(&dequeued);
    advance(500);
    assert(atomic_load(&dequeued) == before);
    app->current.set_paused(app->current.context, false);
    advance(500); assert(atomic_load(&dequeued) > before); check_frame(image, true);

    lv_obj_t *controls = NULL;
    for(unsigned i = 0; i < lv_obj_get_child_count(app->current.root); ++i) {
        lv_obj_t *child = lv_obj_get_child(app->current.root, i);
        if(lv_obj_get_x(child) == 20 && lv_obj_get_y(child) == 588) controls = child;
    }
    assert(controls);
    lv_obj_send_event(lv_obj_get_child(controls, 1), LV_EVENT_CLICKED, NULL);
    check_photo(photos, true);
    lv_obj_t *album = lv_obj_get_child(controls, 0);
    lv_obj_t *badge = lv_obj_get_child(album, -1);
    lv_obj_update_layout(badge);
    assert(lv_obj_get_width(badge) == 24);
    for(unsigned i = 0; i < 8; ++i) {
        lv_obj_send_event(lv_obj_get_child(controls, 2), LV_EVENT_CLICKED, NULL);
        lv_obj_update_layout(badge);
        assert(lv_obj_get_width(badge) == 24);
    }
    lv_obj_send_event(lv_obj_get_child(controls, 2), LV_EVENT_CLICKED, NULL);
    advance(500); check_frame(image, false);
    lv_obj_send_event(lv_obj_get_child(controls, 1), LV_EVENT_CLICKED, NULL);
    check_photo(photos, false);
    lv_obj_send_event(lv_obj_get_child(controls, 0), LV_EVENT_CLICKED, NULL);
    usleep(30000);
    before = atomic_load(&dequeued);
    advance(500); assert(atomic_load(&dequeued) == before);
    lv_obj_t *gallery = lv_obj_get_child(app->current.root, -1);
    lv_obj_t *back = NULL;
    for(unsigned i = 0; i < lv_obj_get_child_count(gallery); ++i) {
        lv_obj_t *child = lv_obj_get_child(gallery, i);
        if(lv_obj_get_x(child) == 20 && lv_obj_get_y(child) == 20) back = child;
    }
    assert(back);
    lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
    advance(500); assert(atomic_load(&dequeued) > before); check_frame(image, false);

    for(unsigned i = 0; i < 4; ++i) {
        ui_app_navigate(app, UI_ROUTE_APPS); advance(100); assert(!mapped);
        ui_app_navigate(app, UI_ROUTE_CAMERA); advance(500);
        image = canvas(app->current.root); assert(image); check_frame(image, false);
    }
    setenv("BINBOT_HAND_DETECTION", "1", 1);
    ui_app_navigate(app, UI_ROUTE_CAMERA); advance(1000);
    image = canvas(app->current.root); assert(image); check_frame(image, false);
    ui_screensaver_show(app->screensaver);
    lv_draw_buf_t *saved_frame = lv_canvas_get_draw_buf(image);
    before = atomic_load(&hand_results);
    advance(2000);
    assert(ui_screensaver_active(app->screensaver) && atomic_load(&hand_results) > before);
    assert(lv_canvas_get_draw_buf(image) == saved_frame);
    atomic_store(&hand_label, BINBOT_GESTURE_OK);
    for(unsigned i = 0; i < 100 && ui_screensaver_active(app->screensaver); ++i) advance(100);
    assert(!ui_screensaver_active(app->screensaver) && label(app->root, "是否打开电脑？"));
    app_gesture_settings_t settings = app_state_model(state)->gestures;
    settings.enabled = false; assert(app_state_set_gestures(state, &settings)); advance(100);
    atomic_store(&hand_label, BINBOT_GESTURE_NONE);
    settings.enabled = true; assert(app_state_set_gestures(state, &settings)); advance(1000);
    controls = NULL;
    for(unsigned i = 0; i < lv_obj_get_child_count(app->current.root); ++i) {
        lv_obj_t *child = lv_obj_get_child(app->current.root, i);
        if(lv_obj_get_x(child) == 20 && lv_obj_get_y(child) == 588) controls = child;
    }
    assert(controls);
    lv_obj_send_event(lv_obj_get_child(controls, 0), LV_EVENT_CLICKED, NULL);
    before = atomic_load(&hand_results); saved_frame = lv_canvas_get_draw_buf(image);
    advance(1000);
    assert(atomic_load(&hand_results) > before && lv_canvas_get_draw_buf(image) == saved_frame);
    settings.enabled = false; assert(app_state_set_gestures(state, &settings)); usleep(60000);
    before = atomic_load(&dequeued); advance(500); assert(atomic_load(&dequeued) == before);
    settings.enabled = true; assert(app_state_set_gestures(state, &settings));
    advance(1000); assert(atomic_load(&dequeued) > before);
    assert(!ui_camera_take_photo(app));
    advance(1000); assert(ui_camera_take_photo(app));
    check_frame(image, false);
    ui_app_destroy(app); app_state_destroy(state);
    assert(!mapped && streams_closed == 6);
    assert(buffers_released == streams_closed && mappings_protected == BUFFER_COUNT * streams_closed);
    assert(lv_mem_test() == LV_RESULT_OK); lv_deinit();
    for(unsigned i = 0; i < BUFFER_COUNT; ++i) free(buffers[i]);
    DIR *directory = opendir(photos); assert(directory);
    struct dirent *entry;
    while((entry = readdir(directory))) {
        if(strncmp(entry->d_name, "shot-", 5)) continue;
        char photo[512]; snprintf(photo, sizeof(photo), "%s/%s", photos, entry->d_name);
        assert(!unlink(photo));
    }
    closedir(directory); assert(!rmdir(photos));
    puts("Camera: preview/photos, mirror, gesture recognition during screensaver/album, disabled pause and cleanup passed");
    return 0;
}
