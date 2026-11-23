#include "binbot/vision/cup_detector.h"

#include "cup_postprocess.h"
#include "rknn_api.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    CUP_MODEL_SIZE = 640,
    CUP_ERROR_CAPACITY = 192,
};

typedef struct {
    rknn_context context;
    bool initialized;
    void *model_data;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attrs[BINBOT_CUP_YOLO_OUTPUT_COUNT];
    rknn_tensor_attr native_output_attrs[BINBOT_CUP_YOLO_OUTPUT_COUNT];
    rknn_tensor_mem *input_memory;
    rknn_tensor_mem *output_memories[BINBOT_CUP_YOLO_OUTPUT_COUNT];
} cup_rknn_model_t;

typedef struct {
    uint32_t resized_width;
    uint32_t resized_height;
    uint32_t pad_left;
    uint32_t pad_top;
} cup_letterbox_t;

struct binbot_cup_detector {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    bool mutex_ready;
    bool condition_ready;
    bool worker_started;
    bool available;
    bool shutting_down;
    bool pending;
    uint16_t width;
    uint16_t height;
    float minimum_score;
    float nms_threshold;
    uint32_t minimum_interval_ms;
    uint32_t next_frame_id;
    uint32_t pending_frame_id;
    uint64_t last_submit_ms;
    uint64_t result_sequence;
    size_t frame_size;
    uint8_t *pending_frame;
    uint8_t *working_frame;
    uint8_t *model_rgb;
    cup_rknn_model_t model;
    binbot_cup_result_t latest;
    char error[CUP_ERROR_CAPACITY];
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void set_error(binbot_cup_detector_t *detector, const char *format, ...)
{
    if(!detector) return;
    va_list args;
    va_start(args, format);
    vsnprintf(detector->error, sizeof(detector->error), format, args);
    va_end(args);
}

static void *read_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if(!file) return NULL;
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if(length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    void *data = malloc((size_t)length);
    if(!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)length;
    return data;
}

static void release_model(cup_rknn_model_t *model)
{
    if(!model) return;
    if(model->initialized) {
        if(model->input_memory) {
            rknn_destroy_mem(model->context, model->input_memory);
        }
        for(uint32_t index = 0;
            index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
            if(model->output_memories[index]) {
                rknn_destroy_mem(model->context,
                                 model->output_memories[index]);
            }
        }
        rknn_destroy(model->context);
    }
    free(model->model_data);
    memset(model, 0, sizeof(*model));
}

static bool initialize_model(binbot_cup_detector_t *detector,
                             const char *path)
{
    cup_rknn_model_t *model = &detector->model;
    size_t model_size = 0;
    model->model_data = read_file(path, &model_size);
    if(!model->model_data) {
        set_error(detector, "cannot read RKNN cup model %s: %s", path,
                  strerror(errno));
        return false;
    }
    int code = rknn_init(&model->context, model->model_data,
                         (uint32_t)model_size, 0, NULL);
    if(code != RKNN_SUCC) {
        set_error(detector, "rknn_init(%s) failed: %d", path, code);
        return false;
    }
    model->initialized = true;

    rknn_input_output_num count;
    memset(&count, 0, sizeof(count));
    code = rknn_query(model->context, RKNN_QUERY_IN_OUT_NUM,
                      &count, sizeof(count));
    if(code != RKNN_SUCC || count.n_input != 1 ||
       count.n_output != BINBOT_CUP_YOLO_OUTPUT_COUNT) {
        set_error(detector, "unexpected RKNN cup I/O count: %u/%u",
                  count.n_input, count.n_output);
        return false;
    }

    memset(&model->input_attr, 0, sizeof(model->input_attr));
    model->input_attr.index = 0;
    code = rknn_query(model->context, RKNN_QUERY_NATIVE_INPUT_ATTR,
                      &model->input_attr, sizeof(model->input_attr));
    if(code != RKNN_SUCC || model->input_attr.n_dims != 4 ||
       model->input_attr.fmt != RKNN_TENSOR_NHWC ||
       model->input_attr.dims[0] != 1 ||
       model->input_attr.dims[1] != CUP_MODEL_SIZE ||
       model->input_attr.dims[2] != CUP_MODEL_SIZE ||
       model->input_attr.dims[3] != 3) {
        set_error(detector, "unsupported RKNN cup input tensor");
        return false;
    }
    model->input_attr.type = RKNN_TENSOR_UINT8;
    model->input_attr.fmt = RKNN_TENSOR_NHWC;
    model->input_attr.pass_through = 0;

    for(uint32_t index = 0;
        index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
        rknn_tensor_attr *attr = &model->output_attrs[index];
        rknn_tensor_attr *native_attr = &model->native_output_attrs[index];
        memset(attr, 0, sizeof(*attr));
        memset(native_attr, 0, sizeof(*native_attr));
        attr->index = index;
        native_attr->index = index;
        code = rknn_query(model->context, RKNN_QUERY_OUTPUT_ATTR,
                          attr, sizeof(*attr));
        if(code == RKNN_SUCC) {
            code = rknn_query(model->context, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                              native_attr, sizeof(*native_attr));
        }
        uint32_t expected_grid = 80U >> index;
        if(code != RKNN_SUCC || attr->n_dims != 4 ||
           attr->fmt != RKNN_TENSOR_NCHW ||
           attr->dims[0] != 1 ||
           attr->dims[1] != BINBOT_CUP_YOLO_CHANNELS ||
           attr->dims[2] != expected_grid ||
           attr->dims[3] != expected_grid ||
           attr->type != RKNN_TENSOR_INT8 ||
           attr->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
           attr->scale <= 0.0f) {
            set_error(detector, "unsupported RKNN cup output tensor %u", index);
            return false;
        }
        if(native_attr->n_dims != 5 ||
           native_attr->fmt != RKNN_TENSOR_NC1HWC2 ||
           native_attr->dims[0] != 1 ||
           native_attr->dims[1] * native_attr->dims[4] <
               BINBOT_CUP_YOLO_CHANNELS ||
           native_attr->dims[2] != expected_grid ||
           native_attr->dims[3] != expected_grid ||
           native_attr->type != RKNN_TENSOR_INT8 ||
           native_attr->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
           native_attr->scale <= 0.0f) {
            set_error(detector, "unsupported native RKNN cup output %u", index);
            return false;
        }
    }

    uint32_t input_bytes = model->input_attr.size_with_stride
                               ? model->input_attr.size_with_stride
                               : model->input_attr.size;
    model->input_memory = rknn_create_mem(model->context, input_bytes);
    if(!model->input_memory ||
       rknn_set_io_mem(model->context, model->input_memory,
                       &model->input_attr) != RKNN_SUCC) {
        set_error(detector, "cannot bind RKNN cup input");
        return false;
    }
    for(uint32_t index = 0;
        index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
        rknn_tensor_attr *attr = &model->native_output_attrs[index];
        uint32_t output_bytes = attr->size_with_stride
                                    ? attr->size_with_stride : attr->size;
        model->output_memories[index] =
            rknn_create_mem(model->context, output_bytes);
        if(!model->output_memories[index] ||
           rknn_set_io_mem(model->context, model->output_memories[index],
                           attr) != RKNN_SUCC) {
            set_error(detector, "cannot bind RKNN cup output %u", index);
            return false;
        }
    }
    return true;
}

static uint8_t clamp_u8(int value)
{
    if(value < 0) return 0;
    if(value > 255) return 255;
    return (uint8_t)value;
}

static void nv12_pixel(const uint8_t *frame,
                       uint32_t width,
                       uint32_t height,
                       int x,
                       int y,
                       uint8_t rgb[3])
{
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x >= (int)width) x = (int)width - 1;
    if(y >= (int)height) y = (int)height - 1;
    int luminance = frame[(size_t)y * width + (size_t)x];
    size_t uv_index = (size_t)width * height +
                      (size_t)(y / 2) * width + (size_t)(x & ~1);
    int u = (int)frame[uv_index] - 128;
    int v = (int)frame[uv_index + 1U] - 128;
    rgb[0] = clamp_u8(luminance + ((359 * v) >> 8));
    rgb[1] = clamp_u8(luminance - ((88 * u + 183 * v) >> 8));
    rgb[2] = clamp_u8(luminance + ((454 * u) >> 8));
}

static void sample_nv12(const uint8_t *frame,
                        uint32_t width,
                        uint32_t height,
                        float x,
                        float y,
                        uint8_t rgb[3])
{
    if(x < 0.0f) x = 0.0f;
    if(y < 0.0f) y = 0.0f;
    if(x > (float)(width - 1U)) x = (float)(width - 1U);
    if(y > (float)(height - 1U)) y = (float)(height - 1U);
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1 < (int)width ? x0 + 1 : x0;
    int y1 = y0 + 1 < (int)height ? y0 + 1 : y0;
    float x_fraction = x - (float)x0;
    float y_fraction = y - (float)y0;
    uint8_t samples[4][3];
    nv12_pixel(frame, width, height, x0, y0, samples[0]);
    nv12_pixel(frame, width, height, x1, y0, samples[1]);
    nv12_pixel(frame, width, height, x0, y1, samples[2]);
    nv12_pixel(frame, width, height, x1, y1, samples[3]);
    for(size_t channel = 0; channel < 3; ++channel) {
        float top = (1.0f - x_fraction) * samples[0][channel] +
                    x_fraction * samples[1][channel];
        float bottom = (1.0f - x_fraction) * samples[2][channel] +
                       x_fraction * samples[3][channel];
        rgb[channel] = clamp_u8((int)lroundf(
            (1.0f - y_fraction) * top + y_fraction * bottom));
    }
}

static void prepare_model_input(const uint8_t *frame,
                                uint32_t width,
                                uint32_t height,
                                uint8_t *rgb,
                                cup_letterbox_t *letterbox)
{
    float scale = fminf((float)CUP_MODEL_SIZE / width,
                        (float)CUP_MODEL_SIZE / height);
    uint32_t resized_width = (uint32_t)lroundf(width * scale);
    uint32_t resized_height = (uint32_t)lroundf(height * scale);
    if(resized_width < 1U) resized_width = 1U;
    if(resized_height < 1U) resized_height = 1U;
    if(resized_width > CUP_MODEL_SIZE) resized_width = CUP_MODEL_SIZE;
    if(resized_height > CUP_MODEL_SIZE) resized_height = CUP_MODEL_SIZE;
    uint32_t left = (CUP_MODEL_SIZE - resized_width) / 2U;
    uint32_t top = (CUP_MODEL_SIZE - resized_height) / 2U;
    memset(rgb, 114, (size_t)CUP_MODEL_SIZE * CUP_MODEL_SIZE * 3U);

    bool same_size = resized_width == width && resized_height == height;
    for(uint32_t destination_y = 0; destination_y < resized_height;
        ++destination_y) {
        float source_y = ((float)destination_y + 0.5f) * height /
                         resized_height - 0.5f;
        for(uint32_t destination_x = 0; destination_x < resized_width;
            ++destination_x) {
            uint8_t *pixel = rgb +
                ((size_t)(top + destination_y) * CUP_MODEL_SIZE +
                 left + destination_x) * 3U;
            if(same_size) {
                nv12_pixel(frame, width, height, (int)destination_x,
                           (int)destination_y, pixel);
            }
            else {
                float source_x = ((float)destination_x + 0.5f) * width /
                                 resized_width - 0.5f;
                sample_nv12(frame, width, height, source_x, source_y, pixel);
            }
        }
    }
    letterbox->resized_width = resized_width;
    letterbox->resized_height = resized_height;
    letterbox->pad_left = left;
    letterbox->pad_top = top;
}

static bool copy_model_input(cup_rknn_model_t *model, const uint8_t *rgb)
{
    uint32_t width_stride = model->input_attr.w_stride
                                ? model->input_attr.w_stride : CUP_MODEL_SIZE;
    size_t source_row = (size_t)CUP_MODEL_SIZE * 3U;
    size_t destination_row = (size_t)width_stride * 3U;
    if(width_stride < CUP_MODEL_SIZE ||
       (size_t)CUP_MODEL_SIZE * destination_row >
           model->input_memory->size) {
        return false;
    }
    memset(model->input_memory->virt_addr, 0, model->input_memory->size);
    uint8_t *destination = model->input_memory->virt_addr;
    for(uint32_t row = 0; row < CUP_MODEL_SIZE; ++row) {
        memcpy(destination + (size_t)row * destination_row,
               rgb + (size_t)row * source_row, source_row);
    }
    return true;
}

static bool process_frame(binbot_cup_detector_t *detector,
                          const uint8_t *frame,
                          uint32_t frame_id,
                          binbot_cup_result_t *result)
{
    cup_letterbox_t letterbox;
    memset(result, 0, sizeof(*result));
    result->frame_id = frame_id;
    prepare_model_input(frame, detector->width, detector->height,
                        detector->model_rgb, &letterbox);
    if(!copy_model_input(&detector->model, detector->model_rgb)) {
        set_error(detector, "cannot copy RKNN cup input");
        return false;
    }
    int code = rknn_run(detector->model.context, NULL);
    if(code != RKNN_SUCC) {
        set_error(detector, "RKNN cup inference failed: %d", code);
        return false;
    }

    binbot_cup_yolo_tensor_t outputs[BINBOT_CUP_YOLO_OUTPUT_COUNT];
    memset(outputs, 0, sizeof(outputs));
    for(uint32_t index = 0;
        index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
        const rknn_tensor_attr *attr =
            &detector->model.native_output_attrs[index];
        uint32_t grid_height = attr->dims[2];
        uint32_t grid_width = attr->dims[3];
        uint32_t channel_block = attr->dims[4];
        uint32_t row_stride = attr->w_stride ? attr->w_stride : grid_width;
        uint32_t channel_stride = row_stride * grid_height * channel_block;
        size_t required = (size_t)attr->dims[1] * channel_stride;
        if(required > detector->model.output_memories[index]->size) {
            set_error(detector, "invalid RKNN cup output stride %u", index);
            return false;
        }
        outputs[index].data =
            detector->model.output_memories[index]->virt_addr;
        outputs[index].grid_width = grid_width;
        outputs[index].grid_height = grid_height;
        outputs[index].row_stride = row_stride;
        outputs[index].channel_stride = channel_stride;
        outputs[index].channel_block = channel_block;
        outputs[index].zero_point = attr->zp;
        outputs[index].scale = attr->scale;
    }
    if(!binbot_cup_postprocess_yolov5(
           outputs, CUP_MODEL_SIZE, CUP_MODEL_SIZE,
           detector->width, detector->height,
           letterbox.resized_width, letterbox.resized_height,
           letterbox.pad_left, letterbox.pad_top,
           detector->minimum_score, detector->nms_threshold, result)) {
        set_error(detector, "invalid RKNN cup post-processing input");
        return false;
    }
    return true;
}

static void *worker_main(void *argument)
{
    binbot_cup_detector_t *detector = argument;
    for(;;) {
        pthread_mutex_lock(&detector->mutex);
        while(!detector->pending && !detector->shutting_down) {
            pthread_cond_wait(&detector->condition, &detector->mutex);
        }
        if(detector->shutting_down) {
            pthread_mutex_unlock(&detector->mutex);
            break;
        }
        uint8_t *swap = detector->working_frame;
        detector->working_frame = detector->pending_frame;
        detector->pending_frame = swap;
        uint32_t frame_id = detector->pending_frame_id;
        detector->pending = false;
        pthread_mutex_unlock(&detector->mutex);

        binbot_cup_result_t next;
        bool ok = process_frame(detector, detector->working_frame,
                                frame_id, &next);
        next.monotonic_ms = monotonic_ms();
        pthread_mutex_lock(&detector->mutex);
        if(!ok) detector->available = false;
        if(ok && !detector->shutting_down) {
            next.sequence = ++detector->result_sequence;
            detector->latest = next;
        }
        pthread_mutex_unlock(&detector->mutex);
    }
    return NULL;
}

static void release_detector(binbot_cup_detector_t *detector)
{
    if(!detector) return;
    if(detector->worker_started) {
        pthread_mutex_lock(&detector->mutex);
        detector->shutting_down = true;
        pthread_cond_signal(&detector->condition);
        pthread_mutex_unlock(&detector->mutex);
        pthread_join(detector->worker, NULL);
        detector->worker_started = false;
    }
    release_model(&detector->model);
    free(detector->model_rgb);
    free(detector->working_frame);
    free(detector->pending_frame);
    detector->model_rgb = NULL;
    detector->working_frame = NULL;
    detector->pending_frame = NULL;
    if(detector->condition_ready) pthread_cond_destroy(&detector->condition);
    if(detector->mutex_ready) pthread_mutex_destroy(&detector->mutex);
    detector->condition_ready = false;
    detector->mutex_ready = false;
}

binbot_cup_detector_t *binbot_cup_detector_create(
    const binbot_cup_detector_config_t *config)
{
    binbot_cup_detector_t *detector = calloc(1, sizeof(*detector));
    if(!detector) return NULL;
    if(pthread_mutex_init(&detector->mutex, NULL) != 0) {
        set_error(detector, "cannot initialize cup detector mutex");
        return detector;
    }
    detector->mutex_ready = true;
    if(pthread_cond_init(&detector->condition, NULL) != 0) {
        set_error(detector, "cannot initialize cup detector condition");
        return detector;
    }
    detector->condition_ready = true;
    if(!config || config->width == 0 || config->height == 0 ||
       (config->width & 1U) != 0 || (config->height & 1U) != 0) {
        set_error(detector, "cup detector requires a non-zero even NV12 size");
        return detector;
    }
    detector->width = config->width;
    detector->height = config->height;
    detector->minimum_score = config->minimum_score > 0.0f &&
                              config->minimum_score <= 1.0f
                                  ? config->minimum_score : 0.25f;
    detector->nms_threshold = config->nms_threshold >= 0.0f &&
                              config->nms_threshold <= 1.0f
                                  ? config->nms_threshold : 0.45f;
    detector->minimum_interval_ms = config->minimum_interval_ms
                                        ? config->minimum_interval_ms : 500U;
    detector->frame_size = (size_t)detector->width * detector->height * 3U / 2U;
    detector->pending_frame = malloc(detector->frame_size);
    detector->working_frame = malloc(detector->frame_size);
    detector->model_rgb = malloc((size_t)CUP_MODEL_SIZE * CUP_MODEL_SIZE * 3U);
    if(!detector->pending_frame || !detector->working_frame ||
       !detector->model_rgb) {
        set_error(detector, "cannot allocate cup detector frame buffers");
        return detector;
    }
    const char *model_path = config->model_path;
    if(!model_path || !model_path[0]) {
        model_path =
            "/oem/assets/vision/cup/yolov5n_coco_int8_rv1106.rknn";
    }
    if(!initialize_model(detector, model_path)) return detector;
    if(pthread_create(&detector->worker, NULL, worker_main, detector) != 0) {
        set_error(detector, "cannot start cup detector worker");
        return detector;
    }
    detector->worker_started = true;
    detector->available = true;
    fprintf(stdout,
            "vision: RKNN cup detector ready (%ux%u, interval %ums, model %s)\n",
            detector->width, detector->height,
            detector->minimum_interval_ms, model_path);
    return detector;
}

void binbot_cup_detector_destroy(binbot_cup_detector_t *detector)
{
    if(!detector) return;
    release_detector(detector);
    free(detector);
}

bool binbot_cup_detector_is_available(const binbot_cup_detector_t *detector)
{
    return detector && detector->available;
}

const char *binbot_cup_detector_error(const binbot_cup_detector_t *detector)
{
    if(!detector) return "cup detector allocation failed";
    return detector->error[0] ? detector->error : "cup detector unavailable";
}

binbot_cup_submit_status_t binbot_cup_detector_submit_nv12(
    binbot_cup_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride)
{
    if(!detector || !detector->mutex_ready) {
        return BINBOT_CUP_SUBMIT_UNAVAILABLE;
    }
    if(!y_plane || !uv_plane || y_stride < detector->width ||
       uv_stride < detector->width) {
        set_error(detector, "invalid NV12 cup frame");
        return BINBOT_CUP_SUBMIT_ERROR;
    }
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&detector->mutex);
    if(!detector->available || detector->shutting_down) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_CUP_SUBMIT_UNAVAILABLE;
    }
    if(detector->last_submit_ms > 0 && now >= detector->last_submit_ms &&
       now - detector->last_submit_ms < detector->minimum_interval_ms) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_CUP_SUBMIT_THROTTLED;
    }
    if(detector->pending) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_CUP_SUBMIT_QUEUE_FULL;
    }
    for(uint32_t row = 0; row < detector->height; ++row) {
        memcpy(detector->pending_frame + (size_t)row * detector->width,
               y_plane + (size_t)row * y_stride, detector->width);
    }
    uint8_t *pending_uv = detector->pending_frame +
                          (size_t)detector->width * detector->height;
    for(uint32_t row = 0; row < detector->height / 2U; ++row) {
        memcpy(pending_uv + (size_t)row * detector->width,
               uv_plane + (size_t)row * uv_stride, detector->width);
    }
    detector->pending_frame_id = ++detector->next_frame_id;
    detector->pending = true;
    detector->last_submit_ms = now;
    pthread_cond_signal(&detector->condition);
    pthread_mutex_unlock(&detector->mutex);
    return BINBOT_CUP_SUBMIT_ACCEPTED;
}

bool binbot_cup_detector_latest(binbot_cup_detector_t *detector,
                                uint64_t after_sequence,
                                binbot_cup_result_t *result)
{
    if(!detector || !result || !detector->mutex_ready) return false;
    pthread_mutex_lock(&detector->mutex);
    bool available = detector->latest.sequence > after_sequence;
    if(available) *result = detector->latest;
    pthread_mutex_unlock(&detector->mutex);
    return available;
}
