#include "hixiaowen/model.h"
#include <cJSON.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace hixiaowen {
namespace {

bool number_equals(const cJSON *metadata, const char *key, int expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(metadata, key);
    return cJSON_IsNumber(value) && value->valuedouble == expected;
}

bool string_equals(const cJSON *metadata, const char *key, const char *expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(metadata, key);
    return cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
}

}  // namespace

Model::Model(const std::string &directory)
{
    std::ifstream file((directory + "/model.json").c_str(), std::ios::binary);
    if(!file) throw std::runtime_error("cannot read KWS model.json");
    const std::string text((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> metadata(
        cJSON_Parse(text.c_str()), cJSON_Delete);
    if(!metadata) throw std::runtime_error("invalid KWS model.json");
    const cJSON *threshold = cJSON_GetObjectItemCaseSensitive(metadata.get(), "threshold");
    if(!string_equals(metadata.get(), "format", "binbot-kws-ncnn-metadata-v1") ||
       !string_equals(metadata.get(), "precision", "fp32") ||
       !string_equals(metadata.get(), "ncnn_version", "20250503") ||
       !string_equals(metadata.get(), "feature_contract_sha256",
           "9a4db13bdde1f13896070d6f04796a757f0d262153d6b9d79876135d546d3f6d") ||
       !number_equals(metadata.get(), "sample_rate_hz", AUDIO_FBANK_SAMPLE_RATE) ||
       !number_equals(metadata.get(), "window_frames", kFrames) ||
       !number_equals(metadata.get(), "feature_bins", kBins) ||
       !number_equals(metadata.get(), "stride_frames", kStride) ||
       !cJSON_IsNumber(threshold) || !std::isfinite(threshold->valuedouble) ||
       threshold->valuedouble < 0 || threshold->valuedouble > 1) {
        throw std::runtime_error("unsupported KWS metadata or feature contract");
    }
    threshold_ = static_cast<float>(threshold->valuedouble);
    const cJSON *means = cJSON_GetObjectItemCaseSensitive(metadata.get(), "mean");
    const cJSON *scales = cJSON_GetObjectItemCaseSensitive(metadata.get(), "std");
    if(!cJSON_IsArray(means) || !cJSON_IsArray(scales) ||
       cJSON_GetArraySize(means) != kBins || cJSON_GetArraySize(scales) != kBins) {
        throw std::runtime_error("invalid KWS CMVN dimensions");
    }
    for(int i = 0; i < kBins; ++i) {
        const cJSON *mean = cJSON_GetArrayItem(means, i);
        const cJSON *scale = cJSON_GetArrayItem(scales, i);
        if(!cJSON_IsNumber(mean) || !cJSON_IsNumber(scale) ||
           !std::isfinite(mean->valuedouble) || !std::isfinite(scale->valuedouble)) {
            throw std::runtime_error("invalid KWS CMVN");
        }
        mean_[i] = static_cast<float>(mean->valuedouble);
        scale_[i] = static_cast<float>(scale->valuedouble);
        if(!std::isfinite(mean_[i]) || !std::isfinite(scale_[i]) || scale_[i] <= 0) {
            throw std::runtime_error("invalid KWS CMVN range");
        }
    }
    network_.opt.num_threads = 1;
    network_.opt.use_vulkan_compute = false;
    network_.opt.use_fp16_packed = false;
    network_.opt.use_fp16_storage = false;
    network_.opt.use_fp16_arithmetic = false;
    network_.opt.use_bf16_storage = false;
    network_.opt.use_int8_inference = false;
    if(network_.load_param((directory + "/model.param").c_str()) != 0 ||
       network_.load_model((directory + "/model.bin").c_str()) != 0) {
        throw std::runtime_error("cannot load ncnn KWS model");
    }
}

std::array<float, 2> Model::infer(const Window &features)
{
    ncnn::Mat input(kBins, kFrames, 1);
    if(input.empty()) throw std::runtime_error("cannot allocate KWS input");
    memcpy(input.data, features.data(), features.size() * sizeof(float));
    ncnn::Extractor extractor = network_.create_extractor();
    ncnn::Mat output;
    if(extractor.input("features", input) != 0 ||
       extractor.extract("logits", output) != 0 || output.total() != 2) {
        throw std::runtime_error("ncnn KWS inference failed");
    }
    std::array<float, 2> result = {{output[0], output[1]}};
    if(!std::isfinite(result[0]) || !std::isfinite(result[1])) {
        throw std::runtime_error("non-finite KWS model output");
    }
    return result;
}

float keyword_score(const std::array<float, 2> &logits)
{
    const float difference = logits[0] - logits[1];
    if(difference >= 0) {
        const float value = std::exp(-difference);
        return value / (1.0f + value);
    }
    return 1.0f / (1.0f + std::exp(difference));
}

}  // namespace hixiaowen
