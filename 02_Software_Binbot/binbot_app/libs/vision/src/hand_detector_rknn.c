#include "binbot/vision/hand_detector.h"

#include "rknn_api.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    PALM_INPUT_SIZE = 192,
    LANDMARK_INPUT_SIZE = 224,
    PALM_ANCHOR_COUNT = 2016,
    PALM_BOX_VALUE_COUNT = 18,
    PALM_KEYPOINT_COUNT = 7,
    PALM_CANDIDATE_LIMIT = 64,
    RKNN_OUTPUT_LIMIT = 4,
    HAND_ERROR_CAPACITY = 192,
};

typedef struct {
    float left;
    float top;
    float right;
    float bottom;
    float keypoints[PALM_KEYPOINT_COUNT][2];
    float score;
} palm_detection_t;

typedef struct {
    float center_x;
    float center_y;
    float width;
    float height;
    float rotation;
} rotated_roi_t;

typedef struct {
    rknn_context context;
    bool initialized;
    void *model_data;
    rknn_tensor_attr input_attr;
    uint32_t output_count;
    rknn_tensor_attr output_attrs[RKNN_OUTPUT_LIMIT];
    rknn_tensor_attr native_output_attrs[RKNN_OUTPUT_LIMIT];
    rknn_tensor_mem *input_memory;
    rknn_tensor_mem *output_memories[RKNN_OUTPUT_LIMIT];
} hand_rknn_model_t;

struct binbot_hand_detector {
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
    float minimum_palm_score;
    float minimum_presence_score;
    uint32_t minimum_interval_ms;
    uint32_t next_frame_id;
    uint32_t pending_frame_id;
    uint64_t last_submit_ms;
    uint64_t result_sequence;
    size_t frame_size;
    uint8_t *pending_frame;
    uint8_t *working_frame;
    uint8_t *palm_rgb;
    uint8_t *landmark_rgb;
    hand_rknn_model_t palm_model;
    hand_rknn_model_t landmark_model;
    binbot_hand_result_t latest;
    char error[HAND_ERROR_CAPACITY];
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void set_error(binbot_hand_detector_t *detector, const char *format, ...)
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

static size_t tensor_type_size(rknn_tensor_type type)
{
    switch(type) {
    case RKNN_TENSOR_FLOAT32:
    case RKNN_TENSOR_INT32:
    case RKNN_TENSOR_UINT32:
        return 4;
    case RKNN_TENSOR_FLOAT16:
    case RKNN_TENSOR_INT16:
    case RKNN_TENSOR_UINT16:
        return 2;
    case RKNN_TENSOR_INT8:
    case RKNN_TENSOR_UINT8:
    case RKNN_TENSOR_BOOL:
        return 1;
    case RKNN_TENSOR_INT64:
        return 8;
    default:
        return 0;
    }
}

static float half_to_float(uint16_t value)
{
    uint32_t sign = ((uint32_t)value & 0x8000U) << 16;
    uint32_t exponent = ((uint32_t)value >> 10) & 0x1fU;
    uint32_t mantissa = (uint32_t)value & 0x03ffU;
    uint32_t bits;
    if(exponent == 0) {
        if(mantissa == 0) {
            bits = sign;
        }
        else {
            int shift = 0;
            while((mantissa & 0x0400U) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x03ffU;
            bits = sign | ((uint32_t)(127 - 14 - shift) << 23) |
                   (mantissa << 13);
        }
    }
    else if(exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (mantissa << 13);
    }
    else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static float tensor_value(const void *data,
                          size_t index,
                          const rknn_tensor_attr *attr)
{
    double value;
    switch(attr->type) {
    case RKNN_TENSOR_FLOAT32:
        return ((const float *)data)[index];
    case RKNN_TENSOR_FLOAT16:
        return half_to_float(((const uint16_t *)data)[index]);
    case RKNN_TENSOR_INT8:
        value = ((const int8_t *)data)[index];
        break;
    case RKNN_TENSOR_UINT8:
    case RKNN_TENSOR_BOOL:
        value = ((const uint8_t *)data)[index];
        break;
    case RKNN_TENSOR_INT16:
        value = ((const int16_t *)data)[index];
        break;
    case RKNN_TENSOR_UINT16:
        value = ((const uint16_t *)data)[index];
        break;
    case RKNN_TENSOR_INT32:
        value = ((const int32_t *)data)[index];
        break;
    case RKNN_TENSOR_UINT32:
        value = ((const uint32_t *)data)[index];
        break;
    case RKNN_TENSOR_INT64:
        value = (double)((const int64_t *)data)[index];
        break;
    default:
        return NAN;
    }
    if(attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
        value = (value - attr->zp) * attr->scale;
    }
    else if(attr->qnt_type == RKNN_TENSOR_QNT_DFP) {
        value = ldexp(value, -attr->fl);
    }
    return (float)value;
}

static void release_model(hand_rknn_model_t *model)
{
    if(!model) return;
    if(model->initialized) {
        if(model->input_memory) {
            rknn_destroy_mem(model->context, model->input_memory);
        }
        for(uint32_t i = 0; i < model->output_count; ++i) {
            if(model->output_memories[i]) {
                rknn_destroy_mem(model->context, model->output_memories[i]);
            }
        }
        rknn_destroy(model->context);
    }
    free(model->model_data);
    memset(model, 0, sizeof(*model));
}

static bool initialize_model(binbot_hand_detector_t *detector,
                             hand_rknn_model_t *model,
                             const char *path,
                             uint32_t input_size,
                             uint32_t expected_outputs,
                             const uint32_t *expected_elements)
{
    size_t model_size = 0;
    model->model_data = read_file(path, &model_size);
    if(!model->model_data) {
        set_error(detector, "cannot read RKNN model %s: %s", path, strerror(errno));
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
       count.n_output != expected_outputs || count.n_output > RKNN_OUTPUT_LIMIT) {
        set_error(detector, "unexpected RKNN I/O count for %s: %u/%u", path,
                  count.n_input, count.n_output);
        return false;
    }
    model->output_count = count.n_output;

    memset(&model->input_attr, 0, sizeof(model->input_attr));
    model->input_attr.index = 0;
    code = rknn_query(model->context, RKNN_QUERY_INPUT_ATTR,
                      &model->input_attr, sizeof(model->input_attr));
    if(code != RKNN_SUCC || model->input_attr.n_dims != 4 ||
       model->input_attr.fmt != RKNN_TENSOR_NHWC ||
       model->input_attr.dims[0] != 1 ||
       model->input_attr.dims[1] != input_size ||
       model->input_attr.dims[2] != input_size ||
       model->input_attr.dims[3] != 3 ||
       model->input_attr.type != RKNN_TENSOR_INT8 ||
       model->input_attr.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
       model->input_attr.scale <= 0.0f) {
        set_error(detector, "unsupported RKNN input tensor for %s", path);
        return false;
    }

    for(uint32_t i = 0; i < model->output_count; ++i) {
        memset(&model->output_attrs[i], 0, sizeof(model->output_attrs[i]));
        memset(&model->native_output_attrs[i], 0,
               sizeof(model->native_output_attrs[i]));
        model->output_attrs[i].index = i;
        model->native_output_attrs[i].index = i;
        code = rknn_query(model->context, RKNN_QUERY_OUTPUT_ATTR,
                          &model->output_attrs[i],
                          sizeof(model->output_attrs[i]));
        if(code == RKNN_SUCC) {
            code = rknn_query(model->context, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                              &model->native_output_attrs[i],
                              sizeof(model->native_output_attrs[i]));
        }
        if(code != RKNN_SUCC ||
           model->native_output_attrs[i].n_elems != expected_elements[i] ||
           tensor_type_size(model->native_output_attrs[i].type) == 0) {
            set_error(detector, "unexpected RKNN output %u for %s", i, path);
            return false;
        }
    }

    model->input_attr.pass_through = 1;
    uint32_t input_bytes = model->input_attr.size_with_stride
                               ? model->input_attr.size_with_stride
                               : model->input_attr.size;
    model->input_memory = rknn_create_mem(model->context, input_bytes);
    if(!model->input_memory ||
       rknn_set_io_mem(model->context, model->input_memory,
                       &model->input_attr) != RKNN_SUCC) {
        set_error(detector, "cannot bind RKNN input for %s", path);
        return false;
    }
    for(uint32_t i = 0; i < model->output_count; ++i) {
        uint32_t output_bytes = model->native_output_attrs[i].size_with_stride
                                    ? model->native_output_attrs[i].size_with_stride
                                    : model->native_output_attrs[i].size;
        model->output_memories[i] = rknn_create_mem(model->context, output_bytes);
        if(!model->output_memories[i] ||
           rknn_set_io_mem(model->context, model->output_memories[i],
                           &model->native_output_attrs[i]) != RKNN_SUCC) {
            set_error(detector, "cannot bind RKNN output %u for %s", i, path);
            return false;
        }
    }
    return true;
}

static bool run_model(hand_rknn_model_t *model, const uint8_t *rgb)
{
    const uint32_t height = model->input_attr.dims[1];
    const uint32_t width = model->input_attr.dims[2];
    const uint32_t channels = model->input_attr.dims[3];
    const uint32_t width_stride = model->input_attr.w_stride
                                      ? model->input_attr.w_stride : width;
    const size_t destination_row = (size_t)width_stride * channels;
    const size_t source_row = (size_t)width * channels;
    if(width_stride < width ||
       (size_t)height * destination_row > model->input_memory->size) {
        return false;
    }

    int8_t table[256];
    for(size_t i = 0; i < 256; ++i) {
        float normalized = (float)i / 255.0f;
        long quantized = lroundf(normalized / model->input_attr.scale) +
                         model->input_attr.zp;
        if(quantized < INT8_MIN) quantized = INT8_MIN;
        if(quantized > INT8_MAX) quantized = INT8_MAX;
        table[i] = (int8_t)quantized;
    }

    memset(model->input_memory->virt_addr, 0, model->input_memory->size);
    int8_t *destination = model->input_memory->virt_addr;
    for(uint32_t row = 0; row < height; ++row) {
        for(size_t column = 0; column < source_row; ++column) {
            destination[(size_t)row * destination_row + column] =
                table[rgb[(size_t)row * source_row + column]];
        }
    }
    return rknn_run(model->context, NULL) == RKNN_SUCC;
}

static float model_output(const hand_rknn_model_t *model,
                          uint32_t output,
                          size_t index)
{
    return tensor_value(model->output_memories[output]->virt_addr, index,
                        &model->native_output_attrs[output]);
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
    int v = (int)frame[uv_index + 1] - 128;
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
    if(x > (float)(width - 1)) x = (float)(width - 1);
    if(y > (float)(height - 1)) y = (float)(height - 1);
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

static void prepare_palm_input(const uint8_t *frame,
                               uint32_t width,
                               uint32_t height,
                               uint8_t *rgb,
                               float padding[4])
{
    float scale = fminf((float)PALM_INPUT_SIZE / (float)width,
                        (float)PALM_INPUT_SIZE / (float)height);
    uint32_t resized_width = (uint32_t)lroundf((float)width * scale);
    uint32_t resized_height = (uint32_t)lroundf((float)height * scale);
    if(resized_width < 1) resized_width = 1;
    if(resized_height < 1) resized_height = 1;
    uint32_t left = (PALM_INPUT_SIZE - resized_width) / 2U;
    uint32_t top = (PALM_INPUT_SIZE - resized_height) / 2U;
    memset(rgb, 0, PALM_INPUT_SIZE * PALM_INPUT_SIZE * 3U);
    for(uint32_t destination_y = 0; destination_y < resized_height;
        ++destination_y) {
        float source_y = ((float)destination_y + 0.5f) *
                         (float)height / (float)resized_height - 0.5f;
        for(uint32_t destination_x = 0; destination_x < resized_width;
            ++destination_x) {
            float source_x = ((float)destination_x + 0.5f) *
                             (float)width / (float)resized_width - 0.5f;
            uint8_t *pixel = rgb +
                ((size_t)(top + destination_y) * PALM_INPUT_SIZE +
                 left + destination_x) * 3U;
            sample_nv12(frame, width, height, source_x, source_y, pixel);
        }
    }
    padding[0] = (float)left / PALM_INPUT_SIZE;
    padding[1] = (float)top / PALM_INPUT_SIZE;
    padding[2] = (float)(PALM_INPUT_SIZE - left - resized_width) /
                 PALM_INPUT_SIZE;
    padding[3] = (float)(PALM_INPUT_SIZE - top - resized_height) /
                 PALM_INPUT_SIZE;
}

static float sigmoidf_stable(float value)
{
    if(value >= 0.0f) return 1.0f / (1.0f + expf(-value));
    float exponential = expf(value);
    return exponential / (1.0f + exponential);
}

static void palm_anchor(size_t index, float *x, float *y)
{
    const size_t stride8_count = 24U * 24U * 2U;
    size_t cell;
    size_t feature_size;
    if(index < stride8_count) {
        cell = index / 2U;
        feature_size = 24;
    }
    else {
        cell = (index - stride8_count) / 6U;
        feature_size = 12;
    }
    *x = ((float)(cell % feature_size) + 0.5f) / (float)feature_size;
    *y = ((float)(cell / feature_size) + 0.5f) / (float)feature_size;
}

static bool decode_palm_candidate(const hand_rknn_model_t *model,
                                  size_t index,
                                  float score,
                                  const float padding[4],
                                  palm_detection_t *detection)
{
    float anchor_x;
    float anchor_y;
    palm_anchor(index, &anchor_x, &anchor_y);
    size_t offset = index * PALM_BOX_VALUE_COUNT;
    float center_x = model_output(model, 0, offset) / PALM_INPUT_SIZE + anchor_x;
    float center_y = model_output(model, 0, offset + 1) / PALM_INPUT_SIZE + anchor_y;
    float width = model_output(model, 0, offset + 2) / PALM_INPUT_SIZE;
    float height = model_output(model, 0, offset + 3) / PALM_INPUT_SIZE;
    if(width <= 0.0f || height <= 0.0f) return false;

    float horizontal_scale = 1.0f - padding[0] - padding[2];
    float vertical_scale = 1.0f - padding[1] - padding[3];
    if(horizontal_scale <= 0.0f || vertical_scale <= 0.0f) return false;
    detection->left = (center_x - width * 0.5f - padding[0]) / horizontal_scale;
    detection->right = (center_x + width * 0.5f - padding[0]) / horizontal_scale;
    detection->top = (center_y - height * 0.5f - padding[1]) / vertical_scale;
    detection->bottom = (center_y + height * 0.5f - padding[1]) / vertical_scale;
    for(size_t point = 0; point < PALM_KEYPOINT_COUNT; ++point) {
        size_t point_offset = offset + 4U + point * 2U;
        float point_x = model_output(model, 0, point_offset) /
                        PALM_INPUT_SIZE + anchor_x;
        float point_y = model_output(model, 0, point_offset + 1U) /
                        PALM_INPUT_SIZE + anchor_y;
        detection->keypoints[point][0] =
            (point_x - padding[0]) / horizontal_scale;
        detection->keypoints[point][1] =
            (point_y - padding[1]) / vertical_scale;
    }
    detection->score = score;
    return detection->right > detection->left &&
           detection->bottom > detection->top;
}

static float palm_iou(const palm_detection_t *first,
                      const palm_detection_t *second)
{
    float left = fmaxf(first->left, second->left);
    float top = fmaxf(first->top, second->top);
    float right = fminf(first->right, second->right);
    float bottom = fminf(first->bottom, second->bottom);
    float intersection_width = fmaxf(right - left, 0.0f);
    float intersection_height = fmaxf(bottom - top, 0.0f);
    float intersection = intersection_width * intersection_height;
    float first_area = fmaxf(first->right - first->left, 0.0f) *
                       fmaxf(first->bottom - first->top, 0.0f);
    float second_area = fmaxf(second->right - second->left, 0.0f) *
                        fmaxf(second->bottom - second->top, 0.0f);
    float union_area = first_area + second_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static void insert_candidate(palm_detection_t candidates[],
                             size_t *count,
                             const palm_detection_t *candidate)
{
    size_t position = 0;
    while(position < *count && candidates[position].score >= candidate->score) {
        ++position;
    }
    if(position >= PALM_CANDIDATE_LIMIT) return;
    size_t new_count = *count < PALM_CANDIDATE_LIMIT ? *count + 1U : *count;
    for(size_t index = new_count - 1U; index > position; --index) {
        candidates[index] = candidates[index - 1U];
    }
    candidates[position] = *candidate;
    *count = new_count;
}

static bool decode_best_palm(const hand_rknn_model_t *model,
                             const float padding[4],
                             float minimum_score,
                             palm_detection_t *result)
{
    palm_detection_t candidates[PALM_CANDIDATE_LIMIT];
    size_t count = 0;
    float threshold_logit = logf(minimum_score / (1.0f - minimum_score));
    for(size_t index = 0; index < PALM_ANCHOR_COUNT; ++index) {
        float logit = model_output(model, 1, index);
        if(logit < threshold_logit) continue;
        palm_detection_t candidate;
        if(decode_palm_candidate(model, index, sigmoidf_stable(logit),
                                 padding, &candidate)) {
            insert_candidate(candidates, &count, &candidate);
        }
    }
    if(count == 0) return false;

    *result = candidates[0];
    float total_weight = 0.0f;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float keypoints[PALM_KEYPOINT_COUNT][2] = {{0}};
    for(size_t index = 0; index < count; ++index) {
        if(palm_iou(&candidates[0], &candidates[index]) <= 0.3f) continue;
        float weight = candidates[index].score;
        total_weight += weight;
        left += candidates[index].left * weight;
        top += candidates[index].top * weight;
        right += candidates[index].right * weight;
        bottom += candidates[index].bottom * weight;
        for(size_t point = 0; point < PALM_KEYPOINT_COUNT; ++point) {
            keypoints[point][0] += candidates[index].keypoints[point][0] * weight;
            keypoints[point][1] += candidates[index].keypoints[point][1] * weight;
        }
    }
    if(total_weight <= 0.0f) return false;
    result->left = left / total_weight;
    result->top = top / total_weight;
    result->right = right / total_weight;
    result->bottom = bottom / total_weight;
    for(size_t point = 0; point < PALM_KEYPOINT_COUNT; ++point) {
        result->keypoints[point][0] = keypoints[point][0] / total_weight;
        result->keypoints[point][1] = keypoints[point][1] / total_weight;
    }
    return true;
}

static float normalize_radians(float angle)
{
    const float two_pi = 2.0f * (float)M_PI;
    return angle - two_pi * floorf((angle + (float)M_PI) / two_pi);
}

static rotated_roi_t palm_to_roi(const palm_detection_t *detection,
                                 uint32_t image_width,
                                 uint32_t image_height)
{
    float box_width = detection->right - detection->left;
    float box_height = detection->bottom - detection->top;
    float center_x = (detection->left + detection->right) * 0.5f;
    float center_y = (detection->top + detection->bottom) * 0.5f;
    float wrist_x = detection->keypoints[0][0] * image_width;
    float wrist_y = detection->keypoints[0][1] * image_height;
    float middle_x = detection->keypoints[2][0] * image_width;
    float middle_y = detection->keypoints[2][1] * image_height;
    float rotation = normalize_radians(
        (float)M_PI * 0.5f - atan2f(-(middle_y - wrist_y),
                                    middle_x - wrist_x));
    center_x += (-box_height * -0.5f * sinf(rotation) * image_height /
                 image_width);
    center_y += box_height * -0.5f * cosf(rotation);
    float long_side = fmaxf(box_width * image_width,
                            box_height * image_height);
    rotated_roi_t roi = {
        .center_x = center_x,
        .center_y = center_y,
        .width = long_side / image_width * 2.6f,
        .height = long_side / image_height * 2.6f,
        .rotation = rotation,
    };
    return roi;
}

static void prepare_landmark_input(const uint8_t *frame,
                                   uint32_t image_width,
                                   uint32_t image_height,
                                   const rotated_roi_t *roi,
                                   uint8_t *rgb)
{
    float cosine = cosf(roi->rotation);
    float sine = sinf(roi->rotation);
    for(uint32_t y = 0; y < LANDMARK_INPUT_SIZE; ++y) {
        float v = (float)y / (float)(LANDMARK_INPUT_SIZE - 1U);
        float local_y = (v - 0.5f) * roi->height;
        for(uint32_t x = 0; x < LANDMARK_INPUT_SIZE; ++x) {
            float u = (float)x / (float)(LANDMARK_INPUT_SIZE - 1U);
            float local_x = (u - 0.5f) * roi->width;
            float source_x = (roi->center_x + cosine * local_x -
                              sine * local_y) * image_width;
            float source_y = (roi->center_y + sine * local_x +
                              cosine * local_y) * image_height;
            sample_nv12(frame, image_width, image_height,
                        source_x, source_y,
                        rgb + ((size_t)y * LANDMARK_INPUT_SIZE + x) * 3U);
        }
    }
}

static void project_landmarks(const hand_rknn_model_t *model,
                              const rotated_roi_t *roi,
                              binbot_hand_landmark_t landmarks[])
{
    float cosine = cosf(roi->rotation);
    float sine = sinf(roi->rotation);
    for(size_t index = 0; index < BINBOT_HAND_LANDMARK_COUNT; ++index) {
        float local_x = model_output(model, 0, index * 3U) /
                        LANDMARK_INPUT_SIZE;
        float local_y = model_output(model, 0, index * 3U + 1U) /
                        LANDMARK_INPUT_SIZE;
        float local_z = model_output(model, 0, index * 3U + 2U) /
                        (LANDMARK_INPUT_SIZE * 0.4f);
        float centered_x = local_x - 0.5f;
        float centered_y = local_y - 0.5f;
        landmarks[index].x =
            (cosine * centered_x - sine * centered_y) * roi->width +
            roi->center_x;
        landmarks[index].y =
            (sine * centered_x + cosine * centered_y) * roi->height +
            roi->center_y;
        landmarks[index].z = local_z * roi->width;
    }
}

static bool process_frame(binbot_hand_detector_t *detector,
                          const uint8_t *frame,
                          uint32_t frame_id,
                          binbot_hand_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->frame_id = frame_id;
    result->gesture.label = BINBOT_GESTURE_NONE;
    float padding[4];
    prepare_palm_input(frame, detector->width, detector->height,
                       detector->palm_rgb, padding);
    if(!run_model(&detector->palm_model, detector->palm_rgb)) {
        set_error(detector, "Palm RKNN inference failed");
        return false;
    }

    palm_detection_t palm;
    if(!decode_best_palm(&detector->palm_model, padding,
                         detector->minimum_palm_score, &palm)) {
        return true;
    }
    result->palm_score = palm.score;
    rotated_roi_t roi = palm_to_roi(&palm, detector->width, detector->height);
    prepare_landmark_input(frame, detector->width, detector->height,
                           &roi, detector->landmark_rgb);
    if(!run_model(&detector->landmark_model, detector->landmark_rgb)) {
        set_error(detector, "Landmark RKNN inference failed");
        return false;
    }

    result->presence_score = model_output(&detector->landmark_model, 1, 0);
    result->handedness_score = model_output(&detector->landmark_model, 2, 0);
    if(result->presence_score < detector->minimum_presence_score) return true;
    project_landmarks(&detector->landmark_model, &roi, result->landmarks);
    (void)binbot_gesture_classify(result->landmarks, result->presence_score,
                                  &result->gesture);
    return true;
}

static void *worker_main(void *argument)
{
    binbot_hand_detector_t *detector = argument;
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

        binbot_hand_result_t next;
        bool success = process_frame(detector, detector->working_frame,
                                     frame_id, &next);
        next.monotonic_ms = monotonic_ms();
        pthread_mutex_lock(&detector->mutex);
        if(!success) detector->available = false;
        if(!detector->shutting_down) {
            next.sequence = ++detector->result_sequence;
            detector->latest = next;
        }
        pthread_mutex_unlock(&detector->mutex);
    }
    return NULL;
}

static void release_detector(binbot_hand_detector_t *detector)
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
    release_model(&detector->landmark_model);
    release_model(&detector->palm_model);
    free(detector->landmark_rgb);
    free(detector->palm_rgb);
    free(detector->working_frame);
    free(detector->pending_frame);
    detector->landmark_rgb = NULL;
    detector->palm_rgb = NULL;
    detector->working_frame = NULL;
    detector->pending_frame = NULL;
    if(detector->condition_ready) {
        pthread_cond_destroy(&detector->condition);
        detector->condition_ready = false;
    }
    if(detector->mutex_ready) {
        pthread_mutex_destroy(&detector->mutex);
        detector->mutex_ready = false;
    }
}

binbot_hand_detector_t *binbot_hand_detector_create(
    const binbot_hand_detector_config_t *config)
{
    binbot_hand_detector_t *detector = calloc(1, sizeof(*detector));
    if(!detector) return NULL;
    if(pthread_mutex_init(&detector->mutex, NULL) != 0) {
        set_error(detector, "cannot initialize hand detector mutex");
        return detector;
    }
    detector->mutex_ready = true;
    if(pthread_cond_init(&detector->condition, NULL) != 0) {
        set_error(detector, "cannot initialize hand detector condition");
        return detector;
    }
    detector->condition_ready = true;

    if(!config || config->width == 0 || config->height == 0 ||
       (config->width & 1U) != 0 || (config->height & 1U) != 0) {
        set_error(detector, "hand detector requires a non-zero, even NV12 size");
        return detector;
    }
    detector->width = config->width;
    detector->height = config->height;
    detector->minimum_palm_score = config->minimum_palm_score > 0.0f
                                       ? config->minimum_palm_score : 0.5f;
    detector->minimum_presence_score = config->minimum_presence_score > 0.0f
                                           ? config->minimum_presence_score : 0.5f;
    detector->minimum_interval_ms = config->minimum_interval_ms
                                        ? config->minimum_interval_ms : 200U;
    if(detector->minimum_palm_score <= 0.0f ||
       detector->minimum_palm_score >= 1.0f ||
       detector->minimum_presence_score <= 0.0f ||
       detector->minimum_presence_score > 1.0f) {
        set_error(detector, "invalid hand detector confidence threshold");
        return detector;
    }

    const char *palm_path = config->palm_model_path && config->palm_model_path[0]
                                ? config->palm_model_path
                                : "/oem/assets/vision/hand/palm_detection_lite_int8_rv1106.rknn";
    const char *landmark_path =
        config->landmark_model_path && config->landmark_model_path[0]
            ? config->landmark_model_path
            : "/oem/assets/vision/hand/hand_landmark_lite_int8_rv1106.rknn";

    detector->frame_size = (size_t)detector->width * detector->height * 3U / 2U;
    detector->pending_frame = malloc(detector->frame_size);
    detector->working_frame = malloc(detector->frame_size);
    detector->palm_rgb = malloc(PALM_INPUT_SIZE * PALM_INPUT_SIZE * 3U);
    detector->landmark_rgb = malloc(LANDMARK_INPUT_SIZE * LANDMARK_INPUT_SIZE * 3U);
    if(!detector->pending_frame || !detector->working_frame ||
       !detector->palm_rgb || !detector->landmark_rgb) {
        set_error(detector, "cannot allocate hand detector frame buffers");
        return detector;
    }

    const uint32_t palm_outputs[] = {
        PALM_ANCHOR_COUNT * PALM_BOX_VALUE_COUNT,
        PALM_ANCHOR_COUNT,
    };
    const uint32_t landmark_outputs[] = {63, 1, 1, 63};
    if(!initialize_model(detector, &detector->palm_model, palm_path,
                         PALM_INPUT_SIZE, 2, palm_outputs) ||
       !initialize_model(detector, &detector->landmark_model, landmark_path,
                         LANDMARK_INPUT_SIZE, 4, landmark_outputs)) {
        return detector;
    }

    if(pthread_create(&detector->worker, NULL, worker_main, detector) != 0) {
        set_error(detector, "cannot start hand detector worker");
        return detector;
    }
    detector->worker_started = true;
    detector->available = true;
    detector->error[0] = '\0';
    fprintf(stdout,
            "vision: RKNN hand detector ready (%ux%u, interval %ums)\n",
            detector->width, detector->height,
            detector->minimum_interval_ms);
    return detector;
}

void binbot_hand_detector_destroy(binbot_hand_detector_t *detector)
{
    if(!detector) return;
    release_detector(detector);
    free(detector);
}

bool binbot_hand_detector_is_available(const binbot_hand_detector_t *detector)
{
    return detector && detector->available;
}

const char *binbot_hand_detector_error(const binbot_hand_detector_t *detector)
{
    if(!detector) return "hand detector allocation failed";
    return detector->error[0] ? detector->error : "";
}

binbot_hand_submit_status_t binbot_hand_detector_submit_nv12(
    binbot_hand_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride)
{
    if(!detector || !detector->available) return BINBOT_HAND_SUBMIT_UNAVAILABLE;
    if(!y_plane || !uv_plane || y_stride < detector->width ||
       uv_stride < detector->width) {
        return BINBOT_HAND_SUBMIT_ERROR;
    }
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&detector->mutex);
    if(detector->shutting_down || !detector->available) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_HAND_SUBMIT_UNAVAILABLE;
    }
    if(detector->last_submit_ms != 0 && now >= detector->last_submit_ms &&
       now - detector->last_submit_ms < detector->minimum_interval_ms) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_HAND_SUBMIT_THROTTLED;
    }
    if(detector->pending) {
        pthread_mutex_unlock(&detector->mutex);
        return BINBOT_HAND_SUBMIT_QUEUE_FULL;
    }

    for(uint32_t row = 0; row < detector->height; ++row) {
        memcpy(detector->pending_frame + (size_t)row * detector->width,
               y_plane + (size_t)row * y_stride, detector->width);
    }
    uint8_t *destination = detector->pending_frame +
                           (size_t)detector->width * detector->height;
    for(uint32_t row = 0; row < detector->height / 2U; ++row) {
        memcpy(destination + (size_t)row * detector->width,
               uv_plane + (size_t)row * uv_stride, detector->width);
    }
    detector->pending_frame_id = ++detector->next_frame_id;
    detector->last_submit_ms = now;
    detector->pending = true;
    pthread_cond_signal(&detector->condition);
    pthread_mutex_unlock(&detector->mutex);
    return BINBOT_HAND_SUBMIT_ACCEPTED;
}

bool binbot_hand_detector_latest(binbot_hand_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_hand_result_t *result)
{
    if(!detector || !result || !detector->mutex_ready) return false;
    pthread_mutex_lock(&detector->mutex);
    bool updated = detector->latest.sequence > after_sequence;
    if(updated) *result = detector->latest;
    pthread_mutex_unlock(&detector->mutex);
    return updated;
}
