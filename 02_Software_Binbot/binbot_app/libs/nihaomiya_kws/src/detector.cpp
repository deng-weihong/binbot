#include "nihaomiya/detector.h"
#include "miya/engine.h"
#include <exception>
#include <stdexcept>

namespace nihaomiya {
class Detector::Impl {
public:
    std::unique_ptr<miya::Engine> engine;
    std::string error;
    uint64_t samples = 0;
    Impl(const std::string &model, const std::string &legacy_frontend, float level,
         const std::string &input_equalizer) {
        try {
            if(!legacy_frontend.empty())
                throw std::runtime_error("legacy BINFMAP1 frontend is unsupported; use reconstructed default DSP");
            engine.reset(new miya::Engine(model, level, input_equalizer));
        } catch(const std::exception &failure) { error = failure.what(); }
    }
};

Detector::Detector(const std::string &model, const std::string &legacy_frontend, float level,
                   const std::string &input_equalizer)
    : impl_(new Impl(model, legacy_frontend, level, input_equalizer)) {}
Detector::~Detector() = default;
bool Detector::valid() const { return static_cast<bool>(impl_->engine); }
const std::string &Detector::error() const { return impl_->error; }
void Detector::reset() {
    if(impl_->engine) impl_->engine->reset();
    impl_->samples = 0;
}
Detection Detector::accept(const int16_t *left, const int16_t *right,
                           int sample_count, const int16_t *reference) {
    Detection result = {};
    if(!valid()) return result;
    if(sample_count != kInputFrameSamples)
        throw std::runtime_error("Miya needs consecutive 320-sample raw stereo frames");
    const auto out = impl_->engine->step(left, right, reference);
    impl_->samples += sample_count;
    result.detected = out.detected;
    result.evaluated = out.evaluated;
    result.confidence = out.confidence;
    result.channel = out.channel;
    result.processed_samples = impl_->samples;
    return result;
}
} // namespace nihaomiya
