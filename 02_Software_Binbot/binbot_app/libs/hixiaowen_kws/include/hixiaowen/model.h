#ifndef BINBOT_HIXIAOWEN_MODEL_H
#define BINBOT_HIXIAOWEN_MODEL_H

#include "audio/fbank.h"
#include "net.h"

#include <array>
#include <string>

namespace hixiaowen {

enum { kFrames = 50, kBins = AUDIO_FBANK_FEATURE_BINS, kStride = 5 };
typedef std::array<float, kFrames * kBins> Window;

class Model {
public:
    explicit Model(const std::string &directory);
    std::array<float, 2> infer(const Window &normalized_features);
    const std::array<float, kBins> &mean() const { return mean_; }
    const std::array<float, kBins> &scale() const { return scale_; }
    float threshold() const { return threshold_; }

private:
    ncnn::Net network_;
    std::array<float, kBins> mean_, scale_;
    float threshold_;
};

float keyword_score(const std::array<float, 2> &logits);

}  // namespace hixiaowen
#endif
