#ifndef BINBOT_SNOWBOY_KWS_DETECTOR_H
#define BINBOT_SNOWBOY_KWS_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

namespace snowboy_kws {

struct Detection {
    bool detected;
    int result;
};

/*
 * Thin BinBot adapter around SnowboyDetect.  It only accepts PCM supplied by
 * binbot_audio; it never opens an audio device or owns an audio thread.
 */
class Detector {
public:
    Detector(const char *resource_path,
             const char *model_path,
             const char *sensitivity,
             bool apply_frontend,
             float audio_gain);
    ~Detector();

    bool valid() const;
    const char *error() const;
    void reset();
    Detection accept(const int16_t *samples, size_t sample_count);

    int sample_rate() const;
    int channels() const;
    int bits_per_sample() const;

private:
    void *detector_;
    bool valid_;
    char error_[192];
};

}  // namespace snowboy_kws

#endif
