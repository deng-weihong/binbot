#ifndef BINBOT_APP_VOCABULARY_AUDIO_H
#define BINBOT_APP_VOCABULARY_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_VOCABULARY_ACCENT_US = 0,
    APP_VOCABULARY_ACCENT_UK = 1,
} app_vocabulary_accent_t;

/* Uses exact spelling first, then lowercase. Never accepts path components.
 * Files are local MP3s; no large dictionary/index is loaded into App memory. */
bool app_vocabulary_audio_path(const char *word, app_vocabulary_accent_t accent,
                                char *path, size_t capacity);

#endif
