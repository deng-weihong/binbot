#ifndef NIHAOMIYA_DETECTOR_H
#define NIHAOMIYA_DETECTOR_H

#include <stdint.h>

#include <memory>
#include <string>

namespace nihaomiya {

enum {
    kSampleRate = 16000,
    kInputFrameSamples = 320,
};

struct Detection {
    bool detected;
    bool evaluated;
    float confidence;
    int channel;
    uint64_t processed_samples;
};

class Detector {
public:
    explicit Detector(const std::string &model_path,
                      const std::string &legacy_frontend_path = std::string(),
                      float hmm_level = .81f,
                      const std::string &input_equalizer_path = std::string());
    ~Detector();

    Detector(const Detector &) = delete;
    Detector &operator=(const Detector &) = delete;

    bool valid() const;
    const std::string &error() const;
    void reset();

    Detection accept(const int16_t *left, const int16_t *right,
                     int sample_count = kInputFrameSamples,
                     const int16_t *reference = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nihaomiya

#endif
