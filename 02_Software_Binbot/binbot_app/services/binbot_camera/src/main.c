#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rtsp_demo.h"

enum {
    DEFAULT_WIDTH = 640,
    DEFAULT_HEIGHT = 480,
    DEFAULT_FPS = 15,
    DEFAULT_BITRATE_KBPS = 1024,
    DEFAULT_GOP = 15,
    DEFAULT_VI_CHANNEL = 1,
    DEFAULT_VENC_CHANNEL = 0,
    DEFAULT_RTSP_PORT = 554,
    DEFAULT_SNAPSHOT_QUALITY = 72,
};

typedef struct {
    RK_U32 width;
    RK_U32 height;
    RK_U32 fps;
    RK_U32 bitrate_kbps;
    RK_U32 gop;
    RK_S32 vi_channel;
    RK_S32 venc_channel;
    RK_S32 rtsp_port;
    char rtsp_path[96];
    char snapshot_path[256];
    char capture_request_path[256];
    int snapshot_quality;
} camera_config_t;

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} jpeg_error_t;

typedef struct {
    camera_config_t config;
    bool mpi_initialized;
    bool vi_device_owned;
    bool vi_channel_enabled;
    bool venc_channel_created;
    bool venc_started;
    bool bound;
    rtsp_demo_handle rtsp;
    rtsp_session_handle session;
} camera_service_t;

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static unsigned long environment_ulong(const char *name, unsigned long fallback,
                                       unsigned long minimum, unsigned long maximum)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if(errno != 0 || !end || *end != '\0' || value < minimum || value > maximum) {
        fprintf(stderr, "camera: ignoring invalid %s=%s\n", name, text);
        return fallback;
    }
    return value;
}

static void jpeg_error_exit(j_common_ptr common)
{
    jpeg_error_t *error = (jpeg_error_t *)common->err;
    longjmp(error->jump, 1);
}

static void load_path(char *destination, size_t capacity, const char *name,
                      const char *fallback)
{
    const char *value = getenv(name);
    if(!value || !value[0] || strlen(value) >= capacity) value = fallback;
    snprintf(destination, capacity, "%s", value);
}

static void load_config(camera_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->width = (RK_U32)environment_ulong("BINBOT_STREAM_WIDTH", DEFAULT_WIDTH, 160, 1920);
    config->height = (RK_U32)environment_ulong("BINBOT_STREAM_HEIGHT", DEFAULT_HEIGHT, 120, 1080);
    config->fps = (RK_U32)environment_ulong("BINBOT_STREAM_FPS", DEFAULT_FPS, 1, 30);
    config->bitrate_kbps = (RK_U32)environment_ulong(
        "BINBOT_STREAM_BITRATE_KBPS", DEFAULT_BITRATE_KBPS, 64, 8192);
    config->gop = (RK_U32)environment_ulong("BINBOT_STREAM_GOP", DEFAULT_GOP, 1, 300);
    config->vi_channel = (RK_S32)environment_ulong(
        "BINBOT_STREAM_VI_CHANNEL", DEFAULT_VI_CHANNEL, 0, 2);
    config->venc_channel = (RK_S32)environment_ulong(
        "BINBOT_STREAM_VENC_CHANNEL", DEFAULT_VENC_CHANNEL, 0, 15);
    config->rtsp_port = (RK_S32)environment_ulong(
        "BINBOT_STREAM_RTSP_PORT", DEFAULT_RTSP_PORT, 1, 65535);
    const char *path = getenv("BINBOT_STREAM_RTSP_PATH");
    if(!path || path[0] != '/') path = "/live/binbot";
    snprintf(config->rtsp_path, sizeof(config->rtsp_path), "%s", path);
    load_path(config->snapshot_path, sizeof(config->snapshot_path),
              "BINBOT_AGENT_IMAGE_PATH", "/tmp/binbot_agent_frame.jpg");
    load_path(config->capture_request_path,
              sizeof(config->capture_request_path),
              "BINBOT_AGENT_CAPTURE_REQUEST",
              "/tmp/binbot_agent_capture.request");
    config->snapshot_quality = (int)environment_ulong(
        "BINBOT_AGENT_JPEG_QUALITY", DEFAULT_SNAPSHOT_QUALITY, 30, 95);
}

static RK_S32 initialize_vi_device(camera_service_t *service)
{
    const VI_DEV device = 0;
    VI_DEV_ATTR_S attributes;
    VI_DEV_BIND_PIPE_S binding;
    memset(&attributes, 0, sizeof(attributes));
    memset(&binding, 0, sizeof(binding));

    RK_S32 result = RK_MPI_VI_GetDevAttr(device, &attributes);
    if(result == RK_ERR_VI_NOT_CONFIG) {
        result = RK_MPI_VI_SetDevAttr(device, &attributes);
        if(result != RK_SUCCESS) {
            fprintf(stderr, "camera: RK_MPI_VI_SetDevAttr failed: %#x\n", result);
            return result;
        }
    }
    else if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VI_GetDevAttr failed: %#x\n", result);
        return result;
    }

    result = RK_MPI_VI_GetDevIsEnable(device);
    if(result == RK_SUCCESS) return RK_SUCCESS;

    result = RK_MPI_VI_EnableDev(device);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VI_EnableDev failed: %#x\n", result);
        return result;
    }
    service->vi_device_owned = true;

    binding.u32Num = 1;
    binding.PipeId[0] = 0;
    result = RK_MPI_VI_SetDevBindPipe(device, &binding);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VI_SetDevBindPipe failed: %#x\n", result);
    }
    return result;
}

static RK_S32 initialize_vi_channel(camera_service_t *service)
{
    const camera_config_t *config = &service->config;
    VI_CHN_ATTR_S attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.stIspOpt.u32BufCount = 4;
    attributes.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attributes.stIspOpt.stMaxSize.u32Width = config->width;
    attributes.stIspOpt.stMaxSize.u32Height = config->height;
    attributes.stSize.u32Width = config->width;
    attributes.stSize.u32Height = config->height;
    attributes.enPixelFormat = RK_FMT_YUV420SP;
    attributes.enCompressMode = COMPRESS_MODE_NONE;
    /* One VI frame remains available for on-demand Agent JPEG snapshots. */
    attributes.u32Depth = 1;

    RK_S32 result = RK_MPI_VI_SetChnAttr(0, config->vi_channel, &attributes);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VI_SetChnAttr(%d) failed: %#x\n",
                config->vi_channel, result);
        return result;
    }
    result = RK_MPI_VI_EnableChn(0, config->vi_channel);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VI_EnableChn(%d) failed: %#x\n",
                config->vi_channel, result);
        return result;
    }
    service->vi_channel_enabled = true;
    return RK_SUCCESS;
}

static RK_S32 initialize_venc(camera_service_t *service)
{
    const camera_config_t *config = &service->config;
    VENC_CHN_ATTR_S attributes;
    memset(&attributes, 0, sizeof(attributes));

    attributes.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attributes.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attributes.stVencAttr.u32Profile = H264E_PROFILE_BASELINE;
    attributes.stVencAttr.bByFrame = RK_TRUE;
    attributes.stVencAttr.u32MaxPicWidth = config->width;
    attributes.stVencAttr.u32MaxPicHeight = config->height;
    attributes.stVencAttr.u32PicWidth = config->width;
    attributes.stVencAttr.u32PicHeight = config->height;
    attributes.stVencAttr.u32VirWidth = config->width;
    attributes.stVencAttr.u32VirHeight = config->height;
    attributes.stVencAttr.u32StreamBufCnt = 4;
    attributes.stVencAttr.u32BufSize = config->width * config->height * 3U / 2U;
    attributes.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attributes.stRcAttr.stH264Cbr.u32Gop = config->gop;
    attributes.stRcAttr.stH264Cbr.u32SrcFrameRateNum = config->fps;
    attributes.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attributes.stRcAttr.stH264Cbr.fr32DstFrameRateNum = config->fps;
    attributes.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attributes.stRcAttr.stH264Cbr.u32BitRate = config->bitrate_kbps;
    attributes.stRcAttr.stH264Cbr.u32StatTime = 1;
    attributes.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    RK_S32 result = RK_MPI_VENC_CreateChn(config->venc_channel, &attributes);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VENC_CreateChn(%d) failed: %#x\n",
                config->venc_channel, result);
        return result;
    }
    service->venc_channel_created = true;

    VENC_RECV_PIC_PARAM_S receive;
    memset(&receive, 0, sizeof(receive));
    receive.s32RecvPicNum = -1;
    result = RK_MPI_VENC_StartRecvFrame(config->venc_channel, &receive);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_VENC_StartRecvFrame failed: %#x\n", result);
        return result;
    }
    service->venc_started = true;
    return RK_SUCCESS;
}

static RK_S32 bind_pipeline(camera_service_t *service)
{
    MPP_CHN_S source;
    MPP_CHN_S destination;
    memset(&source, 0, sizeof(source));
    memset(&destination, 0, sizeof(destination));
    source.enModId = RK_ID_VI;
    source.s32DevId = 0;
    source.s32ChnId = service->config.vi_channel;
    destination.enModId = RK_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = service->config.venc_channel;
    RK_S32 result = RK_MPI_SYS_Bind(&source, &destination);
    if(result == RK_SUCCESS) service->bound = true;
    else fprintf(stderr, "camera: RK_MPI_SYS_Bind failed: %#x\n", result);
    return result;
}

static bool initialize_rtsp(camera_service_t *service)
{
    service->rtsp = create_rtsp_demo(service->config.rtsp_port);
    if(!service->rtsp) {
        fprintf(stderr, "camera: cannot listen on RTSP port %d\n", service->config.rtsp_port);
        return false;
    }
    service->session = rtsp_new_session(service->rtsp, service->config.rtsp_path);
    if(!service->session ||
       rtsp_set_video(service->session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0) != 0) {
        fprintf(stderr, "camera: cannot create RTSP session %s\n", service->config.rtsp_path);
        return false;
    }
    rtsp_sync_video_ts(service->session, rtsp_get_reltime(), rtsp_get_ntptime());
    return true;
}

static unsigned char clamp_color(int value)
{
    if(value < 0) return 0;
    if(value > 255) return 255;
    return (unsigned char)value;
}

static bool write_nv12_jpeg(const camera_config_t *config,
                            const VIDEO_FRAME_S *frame, const uint8_t *nv12)
{
    RK_U32 width = frame->u32Width ? frame->u32Width : config->width;
    RK_U32 height = frame->u32Height ? frame->u32Height : config->height;
    RK_U32 stride = frame->u32VirWidth ? frame->u32VirWidth : width;
    RK_U32 virtual_height = frame->u32VirHeight ? frame->u32VirHeight : height;
    if(!nv12 || width == 0 || height == 0 || stride < width ||
       virtual_height < height || frame->enPixelFormat != RK_FMT_YUV420SP ||
       frame->enCompressMode != COMPRESS_MODE_NONE) return false;

    char temporary[sizeof(config->snapshot_path) + 8U];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp",
                          config->snapshot_path);
    if(length <= 0 || (size_t)length >= sizeof(temporary)) return false;
    FILE *stream = fopen(temporary, "wb");
    if(!stream) return false;
    uint8_t *row = malloc((size_t)width * 3U);
    if(!row) {
        fclose(stream);
        unlink(temporary);
        return false;
    }

    struct jpeg_compress_struct jpeg;
    jpeg_error_t jpeg_error;
    memset(&jpeg, 0, sizeof(jpeg));
    jpeg.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_error_exit;
    if(setjmp(jpeg_error.jump)) {
        jpeg_destroy_compress(&jpeg);
        free(row);
        fclose(stream);
        unlink(temporary);
        return false;
    }
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, stream);
    jpeg.image_width = width;
    jpeg.image_height = height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, config->snapshot_quality, TRUE);
    jpeg_start_compress(&jpeg, TRUE);

    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + (size_t)stride * virtual_height;
    while(jpeg.next_scanline < jpeg.image_height) {
        RK_U32 y = jpeg.next_scanline;
        const uint8_t *y_row = y_plane + (size_t)y * stride;
        const uint8_t *uv_row = uv_plane + (size_t)(y / 2U) * stride;
        for(RK_U32 x = 0; x < width; ++x) {
            int luminance = (int)y_row[x] - 16;
            int u = (int)uv_row[x & ~1U] - 128;
            int v = (int)uv_row[(x & ~1U) + 1U] - 128;
            if(luminance < 0) luminance = 0;
            row[x * 3U] = clamp_color((298 * luminance + 409 * v + 128) >> 8);
            row[x * 3U + 1U] = clamp_color(
                (298 * luminance - 100 * u - 208 * v + 128) >> 8);
            row[x * 3U + 2U] = clamp_color(
                (298 * luminance + 516 * u + 128) >> 8);
        }
        JSAMPROW scanline = row;
        (void)jpeg_write_scanlines(&jpeg, &scanline, 1);
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    free(row);
    bool ok = fflush(stream) == 0;
    if(fsync(fileno(stream)) != 0) ok = false;
    if(fclose(stream) != 0) ok = false;
    if(ok && rename(temporary, config->snapshot_path) != 0) ok = false;
    if(!ok) unlink(temporary);
    return ok;
}

static void capture_snapshot_if_requested(camera_service_t *service)
{
    const camera_config_t *config = &service->config;
    if(access(config->capture_request_path, F_OK) != 0) return;
    (void)unlink(config->capture_request_path);
    VIDEO_FRAME_INFO_S frame;
    memset(&frame, 0, sizeof(frame));
    RK_S32 result = RK_MPI_VI_GetChnFrame(
        0, config->vi_channel, &frame, 250);
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: snapshot frame unavailable: %#x\n", result);
        return;
    }
    void *data = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
    bool ok = write_nv12_jpeg(config, &frame.stVFrame, data);
    RK_MPI_VI_ReleaseChnFrame(0, config->vi_channel, &frame);
    if(ok) fprintf(stdout, "camera: Agent snapshot updated: %s\n",
                   config->snapshot_path);
    else fprintf(stderr, "camera: failed to encode Agent snapshot\n");
}

static void release_service(camera_service_t *service)
{
    MPP_CHN_S source;
    MPP_CHN_S destination;
    memset(&source, 0, sizeof(source));
    memset(&destination, 0, sizeof(destination));
    source.enModId = RK_ID_VI;
    source.s32DevId = 0;
    source.s32ChnId = service->config.vi_channel;
    destination.enModId = RK_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = service->config.venc_channel;

    if(service->bound) RK_MPI_SYS_UnBind(&source, &destination);
    if(service->venc_started) RK_MPI_VENC_StopRecvFrame(service->config.venc_channel);
    if(service->venc_channel_created) RK_MPI_VENC_DestroyChn(service->config.venc_channel);
    if(service->vi_channel_enabled) RK_MPI_VI_DisableChn(0, service->config.vi_channel);
    if(service->vi_device_owned) RK_MPI_VI_DisableDev(0);
    if(service->mpi_initialized) RK_MPI_SYS_Exit();
    if(service->session) rtsp_del_session(service->session);
    if(service->rtsp) rtsp_del_demo(service->rtsp);
}

static int stream(camera_service_t *service)
{
    VENC_PACK_S packet;
    VENC_STREAM_S encoded;
    memset(&packet, 0, sizeof(packet));
    memset(&encoded, 0, sizeof(encoded));
    encoded.pstPack = &packet;
    uint64_t frames = 0;

    while(!stop_requested) {
        capture_snapshot_if_requested(service);
        RK_S32 result = RK_MPI_VENC_GetStream(service->config.venc_channel, &encoded, 500);
        if(result != RK_SUCCESS) continue;
        void *data = RK_MPI_MB_Handle2VirAddr(packet.pMbBlk);
        if(data && packet.u32Len > 0) {
            rtsp_tx_video(service->session, data, (int)packet.u32Len, packet.u64PTS);
            rtsp_do_event(service->rtsp);
            ++frames;
            if(frames == 1 || frames % 300 == 0) {
                fprintf(stdout, "camera: streamed %llu H.264 frames, last=%u bytes\n",
                        (unsigned long long)frames, packet.u32Len);
            }
        }
        RK_MPI_VENC_ReleaseStream(service->config.venc_channel, &encoded);
    }
    return 0;
}

int main(void)
{
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    camera_service_t service;
    memset(&service, 0, sizeof(service));
    load_config(&service.config);
    fprintf(stdout,
            "camera: RKIPC pipeline VI%d -> VENC%d H.264 %ux%u@%u %uKbps, rtsp://0.0.0.0:%d%s\n",
            service.config.vi_channel, service.config.venc_channel,
            service.config.width, service.config.height, service.config.fps,
            service.config.bitrate_kbps, service.config.rtsp_port,
            service.config.rtsp_path);

    int exit_code = 1;
    if(!initialize_rtsp(&service)) goto done;
    RK_S32 result = RK_MPI_SYS_Init();
    if(result != RK_SUCCESS) {
        fprintf(stderr, "camera: RK_MPI_SYS_Init failed: %#x\n", result);
        goto done;
    }
    service.mpi_initialized = true;
    if(initialize_vi_device(&service) != RK_SUCCESS ||
       initialize_vi_channel(&service) != RK_SUCCESS ||
       initialize_venc(&service) != RK_SUCCESS ||
       bind_pipeline(&service) != RK_SUCCESS) {
        goto done;
    }

    fprintf(stdout, "camera: stream ready\n");
    exit_code = stream(&service);

done:
    release_service(&service);
    fprintf(stdout, "camera: stopped cleanly\n");
    return exit_code;
}
