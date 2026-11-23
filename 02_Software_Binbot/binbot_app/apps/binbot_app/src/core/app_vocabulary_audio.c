#include "binbot/app_vocabulary_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool playable(const char *path)
{
    struct stat status;
    if(stat(path, &status) || !S_ISREG(status.st_mode) || status.st_size < 4 ||
       status.st_size > 512 * 1024) return false;
    FILE *file = fopen(path, "rb");
    if(!file) return false;
    unsigned char header[4];
    bool result = fread(header, 1, sizeof(header), file) == sizeof(header) &&
        ((header[0] == 0xff && (header[1] & 0xe0) == 0xe0) || !memcmp(header, "ID3", 3));
    fclose(file);
    return result;
}

bool app_vocabulary_audio_path(const char *word, app_vocabulary_accent_t accent,
                                char *path, size_t capacity)
{
    if(!path || !capacity) return false;
    path[0] = '\0';
    if(!word || !word[0] || strlen(word) > 64 ||
       (accent != APP_VOCABULARY_ACCENT_US && accent != APP_VOCABULARY_ACCENT_UK)) return false;
    char lower[65];
    size_t length = strlen(word);
    for(size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)word[i];
        if(!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '\''))
            return false;
        lower[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 'a' - 'A') : (char)ch;
    }
    lower[length] = '\0';
    const char *root = getenv("BINBOT_VOCABULARY_AUDIO_ROOT");
    char default_root[256];
    if(!root || !root[0]) {
        const char *assets = getenv("BINBOT_ASSET_ROOT");
        if(!assets || !assets[0]) assets = "/oem/assets";
        if(assets[0] == 'A' && assets[1] == ':') assets += 2;
        int size = snprintf(default_root, sizeof(default_root), "%s/vocabulary-audio", assets);
        if(size < 0 || (size_t)size >= sizeof(default_root)) return false;
        root = default_root;
    }
    const char *locale = accent == APP_VOCABULARY_ACCENT_US ? "us" : "uk";
    for(unsigned attempt = 0; attempt < 2; ++attempt) {
        int size = snprintf(path, capacity, "%s/%s/%s.mp3", root, locale, attempt ? lower : word);
        if(size < 0 || (size_t)size >= capacity) { path[0] = '\0'; return false; }
        if(playable(path)) return true;
        if(!strcmp(lower, word)) break;
    }
    path[0] = '\0';
    return false;
}
