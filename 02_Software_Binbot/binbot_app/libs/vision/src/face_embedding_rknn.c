#include "binbot/vision/face_embedding.h"

#include "rknn_api.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FACE_EMBEDDER_ERROR_CAPACITY = 192 };

struct binbot_face_embedder {
    rknn_context context;
    bool initialized;
    void *model_data;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attr;
    rknn_tensor_attr native_output_attr;
    rknn_tensor_mem *input_memory;
    rknn_tensor_mem *output_memory;
    uint32_t input_width;
    uint32_t input_height;
    char error[FACE_EMBEDDER_ERROR_CAPACITY];
};

static void set_error(binbot_face_embedder_t *embedder,
                      const char *format, ...)
{
    if(!embedder) return;
    va_list args;
    va_start(args, format);
    vsnprintf(embedder->error, sizeof(embedder->error), format, args);
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

static float output_value(const void *data, size_t index,
                          const rknn_tensor_attr *attr,
                          size_t available_bytes)
{
    if(!data || !attr) return NAN;
    if(attr->type == RKNN_TENSOR_FLOAT32 && available_bytes >=
       (index + 1U) * sizeof(float)) {
        return ((const float *)data)[index];
    }
    if(attr->type == RKNN_TENSOR_INT8 && available_bytes >= index + 1U) {
        float value = (float)((const int8_t *)data)[index];
        if(attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
            value = (value - (float)attr->zp) * attr->scale;
        }
        return value;
    }
    if((attr->type == RKNN_TENSOR_UINT8 || attr->type == RKNN_TENSOR_BOOL) &&
       available_bytes >= index + 1U) {
        float value = (float)((const uint8_t *)data)[index];
        if(attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
            value = (value - (float)attr->zp) * attr->scale;
        }
        return value;
    }
    return NAN;
}

binbot_face_embedder_t *binbot_face_embedder_create(const char *model_path)
{
    binbot_face_embedder_t *embedder = calloc(1, sizeof(*embedder));
    if(!embedder) return NULL;
    const char *path = model_path && model_path[0]
                           ? model_path
                           : "/oem/assets/vision/face/recognition.rknn";
    size_t model_size = 0;
    embedder->model_data = read_file(path, &model_size);
    if(!embedder->model_data) {
        set_error(embedder, "cannot read recognition model %s: %s", path,
                  strerror(errno));
        return embedder;
    }
    int code = rknn_init(&embedder->context, embedder->model_data,
                         (uint32_t)model_size, 0, NULL);
    if(code != RKNN_SUCC) {
        set_error(embedder, "rknn_init(%s) failed: %d", path, code);
        return embedder;
    }
    embedder->initialized = true;

    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    if(rknn_query(embedder->context, RKNN_QUERY_IN_OUT_NUM, &io,
                  sizeof(io)) != RKNN_SUCC || io.n_input != 1 ||
       io.n_output != 1) {
        set_error(embedder, "recognition model must have one input and one output");
        return embedder;
    }

    memset(&embedder->input_attr, 0, sizeof(embedder->input_attr));
    embedder->input_attr.index = 0;
    if(rknn_query(embedder->context, RKNN_QUERY_INPUT_ATTR,
                  &embedder->input_attr, sizeof(embedder->input_attr)) !=
       RKNN_SUCC) {
        set_error(embedder, "cannot query recognition input tensor");
        return embedder;
    }
    bool nhwc = embedder->input_attr.fmt == RKNN_TENSOR_NHWC;
    bool nchw = embedder->input_attr.fmt == RKNN_TENSOR_NCHW;
    if(embedder->input_attr.n_dims != 4 || (!nhwc && !nchw) ||
       (nhwc && embedder->input_attr.dims[3] != 3) ||
       (nchw && embedder->input_attr.dims[1] != 3)) {
        set_error(embedder, "unsupported recognition input layout");
        return embedder;
    }
    embedder->input_width = nhwc ? embedder->input_attr.dims[2]
                                 : embedder->input_attr.dims[3];
    embedder->input_height = nhwc ? embedder->input_attr.dims[1]
                                  : embedder->input_attr.dims[2];
    if(embedder->input_width != 112U || embedder->input_height != 112U ||
       embedder->input_attr.type != RKNN_TENSOR_UINT8) {
        set_error(embedder, "recognition input is not UINT8 112x112 (got %ux%u type %d)",
                  embedder->input_width, embedder->input_height,
                  embedder->input_attr.type);
        return embedder;
    }

    memset(&embedder->output_attr, 0, sizeof(embedder->output_attr));
    embedder->output_attr.index = 0;
    if(rknn_query(embedder->context, RKNN_QUERY_OUTPUT_ATTR,
                  &embedder->output_attr, sizeof(embedder->output_attr)) !=
       RKNN_SUCC || embedder->output_attr.n_elems < BINBOT_FACE_EMBEDDING_DIM) {
        set_error(embedder, "recognition output is not a 128-element tensor");
        return embedder;
    }
    memset(&embedder->native_output_attr, 0,
           sizeof(embedder->native_output_attr));
    embedder->native_output_attr.index = 0;
    if(rknn_query(embedder->context, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                  &embedder->native_output_attr,
                  sizeof(embedder->native_output_attr)) != RKNN_SUCC) {
        set_error(embedder, "cannot query native recognition output tensor");
        return embedder;
    }

    embedder->input_attr.pass_through = 0;
    uint32_t input_bytes = embedder->input_attr.size_with_stride
                               ? embedder->input_attr.size_with_stride
                               : embedder->input_attr.size;
    embedder->input_memory = rknn_create_mem(embedder->context, input_bytes);
    if(!embedder->input_memory ||
       rknn_set_io_mem(embedder->context, embedder->input_memory,
                       &embedder->input_attr) != RKNN_SUCC) {
        set_error(embedder, "cannot bind recognition input memory");
        return embedder;
    }
    uint32_t output_bytes = embedder->native_output_attr.size_with_stride
                                ? embedder->native_output_attr.size_with_stride
                                : embedder->native_output_attr.size;
    embedder->output_memory = rknn_create_mem(embedder->context, output_bytes);
    if(!embedder->output_memory ||
       rknn_set_io_mem(embedder->context, embedder->output_memory,
                       &embedder->native_output_attr) != RKNN_SUCC) {
        set_error(embedder, "cannot bind recognition output memory");
        return embedder;
    }
    embedder->error[0] = '\0';
    fprintf(stdout, "vision: recognition RKNN ready (%ux%u, model %s)\n",
            embedder->input_width, embedder->input_height, path);
    return embedder;
}

void binbot_face_embedder_destroy(binbot_face_embedder_t *embedder)
{
    if(!embedder) return;
    if(embedder->initialized) {
        if(embedder->input_memory) {
            rknn_destroy_mem(embedder->context, embedder->input_memory);
        }
        if(embedder->output_memory) {
            rknn_destroy_mem(embedder->context, embedder->output_memory);
        }
        rknn_destroy(embedder->context);
    }
    free(embedder->model_data);
    free(embedder);
}

bool binbot_face_embedder_is_available(const binbot_face_embedder_t *embedder)
{
    return embedder && embedder->initialized && embedder->error[0] == '\0';
}

const char *binbot_face_embedder_error(const binbot_face_embedder_t *embedder)
{
    return embedder && embedder->error[0] ? embedder->error : "";
}

bool binbot_face_embedder_run_rgb(
    binbot_face_embedder_t *embedder,
    const unsigned char *rgb,
    float embedding[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!binbot_face_embedder_is_available(embedder) || !rgb || !embedding) {
        return false;
    }
    size_t row_bytes = (size_t)embedder->input_width * 3U;
    size_t destination_row = (size_t)(embedder->input_attr.w_stride
                                          ? embedder->input_attr.w_stride
                                          : embedder->input_width) * 3U;
    if(destination_row < row_bytes ||
       (size_t)embedder->input_height * destination_row >
           embedder->input_memory->size) {
        set_error(embedder, "invalid recognition input stride");
        return false;
    }
    memset(embedder->input_memory->virt_addr, 0,
           embedder->input_memory->size);
    for(uint32_t row = 0; row < embedder->input_height; ++row) {
        memcpy((uint8_t *)embedder->input_memory->virt_addr +
                   (size_t)row * destination_row,
               rgb + (size_t)row * row_bytes, row_bytes);
    }
    int code = rknn_run(embedder->context, NULL);
    if(code != RKNN_SUCC) {
        set_error(embedder, "recognition inference failed: %d", code);
        return false;
    }
    bool valid = embedder->output_memory->size >= BINBOT_FACE_EMBEDDING_DIM;
    if(valid) {
        for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
            embedding[i] = output_value(embedder->output_memory->virt_addr, i,
                                        &embedder->native_output_attr,
                                        embedder->output_memory->size);
            if(!isfinite(embedding[i])) {
                valid = false;
                break;
            }
        }
    }
    if(valid) valid = binbot_face_embedding_normalize(embedding);
    if(!valid) set_error(embedder, "recognition output is invalid");
    return valid;
}
