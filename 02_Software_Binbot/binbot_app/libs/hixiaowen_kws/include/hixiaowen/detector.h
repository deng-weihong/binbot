#ifndef BINBOT_HIXIAOWEN_DETECTOR_H
#define BINBOT_HIXIAOWEN_DETECTOR_H

#include "hixiaowen/model.h"
#include <memory>

namespace hixiaowen {

struct Detection {
    bool evaluated;
    bool detected;
    float confidence;
};

class Detector {
public:
    explicit Detector(const std::string &model_directory, float threshold = -1.0f,
                      unsigned evaluation_stride_frames = kStride);
    void reset();
    Detection accept(const int16_t *mono, size_t sample_count);
    float threshold() const { return threshold_; }
    unsigned evaluation_stride_frames() const { return evaluation_stride_frames_; }

private:
    Model model_;
    std::unique_ptr<audio_fbank_t, decltype(&audio_fbank_destroy)> fbank_;
    Window ring_;
    uint64_t feature_frames_;
    float threshold_;
    unsigned evaluation_stride_frames_;
};

}  // namespace hixiaowen
#endif
