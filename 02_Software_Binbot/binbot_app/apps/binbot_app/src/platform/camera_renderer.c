#include "binbot/camera_renderer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(BINBOT_HAVE_RGA)
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "rga/im2d.h"

typedef struct {
    int fd;
    void *address;
    size_t size;
    rga_buffer_handle_t handle;
    rga_buffer_t image;
} camera_dma_buffer_t;

/* RV1106 uses Rockchip's CMA heap ABI, not the system DMA heap. */
struct camera_dma_allocation {
    __u64 len;
    __u32 fd, fd_flags;
    __u64 heap_flags;
};
#define CAMERA_CMA_ALLOC _IOWR('R', 0, struct camera_dma_allocation)

static void dma_release(camera_dma_buffer_t *buffer)
{
    if(buffer->handle) releasebuffer_handle(buffer->handle);
    if(buffer->address) munmap(buffer->address, buffer->size);
    if(buffer->fd >= 0) close(buffer->fd);
    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
}

static bool dma_allocate(camera_dma_buffer_t *buffer, uint32_t width, uint32_t height,
                          int format, size_t size)
{
    int heap = open("/dev/rk_dma_heap/rk-dma-heap-cma", O_RDWR | O_CLOEXEC);
    if(heap < 0) return false;
    struct camera_dma_allocation allocation = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    int result = ioctl(heap, CAMERA_CMA_ALLOC, &allocation);
    int saved_errno = errno;
    close(heap);
    if(result < 0) { errno = saved_errno; return false; }
    buffer->fd = (int)allocation.fd;
    buffer->size = size;
    void *address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if(address == MAP_FAILED) return false;
    buffer->address = address;
    if(madvise(address, size, MADV_DONTFORK) < 0) return false;
    im_handle_param_t parameters = {.width = width, .height = height, .format = format};
    buffer->handle = importbuffer_fd(buffer->fd, &parameters);
    if(!buffer->handle) { errno = EIO; return false; }
    buffer->image = wrapbuffer_handle_t(buffer->handle, width, height, width, height, format);
    return true;
}

static bool dma_sync(camera_dma_buffer_t *buffer, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    int result;
    do { result = ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync); } while(result < 0 && errno == EINTR);
    return result == 0;
}
#endif

struct binbot_camera_renderer {
    uint32_t width, height, preview_width, preview_height;
    uint32_t crop_x, crop_y, crop_width, crop_height;
    uint32_t *columns, *rows;
    bool accelerated;
#if defined(BINBOT_HAVE_RGA)
    camera_dma_buffer_t nv12, rgb, preview;
#endif
};

#if defined(BINBOT_HAVE_RGA)
static bool rga_operation(binbot_camera_renderer_t *r, bool preview, bool check)
{
    rga_buffer_t src = preview ? r->rgb.image : r->nv12.image;
    rga_buffer_t dst = preview ? r->preview.image : r->rgb.image;
    rga_buffer_t empty = {0};
    im_rect source = {0, 0, r->width, r->height};
    im_rect target = {0, 0, r->width, r->height}, unused = {0};
    if(preview) {
        source = (im_rect){r->crop_x, r->crop_y, r->crop_width, r->crop_height};
        target = (im_rect){0, 0, r->preview_width, r->preview_height};
    }
    else dst.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
    IM_STATUS result = check ? imcheck_t(src, dst, empty, source, target, unused, IM_SYNC) :
                              improcess(src, dst, empty, source, target, unused, IM_SYNC);
    if(result == (check ? IM_STATUS_NOERROR : IM_STATUS_SUCCESS)) return true;
    fprintf(stderr, "camera: RGA %s %s failed: %s\n", preview ? "preview" : "NV12",
            check ? "validation" : "conversion", imStrError_t(result));
    return false;
}

static void rga_release(binbot_camera_renderer_t *r)
{
    dma_release(&r->preview); dma_release(&r->rgb); dma_release(&r->nv12);
}

static void rga_initialize(binbot_camera_renderer_t *r)
{
    if(!dma_allocate(&r->nv12, r->width, r->height, RK_FORMAT_YCbCr_420_SP,
                     (size_t)r->width * r->height * 3 / 2) ||
       /* RGA names channels from the low bits; LVGL RGB565 has red in bits 11..15. */
       !dma_allocate(&r->rgb, r->width, r->height, RK_FORMAT_BGR_565,
                     (size_t)r->width * r->height * 2) ||
       !dma_allocate(&r->preview, r->preview_width, r->preview_height, RK_FORMAT_BGRA_8888,
                     (size_t)r->preview_width * r->preview_height * 4)) {
        fprintf(stderr, "camera: CMA buffer setup failed: %s; using software renderer\n", strerror(errno));
        rga_release(r);
        return;
    }
    if(!rga_operation(r, false, true) || !rga_operation(r, true, true)) {
        rga_release(r);
        return;
    }
    r->accelerated = true;
}

static bool rga_fallback(binbot_camera_renderer_t *r)
{
    fprintf(stderr, "camera: disabling RGA after conversion or DMA synchronization failure\n");
    r->accelerated = false;
    return false;
}

static bool rga_nv12(binbot_camera_renderer_t *r, const uint8_t *y, uint32_t y_stride,
                      const uint8_t *uv, uint32_t uv_stride, uint16_t *rgb565)
{
    if(!dma_sync(&r->nv12, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return rga_fallback(r);
    uint8_t *packed = r->nv12.address;
    for(uint32_t row = 0; row < r->height; ++row)
        memcpy(packed + (size_t)row * r->width, y + (size_t)row * y_stride, r->width);
    packed += (size_t)r->width * r->height;
    for(uint32_t row = 0; row < r->height / 2; ++row)
        memcpy(packed + (size_t)row * r->width, uv + (size_t)row * uv_stride, r->width);
    if(!dma_sync(&r->nv12, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) ||
       !rga_operation(r, false, false) ||
       !dma_sync(&r->rgb, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) return rga_fallback(r);
    memcpy(rgb565, r->rgb.address, r->rgb.size);
    if(!dma_sync(&r->rgb, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) return rga_fallback(r);
    return true;
}

static bool rga_preview(binbot_camera_renderer_t *r, const uint16_t *rgb565, uint32_t *xrgb8888)
{
    if(!dma_sync(&r->rgb, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return rga_fallback(r);
    memcpy(r->rgb.address, rgb565, r->rgb.size);
    if(!dma_sync(&r->rgb, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) ||
       !rga_operation(r, true, false) ||
       !dma_sync(&r->preview, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) return rga_fallback(r);
    memcpy(xrgb8888, r->preview.address, r->preview.size);
    if(!dma_sync(&r->preview, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) return rga_fallback(r);
    return true;
}
#endif

binbot_camera_renderer_t *binbot_camera_renderer_create(uint32_t width, uint32_t height,
                                                        uint32_t preview_width,
                                                        uint32_t preview_height,
                                                        bool accelerated)
{
    if(!width || !height || (width & 1) || (height & 1) || width > 1280 || height > 720 ||
       !preview_width || !preview_height || preview_width > 720 || preview_height > 720) return NULL;
    binbot_camera_renderer_t *r = calloc(1, sizeof(*r));
    if(!r) return NULL;
#if defined(BINBOT_HAVE_RGA)
    r->nv12.fd = r->rgb.fd = r->preview.fd = -1;
#endif
    r->width = width; r->height = height;
    r->preview_width = preview_width; r->preview_height = preview_height;
    r->crop_width = width; r->crop_height = height;
    if((uint64_t)width * preview_height > (uint64_t)height * preview_width)
        r->crop_width = (height * preview_width / preview_height) & ~1U;
    else r->crop_height = (width * preview_height / preview_width) & ~1U;
    if(r->crop_width < 2) r->crop_width = 2;
    if(r->crop_height < 2) r->crop_height = 2;
    r->crop_x = (width - r->crop_width) / 2;
    r->crop_y = (height - r->crop_height) / 2;
    r->columns = calloc(preview_width, sizeof(*r->columns));
    r->rows = calloc(preview_height, sizeof(*r->rows));
    if(!r->columns || !r->rows) { binbot_camera_renderer_destroy(r); return NULL; }
    for(uint32_t x = 0; x < preview_width; ++x)
        r->columns[x] = r->crop_x + x * r->crop_width / preview_width;
    for(uint32_t y = 0; y < preview_height; ++y)
        r->rows[y] = (r->crop_y + y * r->crop_height / preview_height) * width;
#if defined(BINBOT_HAVE_RGA)
    if(accelerated) rga_initialize(r);
#else
    (void)accelerated;
#endif
    return r;
}

void binbot_camera_renderer_destroy(binbot_camera_renderer_t *r)
{
    if(!r) return;
#if defined(BINBOT_HAVE_RGA)
    rga_release(r);
#endif
    free(r->columns); free(r->rows); free(r);
}

bool binbot_camera_renderer_accelerated(const binbot_camera_renderer_t *r)
{
    return r && r->accelerated;
}

static uint8_t clamp(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

void binbot_camera_renderer_nv12(binbot_camera_renderer_t *r,
                                 const uint8_t *y, uint32_t y_stride,
                                 const uint8_t *uv, uint32_t uv_stride, uint16_t *rgb565)
{
    if(!r || !y || !uv || !rgb565 || y_stride < r->width || uv_stride < r->width) return;
#if defined(BINBOT_HAVE_RGA)
    if(r->accelerated && rga_nv12(r, y, y_stride, uv, uv_stride, rgb565)) return;
#endif
    for(uint32_t row = 0; row < r->height; ++row) {
        const uint8_t *yp = y + (size_t)row * y_stride;
        const uint8_t *uvp = uv + (size_t)(row / 2) * uv_stride;
        uint16_t *out = rgb565 + (size_t)row * r->width;
        for(uint32_t x = 0; x < r->width; ++x) {
            int u = uvp[x & ~1U] - 128, v = uvp[(x & ~1U) + 1] - 128;
            unsigned red = clamp(yp[x] + ((359 * v) >> 8));
            unsigned green = clamp(yp[x] - ((88 * u + 183 * v) >> 8));
            unsigned blue = clamp(yp[x] + ((454 * u) >> 8));
            out[x] = (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
        }
    }
}

void binbot_camera_renderer_preview(binbot_camera_renderer_t *r,
                                    const uint16_t *rgb565, uint32_t *xrgb8888)
{
    if(!r || !rgb565 || !xrgb8888) return;
#if defined(BINBOT_HAVE_RGA)
    if(r->accelerated && rga_preview(r, rgb565, xrgb8888)) return;
#endif
    for(uint32_t y = 0; y < r->preview_height; ++y) {
        const uint16_t *row = rgb565 + r->rows[y];
        uint32_t *out = xrgb8888 + (size_t)y * r->preview_width;
        for(uint32_t x = 0; x < r->preview_width; ++x) {
            uint16_t pixel = row[r->columns[x]];
            uint32_t red = (pixel >> 11) & 31, green = (pixel >> 5) & 63, blue = pixel & 31;
            out[x] = 0xff000000U | (((red << 3) | (red >> 2)) << 16) |
                     (((green << 2) | (green >> 4)) << 8) | (blue << 3) | (blue >> 2);
        }
    }
}
