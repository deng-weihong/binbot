#include "binbot/audio/client.h"
#include "nihaomiya/detector.h"
#ifdef BINBOT_HAS_HIXIAOWEN
#include "hixiaowen/detector.h"
#endif
#ifdef BINBOT_HAS_SNOWBOY
#include "snowboy_kws/detector.h"
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>

#ifndef BINBOT_DEFAULT_WAKE_ENGINE
#define BINBOT_DEFAULT_WAKE_ENGINE "snowboy"
#endif

namespace {

volatile sig_atomic_t running = 1;
volatile sig_atomic_t diagnostic_requested = 0;

void stop_requested(int signal_number)
{
    (void)signal_number;
    running = 0;
}

void diagnostic_trigger(int signal_number)
{
    (void)signal_number;
    diagnostic_requested = 1;
}

uint64_t monotonic_ms()
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<uint64_t>(value.tv_sec) * 1000ULL +
           static_cast<uint64_t>(value.tv_nsec) / 1000000ULL;
}

double process_cpu_seconds()
{
    struct timespec value;
    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
    return value.tv_sec + value.tv_nsec / 1e9;
}

bool readable_file(const char *path)
{
    struct stat value;
    return path && stat(path, &value) == 0 && S_ISREG(value.st_mode) &&
           access(path, R_OK) == 0;
}

const char *argument_value(int argc, char **argv, const char *name, const char *fallback)
{
    for(int index = 1; index + 1 < argc; ++index) {
        if(strcmp(argv[index], name) == 0) return argv[index + 1];
    }
    return fallback;
}

double number_value(int argc, char **argv, const char *argument_name,
                    const char *environment_name, double fallback)
{
    const char *text = argument_value(argc, argv, argument_name, NULL);
    if(!text && environment_name) text = getenv(environment_name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    double value = strtod(text, &end);
    return end && *end == '\0' ? value : fallback;
}

bool send_wake_event(binbot_audio_client_t *client,
                     uint64_t sequence,
                     uint64_t timestamp_ns,
                     int hotword_index)
{
    binbot_audio_packet_t event;
    binbot_audio_packet_init(&event, BINBOT_AUDIO_MSG_WAKE_DETECTED,
                             sizeof(event.payload.wake), sequence, timestamp_ns);
    event.payload.wake.hotword_index = hotword_index > 0 ? hotword_index : 1;
    return binbot_audio_client_send(client, &event);
}

}  // namespace

int main(int argc, char **argv)
{
    signal(SIGINT, stop_requested);
    signal(SIGTERM, stop_requested);
    signal(SIGUSR1, diagnostic_trigger);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Keep the daemon safe when it is launched outside the init script. */
    const char *wake_enable = getenv("BINBOT_WAKE_ENABLE");
    if(wake_enable && strcmp(wake_enable, "0") == 0) {
        fprintf(stdout, "wake: disabled by configuration\n");
        return EXIT_SUCCESS;
    }

    const char *socket_path = argument_value(
        argc, argv, "--socket", getenv("BINBOT_AUDIO_SOCKET"));
    const char *miya_model_default = getenv("BINBOT_MIYA_MODEL");
    if(!miya_model_default || !miya_model_default[0])
        miya_model_default = "/oem/assets/wake/nihaomiya.model.txt";
    const char *model_path = argument_value(
        argc, argv, "--model", miya_model_default);
    const char *frontend_default = getenv("BINBOT_WAKE_FRONTEND");
    if(!frontend_default || !frontend_default[0]) {
        frontend_default = "";
    }
    const char *frontend_path = argument_value(
        argc, argv, "--frontend", frontend_default);
    const char *engine_default = getenv("BINBOT_WAKE_ENGINE");
    if(!engine_default || !engine_default[0]) engine_default = BINBOT_DEFAULT_WAKE_ENGINE;
    const char *engine = argument_value(argc, argv, "--engine", engine_default);
    const double miya_level = number_value(
        argc, argv, "--miya-level", "BINBOT_MIYA_LEVEL", .81);
    const char *miya_eq_default = getenv("BINBOT_MIYA_INPUT_EQ");
    const char *miya_input_eq = argument_value(argc, argv, "--miya-input-eq",
                                             miya_eq_default ? miya_eq_default : "");
    const char *hixiaowen_model_default = getenv("BINBOT_HIXIAOWEN_MODEL_DIR");
    if(!hixiaowen_model_default || !hixiaowen_model_default[0]) {
        hixiaowen_model_default = "/oem/assets/wake/hixiaowen";
    }
    const char *hixiaowen_model = argument_value(
        argc, argv, "--hixiaowen-model-dir", hixiaowen_model_default);
    const double hixiaowen_threshold = number_value(
        argc, argv, "--hixiaowen-threshold", "BINBOT_HIXIAOWEN_THRESHOLD", 0.9);
    const double hixiaowen_interval_ms = number_value(
        argc, argv, "--hixiaowen-interval-ms", "BINBOT_HIXIAOWEN_INTERVAL_MS", 400.0);
    const char *hixiaowen_channel_default = getenv("BINBOT_HIXIAOWEN_CHANNEL");
    if(!hixiaowen_channel_default || !hixiaowen_channel_default[0]) {
        hixiaowen_channel_default = "mix";
    }
    const char *hixiaowen_channel = argument_value(
        argc, argv, "--hixiaowen-channel", hixiaowen_channel_default);
    const char *snowboy_resource_default = getenv("BINBOT_SNOWBOY_RESOURCE");
    if(!snowboy_resource_default || !snowboy_resource_default[0]) {
        snowboy_resource_default = "/oem/assets/wake/snowboy/common.res";
    }
    const char *snowboy_resource = argument_value(
        argc, argv, "--snowboy-resource", snowboy_resource_default);
    const char *snowboy_model_default = getenv("BINBOT_SNOWBOY_MODEL");
    if(!snowboy_model_default || !snowboy_model_default[0]) {
        snowboy_model_default = "/oem/assets/wake/snowboy/aa.pmdl";
    }
    const char *snowboy_model = argument_value(
        argc, argv, "--snowboy-model", snowboy_model_default);
    const char *snowboy_sensitivity_default = getenv("BINBOT_SNOWBOY_SENSITIVITY");
    if(!snowboy_sensitivity_default || !snowboy_sensitivity_default[0]) {
        snowboy_sensitivity_default = "0.4";
    }
    const char *snowboy_sensitivity = argument_value(
        argc, argv, "--snowboy-sensitivity", snowboy_sensitivity_default);
    double snowboy_gain = number_value(
        argc, argv, "--snowboy-gain", "BINBOT_SNOWBOY_GAIN", 1.0);
    const char *snowboy_frontend = getenv("BINBOT_SNOWBOY_FRONTEND");
    bool snowboy_apply_frontend = snowboy_frontend &&
                                  strcmp(snowboy_frontend, "0") != 0;
    double minimum_confidence = number_value(
        argc, argv, "--min-confidence", "BINBOT_WAKE_MIN_CONFIDENCE",
        strcmp(engine, "miya") == 0 ? 0.0 : 0.75);
    double cooldown_ms_value = number_value(
        argc, argv, "--cooldown-ms", "BINBOT_WAKE_COOLDOWN_MS", 3000.0);
    if(!std::isfinite(miya_level) || miya_level < 0.0 || miya_level > 1.0 ||
       !std::isfinite(minimum_confidence) || !std::isfinite(cooldown_ms_value) ||
       !std::isfinite(snowboy_gain) ||
       minimum_confidence < 0.0 || minimum_confidence > 1.0 ||
       cooldown_ms_value < 0.0 || cooldown_ms_value > 60000.0 ||
       snowboy_gain <= 0.0 || snowboy_gain > 20.0 ||
       !std::isfinite(hixiaowen_interval_ms) ||
       hixiaowen_interval_ms < 200.0 || hixiaowen_interval_ms > 2000.0) {
        fprintf(stderr, "wake: invalid confidence or cooldown configuration\n");
        return EXIT_FAILURE;
    }
    const bool use_snowboy = strcmp(engine, "snowboy") == 0;
    const bool use_miya = strcmp(engine, "miya") == 0;
    const bool use_hixiaowen = strcmp(engine, "hixiaowen") == 0;
    if(!use_snowboy && !use_miya && !use_hixiaowen) {
        fprintf(stderr, "wake: unknown engine '%s' (expected snowboy, miya or hixiaowen)\n", engine);
        return EXIT_FAILURE;
    }
    if(use_hixiaowen &&
       (!std::isfinite(hixiaowen_threshold) ||
        (hixiaowen_threshold != -1.0 &&
         (hixiaowen_threshold < 0.0 || hixiaowen_threshold > 1.0)) ||
        (strcmp(hixiaowen_channel, "mix") && strcmp(hixiaowen_channel, "left") &&
         strcmp(hixiaowen_channel, "right")))) {
        fprintf(stderr, "wake: invalid Hi Xiaowen threshold or channel\n");
        return EXIT_FAILURE;
    }
#ifndef BINBOT_HAS_HIXIAOWEN
    if(use_hixiaowen) {
        fprintf(stderr, "wake: this binary was built without Hi Xiaowen support\n");
        return EXIT_FAILURE;
    }
#endif
#ifndef BINBOT_HAS_SNOWBOY
    if(use_snowboy) {
        fprintf(stderr, "wake: this binary was built without Snowboy support\n");
        return EXIT_FAILURE;
    }
#endif
    if(use_miya && !readable_file(model_path)) {
        fprintf(stderr, "wake: Miya model is not readable: %s\n", model_path);
        return EXIT_FAILURE;
    }
#ifdef BINBOT_HAS_SNOWBOY
    if(use_snowboy && (!readable_file(snowboy_resource) ||
                       !readable_file(snowboy_model))) {
        fprintf(stderr, "wake: Snowboy resource/model is not readable: resource=%s model=%s\n",
                snowboy_resource, snowboy_model);
        return EXIT_FAILURE;
    }
#endif

    try {
        std::unique_ptr<nihaomiya::Detector> miya_detector;
#ifdef BINBOT_HAS_HIXIAOWEN
        std::unique_ptr<hixiaowen::Detector> hixiaowen_detector;
#endif
#ifdef BINBOT_HAS_SNOWBOY
        std::unique_ptr<snowboy_kws::Detector> snowboy_detector;
#endif
        if(use_snowboy) {
#ifdef BINBOT_HAS_SNOWBOY
            snowboy_detector.reset(new snowboy_kws::Detector(
                snowboy_resource, snowboy_model, snowboy_sensitivity,
                snowboy_apply_frontend, static_cast<float>(snowboy_gain)));
            if(!snowboy_detector->valid()) {
                fprintf(stderr, "wake: cannot load Snowboy model: %s\n",
                        snowboy_detector->error());
                return EXIT_FAILURE;
            }
#endif
        } else if(use_miya) {
            miya_detector.reset(new nihaomiya::Detector(model_path, frontend_path,
                                                        static_cast<float>(miya_level), miya_input_eq));
            if(!miya_detector->valid()) {
                fprintf(stderr, "wake: cannot load Miya model: %s\n",
                        miya_detector->error().c_str());
                return EXIT_FAILURE;
            }
        } else {
#ifdef BINBOT_HAS_HIXIAOWEN
        const unsigned hixiaowen_stride_frames = std::max(
            static_cast<unsigned>(hixiaowen::kStride),
            static_cast<unsigned>(std::lround(hixiaowen_interval_ms / 40.0)));
            hixiaowen_detector.reset(new hixiaowen::Detector(
                hixiaowen_model, static_cast<float>(hixiaowen_threshold),
                hixiaowen_stride_frames));
#endif
        }
        auto reset_detector = [&]() {
            if(use_miya) miya_detector->reset();
#ifdef BINBOT_HAS_SNOWBOY
            if(use_snowboy) snowboy_detector->reset();
#endif
#ifdef BINBOT_HAS_HIXIAOWEN
            if(use_hixiaowen) hixiaowen_detector->reset();
#endif
        };

        binbot_audio_client_t client;
        binbot_audio_client_init(&client, BINBOT_AUDIO_ROLE_WAKE,
                                 use_miya ? BINBOT_AUDIO_CAP_PCM_FRONTEND :
                                            BINBOT_AUDIO_CAP_PCM_STEREO_LIVE,
                                 0, socket_path);
        std::array<int16_t, BINBOT_AUDIO_FRAME_SAMPLES> left;
        std::array<int16_t, BINBOT_AUDIO_FRAME_SAMPLES> right;
        std::array<int16_t, BINBOT_AUDIO_FRAME_SAMPLES> mono;
        std::array<int16_t, BINBOT_AUDIO_FRAME_SAMPLES> reference;
        bool was_connected = false;
        bool have_sequence = false;
        uint64_t last_sequence = 0;
        uint64_t last_timestamp_ns = 0;
        uint64_t last_detection_ms = 0;
        const uint64_t cooldown_ms = static_cast<uint64_t>(cooldown_ms_value);
        uint64_t stats_started = monotonic_ms();
        double stats_cpu_started = process_cpu_seconds();
        unsigned long long blocks = 0, windows = 0, gaps = 0, events = 0;
        float maximum_score = 0;
        uint64_t maximum_step_ms = 0;

        if(use_hixiaowen) {
#ifdef BINBOT_HAS_HIXIAOWEN
            fprintf(stdout,
                    "wake: engine=hixiaowen backend=ncnn-cpu precision=fp32 threads=1 "
                    "model=%s threshold=%.8f channel=%s window_ms=2000 stride_ms=%u "
                    "cooldown_ms=%llu\n",
                    hixiaowen_model, hixiaowen_detector->threshold(), hixiaowen_channel,
                    hixiaowen_detector->evaluation_stride_frames() * 40U,
                    static_cast<unsigned long long>(cooldown_ms));
#endif
        } else if(use_miya) {
            fprintf(stdout,
                    "wake: engine=miya backend=reconstructed-default-021 precision=fp32 device=cpu "
                    "input=raw-mic0-mic1-render model=%s hmm_level=%.4f secondary_level=0.01 "
                    "min_confidence=%.4f input_eq=%s cooldown_ms=%llu\n",
                    model_path, miya_level, minimum_confidence,
                    miya_input_eq[0] ? miya_input_eq : "none",
                    static_cast<unsigned long long>(cooldown_ms));
        } else fprintf(stdout,
                "wake: engine=%s model=%s resource=%s sensitivity=%s "
                "min_confidence=%.2f cooldown_ms=%llu\n",
                engine, use_snowboy ? snowboy_model : model_path,
                use_snowboy ? snowboy_resource : frontend_path,
                use_snowboy ? snowboy_sensitivity : "n/a", minimum_confidence,
                static_cast<unsigned long long>(cooldown_ms));
        while(running) {
            uint64_t now = monotonic_ms();
            bool connected = binbot_audio_client_connect(&client, now);
            if(!connected) {
                was_connected = false;
                usleep(100000);
                continue;
            }
            if(!was_connected) {
                reset_detector();
                have_sequence = false;
                was_connected = true;
                fprintf(stdout, "wake: connected to LR audio service\n");
            }
            if((use_hixiaowen || use_miya) && now - stats_started >= 30000) {
                const double cpu = process_cpu_seconds();
                fprintf(stdout,
                        "wake: stats blocks=%llu windows=%llu gaps=%llu events=%llu "
                        "cpu_percent=%.1f max_score=%.5f max_step_ms=%llu\n",
                        blocks, windows, gaps, events,
                        (cpu - stats_cpu_started) * 100000.0 / (now - stats_started),
                        maximum_score, static_cast<unsigned long long>(maximum_step_ms));
                stats_started = now;
                stats_cpu_started = cpu;
                blocks = windows = gaps = events = 0;
                maximum_score = 0;
                maximum_step_ms = 0;
            }
            if(diagnostic_requested) {
                diagnostic_requested = 0;
                if(send_wake_event(&client, last_sequence, last_timestamp_ns, 1)) {
                    fprintf(stdout, "wake: diagnostic trigger sent\n");
                }
            }

            struct pollfd descriptor = {
                binbot_audio_client_fd(&client),
                static_cast<short>(POLLIN | POLLERR | POLLHUP),
                0,
            };
            int poll_result = poll(&descriptor, 1, 500);
            if(poll_result < 0) {
                if(errno == EINTR) continue;
                binbot_audio_client_close(&client);
                was_connected = false;
                continue;
            }
            if(poll_result == 0) continue;
            if((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                binbot_audio_client_close(&client);
                was_connected = false;
                continue;
            }

            while(running) {
                binbot_audio_packet_t packet;
                int received = binbot_audio_client_receive(&client, &packet);
                if(received <= 0) {
                    if(received < 0) was_connected = false;
                    break;
                }
                if(use_miya ?
                   (packet.header.type != BINBOT_AUDIO_MSG_PCM_FRONTEND ||
                    packet.header.payload_size != sizeof(packet.payload.pcm_frontend)) :
                   (packet.header.type != BINBOT_AUDIO_MSG_PCM_STEREO ||
                    packet.header.payload_size != sizeof(packet.payload.pcm_stereo))) {
                    continue;
                }
                if(have_sequence && packet.header.sequence != last_sequence + 1U) {
                    fprintf(stdout,
                            "wake: audio sequence gap, resetting detector (%llu -> %llu)\n",
                            static_cast<unsigned long long>(last_sequence),
                            static_cast<unsigned long long>(packet.header.sequence));
                    ++gaps;
                    reset_detector();
                }
                last_sequence = packet.header.sequence;
                last_timestamp_ns = packet.header.timestamp_ns;
                have_sequence = true;
                ++blocks;

                const int16_t *stereo = use_miya ? packet.payload.pcm_frontend.samples :
                                                  packet.payload.pcm_stereo.samples;
                for(int index = 0; index < BINBOT_AUDIO_FRAME_SAMPLES; ++index) {
                    const size_t channels = use_miya ? 3 : 2;
                    left[index] = stereo[index * channels];
                    right[index] = stereo[index * channels + 1];
                    reference[index] = use_miya ? stereo[index * channels + 2] : 0;
                }
                bool detected = false;
                double confidence = 0.0;
                int hotword_index = 1;
                if(use_snowboy || use_hixiaowen) {
                    for(int index = 0; index < BINBOT_AUDIO_FRAME_SAMPLES; ++index) {
                        int32_t mixed = (static_cast<int32_t>(left[index]) + right[index]) / 2;
                        mono[index] = use_hixiaowen && !strcmp(hixiaowen_channel, "left") ? left[index] :
                            use_hixiaowen && !strcmp(hixiaowen_channel, "right") ? right[index] :
                            static_cast<int16_t>(mixed);
                    }
                }
                if(use_snowboy) {
#ifdef BINBOT_HAS_SNOWBOY
                    snowboy_kws::Detection result = snowboy_detector->accept(
                        mono.data(), mono.size());
                    detected = result.detected;
                    hotword_index = result.result > 0 ? result.result : 1;
#endif
                } else if(use_miya) {
                    const uint64_t begin = monotonic_ms();
                    nihaomiya::Detection result = miya_detector->accept(
                        left.data(), right.data(), left.size(), reference.data());
                    const uint64_t elapsed = monotonic_ms() - begin;
                    if(elapsed > maximum_step_ms) maximum_step_ms = elapsed;
                    if(result.evaluated) {
                        ++windows;
                        if(result.confidence > maximum_score) maximum_score = result.confidence;
                    }
                    detected = result.detected;
                    confidence = result.confidence;
                } else {
#ifdef BINBOT_HAS_HIXIAOWEN
                    const uint64_t begin = monotonic_ms();
                    hixiaowen::Detection result = hixiaowen_detector->accept(mono.data(), mono.size());
                    const uint64_t elapsed = monotonic_ms() - begin;
                    if(elapsed > maximum_step_ms) maximum_step_ms = elapsed;
                    if(result.evaluated) {
                        ++windows;
                        if(result.confidence > maximum_score) maximum_score = result.confidence;
                    }
                    detected = result.detected;
                    confidence = result.confidence;
#endif
                }
                if(!detected || (use_miya && confidence < minimum_confidence)) continue;

                now = monotonic_ms();
                /* Other engines need a reset before checking cooldown so a
                 * retained match cannot fire again at the end of the interval.
                 * Miya's HMM owns suppression and preserves its adaptive DSP
                 * and decoder histories across completed keywords. */
                if(!use_miya) reset_detector();
                if(last_detection_ms != 0 && now - last_detection_ms < cooldown_ms) continue;

                const bool sent = send_wake_event(&client, packet.header.sequence,
                                                  packet.header.timestamp_ns,
                                                  hotword_index);
                if(sent) {
                    ++events;
                    last_detection_ms = now;
                    fprintf(stdout,
                            "wake: keyword event engine=%s confidence=%.4f hotword=%d sequence=%llu\n",
                            engine, confidence, hotword_index,
                            static_cast<unsigned long long>(packet.header.sequence));
                }
            }
        }
        binbot_audio_client_close(&client);
    }
    catch(const std::exception &error) {
        fprintf(stderr, "wake: runtime failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    catch(...) {
        fprintf(stderr, "wake: runtime failed with an unknown error\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "wake: stopped cleanly\n");
    return EXIT_SUCCESS;
}
