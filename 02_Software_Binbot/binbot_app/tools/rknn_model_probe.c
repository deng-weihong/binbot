#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rknn_api.h"

static void *read_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if(!file) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
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
        fprintf(stderr, "cannot read %s\n", path);
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)length;
    return data;
}

static void print_attr(const char *kind, const rknn_tensor_attr *attr)
{
    printf("%s[%u] name=%s dims=", kind, attr->index, attr->name);
    for(uint32_t i = 0; i < attr->n_dims; ++i) {
        printf("%s%u", i == 0 ? "[" : ",", attr->dims[i]);
    }
    printf("] elems=%u bytes=%u stride_bytes=%u w_stride=%u fmt=%s type=%s "
           "qnt=%s zp=%d scale=%g\n",
           attr->n_elems, attr->size, attr->size_with_stride, attr->w_stride,
           get_format_string(attr->fmt),
           get_type_string(attr->type), get_qnt_type_string(attr->qnt_type),
           attr->zp, attr->scale);
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
    uint32_t sign = ((uint32_t)value & 0x8000u) << 16;
    uint32_t exponent = ((uint32_t)value >> 10) & 0x1fu;
    uint32_t mantissa = (uint32_t)value & 0x03ffu;
    uint32_t bits;

    if(exponent == 0) {
        if(mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x03ffu;
            bits = sign | ((uint32_t)(127 - 14 - shift) << 23) |
                   (mantissa << 13);
        }
    } else if(exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static float tensor_value(const void *data, size_t index,
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
    } else if(attr->qnt_type == RKNN_TENSOR_QNT_DFP) {
        value = ldexp(value, -attr->fl);
    }
    return (float)value;
}

static int copy_nhwc_input(rknn_tensor_mem *memory,
                           const rknn_tensor_attr *attr,
                           const uint8_t *source)
{
    if(attr->n_dims != 4 || attr->fmt != RKNN_TENSOR_NHWC) {
        fprintf(stderr, "only four-dimensional NHWC input is supported\n");
        return -1;
    }

    const size_t height = attr->dims[1];
    const size_t width = attr->dims[2];
    const size_t channels = attr->dims[3];
    const size_t width_stride = attr->w_stride ? attr->w_stride : width;
    const size_t source_row = width * channels;
    const size_t destination_row = width_stride * channels;

    if(width_stride < width || height * destination_row > memory->size) {
        fprintf(stderr, "invalid input stride or allocation size\n");
        return -1;
    }

    memset(memory->virt_addr, 0, memory->size);
    uint8_t *destination = memory->virt_addr;
    for(size_t row = 0; row < height; ++row) {
        memcpy(destination + row * destination_row,
               source + row * source_row, source_row);
    }
    return 0;
}

static void print_output_stats(uint32_t index, const rknn_tensor_attr *attr,
                               const rknn_tensor_mem *memory)
{
    size_t element_size = tensor_type_size(attr->type);
    size_t available = element_size ? memory->size / element_size : 0;
    size_t value_count = attr->n_elems < available ? attr->n_elems : available;
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    size_t maximum_index = 0;
    double sum = 0.0;

    for(size_t i = 0; i < value_count; ++i) {
        float value = tensor_value(memory->virt_addr, i, attr);
        if(value < minimum) minimum = value;
        if(value > maximum) {
            maximum = value;
            maximum_index = i;
        }
        sum += value;
    }

    printf("result[%u] native_count=%zu min=%g max=%g max_index=%zu mean=%g first=",
           index, value_count, minimum, maximum, maximum_index,
           value_count ? sum / (double)value_count : 0.0);
    size_t preview = value_count < 8 ? value_count : 8;
    for(size_t i = 0; i < preview; ++i) {
        printf("%s%g", i == 0 ? "[" : ",",
               tensor_value(memory->virt_addr, i, attr));
    }
    puts("]");
}

static int dump_output(const char *prefix, uint32_t index,
                       const rknn_tensor_attr *attr,
                       const rknn_tensor_mem *memory)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s.%u.f32", prefix, index);
    if(length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "output prefix is too long\n");
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if(!file) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t element_size = tensor_type_size(attr->type);
    size_t available = element_size ? memory->size / element_size : 0;
    size_t value_count = attr->n_elems < available ? attr->n_elems : available;
    int result = 0;
    for(size_t i = 0; i < value_count; ++i) {
        float value = tensor_value(memory->virt_addr, i, attr);
        if(fwrite(&value, sizeof(value), 1, file) != 1) {
            fprintf(stderr, "cannot write %s\n", path);
            result = -1;
            break;
        }
    }
    if(fclose(file) != 0) result = -1;
    if(result == 0) printf("dump[%u]=%s\n", index, path);
    return result;
}

static uint64_t wall_time_us(void)
{
    struct timeval value;
    if(gettimeofday(&value, NULL) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000U + (uint64_t)value.tv_usec;
}

static void quantize_zero_to_one(uint8_t *data, size_t count,
                                 const rknn_tensor_attr *attr)
{
    int8_t table[256];
    for(size_t i = 0; i < 256; ++i) {
        float normalized = (float)i / 255.0f;
        long quantized = lroundf(normalized / attr->scale) + attr->zp;
        if(quantized < INT8_MIN) quantized = INT8_MIN;
        if(quantized > INT8_MAX) quantized = INT8_MAX;
        table[i] = (int8_t)quantized;
    }
    for(size_t i = 0; i < count; ++i) data[i] = (uint8_t)table[data[i]];
}

int main(int argc, char **argv)
{
    if(argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s MODEL.rknn [INPUT.rgb [OUTPUT_PREFIX]]\n",
                argv[0]);
        return 2;
    }

    size_t model_size = 0;
    void *model = read_file(argv[1], &model_size);
    if(!model) return 1;

    rknn_context context = 0;
    int code = rknn_init(&context, model, (uint32_t)model_size, 0, NULL);
    if(code != RKNN_SUCC) {
        fprintf(stderr, "rknn_init failed: %d\n", code);
        free(model);
        return 1;
    }

    rknn_sdk_version version;
    memset(&version, 0, sizeof(version));
    if(rknn_query(context, RKNN_QUERY_SDK_VERSION, &version,
                  sizeof(version)) == RKNN_SUCC) {
        printf("runtime=%s driver=%s\n", version.api_version, version.drv_version);
    }

    rknn_mem_size memory;
    memset(&memory, 0, sizeof(memory));
    if(rknn_query(context, RKNN_QUERY_MEM_SIZE, &memory, sizeof(memory)) == RKNN_SUCC) {
        printf("memory weights=%u internal=%u dma=%llu sram=%u free_sram=%u\n",
               memory.total_weight_size, memory.total_internal_size,
               (unsigned long long)memory.total_dma_allocated_size,
               memory.total_sram_size, memory.free_sram_size);
    }

    rknn_input_output_num count;
    memset(&count, 0, sizeof(count));
    if(rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count)) != RKNN_SUCC ||
       count.n_input != 1 || count.n_output == 0 || count.n_output > 16) {
        fprintf(stderr, "unsupported model I/O count: %u/%u\n",
                count.n_input, count.n_output);
        rknn_destroy(context);
        free(model);
        return 1;
    }

    rknn_tensor_attr logical_input_attr;
    memset(&logical_input_attr, 0, sizeof(logical_input_attr));
    logical_input_attr.index = 0;
    if(rknn_query(context, RKNN_QUERY_INPUT_ATTR, &logical_input_attr,
                  sizeof(logical_input_attr)) != RKNN_SUCC) {
        fprintf(stderr, "cannot query logical input tensor\n");
        rknn_destroy(context);
        free(model);
        return 1;
    }
    print_attr("input", &logical_input_attr);

    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    if(rknn_query(context, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attr,
                  sizeof(input_attr)) != RKNN_SUCC) {
        fprintf(stderr, "cannot query native input tensor\n");
        rknn_destroy(context);
        free(model);
        return 1;
    }
    print_attr("native_input", &input_attr);

    rknn_tensor_attr output_attrs[16];
    rknn_tensor_attr native_output_attrs[16];
    memset(output_attrs, 0, sizeof(output_attrs));
    memset(native_output_attrs, 0, sizeof(native_output_attrs));
    for(uint32_t i = 0; i < count.n_output; ++i) {
        output_attrs[i].index = i;
        native_output_attrs[i].index = i;
        if(rknn_query(context, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i],
                      sizeof(output_attrs[i])) != RKNN_SUCC ||
           rknn_query(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                      &native_output_attrs[i],
                      sizeof(native_output_attrs[i])) != RKNN_SUCC) {
            fprintf(stderr, "cannot query output tensor %u\n", i);
            rknn_destroy(context);
            free(model);
            return 1;
        }
        print_attr("output", &output_attrs[i]);
        print_attr("native_output", &native_output_attrs[i]);
    }

    size_t input_bytes = input_attr.n_elems;
    uint8_t *rgb_data = calloc(input_bytes, 1);
    if(!rgb_data) {
        free(rgb_data);
        rknn_destroy(context);
        free(model);
        return 1;
    }
    if(argc >= 3) {
        size_t file_size = 0;
        void *file_data = read_file(argv[2], &file_size);
        if(!file_data || file_size != input_bytes) {
            fprintf(stderr, "input must contain exactly %zu RGB bytes\n", input_bytes);
            free(file_data);
            free(rgb_data);
            rknn_destroy(context);
            free(model);
            return 1;
        }
        memcpy(rgb_data, file_data, input_bytes);
        free(file_data);
    }

    rknn_tensor_mem *input_memory = NULL;
    rknn_tensor_mem *output_memories[16];
    memset(output_memories, 0, sizeof(output_memories));

    const char *pass_through = getenv("BINBOT_RKNN_PASSTHROUGH");
    if(pass_through && pass_through[0] && strcmp(pass_through, "0") != 0) {
        if(input_attr.type != RKNN_TENSOR_INT8 ||
           input_attr.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
           input_attr.scale <= 0.0f) {
            fprintf(stderr, "pass-through mode requires affine INT8 input\n");
            free(rgb_data);
            rknn_destroy(context);
            free(model);
            return 1;
        }
        quantize_zero_to_one(rgb_data, input_bytes, &input_attr);
        input_attr.pass_through = 1;
        printf("input_mode=quantized_int8_pass_through\n");
    } else {
        input_attr.type = RKNN_TENSOR_UINT8;
        input_attr.fmt = RKNN_TENSOR_NHWC;
        input_attr.pass_through = 0;
        printf("input_mode=runtime_uint8_conversion\n");
    }
    uint32_t input_allocation = input_attr.size_with_stride ?
                                input_attr.size_with_stride : input_attr.size;
    input_memory = rknn_create_mem(context, input_allocation);
    if(!input_memory || copy_nhwc_input(input_memory, &input_attr, rgb_data) != 0) {
        code = -1;
    } else {
        code = rknn_set_io_mem(context, input_memory, &input_attr);
    }

    for(uint32_t i = 0; code == RKNN_SUCC && i < count.n_output; ++i) {
        uint32_t allocation = native_output_attrs[i].size_with_stride ?
                              native_output_attrs[i].size_with_stride :
                              native_output_attrs[i].size;
        output_memories[i] = rknn_create_mem(context, allocation);
        if(!output_memories[i]) {
            code = -1;
            break;
        }
        code = rknn_set_io_mem(context, output_memories[i],
                               &native_output_attrs[i]);
    }
    uint64_t started_us = wall_time_us();
    if(code == RKNN_SUCC) code = rknn_run(context, NULL);
    uint64_t finished_us = wall_time_us();

    if(code != RKNN_SUCC) {
        fprintf(stderr, "inference failed: %d\n", code);
        if(input_memory) rknn_destroy_mem(context, input_memory);
        for(uint32_t i = 0; i < count.n_output; ++i) {
            if(output_memories[i]) rknn_destroy_mem(context, output_memories[i]);
        }
        free(rgb_data);
        rknn_destroy(context);
        free(model);
        return 1;
    }

    for(uint32_t i = 0; i < count.n_output; ++i) {
        print_output_stats(i, &native_output_attrs[i], output_memories[i]);
        if(argc == 4 && dump_output(argv[3], i, &native_output_attrs[i],
                                    output_memories[i]) != 0) {
            code = -1;
        }
    }
    printf("wall_inference_us=%llu\n",
           (unsigned long long)(finished_us >= started_us ?
                                finished_us - started_us : 0));

    rknn_destroy_mem(context, input_memory);
    for(uint32_t i = 0; i < count.n_output; ++i) {
        rknn_destroy_mem(context, output_memories[i]);
    }
    free(rgb_data);
    rknn_destroy(context);
    free(model);
    return code == RKNN_SUCC ? 0 : 1;
}
