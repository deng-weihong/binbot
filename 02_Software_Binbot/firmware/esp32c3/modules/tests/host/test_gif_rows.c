#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gif_player.h"
#include "storage.h"

struct storage_file { FILE *stream; };

esp_err_t storage_file_open(const char *path, const char *mode, storage_file_handle_t *file)
{
    *file = calloc(1, sizeof(**file));
    assert(*file);
    (*file)->stream = fopen(path, mode);
    assert((*file)->stream);
    return ESP_OK;
}

esp_err_t storage_file_read(storage_file_handle_t file, void *buffer, size_t length, size_t *read)
{
    *read = fread(buffer, 1, length, file->stream);
    return ESP_OK;
}

esp_err_t storage_file_seek(storage_file_handle_t file, long offset, int whence)
{
    return fseek(file->stream, offset, whence) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t storage_file_tell(storage_file_handle_t file, long *position)
{
    *position = ftell(file->stream);
    return *position >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t storage_file_close(storage_file_handle_t file)
{
    int result = fclose(file->stream);
    free(file);
    return result == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

typedef struct {
    FILE *output;
    uint8_t *canvas;
    uint16_t width, height, next_row;
    bool fail;
} output_t;

static esp_err_t flush_rows(void *context, const uint8_t *rows, uint16_t width,
                            uint16_t first_row, uint16_t count)
{
    output_t *output = context;
    assert(width == output->width && first_row == output->next_row);
    assert(count && count <= 8 && first_row + count <= output->height);
    if (output->fail) return ESP_ERR_INVALID_STATE;
    size_t bytes = (size_t)width * count * 2;
    memcpy(output->canvas + (size_t)first_row * width * 2, rows, bytes);
    output->next_row += count;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    gif_player_handle_t rows, full;
    assert(gif_player_open(argv[1], &rows) == ESP_OK);
    assert(gif_player_open(argv[1], &full) == ESP_OK);
    uint16_t width, height;
    assert(gif_player_get_size(rows, &width, &height) == ESP_OK);
    assert(gif_player_get_size(rows, NULL, &height) == ESP_ERR_INVALID_ARG);
    size_t bytes = (size_t)width * height * 2;
    uint8_t *canvas = malloc(bytes), *expected = malloc(bytes);
    size_t buffer_size = (size_t)width * 8 * 2;
    uint8_t *guarded = malloc(buffer_size + 2);
    memset(guarded, 0xA5, buffer_size + 2);
    output_t output = {fopen(argv[2], "wb"), canvas, width, height, 0, false};
    assert(output.output);
    uint32_t delay;
    assert(gif_player_next_frame_rows(rows, guarded + 1, width * 2 - 1,
                                     flush_rows, &output, &delay) == ESP_ERR_INVALID_ARG);
    assert(gif_player_next_frame_rows(rows, guarded + 1, buffer_size,
                                     flush_rows, &output, NULL) == ESP_ERR_INVALID_ARG);
    unsigned frames = (unsigned)atoi(argv[3]);
    for (unsigned i = 0; i < frames * 2; ++i) {
        uint32_t full_delay;
        output.next_row = 0;
        assert(gif_player_next_frame_rows(rows, guarded + 1, buffer_size,
                                         flush_rows, &output, &delay) == ESP_OK);
        assert(gif_player_next_frame(full, expected, bytes, &full_delay) == ESP_OK);
        assert(delay == full_delay && output.next_row == height);
        assert(memcmp(canvas, expected, bytes) == 0);
        assert(guarded[0] == 0xA5 && guarded[buffer_size + 1] == 0xA5);
        assert(fwrite(canvas, 1, bytes, output.output) == bytes);
    }
    assert(gif_player_rewind(rows) == ESP_OK);
    output.fail = true;
    output.next_row = 0;
    assert(gif_player_next_frame_rows(rows, guarded + 1, buffer_size,
                                     flush_rows, &output, &delay) == ESP_ERR_INVALID_STATE);
    assert(gif_player_close(rows) == ESP_OK);
    assert(gif_player_close(full) == ESP_OK);
    fclose(output.output);
    free(canvas);
    free(expected);
    free(guarded);
    return 0;
}

