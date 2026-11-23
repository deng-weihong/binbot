#include "binbot/vision/face_recognizer.h"

#include "binbot/vision/face_embedding.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    FACE_INPUT_SIZE = 112,
    FACE_ERROR_CAPACITY = 192,
};

struct binbot_face_recognizer {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool mutex_ready;
    bool condition_ready;
    bool worker_started;
    pthread_t worker;
    bool available;
    bool shutting_down;
    bool pending;
    uint16_t width;
    uint16_t height;
    uint32_t minimum_interval_ms;
    float crop_margin;
    uint32_t next_frame_id;
    uint32_t pending_frame_id;
    uint64_t last_submit_ms;
    uint64_t result_sequence;
    size_t frame_size;
    uint8_t *pending_frame;
    uint8_t *working_frame;
    uint8_t *face_rgb;
    binbot_face_box_t pending_face;
    binbot_face_embedder_t *embedder;
    binbot_face_matcher_t matcher;
    char *reference_capture_path;
    uint8_t reference_capture_samples;
    uint8_t reference_capture_count;
    uint32_t reference_capture_track_id;
    float reference_capture_sum[BINBOT_FACE_EMBEDDING_DIM];
    binbot_face_recognition_result_t latest;
    char error[FACE_ERROR_CAPACITY];
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000U +
           (uint64_t)value.tv_nsec / 1000000U;
}

static void set_error(binbot_face_recognizer_t *recognizer,
                      const char *format, ...)
{
    if(!recognizer) return;
    va_list args;
    va_start(args, format);
    vsnprintf(recognizer->error, sizeof(recognizer->error), format, args);
    va_end(args);
}

static uint8_t clamp_u8(int value)
{
    if(value < 0) return 0;
    if(value > 255) return 255;
    return (uint8_t)value;
}

static char *copy_string(const char *value)
{
    if(!value || !value[0]) return NULL;
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if(copy) memcpy(copy, value, length);
    return copy;
}

static bool write_reference_atomic(const char *path,
                                   const float reference[BINBOT_FACE_EMBEDDING_DIM])
{
    size_t path_length = strlen(path);
    char *temporary = malloc(path_length + 5U);
    if(!temporary) return false;
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, ".tmp", 5U);
    FILE *file = fopen(temporary, "wb");
    bool success = file && fwrite(reference, sizeof(float),
                                  BINBOT_FACE_EMBEDDING_DIM, file) ==
                                      BINBOT_FACE_EMBEDDING_DIM;
    if(success && fflush(file) != 0) success = false;
    if(success && fsync(fileno(file)) != 0) success = false;
    if(file && fclose(file) != 0) success = false;
    if(success && rename(temporary, path) != 0) success = false;
    if(!success) unlink(temporary);
    free(temporary);
    return success;
}

static void capture_reference(binbot_face_recognizer_t *recognizer,
                              uint32_t track_id,
                              const float embedding[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!recognizer->reference_capture_path ||
       recognizer->reference_capture_count >= recognizer->reference_capture_samples) {
        return;
    }
    if(recognizer->reference_capture_count == 0 ||
       recognizer->reference_capture_track_id != track_id) {
        memset(recognizer->reference_capture_sum, 0,
               sizeof(recognizer->reference_capture_sum));
        recognizer->reference_capture_count = 0;
        recognizer->reference_capture_track_id = track_id;
    }
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        recognizer->reference_capture_sum[i] += embedding[i];
    }
    ++recognizer->reference_capture_count;
    if(recognizer->reference_capture_count < recognizer->reference_capture_samples) {
        return;
    }

    float reference[BINBOT_FACE_EMBEDDING_DIM];
    memcpy(reference, recognizer->reference_capture_sum, sizeof(reference));
    if(!binbot_face_embedding_normalize(reference) ||
       !write_reference_atomic(recognizer->reference_capture_path, reference)) {
        fprintf(stderr, "vision: failed to capture fixed face reference: %s\n",
                recognizer->reference_capture_path);
        return;
    }
    (void)binbot_face_matcher_set_reference(&recognizer->matcher, reference);
    fprintf(stdout,
            "vision: fixed face reference captured path=%s samples=%u track=%u\n",
            recognizer->reference_capture_path,
            recognizer->reference_capture_samples, track_id);
}

static uint8_t nv12_luma(const uint8_t *frame, uint32_t width, uint32_t height,
                         int x, int y)
{
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x >= (int)width) x = (int)width - 1;
    if(y >= (int)height) y = (int)height - 1;
    return frame[(size_t)y * width + (size_t)x];
}

static uint8_t sample_nv12_luma(const uint8_t *frame, uint32_t width,
                                uint32_t height, float x, float y)
{
    if(x < 0.0f) x = 0.0f;
    if(y < 0.0f) y = 0.0f;
    if(x > (float)(width - 1U)) x = (float)(width - 1U);
    if(y > (float)(height - 1U)) y = (float)(height - 1U);
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1 < (int)width ? x0 + 1 : x0;
    int y1 = y0 + 1 < (int)height ? y0 + 1 : y0;
    float xf = x - (float)x0;
    float yf = y - (float)y0;
    uint8_t top_left = nv12_luma(frame, width, height, x0, y0);
    uint8_t top_right = nv12_luma(frame, width, height, x1, y0);
    uint8_t bottom_left = nv12_luma(frame, width, height, x0, y1);
    uint8_t bottom_right = nv12_luma(frame, width, height, x1, y1);
    float top = top_left * (1.0f - xf) + top_right * xf;
    float bottom = bottom_left * (1.0f - xf) + bottom_right * xf;
    return clamp_u8((int)lroundf(top * (1.0f - yf) + bottom * yf));
}

static bool prepare_face_rgb(const uint8_t *frame, uint32_t width,
                             uint32_t height, const binbot_face_box_t *box,
                             float margin, uint8_t *rgb)
{
    if(!frame || !box || !rgb || width == 0 || height == 0) return false;
    float left = (float)box->left * width / BINBOT_FACE_COORDINATE_SCALE;
    float top = (float)box->top * height / BINBOT_FACE_COORDINATE_SCALE;
    float right = (float)box->right * width / BINBOT_FACE_COORDINATE_SCALE;
    float bottom = (float)box->bottom * height / BINBOT_FACE_COORDINATE_SCALE;
    /* RockIVA can report a box that touches or temporarily extends past the
       negotiated frame while a face is moving toward the lens.  Clamp the
       coordinates before expanding the box so a large valid face cannot make
       the square crop larger than the source image. */
    left = fmaxf(0.0f, fminf(left, (float)width));
    top = fmaxf(0.0f, fminf(top, (float)height));
    right = fmaxf(0.0f, fminf(right, (float)width));
    bottom = fmaxf(0.0f, fminf(bottom, (float)height));
    if(right <= left || bottom <= top) return false;
    float box_width = right - left;
    float box_height = bottom - top;
    float side = fmaxf(box_width, box_height) * (1.0f + margin);
    if(side < 8.0f) return false;
    if(side > fminf((float)width, (float)height)) {
        side = fminf((float)width, (float)height);
    }
    float center_x = (left + right) * 0.5f;
    float center_y = (top + bottom) * 0.5f;
    float crop_left = center_x - side * 0.5f;
    float crop_top = center_y - side * 0.5f;
    /* Keep the square crop inside the source. This is a bbox fallback until
       the RockIVA five-point analysis path is enabled. */
    if(crop_left < 0.0f) crop_left = 0.0f;
    if(crop_top < 0.0f) crop_top = 0.0f;
    if(crop_left + side > width) crop_left = (float)width - side;
    if(crop_top + side > height) crop_top = (float)height - side;
    if(crop_left < 0.0f || crop_top < 0.0f) return false;
    if(crop_left + side > width) side = width - crop_left;
    if(crop_top + side > height) side = height - crop_top;
    if(side <= 1.0f) return false;
    for(uint32_t y = 0; y < FACE_INPUT_SIZE; ++y) {
        float source_y = crop_top + ((float)y + 0.5f) * side /
                         FACE_INPUT_SIZE - 0.5f;
        for(uint32_t x = 0; x < FACE_INPUT_SIZE; ++x) {
            float source_x = crop_left + ((float)x + 0.5f) * side /
                             FACE_INPUT_SIZE - 0.5f;
            uint8_t value = sample_nv12_luma(frame, width, height,
                                             source_x, source_y);
            uint8_t *pixel = rgb + ((size_t)y * FACE_INPUT_SIZE + x) * 3U;
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
        }
    }
    return true;
}

typedef enum {
    FACE_PROCESS_OK = 0,
    FACE_PROCESS_DROPPED,
    FACE_PROCESS_FATAL,
} face_process_status_t;

static face_process_status_t process_frame(
    binbot_face_recognizer_t *recognizer, const uint8_t *frame,
    uint32_t frame_id, const binbot_face_box_t *face,
    binbot_face_recognition_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->frame_id = frame_id;
    result->track_id = face ? face->track_id : 0;
    result->has_reference = recognizer->matcher.has_reference;
    result->state = result->has_reference ? BINBOT_FACE_MATCH_INVALID_VECTOR
                                          : BINBOT_FACE_MATCH_NO_REFERENCE;
    if(!face || !prepare_face_rgb(frame, recognizer->width, recognizer->height,
                                  face, recognizer->crop_margin,
                                  recognizer->face_rgb)) {
        /* A detector frame can be stale or partially outside the image.  It
           is a per-frame drop, not a recognizer failure. */
        return FACE_PROCESS_DROPPED;
    }
    float embedding[BINBOT_FACE_EMBEDDING_DIM];
    if(!binbot_face_embedder_run_rgb(recognizer->embedder, recognizer->face_rgb,
                                     embedding)) {
        set_error(recognizer, "recognition inference failed: %s",
                  binbot_face_embedder_error(recognizer->embedder));
        return FACE_PROCESS_FATAL;
    }
    capture_reference(recognizer, face->track_id, embedding);
    result->state = binbot_face_matcher_compare(&recognizer->matcher,
                                                face->track_id, embedding,
                                                &result->score);
    return FACE_PROCESS_OK;
}

static void *worker_main(void *argument)
{
    binbot_face_recognizer_t *recognizer = argument;
    for(;;) {
        pthread_mutex_lock(&recognizer->mutex);
        while(!recognizer->pending && !recognizer->shutting_down) {
            pthread_cond_wait(&recognizer->condition, &recognizer->mutex);
        }
        if(recognizer->shutting_down) {
            pthread_mutex_unlock(&recognizer->mutex);
            break;
        }
        uint8_t *swap = recognizer->working_frame;
        recognizer->working_frame = recognizer->pending_frame;
        recognizer->pending_frame = swap;
        binbot_face_box_t face = recognizer->pending_face;
        uint32_t frame_id = recognizer->pending_frame_id;
        recognizer->pending = false;
        pthread_mutex_unlock(&recognizer->mutex);

        binbot_face_recognition_result_t next;
        face_process_status_t process_status = process_frame(
            recognizer, recognizer->working_frame, frame_id, &face, &next);
        next.monotonic_ms = monotonic_ms();
        pthread_mutex_lock(&recognizer->mutex);
        if(process_status == FACE_PROCESS_FATAL) recognizer->available = false;
        if(!recognizer->shutting_down) {
            next.sequence = ++recognizer->result_sequence;
            recognizer->latest = next;
        }
        pthread_mutex_unlock(&recognizer->mutex);
    }
    return NULL;
}

static void release_recognizer(binbot_face_recognizer_t *recognizer)
{
    if(!recognizer) return;
    if(recognizer->worker_started) {
        pthread_mutex_lock(&recognizer->mutex);
        recognizer->shutting_down = true;
        pthread_cond_signal(&recognizer->condition);
        pthread_mutex_unlock(&recognizer->mutex);
        pthread_join(recognizer->worker, NULL);
        recognizer->worker_started = false;
    }
    binbot_face_embedder_destroy(recognizer->embedder);
    recognizer->embedder = NULL;
    free(recognizer->face_rgb);
    free(recognizer->working_frame);
    free(recognizer->pending_frame);
    free(recognizer->reference_capture_path);
    recognizer->face_rgb = NULL;
    recognizer->working_frame = NULL;
    recognizer->pending_frame = NULL;
    recognizer->reference_capture_path = NULL;
    if(recognizer->condition_ready) pthread_cond_destroy(&recognizer->condition);
    if(recognizer->mutex_ready) pthread_mutex_destroy(&recognizer->mutex);
    recognizer->condition_ready = false;
    recognizer->mutex_ready = false;
}

binbot_face_recognizer_t *binbot_face_recognizer_create(
    const binbot_face_recognizer_config_t *config)
{
    binbot_face_recognizer_t *recognizer = calloc(1, sizeof(*recognizer));
    if(!recognizer) return NULL;
    if(pthread_mutex_init(&recognizer->mutex, NULL) != 0) {
        set_error(recognizer, "cannot initialize face recognizer mutex");
        return recognizer;
    }
    recognizer->mutex_ready = true;
    if(pthread_cond_init(&recognizer->condition, NULL) != 0) {
        set_error(recognizer, "cannot initialize face recognizer condition");
        return recognizer;
    }
    recognizer->condition_ready = true;
    if(!config || config->width == 0 || config->height == 0 ||
       (config->width & 1U) != 0 || (config->height & 1U) != 0) {
        set_error(recognizer, "face recognizer requires a non-zero even NV12 size");
        return recognizer;
    }
    recognizer->width = config->width;
    recognizer->height = config->height;
    recognizer->minimum_interval_ms = config->minimum_interval_ms
                                          ? config->minimum_interval_ms : 500U;
    recognizer->crop_margin = config->crop_margin >= 0.0f &&
                              config->crop_margin <= 1.0f
                                  ? config->crop_margin : 0.35f;
    recognizer->reference_capture_path =
        copy_string(config->reference_capture_path);
    recognizer->reference_capture_samples =
        config->reference_capture_samples >= 3U
            ? config->reference_capture_samples : 10U;
    binbot_face_matcher_init(&recognizer->matcher, config->match_threshold,
                             config->confirm_frames);
    recognizer->frame_size = (size_t)recognizer->width * recognizer->height * 3U / 2U;
    recognizer->pending_frame = malloc(recognizer->frame_size);
    recognizer->working_frame = malloc(recognizer->frame_size);
    recognizer->face_rgb = malloc(FACE_INPUT_SIZE * FACE_INPUT_SIZE * 3U);
    if(!recognizer->pending_frame || !recognizer->working_frame ||
       !recognizer->face_rgb) {
        set_error(recognizer, "cannot allocate face recognizer buffers");
        return recognizer;
    }
    const char *model_path = config->model_path && config->model_path[0]
                                 ? config->model_path
                                 : "/oem/assets/vision/face/recognition.rknn";
    recognizer->embedder = binbot_face_embedder_create(model_path);
    if(!binbot_face_embedder_is_available(recognizer->embedder)) {
        set_error(recognizer, "face embedding unavailable: %s",
                  binbot_face_embedder_error(recognizer->embedder));
        return recognizer;
    }
    if(config->reference_feature_path && config->reference_feature_path[0] &&
       !binbot_face_matcher_load_reference(&recognizer->matcher,
                                           config->reference_feature_path)) {
        set_error(recognizer, "cannot load fixed face reference: %s",
                  config->reference_feature_path);
        return recognizer;
    }
    if(pthread_create(&recognizer->worker, NULL, worker_main, recognizer) != 0) {
        set_error(recognizer, "cannot start face recognizer worker");
        return recognizer;
    }
    recognizer->worker_started = true;
    recognizer->available = true;
    recognizer->latest.state = recognizer->matcher.has_reference
                                   ? BINBOT_FACE_MATCH_MISMATCH
                                   : BINBOT_FACE_MATCH_NO_REFERENCE;
    recognizer->latest.has_reference = recognizer->matcher.has_reference;
    recognizer->error[0] = '\0';
    fprintf(stdout, "vision: fixed face recognizer ready (%ux%u, interval %ums, threshold %.3f)\n",
            recognizer->width, recognizer->height,
            recognizer->minimum_interval_ms, recognizer->matcher.threshold);
    return recognizer;
}

void binbot_face_recognizer_destroy(binbot_face_recognizer_t *recognizer)
{
    if(!recognizer) return;
    release_recognizer(recognizer);
    free(recognizer);
}

bool binbot_face_recognizer_is_available(
    const binbot_face_recognizer_t *recognizer)
{
    return recognizer && recognizer->available && recognizer->worker_started &&
           recognizer->error[0] == '\0';
}

const char *binbot_face_recognizer_error(
    const binbot_face_recognizer_t *recognizer)
{
    return recognizer && recognizer->error[0] ? recognizer->error : "";
}

binbot_face_recognition_result_t binbot_face_recognizer_status(
    const binbot_face_recognizer_t *recognizer)
{
    binbot_face_recognition_result_t result;
    memset(&result, 0, sizeof(result));
    result.state = BINBOT_FACE_MATCH_NO_REFERENCE;
    if(!recognizer) return result;
    pthread_mutex_lock((pthread_mutex_t *)&recognizer->mutex);
    result = recognizer->latest;
    pthread_mutex_unlock((pthread_mutex_t *)&recognizer->mutex);
    return result;
}

binbot_face_recognition_submit_status_t binbot_face_recognizer_submit_nv12(
    binbot_face_recognizer_t *recognizer, const uint8_t *y_plane,
    uint32_t y_stride, const uint8_t *uv_plane, uint32_t uv_stride,
    const binbot_face_box_t *face)
{
    if(!binbot_face_recognizer_is_available(recognizer)) {
        return BINBOT_FACE_RECOGNITION_UNAVAILABLE;
    }
    if(!y_plane || !uv_plane || !face || y_stride < recognizer->width ||
       uv_stride < recognizer->width) return BINBOT_FACE_RECOGNITION_ERROR;
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&recognizer->mutex);
    if(recognizer->last_submit_ms != 0 &&
       now < recognizer->last_submit_ms + recognizer->minimum_interval_ms) {
        pthread_mutex_unlock(&recognizer->mutex);
        return BINBOT_FACE_RECOGNITION_THROTTLED;
    }
    if(recognizer->pending) {
        pthread_mutex_unlock(&recognizer->mutex);
        return BINBOT_FACE_RECOGNITION_QUEUE_FULL;
    }
    uint8_t *destination = recognizer->pending_frame;
    for(uint32_t row = 0; row < recognizer->height; ++row) {
        memcpy(destination + (size_t)row * recognizer->width,
               y_plane + (size_t)row * y_stride, recognizer->width);
    }
    uint8_t *uv_destination = destination +
                               (size_t)recognizer->width * recognizer->height;
    for(uint32_t row = 0; row < recognizer->height / 2U; ++row) {
        memcpy(uv_destination + (size_t)row * recognizer->width,
               uv_plane + (size_t)row * uv_stride, recognizer->width);
    }
    recognizer->pending_face = *face;
    recognizer->pending_frame_id = ++recognizer->next_frame_id;
    recognizer->last_submit_ms = now;
    recognizer->pending = true;
    pthread_cond_signal(&recognizer->condition);
    pthread_mutex_unlock(&recognizer->mutex);
    return BINBOT_FACE_RECOGNITION_ACCEPTED;
}

bool binbot_face_recognizer_latest(
    binbot_face_recognizer_t *recognizer, uint64_t after_sequence,
    binbot_face_recognition_result_t *result)
{
    if(!recognizer || !result) return false;
    pthread_mutex_lock(&recognizer->mutex);
    bool available = recognizer->latest.sequence > after_sequence;
    if(available) *result = recognizer->latest;
    pthread_mutex_unlock(&recognizer->mutex);
    return available;
}
