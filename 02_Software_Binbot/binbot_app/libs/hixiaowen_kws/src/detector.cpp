#include "hixiaowen/detector.h"
#include <cmath>
#include <stdexcept>

namespace hixiaowen {

Detector::Detector(const std::string &directory, float threshold,
                   unsigned evaluation_stride_frames)
    : model_(directory), fbank_(nullptr, audio_fbank_destroy), feature_frames_(0),
      threshold_(threshold == -1.0f ? model_.threshold() : threshold),
      evaluation_stride_frames_(evaluation_stride_frames >= kStride
                                    ? evaluation_stride_frames
                                    : static_cast<unsigned>(kStride))
{
    if(!std::isfinite(threshold_) || threshold_ < 0 || threshold_ > 1) {
        throw std::runtime_error("invalid Hi Xiaowen threshold");
    }
    audio_fbank_config_t config = audio_fbank_default_config();
    fbank_.reset(audio_fbank_create(&config));
    if(!fbank_) throw std::runtime_error("cannot create KWS C Fbank");
    reset();
    Window warmup = {};
    model_.infer(warmup);
}

void Detector::reset()
{
    audio_fbank_reset(fbank_.get());
    feature_frames_ = 0;
    ring_.fill(0);
}

Detection Detector::accept(const int16_t *mono, size_t sample_count)
{
    if(!mono || sample_count != AUDIO_FBANK_INPUT_FRAME_SAMPLES) {
        throw std::runtime_error("KWS expects exactly 320 mono samples per block");
    }
    Detection result = {false, false, 0.0f};
    if(!audio_fbank_process(fbank_.get(), mono, sample_count,
        &ring_[(feature_frames_ % kFrames) * kBins], kBins)) return result;
    ++feature_frames_;
    if(feature_frames_ < kFrames ||
       (feature_frames_ - kFrames) % evaluation_stride_frames_ != 0) {
        return result;
    }
    Window window;
    const uint64_t first = feature_frames_ - kFrames;
    for(int frame = 0; frame < kFrames; ++frame) {
        for(int bin = 0; bin < kBins; ++bin) {
            window[frame * kBins + bin] =
                (ring_[((first + frame) % kFrames) * kBins + bin] - model_.mean()[bin]) /
                model_.scale()[bin];
        }
    }
    result.evaluated = true;
    result.confidence = keyword_score(model_.infer(window));
    result.detected = result.confidence >= threshold_;
    return result;
}

}  // namespace hixiaowen
