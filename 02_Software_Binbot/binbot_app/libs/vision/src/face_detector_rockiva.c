#include "binbot/vision/face_detector.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rockiva_det_api.h"

enum {
    FACE_INFERENCE_SLOT_COUNT = 3,
    FACE_ERROR_CAPACITY = 192,
};

typedef RockIvaRetCode (*rockiva_init_fn)(RockIvaHandle *, RockIvaWorkMode,
                                          const RockIvaInitParam *, void *);
typedef RockIvaRetCode (*rockiva_release_fn)(RockIvaHandle);
typedef RockIvaRetCode (*rockiva_wait_finish_fn)(RockIvaHandle, long, int);
typedef RockIvaRetCode (*rockiva_set_release_callback_fn)(
    RockIvaHandle, ROCKIVA_FrameReleaseCallback);
typedef RockIvaRetCode (*rockiva_push_frame_fn)(
    RockIvaHandle, const RockIvaImage *, const RockIvaFrameExtraInfo *);
typedef RockIvaRetCode (*rockiva_image_alloc_mem_fn)(RockIvaImage *, RockIvaMemType);
typedef RockIvaRetCode (*rockiva_image_free_mem_fn)(RockIvaImage *);
typedef RockIvaRetCode (*rockiva_detect_init_fn)(
    RockIvaHandle, const RockIvaDetTaskParams *, ROCKIVA_DetectResultCallback);
typedef RockIvaRetCode (*rockiva_detect_release_fn)(RockIvaHandle);

typedef struct {
    rockiva_init_fn init;
    rockiva_release_fn release;
    rockiva_wait_finish_fn wait_finish;
    rockiva_set_release_callback_fn set_release_callback;
    rockiva_push_frame_fn push_frame;
    rockiva_image_alloc_mem_fn image_alloc_mem;
    rockiva_image_free_mem_fn image_free_mem;
    rockiva_detect_init_fn detect_init;
    rockiva_detect_release_fn detect_release;
} rockiva_api_t;

typedef struct {
    RockIvaImage image;
    bool memory_allocated;
    bool busy;
} inference_slot_t;

struct binbot_face_detector {
    pthread_mutex_t mutex;
    bool mutex_ready;
    bool available;
    bool global_initialized;
    bool detector_initialized;
    bool shutting_down;
    uint16_t width;
    uint16_t height;
    uint32_t minimum_interval_ms;
    uint32_t next_frame_id;
    uint64_t last_submit_ms;
    uint64_t result_sequence;
    size_t frame_size;
    char error[FACE_ERROR_CAPACITY];
    void *library;
    RockIvaHandle handle;
    rockiva_api_t api;
    inference_slot_t slots[FACE_INFERENCE_SLOT_COUNT];
    binbot_face_result_t latest;
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void set_error(binbot_face_detector_t *detector, const char *format, ...)
{
    if(!detector) return;
    va_list args;
    va_start(args, format);
    vsnprintf(detector->error, sizeof(detector->error), format, args);
    va_end(args);
}

static bool load_symbol(binbot_face_detector_t *detector,
                        void *destination,
                        size_t destination_size,
                        const char *name)
{
    if(destination_size != sizeof(void *)) {
        set_error(detector, "unsupported function pointer size for %s", name);
        return false;
    }
    (void)dlerror();
    void *symbol = dlsym(detector->library, name);
    const char *error = dlerror();
    if(error || !symbol) {
        set_error(detector, "RockIVA symbol %s is unavailable: %s", name,
                  error ? error : "not found");
        return false;
    }
    memcpy(destination, &symbol, sizeof(symbol));
    return true;
}

#define LOAD_API(detector, member, symbol)                                      \
    load_symbol((detector), &(detector)->api.member,                            \
                sizeof((detector)->api.member), (symbol))

static bool load_api(binbot_face_detector_t *detector, const char *library_path)
{
    detector->library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if(!detector->library) {
        set_error(detector, "cannot load %s: %s", library_path, dlerror());
        return false;
    }

    return LOAD_API(detector, init, "ROCKIVA_Init") &&
           LOAD_API(detector, release, "ROCKIVA_Release") &&
           LOAD_API(detector, wait_finish, "ROCKIVA_WaitFinish") &&
           LOAD_API(detector, set_release_callback,
                    "ROCKIVA_SetFrameReleaseCallback") &&
           LOAD_API(detector, push_frame, "ROCKIVA_PushFrame") &&
           LOAD_API(detector, image_alloc_mem, "ROCKIVA_IMAGE_AllocMem") &&
           LOAD_API(detector, image_free_mem, "ROCKIVA_IMAGE_FreeMem") &&
           LOAD_API(detector, detect_init, "ROCKIVA_DETECT_Init") &&
           LOAD_API(detector, detect_release, "ROCKIVA_DETECT_Release");
}

static void release_callback(const RockIvaReleaseFrames *release_frames,
                             void *user_data)
{
    binbot_face_detector_t *detector = user_data;
    if(!detector || !release_frames || !detector->mutex_ready) return;

    pthread_mutex_lock(&detector->mutex);
    for(uint32_t frame = 0; frame < release_frames->count; ++frame) {
        void *slot_pointer = release_frames->frames[frame].extData;
        for(size_t slot = 0; slot < FACE_INFERENCE_SLOT_COUNT; ++slot) {
            if(slot_pointer == &detector->slots[slot]) {
                detector->slots[slot].busy = false;
                break;
            }
        }
    }
    pthread_mutex_unlock(&detector->mutex);
}

static int32_t clamp_ratio(int32_t value)
{
    if(value < 0) return 0;
    if(value > BINBOT_FACE_COORDINATE_SCALE) return BINBOT_FACE_COORDINATE_SCALE;
    return value;
}

static void detect_callback(const RockIvaDetectResult *result,
                            RockIvaExecuteStatus status,
                            void *user_data)
{
    binbot_face_detector_t *detector = user_data;
    if(!detector || !result || !detector->mutex_ready || status != ROCKIVA_SUCCESS) return;

    binbot_face_result_t next;
    memset(&next, 0, sizeof(next));
    next.frame_id = result->frameId;
    next.monotonic_ms = monotonic_ms();
    for(uint32_t i = 0; i < result->objNum && next.count < BINBOT_FACE_MAX_DETECTIONS; ++i) {
        const RockIvaObjectInfo *object = &result->objInfo[i];
        if(object->type != ROCKIVA_OBJECT_TYPE_FACE) continue;
        binbot_face_box_t *face = &next.faces[next.count++];
        face->track_id = object->objId;
        face->score = object->score > 100 ? 100 : (uint8_t)object->score;
        face->left = clamp_ratio(object->rect.topLeft.x);
        face->top = clamp_ratio(object->rect.topLeft.y);
        face->right = clamp_ratio(object->rect.bottomRight.x);
        face->bottom = clamp_ratio(object->rect.bottomRight.y);
    }

    pthread_mutex_lock(&detector->mutex);
    if(!detector->shutting_down) {
        next.sequence = ++detector->result_sequence;
        detector->latest = next;
    }
    pthread_mutex_unlock(&detector->mutex);
}

static void release_resources(binbot_face_detector_t *detector)
{
    if(!detector) return;
    if(detector->detector_initialized) {
        detector->api.detect_release(detector->handle);
        detector->detector_initialized = false;
    }
    for(size_t i = 0; i < FACE_INFERENCE_SLOT_COUNT; ++i) {
        if(detector->slots[i].memory_allocated && detector->api.image_free_mem) {
            detector->api.image_free_mem(&detector->slots[i].image);
        }
        memset(&detector->slots[i].image, 0, sizeof(detector->slots[i].image));
        detector->slots[i].memory_allocated = false;
        detector->slots[i].busy = false;
    }
    if(detector->global_initialized) {
        detector->api.release(detector->handle);
        detector->global_initialized = false;
        detector->handle = NULL;
    }
    if(detector->library) {
        dlclose(detector->library);
        detector->library = NULL;
    }
}

static bool prepare_model_path(binbot_face_detector_t *detector,
                               const char *directory,
                               char output[ROCKIVA_PATH_LENGTH])
{
    char model_file[ROCKIVA_PATH_LENGTH + 32];
    size_t length = strlen(directory);
    const char *separator = length > 0 && directory[length - 1] == '/' ? "" : "/";
    int written = snprintf(model_file, sizeof(model_file), "%s%sobject_detection_pfp.data",
                           directory, separator);
    if(written < 0 || (size_t)written >= sizeof(model_file) || access(model_file, R_OK) != 0) {
        set_error(detector, "RockIVA PFP model is not readable: %s", model_file);
        return false;
    }
    written = snprintf(output, ROCKIVA_PATH_LENGTH, "%s%s", directory, separator);
    if(written < 0 || written >= ROCKIVA_PATH_LENGTH) {
        set_error(detector, "RockIVA model directory is too long");
        return false;
    }
    return true;
}

binbot_face_detector_t *binbot_face_detector_create(
    const binbot_face_detector_config_t *config)
{
    binbot_face_detector_t *detector = calloc(1, sizeof(*detector));
    if(!detector) return NULL;
    if(pthread_mutex_init(&detector->mutex, NULL) != 0) {
        set_error(detector, "cannot initialize face detector mutex");
        return detector;
    }
    detector->mutex_ready = true;

    if(!config || config->width == 0 || config->height == 0 ||
       (config->width & 1U) != 0 || (config->height & 1U) != 0) {
        set_error(detector, "face detector requires a non-zero, even NV12 size");
        return detector;
    }
    detector->width = config->width;
    detector->height = config->height;
    detector->minimum_interval_ms = config->minimum_interval_ms;
    detector->frame_size = (size_t)config->width * config->height * 3U / 2U;

    const char *library_path = config->library_path && config->library_path[0]
                                   ? config->library_path
                                   : "librockiva.so";
    const char *model_directory = config->model_directory && config->model_directory[0]
                                      ? config->model_directory
                                      : "/oem/models/rockiva";
    char rockiva_model_path[ROCKIVA_PATH_LENGTH];
    if(!prepare_model_path(detector, model_directory, rockiva_model_path) ||
       !load_api(detector, library_path)) {
        release_resources(detector);
        return detector;
    }

    RockIvaInitParam global;
    memset(&global, 0, sizeof(global));
    memcpy(global.modelPath, rockiva_model_path, strlen(rockiva_model_path) + 1);
    global.logLevel = ROCKIVA_LOG_ERROR;
    global.channelId = 0;
    global.coreMask = 0x04;
    global.imageInfo.width = config->width;
    global.imageInfo.height = config->height;
    global.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    global.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
    global.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    global.detModel = ROCKIVA_DET_MODEL_PFP;

    RockIvaRetCode code = detector->api.init(&detector->handle, ROCKIVA_MODE_VIDEO,
                                              &global, detector);
    if(code != ROCKIVA_RET_SUCCESS) {
        set_error(detector, "ROCKIVA_Init failed: %d%s", code,
                  code == ROCKIVA_RET_LICENSE_ERROR ? " (license)" : "");
        release_resources(detector);
        return detector;
    }
    detector->global_initialized = true;

    for(size_t i = 0; i < FACE_INFERENCE_SLOT_COUNT; ++i) {
        inference_slot_t *slot = &detector->slots[i];
        memset(&slot->image, 0, sizeof(slot->image));
        slot->image.info.width = config->width;
        slot->image.info.height = config->height;
        slot->image.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        slot->image.info.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
        slot->image.dataFd = -1;
        RockIvaRetCode alloc_code = detector->api.image_alloc_mem(
            &slot->image, ROCKIVA_MEM_TYPE_DMA);
        if(alloc_code != ROCKIVA_RET_SUCCESS || !slot->image.dataAddr ||
           slot->image.dataFd < 0) {
            set_error(detector, "ROCKIVA DMA frame allocation failed: %d", alloc_code);
            release_resources(detector);
            return detector;
        }
        slot->memory_allocated = true;
        slot->image.size = (uint32_t)detector->frame_size;
        slot->image.channelId = 0;
        slot->image.extData = slot;
    }

    RockIvaDetTaskParams task;
    memset(&task, 0, sizeof(task));
    task.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_FACE);
    task.scores[ROCKIVA_OBJECT_TYPE_FACE] = config->minimum_score;
    task.min_det_count = config->stable_frame_count;
    code = detector->api.detect_init(detector->handle, &task, detect_callback);
    if(code != ROCKIVA_RET_SUCCESS) {
        set_error(detector, "ROCKIVA_DETECT_Init failed: %d", code);
        release_resources(detector);
        return detector;
    }
    detector->detector_initialized = true;

    code = detector->api.set_release_callback(detector->handle, release_callback);
    if(code != ROCKIVA_RET_SUCCESS) {
        set_error(detector, "ROCKIVA_SetFrameReleaseCallback failed: %d", code);
        release_resources(detector);
        return detector;
    }

    detector->available = true;
    detector->error[0] = '\0';
    fprintf(stdout, "vision: RockIVA face detector ready (%ux%u, model %s)\n",
            detector->width, detector->height, rockiva_model_path);
    return detector;
}

void binbot_face_detector_destroy(binbot_face_detector_t *detector)
{
    if(!detector) return;
    if(detector->mutex_ready) {
        pthread_mutex_lock(&detector->mutex);
        detector->shutting_down = true;
        detector->available = false;
        pthread_mutex_unlock(&detector->mutex);
    }
    if(detector->global_initialized && detector->api.wait_finish) {
        RockIvaRetCode code = detector->api.wait_finish(detector->handle, -1, 2000);
        if(code != ROCKIVA_RET_SUCCESS) {
            fprintf(stderr, "vision: ROCKIVA_WaitFinish during shutdown returned %d\n", code);
        }
    }
    release_resources(detector);
    if(detector->mutex_ready) pthread_mutex_destroy(&detector->mutex);
    free(detector);
}

bool binbot_face_detector_is_available(const binbot_face_detector_t *detector)
{
    return detector && detector->available;
}

const char *binbot_face_detector_error(const binbot_face_detector_t *detector)
{
    if(!detector) return "face detector allocation failed";
    return detector->error[0] ? detector->error : "";
}

binbot_face_submit_status_t binbot_face_detector_submit_nv12(
    binbot_face_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride)
{
    if(!detector || !detector->available) return BINBOT_FACE_SUBMIT_UNAVAILABLE;
    if(!y_plane || !uv_plane || y_stride < detector->width || uv_stride < detector->width) {
        return BINBOT_FACE_SUBMIT_ERROR;
    }

    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&detector->mutex);
    if(detector->shutting_down || !detector->available) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_FACE_SUBMIT_UNAVAILABLE;
    }
    if(detector->last_submit_ms != 0 && now >= detector->last_submit_ms &&
       now - detector->last_submit_ms < detector->minimum_interval_ms) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_FACE_SUBMIT_THROTTLED;
    }

    inference_slot_t *selected = NULL;
    for(size_t i = 0; i < FACE_INFERENCE_SLOT_COUNT; ++i) {
        if(!detector->slots[i].busy) {
            selected = &detector->slots[i];
            selected->busy = true;
            break;
        }
    }
    if(!selected) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_FACE_SUBMIT_QUEUE_FULL;
    }
    uint32_t frame_id = ++detector->next_frame_id;
    detector->last_submit_ms = now;
    pthread_mutex_unlock(&detector->mutex);

    uint8_t *destination = selected->image.dataAddr;
    for(uint32_t row = 0; row < detector->height; ++row) {
        memcpy(destination + (size_t)row * detector->width,
               y_plane + (size_t)row * y_stride, detector->width);
    }
    destination += (size_t)detector->width * detector->height;
    for(uint32_t row = 0; row < detector->height / 2U; ++row) {
        memcpy(destination + (size_t)row * detector->width,
               uv_plane + (size_t)row * uv_stride, detector->width);
    }

    selected->image.frameId = frame_id;

    RockIvaRetCode code = detector->api.push_frame(detector->handle,
                                                   &selected->image, NULL);
    if(code != ROCKIVA_RET_SUCCESS) {
        pthread_mutex_lock(&detector->mutex);
        selected->busy = false;
        pthread_mutex_unlock(&detector->mutex);
        if(code == ROCKIVA_RET_BUFFER_FULL) return BINBOT_FACE_SUBMIT_QUEUE_FULL;
        set_error(detector, "ROCKIVA_PushFrame failed: %d", code);
        return BINBOT_FACE_SUBMIT_ERROR;
    }
    return BINBOT_FACE_SUBMIT_ACCEPTED;
}

bool binbot_face_detector_latest(binbot_face_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_face_result_t *result)
{
    if(!detector || !result || !detector->mutex_ready) return false;
    pthread_mutex_lock(&detector->mutex);
    bool updated = detector->latest.sequence > after_sequence;
    if(updated) *result = detector->latest;
    pthread_mutex_unlock(&detector->mutex);
    return updated;
}
