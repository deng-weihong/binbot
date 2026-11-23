#include "../ui_internal.h"

#include "binbot/camera_renderer.h"
#include "binbot/vision/cup_detector.h"
#include "binbot/vision/cup_overlay.h"
#include "binbot/vision/face_detector.h"
#include "binbot/vision/face_overlay.h"
#include "binbot/vision/face_recognizer.h"
#include "binbot/vision/hand_detector.h"
#include "binbot/vision/hand_overlay.h"
#include "libs/lodepng/lodepng.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    CAMERA_DEFAULT_WIDTH = 640,
    CAMERA_DEFAULT_HEIGHT = 480,
    CAMERA_BUFFER_COUNT = 4,
    CAMERA_REFRESH_MS = 33,
    CAMERA_FRAME_INTERVAL_MS = 50,
    /* Gesture capture runs in the background on the single-core RV1106.  Two
     * frames per second keeps the interaction responsive while the detector
     * still has enough temporal resolution for the stabilizer. */
    CAMERA_GESTURE_FRAME_INTERVAL_MS = 500,
    CAMERA_PREVIEW_WIDTH = 680,
    CAMERA_PREVIEW_HEIGHT = 510,
    CAMERA_PHOTO_WIDTH = 640,
    CAMERA_PHOTO_HEIGHT = 480,
    CAMERA_FRAME_COUNT = 3,
    FACE_RESULT_TTL_MS = 1000,
    HAND_RESULT_TTL_MS = 800,
    CUP_RESULT_TTL_MS = 1500,
};

typedef struct {
    void *address[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
    uint32_t data_offset[VIDEO_MAX_PLANES];
} camera_buffer_t;

typedef struct {
    uint16_t *pixels;
    uint32_t *preview;
} camera_frame_t;

typedef struct {
    ui_app_t *app;
    lv_obj_t *canvas;
    lv_obj_t *status;
    lv_obj_t *root, *preview, *thumbnail, *thumbnail_icon, *shot_count;
    lv_obj_t *mirror_button, *mirror_label, *mirror_disc, *mirror_icon, *nonmirror_badge;
    lv_obj_t *flash, *gallery;
    lv_timer_t *controls_timer;
    uint64_t flash_until;
    char photo_root[320];
    char (*photos)[384];
    size_t photo_count, photo_capacity, viewed;
    lv_timer_t *timer;
    int fd;
    bool streaming;
    atomic_bool mirrored;
    uint32_t width;
    uint32_t height;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t plane_count;
    uint32_t buffer_count;
    camera_buffer_t buffers[CAMERA_BUFFER_COUNT];
    camera_frame_t frames[CAMERA_FRAME_COUNT];
    binbot_camera_renderer_t *renderer;
    pthread_t worker;
    pthread_mutex_t frame_mutex;
    bool mutex_ready, worker_started, frame_presented, content_paused;
    bool gestures_only;
    atomic_bool stopping, paused, recognition_only;
    atomic_uint presented_frames;
    int front_frame, pending_frame, writing_frame;
    uint64_t capture_time_ms;
    uint16_t *pixels;
    binbot_face_detector_t *face_detector;
    binbot_face_result_t face_result;
    uint64_t face_sequence;
    bool face_submit_error_logged;
    binbot_face_recognizer_t *face_recognizer;
    binbot_face_recognition_result_t face_recognition;
    uint64_t face_recognition_sequence;
    bool face_recognition_error_logged;
    bool log_face_recognition_results;
    binbot_hand_detector_t *hand_detector;
    binbot_hand_result_t hand_result;
    uint64_t hand_sequence;
    binbot_hand_result_t published_hand;
    uint64_t consumed_hand_sequence;
    bool hand_submit_error_logged;
    binbot_cup_detector_t *cup_detector;
    binbot_cup_result_t cup_result;
    uint64_t cup_sequence;
    bool cup_submit_error_logged;
} camera_context_t;

static bool camera_mirror_preference;
static bool camera_mirror_preference_initialized;

static uint64_t camera_monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static uint32_t environment_u32(const char *name, uint32_t fallback,
                                uint32_t minimum, uint32_t maximum)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if(errno != 0 || !end || *end != '\0' || value < minimum || value > maximum) {
        fprintf(stderr, "camera: ignoring invalid %s=%s\n", name, text);
        return fallback;
    }
    return (uint32_t)value;
}

static bool environment_enabled(const char *name, bool fallback)
{
    const char *value = getenv(name);
    if(!value || !value[0]) return fallback;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
}

static float environment_float(const char *name, float fallback,
                               float minimum, float maximum)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    errno = 0;
    float value = strtof(text, &end);
    if(errno != 0 || !end || *end != '\0' || !isfinite(value) ||
       value < minimum || value > maximum) {
        fprintf(stderr, "camera: ignoring invalid %s=%s\n", name, text);
        return fallback;
    }
    return value;
}

static const char *face_match_state_name(binbot_face_match_state_t state)
{
    switch(state) {
    case BINBOT_FACE_MATCH_NO_REFERENCE: return "NO_REFERENCE";
    case BINBOT_FACE_MATCH_INVALID_VECTOR: return "INVALID_VECTOR";
    case BINBOT_FACE_MATCH_MISMATCH: return "UNKNOWN";
    case BINBOT_FACE_MATCH_PENDING: return "PENDING";
    case BINBOT_FACE_MATCH_CONFIRMED: return "MATCH";
    }
    return "INVALID";
}

static void camera_start_face_detector(camera_context_t *context)
{
    if(!environment_enabled("BINBOT_FACE_DETECTION", true)) {
        fprintf(stdout, "vision: face detection disabled by configuration\n");
        return;
    }

    binbot_face_detector_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = (uint16_t)context->width;
    config.height = (uint16_t)context->height;
    config.library_path = getenv("BINBOT_ROCKIVA_LIBRARY");
    config.model_directory = getenv("BINBOT_ROCKIVA_MODEL_DIR");
    config.minimum_score = (uint8_t)environment_u32("BINBOT_FACE_MIN_SCORE", 45, 1, 100);
    config.stable_frame_count =
        (uint8_t)environment_u32("BINBOT_FACE_STABLE_FRAMES", 2, 0, 10);
    config.minimum_interval_ms =
        environment_u32("BINBOT_FACE_INTERVAL_MS", 200, CAMERA_REFRESH_MS, 2000);
    context->face_detector = binbot_face_detector_create(&config);
    if(!binbot_face_detector_is_available(context->face_detector)) {
        fprintf(stderr, "vision: face detection unavailable: %s\n",
                binbot_face_detector_error(context->face_detector));
    }
}

static void camera_start_hand_detector(camera_context_t *context)
{
    if(!environment_enabled("BINBOT_HAND_DETECTION", true)) {
        fprintf(stdout, "vision: hand detection disabled by configuration\n");
        return;
    }

    binbot_hand_detector_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = (uint16_t)context->width;
    config.height = (uint16_t)context->height;
    config.palm_model_path = getenv("BINBOT_HAND_PALM_MODEL");
    config.landmark_model_path = getenv("BINBOT_HAND_LANDMARK_MODEL");
    config.minimum_palm_score =
        (float)environment_u32("BINBOT_HAND_PALM_SCORE", 50, 1, 99) / 100.0f;
    config.minimum_presence_score =
        (float)environment_u32("BINBOT_HAND_PRESENCE_SCORE", 50, 1, 100) /
        100.0f;
    config.minimum_interval_ms = environment_u32(
        "BINBOT_HAND_INTERVAL_MS", context->gestures_only ? 500 : 200,
        CAMERA_REFRESH_MS, 2000);
    context->hand_detector = binbot_hand_detector_create(&config);
    if(!binbot_hand_detector_is_available(context->hand_detector)) {
        fprintf(stderr, "vision: hand detection unavailable: %s\n",
                binbot_hand_detector_error(context->hand_detector));
    }
}

static void camera_start_cup_detector(camera_context_t *context)
{
    if(!environment_enabled("BINBOT_CUP_DETECTION", true)) {
        fprintf(stdout, "vision: cup detection disabled by configuration\n");
        return;
    }

    binbot_cup_detector_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = (uint16_t)context->width;
    config.height = (uint16_t)context->height;
    config.model_path = getenv("BINBOT_CUP_MODEL");
    config.minimum_score = environment_float(
        "BINBOT_CUP_SCORE", 0.25f, 0.01f, 0.99f);
    config.nms_threshold = environment_float(
        "BINBOT_CUP_NMS", 0.45f, 0.0f, 1.0f);
    config.minimum_interval_ms = environment_u32(
        "BINBOT_CUP_INTERVAL_MS", 500, CAMERA_REFRESH_MS, 5000);
    context->cup_detector = binbot_cup_detector_create(&config);
    if(!binbot_cup_detector_is_available(context->cup_detector)) {
        fprintf(stderr, "vision: cup detection unavailable: %s\n",
                binbot_cup_detector_error(context->cup_detector));
    }
}

static void camera_start_face_recognizer(camera_context_t *context)
{
    context->log_face_recognition_results =
        environment_enabled("BINBOT_FACE_LOG_RESULTS", false);
    if(!environment_enabled("BINBOT_FACE_RECOGNITION", false)) {
        fprintf(stdout, "vision: fixed face recognition disabled by configuration\n");
        return;
    }

    binbot_face_recognizer_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = (uint16_t)context->width;
    config.height = (uint16_t)context->height;
    config.model_path = getenv("BINBOT_FACE_RECOGNITION_MODEL");
    config.reference_feature_path = getenv("BINBOT_FACE_REFERENCE_FEATURE");
    config.match_threshold = environment_float(
        "BINBOT_FACE_MATCH_THRESHOLD", 0.65f, -0.99f, 0.99f);
    config.confirm_frames = (uint8_t)environment_u32(
        "BINBOT_FACE_CONFIRM_FRAMES", 2, 1, 10);
    config.minimum_interval_ms = environment_u32(
        "BINBOT_FACE_RECOGNITION_INTERVAL_MS", 500, CAMERA_REFRESH_MS, 5000);
    config.crop_margin = environment_float(
        "BINBOT_FACE_CROP_MARGIN", 0.35f, 0.0f, 1.0f);
    config.reference_capture_path = getenv("BINBOT_FACE_CAPTURE_REFERENCE");
    config.reference_capture_samples = (uint8_t)environment_u32(
        "BINBOT_FACE_CAPTURE_SAMPLES", 10, 3, 50);
    context->face_recognizer = binbot_face_recognizer_create(&config);
    if(!binbot_face_recognizer_is_available(context->face_recognizer)) {
        fprintf(stderr, "vision: fixed face recognition unavailable: %s\n",
                binbot_face_recognizer_error(context->face_recognizer));
    }
}

static int camera_ioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while(result < 0 && errno == EINTR);
    return result;
}

static void camera_release(camera_context_t *context)
{
    if(!context) return;
    if(context->worker_started) {
        atomic_store(&context->stopping, true);
        pthread_join(context->worker, NULL);
        context->worker_started = false;
    }
    if(context->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        (void)camera_ioctl(context->fd, VIDIOC_STREAMOFF, &type);
        context->streaming = false;
    }
    binbot_face_detector_destroy(context->face_detector);
    context->face_detector = NULL;
    binbot_face_recognizer_destroy(context->face_recognizer);
    context->face_recognizer = NULL;
    binbot_hand_detector_destroy(context->hand_detector);
    context->hand_detector = NULL;
    binbot_cup_detector_destroy(context->cup_detector);
    context->cup_detector = NULL;
    for(uint32_t i = 0; i < context->buffer_count; ++i) {
        for(uint32_t plane = 0; plane < context->plane_count; ++plane) {
            if(context->buffers[i].address[plane] &&
               context->buffers[i].address[plane] != MAP_FAILED) {
                munmap(context->buffers[i].address[plane], context->buffers[i].length[plane]);
                context->buffers[i].address[plane] = NULL;
            }
        }
    }
    if(context->fd >= 0 && context->buffer_count) {
        struct v4l2_requestbuffers release = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if(camera_ioctl(context->fd, VIDIOC_REQBUFS, &release) < 0)
            fprintf(stderr, "camera: releasing capture buffers failed: %s\n", strerror(errno));
    }
    context->buffer_count = 0;
    context->plane_count = 0;
    if(context->fd >= 0) close(context->fd);
    context->fd = -1;
    for(unsigned i = 0; i < CAMERA_FRAME_COUNT; ++i) {
        free(context->frames[i].pixels);
        free(context->frames[i].preview);
        context->frames[i] = (camera_frame_t){0};
    }
    context->pixels = NULL;
    binbot_camera_renderer_destroy(context->renderer);
    context->renderer = NULL;
    if(context->mutex_ready) {
        pthread_mutex_destroy(&context->frame_mutex);
        context->mutex_ready = false;
    }
}

static bool camera_open(camera_context_t *context)
{
    const char *device = getenv("BINBOT_CAMERA_DEVICE");
    if(!device || !device[0]) device = "/dev/video11";

    context->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(context->fd < 0) {
        fprintf(stderr, "camera: failed to open %s: %s\n", device, strerror(errno));
        return false;
    }

    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    if(camera_ioctl(context->fd, VIDIOC_QUERYCAP, &capability) < 0 ||
       !(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
       !(capability.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "camera: %s is not a streaming mplane capture device\n", device);
        camera_release(context);
        return false;
    }

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = CAMERA_DEFAULT_WIDTH;
    format.fmt.pix_mp.height = CAMERA_DEFAULT_HEIGHT;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if(camera_ioctl(context->fd, VIDIOC_S_FMT, &format) < 0 ||
       format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
       format.fmt.pix_mp.num_planes == 0 ||
       format.fmt.pix_mp.num_planes > VIDEO_MAX_PLANES) {
        fprintf(stderr, "camera: failed to configure NV12 on %s\n", device);
        camera_release(context);
        return false;
    }

    context->width = format.fmt.pix_mp.width;
    context->height = format.fmt.pix_mp.height;
    context->plane_count = format.fmt.pix_mp.num_planes;
    context->y_stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    if(context->y_stride < context->width) context->y_stride = context->width;
    context->uv_stride = context->plane_count > 1
                             ? format.fmt.pix_mp.plane_fmt[1].bytesperline
                             : context->y_stride;
    if(context->uv_stride < context->width) context->uv_stride = context->width;
    if(context->width == 0 || context->height == 0 || context->width > 1280 ||
       context->height > 720) {
        fprintf(stderr, "camera: unsupported negotiated size %ux%u\n",
                context->width, context->height);
        camera_release(context);
        return false;
    }

    struct v4l2_requestbuffers request;
    memset(&request, 0, sizeof(request));
    request.count = CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if(camera_ioctl(context->fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0 ||
       request.count > CAMERA_BUFFER_COUNT) {
        fprintf(stderr, "camera: failed to allocate capture buffers (count %u): %s\n",
                request.count, strerror(errno));
        camera_release(context);
        return false;
    }
    context->buffer_count = request.count;

    for(uint32_t index = 0; index < context->buffer_count; ++index) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.m.planes = planes;
        buffer.length = context->plane_count;
        if(camera_ioctl(context->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            fprintf(stderr, "camera: VIDIOC_QUERYBUF failed for buffer %u\n", index);
            camera_release(context);
            return false;
        }
        for(uint32_t plane = 0; plane < context->plane_count; ++plane) {
            context->buffers[index].length[plane] = planes[plane].length;
            context->buffers[index].data_offset[plane] = planes[plane].data_offset;
            context->buffers[index].address[plane] = mmap(NULL, planes[plane].length,
                                                           PROT_READ | PROT_WRITE, MAP_SHARED,
                                                           context->fd,
                                                           planes[plane].m.mem_offset);
            if(context->buffers[index].address[plane] == MAP_FAILED) {
                context->buffers[index].address[plane] = NULL;
                fprintf(stderr, "camera: mmap failed for buffer %u plane %u\n", index, plane);
                camera_release(context);
                return false;
            }
            if(madvise(context->buffers[index].address[plane], planes[plane].length, MADV_DONTFORK) < 0) {
                fprintf(stderr, "camera: protecting capture mapping failed: %s\n", strerror(errno));
                camera_release(context);
                return false;
            }
        }
        if(camera_ioctl(context->fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "camera: initial VIDIOC_QBUF failed for buffer %u\n", index);
            camera_release(context);
            return false;
        }
    }

    if(!context->gestures_only) {
        context->renderer = binbot_camera_renderer_create(context->width, context->height,
            CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT, environment_enabled("BINBOT_CAMERA_RGA", true));
        if(!context->renderer) { camera_release(context); return false; }
        for(unsigned i = 0; i < CAMERA_FRAME_COUNT; ++i) {
            context->frames[i].pixels = calloc((size_t)context->width * context->height, sizeof(uint16_t));
            context->frames[i].preview = calloc(CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT, sizeof(uint32_t));
            if(!context->frames[i].pixels || !context->frames[i].preview) {
                fprintf(stderr, "camera: failed to allocate preview frames\n");
                camera_release(context);
                return false;
            }
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if(camera_ioctl(context->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
        camera_release(context);
        return false;
    }
    context->streaming = true;
    fprintf(stdout, "camera: streaming %ux%u NV12 from %s\n",
            context->width, context->height, device);
    return true;
}

static void camera_mirror_pixels(camera_context_t *context)
{
    if(!context || !context->pixels) return;
    for(uint32_t y = 0; y < context->height; ++y) {
        uint16_t *row = context->pixels + (size_t)y * context->width;
        for(uint32_t left = 0, right = context->width - 1U;
            left < right; ++left, --right) {
            uint16_t pixel = row[left];
            row[left] = row[right];
            row[right] = pixel;
        }
    }
}

static bool camera_dequeue_frame(camera_context_t *context, bool render)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.m.planes = planes;
    buffer.length = context->plane_count;
    if(camera_ioctl(context->fd, VIDIOC_DQBUF, &buffer) < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) return false;
        fprintf(stderr, "camera: VIDIOC_DQBUF failed: %s\n", strerror(errno));
        return false;
    }
    if(buffer.index >= context->buffer_count) return false;

    /* Drain only the bounded capture queue; processing an old frame adds visible latency. */
    for(uint32_t i = 1; i < context->buffer_count; ++i) {
        struct v4l2_plane next_planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer next = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .m.planes = next_planes,
            .length = context->plane_count,
        };
        if(camera_ioctl(context->fd, VIDIOC_DQBUF, &next) < 0) break;
        if(next.index >= context->buffer_count) {
            (void)camera_ioctl(context->fd, VIDIOC_QBUF, &buffer);
            return false;
        }
        if(camera_ioctl(context->fd, VIDIOC_QBUF, &buffer) < 0) {
            (void)camera_ioctl(context->fd, VIDIOC_QBUF, &next);
            return false;
        }
        buffer = next;
        memcpy(planes, next_planes, sizeof(planes));
        buffer.m.planes = planes;
    }

    camera_buffer_t *mapped = &context->buffers[buffer.index];
    context->capture_time_ms = camera_monotonic_ms();
    if((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        context->capture_time_ms = (uint64_t)buffer.timestamp.tv_sec * 1000U +
                                    (uint64_t)buffer.timestamp.tv_usec / 1000U;
    }
    const uint8_t *y_plane = (const uint8_t *)mapped->address[0] + mapped->data_offset[0];
    const uint8_t *uv_plane;
    if(context->plane_count > 1) {
        uv_plane = (const uint8_t *)mapped->address[1] + mapped->data_offset[1];
    }
    else {
        uv_plane = y_plane + (size_t)context->y_stride * context->height;
    }
    if(render)
        binbot_camera_renderer_nv12(context->renderer, y_plane, context->y_stride,
                                    uv_plane, context->uv_stride, context->pixels);
    if(render && binbot_face_detector_is_available(context->face_detector)) {
        binbot_face_submit_status_t submit = binbot_face_detector_submit_nv12(
            context->face_detector, y_plane, context->y_stride,
            uv_plane, context->uv_stride);
        if(submit == BINBOT_FACE_SUBMIT_ERROR) {
            if(!context->face_submit_error_logged) {
                fprintf(stderr, "vision: face frame submission failed: %s\n",
                        binbot_face_detector_error(context->face_detector));
                context->face_submit_error_logged = true;
            }
        }
        else if(submit == BINBOT_FACE_SUBMIT_ACCEPTED) {
            context->face_submit_error_logged = false;
        }
        if(binbot_face_detector_latest(context->face_detector,
                                       context->face_sequence,
                                       &context->face_result)) {
            context->face_sequence = context->face_result.sequence;
            if(context->log_face_recognition_results &&
               context->face_result.count > 0) {
                const binbot_face_box_t *face = &context->face_result.faces[0];
                fprintf(stdout,
                        "vision: face detection result sequence=%llu track=%u "
                        "score=%u box=%d,%d,%d,%d\n",
                        (unsigned long long)context->face_result.sequence,
                        face->track_id, face->score, face->left, face->top,
                        face->right, face->bottom);
            }
        }
        uint64_t now = camera_monotonic_ms();
        if(context->face_result.sequence > 0 && now >= context->face_result.monotonic_ms &&
           now - context->face_result.monotonic_ms <= FACE_RESULT_TTL_MS) {
            binbot_face_draw_rgb565(context->pixels, context->width, context->height,
                                    context->width, &context->face_result,
                                    0x07ff, 3);
            if(binbot_face_recognizer_is_available(context->face_recognizer) &&
               context->face_result.count > 0) {
                binbot_face_recognition_submit_status_t recognition_submit =
                    binbot_face_recognizer_submit_nv12(
                        context->face_recognizer, y_plane, context->y_stride,
                        uv_plane, context->uv_stride,
                        &context->face_result.faces[0]);
                if(recognition_submit == BINBOT_FACE_RECOGNITION_ERROR) {
                    if(!context->face_recognition_error_logged) {
                        fprintf(stderr, "vision: face recognition submission failed: %s\n",
                                binbot_face_recognizer_error(context->face_recognizer));
                        context->face_recognition_error_logged = true;
                    }
                }
                else if(recognition_submit == BINBOT_FACE_RECOGNITION_ACCEPTED) {
                    context->face_recognition_error_logged = false;
                }
            }
            if(binbot_face_recognizer_latest(
                   context->face_recognizer, context->face_recognition_sequence,
                   &context->face_recognition)) {
                context->face_recognition_sequence =
                    context->face_recognition.sequence;
                if(context->log_face_recognition_results) {
                    fprintf(stdout,
                            "vision: face recognition result sequence=%llu "
                            "track=%u score=%.4f state=%s\n",
                            (unsigned long long)context->face_recognition.sequence,
                            context->face_recognition.track_id,
                            context->face_recognition.score,
                            face_match_state_name(context->face_recognition.state));
                }
            }
        }
    }
    if(binbot_hand_detector_is_available(context->hand_detector)) {
        binbot_hand_submit_status_t submit = binbot_hand_detector_submit_nv12(
            context->hand_detector, y_plane, context->y_stride,
            uv_plane, context->uv_stride);
        if(submit == BINBOT_HAND_SUBMIT_ERROR) {
            if(!context->hand_submit_error_logged) {
                fprintf(stderr, "vision: hand frame submission failed: %s\n",
                        binbot_hand_detector_error(context->hand_detector));
                context->hand_submit_error_logged = true;
            }
        }
        else if(submit == BINBOT_HAND_SUBMIT_ACCEPTED) {
            context->hand_submit_error_logged = false;
        }
        if(binbot_hand_detector_latest(context->hand_detector,
                                       context->hand_sequence,
                                       &context->hand_result)) {
            context->hand_sequence = context->hand_result.sequence;
            pthread_mutex_lock(&context->frame_mutex);
            context->published_hand = context->hand_result;
            pthread_mutex_unlock(&context->frame_mutex);
        }
        uint64_t now = camera_monotonic_ms();
        if(render && context->hand_result.sequence > 0 &&
           context->hand_result.presence_score > 0.0f &&
           now >= context->hand_result.monotonic_ms &&
           now - context->hand_result.monotonic_ms <= HAND_RESULT_TTL_MS) {
            binbot_hand_draw_rgb565(context->pixels, context->width,
                                    context->height, context->width,
                                    &context->hand_result, 0xffe0, 2);
        }
    }
    if(render && binbot_cup_detector_is_available(context->cup_detector)) {
        binbot_cup_submit_status_t submit = binbot_cup_detector_submit_nv12(
            context->cup_detector, y_plane, context->y_stride,
            uv_plane, context->uv_stride);
        if(submit == BINBOT_CUP_SUBMIT_ERROR) {
            if(!context->cup_submit_error_logged) {
                fprintf(stderr, "vision: cup frame submission failed: %s\n",
                        binbot_cup_detector_error(context->cup_detector));
                context->cup_submit_error_logged = true;
            }
        }
        else if(submit == BINBOT_CUP_SUBMIT_ACCEPTED) {
            context->cup_submit_error_logged = false;
        }
        if(binbot_cup_detector_latest(context->cup_detector,
                                      context->cup_sequence,
                                      &context->cup_result)) {
            context->cup_sequence = context->cup_result.sequence;
        }
        uint64_t now = camera_monotonic_ms();
        if(context->cup_result.sequence > 0 &&
           context->cup_result.count > 0 &&
           now >= context->cup_result.monotonic_ms &&
           now - context->cup_result.monotonic_ms <= CUP_RESULT_TTL_MS) {
            binbot_cup_draw_rgb565(context->pixels, context->width,
                                   context->height, context->width,
                                   &context->cup_result, 0xfbc0, 3);
        }
    }
    if(render && atomic_load(&context->mirrored)) camera_mirror_pixels(context);
    if(camera_ioctl(context->fd, VIDIOC_QBUF, &buffer) < 0) {
        fprintf(stderr, "camera: VIDIOC_QBUF failed after frame: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static void *camera_worker(void *argument)
{
    camera_context_t *c = argument;
    if(c->gestures_only) {
        /* Keep background vision behind the LVGL thread on the single-core
         * target.  The camera page keeps its normal scheduling priority. */
        (void)setpriority(PRIO_PROCESS, 0, 5);
    }
    camera_start_hand_detector(c);
    if(!c->gestures_only) {
        camera_start_face_detector(c);
        camera_start_face_recognizer(c);
        camera_start_cup_detector(c);
    }
    const bool stats = environment_enabled("BINBOT_CAMERA_STATS", false);
    uint64_t started = camera_monotonic_ms(), processing = 0, age = 0;
    const uint32_t frame_interval_ms = c->gestures_only
                                           ? CAMERA_GESTURE_FRAME_INTERVAL_MS
                                           : CAMERA_FRAME_INTERVAL_MS;
    uint64_t next_frame = 0;
    unsigned frames = 0;
    while(!atomic_load(&c->stopping)) {
        if(atomic_load(&c->paused)) {
            next_frame = 0;
            struct timespec delay = {.tv_nsec = 33000000};
            nanosleep(&delay, NULL);
            continue;
        }
        /* Leave CPU time for LVGL instead of converting frames it cannot present. */
        uint64_t now = camera_monotonic_ms();
        if(now < next_frame) {
            struct timespec delay = {.tv_nsec = (long)(next_frame - now) * 1000000L};
            nanosleep(&delay, NULL);
            continue;
        }
        struct pollfd capture = {.fd = c->fd, .events = POLLIN};
        int result = poll(&capture, 1, 50);
        if(result < 0 && errno == EINTR) continue;
        if(result < 0 || (capture.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "camera: capture polling failed\n");
            break;
        }
        if(result == 0 || !(capture.revents & POLLIN)) continue;
        uint64_t before = camera_monotonic_ms();
        next_frame = before + frame_interval_ms;
        c->pixels = c->frames[c->writing_frame].pixels;
        bool render = !c->gestures_only && !atomic_load(&c->recognition_only);
        if(!camera_dequeue_frame(c, render)) continue;
        if(!render) continue;
        binbot_camera_renderer_preview(c->renderer, c->pixels, c->frames[c->writing_frame].preview);

        /* Front belongs to LVGL, pending may be replaced, and only the worker writes back. */
        pthread_mutex_lock(&c->frame_mutex);
        int replaced = c->pending_frame;
        c->pending_frame = c->writing_frame;
        if(replaced >= 0) c->writing_frame = replaced;
        else {
            for(int i = 0; i < CAMERA_FRAME_COUNT; ++i) {
                if(i != c->front_frame && i != c->pending_frame) { c->writing_frame = i; break; }
            }
        }
        pthread_mutex_unlock(&c->frame_mutex);

        processing += camera_monotonic_ms() - before;
        ++frames;
        now = camera_monotonic_ms();
        if(now >= c->capture_time_ms) age += now - c->capture_time_ms;
        if(stats && now - started >= 5000) {
            unsigned displayed = atomic_exchange(&c->presented_frames, 0);
            fprintf(stdout, "camera: preview %.1f fps, display %.1f fps, processing %.1f ms, frame age %.1f ms (%s)\n",
                    frames * 1000.0 / (now - started), displayed * 1000.0 / (now - started),
                    processing * 1.0 / frames, age * 1.0 / frames,
                    binbot_camera_renderer_accelerated(c->renderer) ? "RGA" : "software");
            started = now; frames = 0; processing = 0; age = 0;
        }
    }
    return NULL;
}

void *ui_camera_gestures_start(void)
{
    const char *device = getenv("BINBOT_CAMERA_DEVICE");
    if(!device || !device[0]) device = "/dev/video11";
    if(access(device, R_OK | W_OK) != 0 || !environment_enabled("BINBOT_HAND_DETECTION", true)) return NULL;
    camera_context_t *c = calloc(1, sizeof(*c));
    if(!c) return NULL;
    c->fd = -1; c->gestures_only = true;
    atomic_init(&c->stopping, false); atomic_init(&c->paused, false);
    atomic_init(&c->recognition_only, true);
    atomic_init(&c->mirrored, false); atomic_init(&c->presented_frames, 0);
    if(pthread_mutex_init(&c->frame_mutex, NULL) != 0) { free(c); return NULL; }
    c->mutex_ready = true;
    if(!camera_open(c) || pthread_create(&c->worker, NULL, camera_worker, c) != 0) {
        camera_release(c); free(c); return NULL;
    }
    c->worker_started = true;
    fprintf(stdout, "gestures: background local recognition started\n");
    return c;
}

void ui_camera_gestures_stop(void *capture)
{
    if(!capture) return;
    camera_release(capture); free(capture);
}

bool ui_camera_gestures_latest(void *capture, uint64_t sequence, binbot_hand_result_t *result)
{
    camera_context_t *c = capture;
    if(!c || !result || !c->mutex_ready) return false;
    pthread_mutex_lock(&c->frame_mutex);
    bool fresh = c->published_hand.sequence > sequence;
    if(fresh) *result = c->published_hand;
    pthread_mutex_unlock(&c->frame_mutex);
    return fresh;
}

static void camera_timer(lv_timer_t *timer)
{
    camera_context_t *c = lv_timer_get_user_data(timer);
    binbot_hand_result_t hand;
    if(ui_camera_gestures_latest(c, c->consumed_hand_sequence, &hand)) {
        c->consumed_hand_sequence = hand.sequence;
        ui_gesture_receive(c->app, &hand);
    }
    if(c->content_paused || c->gallery) return;
    pthread_mutex_lock(&c->frame_mutex);
    int next = c->pending_frame;
    if(next >= 0) {
        c->front_frame = next;
        c->pending_frame = -1;
    }
    pthread_mutex_unlock(&c->frame_mutex);
    if(next < 0) return;
    lv_canvas_set_buffer(c->canvas, c->frames[next].preview,
                         CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT, LV_COLOR_FORMAT_XRGB8888);
    c->frame_presented = true;
    atomic_fetch_add(&c->presented_frames, 1);
}

static void camera_destroy(void *user_data)
{
    camera_context_t *context = user_data;
    if(!context) return;
    if(context->timer) {
        lv_timer_delete(context->timer);
        context->timer = NULL;
    }
    if(context->controls_timer) lv_timer_delete(context->controls_timer);
    /* The screen root is deleted immediately after this callback. */
    if(context->canvas) lv_image_set_src(context->canvas, NULL);
    camera_release(context);
    free(context->photos);
    free(context);
}

static void camera_update_capture_state(camera_context_t *context)
{
    bool hidden = context->content_paused || context->gallery;
    bool gestures = app_state_model(context->app->state)->gestures.enabled &&
                    environment_enabled("BINBOT_HAND_DETECTION", true);
    atomic_store(&context->recognition_only, hidden);
    atomic_store(&context->paused, hidden && !gestures);
    if(!context->timer) return;
    if(hidden && !gestures) lv_timer_pause(context->timer);
    else {
        lv_timer_resume(context->timer);
        lv_timer_ready(context->timer);
    }
}

void ui_camera_gestures_changed(ui_app_t *app)
{
    if(app && app->route == UI_ROUTE_CAMERA && app->current.context)
        camera_update_capture_state(app->current.context);
}

static void camera_set_paused(void *user_data, bool paused)
{
    camera_context_t *context = user_data;
    if(!context) return;
    context->content_paused = paused;
    camera_update_capture_state(context);
    if(context->controls_timer) {
        if(paused) lv_timer_pause(context->controls_timer);
        else lv_timer_resume(context->controls_timer);
    }
}

static void camera_back_clicked(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    ui_app_go_back(app);
}

static void camera_image(lv_obj_t *image, const char *path, int width, int height)
{
    char source[400];
    snprintf(source, sizeof(source), "A:%s", path);
    lv_image_set_src(image, source);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static void camera_load_photos(camera_context_t *c)
{
    const char *directory = getenv("BINBOT_PHOTO_ROOT");
    snprintf(c->photo_root, sizeof(c->photo_root), "%s", directory && directory[0] ? directory : "/oem/data/photos");
    struct dirent **entries = NULL;
    int count = scandir(c->photo_root, &entries, NULL, alphasort);
    c->photo_capacity = count > 0 ? (size_t)count + 1 : 8;
    c->photos = calloc(c->photo_capacity, sizeof(*c->photos));
    if(!c->photos) c->photo_capacity = 0;
    for(int i = count - 1; i >= 0; --i) {
        const char *name = entries[i]->d_name;
        size_t length = strlen(name);
        if(c->photos && strncmp(name, "shot-", 5) == 0 &&
           length > 4 && length < 64 && strcmp(name + length - 4, ".png") == 0)
            snprintf(c->photos[c->photo_count++], sizeof(c->photos[0]), "%s/%.63s", c->photo_root, name);
        free(entries[i]);
    }
    free(entries);
}

static void camera_controls_sync(camera_context_t *c)
{
    bool mirror = atomic_load(&c->mirrored);
    lv_obj_set_style_bg_color(c->mirror_button, lv_color_hex(mirror ? UI_REF_INK : 0xffffff), 0);
    lv_obj_set_style_bg_image_opa(c->mirror_button, mirror ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_opa(c->mirror_button, mirror ? LV_OPA_TRANSP : 31, 0);
    lv_obj_set_style_bg_color(c->mirror_disc, lv_color_hex(mirror ? 0xffffff : 0xe7e7e7), 0);
    lv_obj_set_style_bg_opa(c->mirror_disc, mirror ? 38 : LV_OPA_COVER, 0);
    lv_label_set_text(c->mirror_label, mirror ? "镜像 开" : "镜像 关");
    lv_obj_set_style_text_color(c->mirror_label, lv_color_hex(mirror ? 0xffffff : 0x525252), 0);
    lv_obj_t *image = lv_obj_get_child(c->mirror_icon, 0);
    lv_obj_set_style_image_recolor(image, lv_color_hex(mirror ? 0xffffff : 0x525252), 0);
    ui_ref_icon_set_name(c->mirror_icon, mirror ? "camera_mirror_on" : "camera_mirror_off");
    lv_obj_set_flag(c->nonmirror_badge, LV_OBJ_FLAG_HIDDEN, mirror);
    lv_obj_set_flag(c->thumbnail_icon, LV_OBJ_FLAG_HIDDEN, c->photo_count > 0);
    lv_obj_set_flag(c->thumbnail, LV_OBJ_FLAG_HIDDEN, c->photo_count == 0);
    lv_obj_set_flag(c->shot_count, LV_OBJ_FLAG_HIDDEN, c->photo_count == 0);
    if(c->photo_count) {
        char text[12]; snprintf(text, sizeof(text), "%zu", c->photo_count);
        lv_obj_t *label = lv_obj_get_child(c->shot_count, 0);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_update_layout(label);
        int width = lv_obj_get_width(label) + 12;
        if(width < 24) width = 24;
        lv_obj_set_width(c->shot_count, width);
        lv_obj_set_x(c->shot_count, 92 - width);
        lv_obj_center(label);
        camera_image(c->thumbnail, c->photos[0], 96, 72);
    }
}

static bool camera_capture(camera_context_t *c)
{
    if(c->timer && !c->frame_presented) {
        ui_app_show_toast(c->app, "画面尚未就绪");
        return false;
    }
    const uint16_t *pixels = c->frames[c->front_frame].pixels;
    if(c->photo_count == c->photo_capacity) {
        size_t capacity = c->photo_capacity + 8;
        void *photos = realloc(c->photos, capacity * sizeof(*c->photos));
        if(!photos) return false;
        c->photos = photos;
        c->photo_capacity = capacity;
    }
    if(mkdir(c->photo_root, 0755) != 0 && errno != EEXIST) {
        ui_app_show_toast(c->app, "无法保存照片");
        return false;
    }
    unsigned char *rgb = malloc(CAMERA_PHOTO_WIDTH * CAMERA_PHOTO_HEIGHT * 3U);
    if(!rgb) return false;
    uint32_t crop_width = c->width, crop_height = c->height;
    if((uint64_t)crop_width * CAMERA_PHOTO_HEIGHT > (uint64_t)crop_height * CAMERA_PHOTO_WIDTH)
        crop_width = (crop_height * CAMERA_PHOTO_WIDTH / CAMERA_PHOTO_HEIGHT) & ~1U;
    else crop_height = (crop_width * CAMERA_PHOTO_HEIGHT / CAMERA_PHOTO_WIDTH) & ~1U;
    for(int y = 0; y < CAMERA_PHOTO_HEIGHT; ++y) {
        for(int x = 0; x < CAMERA_PHOTO_WIDTH; ++x) {
            unsigned char *pixel = rgb + (y * CAMERA_PHOTO_WIDTH + x) * 3;
            if(pixels && crop_width && crop_height) {
                uint32_t sx = (c->width - crop_width) / 2 + x * crop_width / CAMERA_PHOTO_WIDTH;
                uint32_t sy = (c->height - crop_height) / 2 + y * crop_height / CAMERA_PHOTO_HEIGHT;
                uint16_t value = pixels[sy * c->width + sx];
                pixel[0] = ((value >> 11) & 31) * 255 / 31;
                pixel[1] = ((value >> 5) & 63) * 255 / 63;
                pixel[2] = (value & 31) * 255 / 31;
            }
            else {
                double t = (x + y) / (double)(CAMERA_PHOTO_WIDTH + CAMERA_PHOTO_HEIGHT);
                pixel[0] = (unsigned char)(243 + (207 - 243) * t);
                pixel[1] = (unsigned char)(230 + (198 - 230) * t);
                pixel[2] = (unsigned char)(220 + (191 - 220) * t);
                if((x - 448) * (x - 448) + (y - 144) * (y - 144) < 2209) {
                    pixel[0] = 232; pixel[1] = 100; pixel[2] = 60;
                }
            }
        }
    }
    LodePNGState state;
    lodepng_state_init(&state);
    state.info_raw.colortype = LCT_RGB;
    state.info_raw.bitdepth = 8;
    state.info_png.color.colortype = LCT_RGB;
    state.info_png.color.bitdepth = 8;
    state.encoder.auto_convert = 0;
    /* Store blocks keep the shutter responsive on the device's single core. */
    state.encoder.zlibsettings.btype = 0;
    unsigned char *png = NULL;
    size_t size = 0;
    unsigned error = lodepng_encode(&png, &size, rgb, CAMERA_PHOTO_WIDTH, CAMERA_PHOTO_HEIGHT, &state);
    lodepng_state_cleanup(&state);
    free(rgb);
    char path[384];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    snprintf(path, sizeof(path), "%s/shot-%013llu-%09ld.png", c->photo_root,
             (unsigned long long)now.tv_sec * 1000U + now.tv_nsec / 1000000U, now.tv_nsec);
    char temporary[400];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if(!error) {
        FILE *file = fopen(temporary, "wb");
        if(!file) error = 79;
        else {
            if(fwrite(png, 1, size, file) != size) error = 79;
            if(fclose(file) != 0) error = 79;
            if(!error && rename(temporary, path) != 0) error = 79;
        }
    }
    lv_free(png);
    if(error) {
        unlink(temporary);
        ui_app_show_toast(c->app, "无法保存照片");
        return false;
    }
    memmove(c->photos[1], c->photos[0], c->photo_count * sizeof(c->photos[0]));
    snprintf(c->photos[0], sizeof(c->photos[0]), "%s", path);
    ++c->photo_count;
    c->flash_until = camera_monotonic_ms() + 220;
    lv_obj_remove_flag(c->flash, LV_OBJ_FLAG_HIDDEN);
    camera_controls_sync(c);
    return true;
}

static void camera_gallery_open(camera_context_t *c);

static void camera_close_gallery(camera_context_t *c)
{
    lv_obj_delete(c->gallery);
    c->gallery = NULL;
    c->frame_presented = false;
    camera_update_capture_state(c);
}

static void camera_gallery_close(lv_event_t *event)
{
    camera_close_gallery(lv_event_get_user_data(event));
}

static void camera_photo_select(lv_event_t *event)
{
    camera_context_t *c = lv_event_get_user_data(event);
    c->viewed = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    lv_obj_delete(c->gallery);
    c->gallery = NULL;
    camera_gallery_open(c);
}

static void camera_photo_delete(lv_event_t *event)
{
    camera_context_t *c = lv_event_get_user_data(event);
    char path[384], source[400];
    snprintf(path, sizeof(path), "%s", c->photos[c->viewed]);
    snprintf(source, sizeof(source), "A:%s", path);
    lv_obj_delete(c->gallery);
    c->gallery = NULL;
    lv_image_set_src(c->thumbnail, NULL);
    if(unlink(path) != 0) {
        ui_app_show_toast(c->app, "无法删除照片");
        camera_controls_sync(c);
        camera_gallery_open(c);
        return;
    }
    lv_image_cache_drop(source);
    memmove(c->photos[c->viewed], c->photos[c->viewed + 1],
            (c->photo_count - c->viewed - 1) * sizeof(c->photos[0]));
    --c->photo_count;
    c->viewed = 0;
    camera_controls_sync(c);
    if(c->photo_count) camera_gallery_open(c);
    else {
        camera_update_capture_state(c);
    }
}

static void camera_gallery_open(camera_context_t *c)
{
    if(c->photo_count == 0) return;
    c->gallery = ui_box(c->root, 0, 0, 720, 720, 0xefefef, 0);
    camera_update_capture_state(c);
    lv_obj_set_style_bg_opa(c->gallery, 0, 0);
    lv_obj_add_flag(c->gallery, LV_OBJ_FLAG_CLICKABLE);
    ui_ref_backdrop(c->gallery, c->root, 0xefefef, 204, 24);
    lv_obj_t *back = ui_ref_button(c->gallery, 20, 20, 56, 56, 28, "circle50");
    lv_obj_t *icon = ui_ref_icon(back, "back", 24, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(back, camera_gallery_close, LV_EVENT_CLICKED, c);
    char title[48]; snprintf(title, sizeof(title), "相册 · %zu/%zu", c->viewed + 1, c->photo_count);
    ui_ref_label(c->gallery, title, 15, UI_FONT_REGULAR, UI_REF_FAINT, 92, 24);
    ui_ref_label(c->gallery, "刚刚拍摄", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 92, 47);
    lv_obj_t *delete = ui_ref_button(c->gallery, 598, 21, 102, 56, 28, "circle50");
    icon = ui_ref_icon(delete, "trash", 20, UI_REF_INK);
    lv_obj_set_pos(icon, 20, 18);
    ui_ref_label(delete, "删除", 17, UI_FONT_MEDIUM, UI_REF_INK, 48, 18);
    lv_obj_add_event_cb(delete, camera_photo_delete, LV_EVENT_CLICKED, c);
    char source[400];
    snprintf(source, sizeof(source), "A:%s", c->photos[c->viewed]);
    lv_image_header_t header;
    int width = 680, height = 510;
    if(lv_image_decoder_get_info(source, &header) == LV_RESULT_OK && header.w && header.h) {
        if((uint64_t)header.w * height > (uint64_t)header.h * width)
            height = width * header.h / header.w;
        else width = height * header.w / header.h;
    }
    lv_obj_t *frame = ui_box(c->gallery, (720 - width) / 2, 94 + (510 - height) / 2,
                             width, height, 0, 26);
    lv_obj_set_style_clip_corner(frame, true, 0);
    ui_ref_shadow(frame, 0, 102, 40, -20, 20);
    lv_obj_t *image = lv_image_create(frame);
    camera_image(image, c->photos[c->viewed], width, height);
    lv_obj_t *list = ui_box(c->gallery, 17, 613, 686, 90, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *selected = NULL;
    for(size_t i = 0; i < c->photo_count; ++i) {
        lv_obj_t *button = ui_button(list, 3 + i * 122, 3, 112, 84, 0, 16);
        lv_obj_set_style_clip_corner(button, true, 0);
        image = lv_image_create(button);
        camera_image(image, c->photos[i], 112, 84);
        lv_obj_set_style_outline_color(button, lv_color_hex(UI_REF_CLAY), 0);
        lv_obj_set_style_outline_width(button, i == c->viewed ? 3 : 0, 0);
        if(i == c->viewed) selected = button;
        lv_obj_set_user_data(button, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(button, camera_photo_select, LV_EVENT_CLICKED, c);
    }
    if(selected) lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
}

static void camera_album_clicked(lv_event_t *event)
{
    camera_context_t *c = lv_event_get_user_data(event);
    c->viewed = 0;
    camera_gallery_open(c);
}

static void camera_mirror_clicked(lv_event_t *event)
{
    camera_context_t *c = lv_event_get_user_data(event);
    camera_mirror_preference = !atomic_load(&c->mirrored);
    atomic_store(&c->mirrored, camera_mirror_preference);
    camera_controls_sync(c);
}

static void camera_shutter_clicked(lv_event_t *event)
{
    camera_context_t *c = lv_event_get_user_data(event);
    camera_capture(c);
}

bool ui_camera_take_photo(ui_app_t *app)
{
    if(!app || app->route != UI_ROUTE_CAMERA || !app->current.context) return false;
    camera_context_t *c = app->current.context;
    if(c->gallery) { camera_close_gallery(c); return false; }
    return c->streaming && c->frame_presented && camera_capture(c);
}

static void camera_controls_tick(lv_timer_t *timer)
{
    camera_context_t *c = lv_timer_get_user_data(timer);
    uint64_t now = camera_monotonic_ms();
    if(c->flash_until && now >= c->flash_until) {
        c->flash_until = 0;
        lv_obj_add_flag(c->flash, LV_OBJ_FLAG_HIDDEN);
    }
}

ui_screen_t ui_camera_screen_create(ui_app_t *app)
{
    camera_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app; c->fd = -1;
    atomic_init(&c->stopping, false);
    atomic_init(&c->paused, false);
    atomic_init(&c->recognition_only, false);
    atomic_init(&c->presented_frames, 0);
    c->pending_frame = -1; c->writing_frame = 1;
    if(pthread_mutex_init(&c->frame_mutex, NULL) != 0) { free(c); return (ui_screen_t){0}; }
    c->mutex_ready = true;
    if(!camera_mirror_preference_initialized) {
        camera_mirror_preference = environment_enabled("BINBOT_CAMERA_MIRROR", true);
        camera_mirror_preference_initialized = true;
    }
    atomic_init(&c->mirrored, camera_mirror_preference);
    c->root = ui_ref_page(app);
    /* The updated camera reference uses the full top edge for the viewfinder. */
    lv_obj_add_flag(lv_obj_get_child(c->root, 0), LV_OBJ_FLAG_HIDDEN);
    c->preview = ui_box(c->root, 20, 41, CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT, 0x2a2a2a, 30);
    lv_obj_set_style_clip_corner(c->preview, true, 0);
    bool available = camera_open(c);
    c->status = ui_ref_label(c->preview, "", 13, UI_FONT_REGULAR, UI_REF_MUTED, 16, 470);
    lv_obj_add_flag(c->status, LV_OBJ_FLAG_HIDDEN);
    if(available) {
        c->canvas = lv_canvas_create(c->preview);
        lv_canvas_set_buffer(c->canvas, c->frames[c->front_frame].preview,
                             CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT, LV_COLOR_FORMAT_XRGB8888);
        c->timer = lv_timer_create(camera_timer, CAMERA_REFRESH_MS, c);
    }
    else {
        ui_ref_surface(c->preview, "camera_landscape");
        lv_obj_t *notice = ui_ref_label(c->preview, "未检测到摄像头 · 预览模拟画面", 16, UI_FONT_REGULAR, 0x77716d, 0, 466);
        lv_obj_set_width(notice, CAMERA_PREVIEW_WIDTH);
        lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
    }
    for(int i = 1; i < 3; ++i) {
        lv_obj_t *line = ui_box(c->preview, i * CAMERA_PREVIEW_WIDTH / 3, 0, 1, CAMERA_PREVIEW_HEIGHT, 0xffffff, 0);
        lv_obj_set_style_bg_opa(line, 51, 0);
        line = ui_box(c->preview, 0, i * CAMERA_PREVIEW_HEIGHT / 3, CAMERA_PREVIEW_WIDTH, 1, 0xffffff, 0);
        lv_obj_set_style_bg_opa(line, 51, 0);
    }
    lv_obj_t *focus = ui_box(c->preview, 300, 215, 80, 80, 0, 0);
    lv_obj_set_style_bg_opa(focus, 0, 0);
    ui_ref_surface(focus, "camera_focus_landscape");
    lv_obj_t *back = ui_button(c->preview, 14, 14, 48, 48, 0, 24);
    lv_obj_set_style_bg_opa(back, 89, 0);
    lv_obj_t *icon = ui_ref_icon(back, "back", 22, 0xffffff);
    lv_obj_center(icon);
    lv_obj_add_event_cb(back, camera_back_clicked, LV_EVENT_CLICKED, app);
    c->nonmirror_badge = ui_box(c->preview, 573, 18, 93, 40, 0, 20);
    lv_obj_set_style_bg_opa(c->nonmirror_badge, 89, 0);
    ui_centered_label(c->nonmirror_badge, "非镜像", ui_ref_font(15, UI_FONT_REGULAR), 0xe6e6e6);
    c->flash = ui_box(c->preview, 0, 0, CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT, 0xffffff, 30);
    lv_obj_set_style_bg_opa(c->flash, 230, 0);
    lv_obj_add_flag(c->flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *controls = ui_box(c->root, 20, 588, 680, 112, 0xe6e6e6, 32);
    ui_ref_surface(controls, "camera_controls");
    lv_obj_set_style_clip_corner(controls, true, 0);
    lv_obj_t *album = ui_ref_button(controls, 24, 20, 96, 72, 20, "camera_album_landscape");
    ui_ref_shadow(album, 0, 31, 30, -12, 10);
    lv_obj_set_style_clip_corner(album, true, 0);
    c->thumbnail_icon = ui_ref_icon(album, "camera", 26, UI_REF_FAINT);
    lv_obj_center(c->thumbnail_icon);
    c->thumbnail = lv_image_create(album);
    c->shot_count = ui_box(album, 68, 4, 24, 24, UI_REF_CLAY, 12);
    ui_centered_label(c->shot_count, "", ui_ref_font(13, UI_FONT_SEMIBOLD), 0xffffff);
    lv_obj_add_event_cb(album, camera_album_clicked, LV_EVENT_CLICKED, c);
    lv_obj_t *shutter = ui_button(controls, 296, 12, 88, 88, 0xffffff, 44);
    ui_ref_shadow(shutter, 0, 89, 30, -12, 14);
    ui_box(shutter, 11, 11, 66, 66, UI_REF_CLAY, 33);
    lv_obj_add_event_cb(shutter, camera_shutter_clicked, LV_EVENT_CLICKED, c);
    c->mirror_button = ui_ref_button(controls, 526, 28, 130, 56, 28, "camera_mirror");
    ui_ref_shadow(c->mirror_button, 0, 31, 30, -12, 10);
    c->mirror_disc = ui_box(c->mirror_button, 12, 10, 36, 36, 0xffffff, 18);
    c->mirror_icon = ui_ref_icon(c->mirror_disc, "camera_mirror_on", 20, 0xffffff);
    lv_obj_center(c->mirror_icon);
    c->mirror_label = ui_ref_label(c->mirror_button, "", 16, UI_FONT_MEDIUM, 0xffffff, 58, 18);
    lv_obj_add_event_cb(c->mirror_button, camera_mirror_clicked, LV_EVENT_CLICKED, c);
    camera_load_photos(c);
    camera_controls_sync(c);
    c->controls_timer = lv_timer_create(camera_controls_tick, 100, c);
    if(available) {
        if(pthread_create(&c->worker, NULL, camera_worker, c) == 0) c->worker_started = true;
        else {
            lv_timer_delete(c->timer); c->timer = NULL;
            lv_image_set_src(c->canvas, NULL);
            camera_release(c);
            ui_app_show_toast(app, "无法启动相机预览");
        }
    }
    return (ui_screen_t){c->root, c, NULL, camera_destroy, camera_set_paused};
}
