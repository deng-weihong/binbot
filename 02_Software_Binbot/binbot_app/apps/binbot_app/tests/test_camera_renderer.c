#include "binbot/camera_renderer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(!binbot_camera_renderer_create(0, 4, 4, 4, false));
    assert(!binbot_camera_renderer_create(3, 4, 4, 4, false));
    binbot_camera_renderer_t *r = binbot_camera_renderer_create(8, 4, 4, 4, false);
    assert(r && !binbot_camera_renderer_accelerated(r));
    uint16_t source[32];
    for(unsigned y = 0; y < 4; ++y) {
        uint16_t row[] = {0, 0, 0xf800, 0x07e0, 0x001f, 0xffff, 0, 0};
        memcpy(source + y * 8, row, sizeof(row));
    }
    uint32_t preview[16];
    binbot_camera_renderer_preview(r, source, preview);
    for(unsigned y = 0; y < 4; ++y) {
        assert(preview[y * 4] == 0xffff0000U);
        assert(preview[y * 4 + 1] == 0xff00ff00U);
        assert(preview[y * 4 + 2] == 0xff0000ffU);
        assert(preview[y * 4 + 3] == 0xffffffffU);
    }
    binbot_camera_renderer_destroy(r);

    r = binbot_camera_renderer_create(2, 4, 2, 2, false); assert(r);
    uint16_t vertical[] = {0, 0, 0xf800, 0xf800, 0x001f, 0x001f, 0, 0};
    binbot_camera_renderer_preview(r, vertical, preview);
    assert(preview[0] == 0xffff0000U && preview[1] == preview[0]);
    assert(preview[2] == 0xff0000ffU && preview[3] == preview[2]);
    uint8_t y_plane[] = {0, 0, 123, 123, 255, 255, 123, 123,
                         0, 0, 123, 123, 255, 255, 123, 123};
    uint8_t uv_plane[] = {128, 128, 0, 0, 128, 128, 0, 0};
    binbot_camera_renderer_nv12(r, y_plane, 4, uv_plane, 4, source);
    for(unsigned i = 0; i < 8; ++i) assert(source[i] == (i % 4 < 2 ? 0 : 0xffff));
    binbot_camera_renderer_destroy(r);
    binbot_camera_renderer_destroy(NULL);
    puts("Camera renderer: center crop, portrait crop, RGB channels and padded NV12 planes passed");
    return 0;
}
