#define _DEFAULT_SOURCE
#include "noise.h"
#include "playback.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void synthesis(void)
{
    double powers[9];
    for(unsigned i = 0; i < 9; ++i) {
        audio_noise_t noise; audio_noise_init(&noise);
        binbot_audio_noise_state_t state = {.master = 100, .playing = 1};
        state.levels[i] = 100;
        double power = 0;
        int16_t samples[480];
        for(unsigned block = 0; block < 150; ++block) {
            audio_noise_render(&noise, &state, samples, 480);
            if(block > 50) for(unsigned j = 0; j < 480; ++j)
                power += (double)samples[j] * samples[j];
        }
        powers[i] = power;
        assert(isfinite(power) && power > 10000000);
        state.master = 0;
        for(unsigned block = 0; block < 150; ++block) audio_noise_render(&noise, &state, samples, 480);
        for(unsigned j = 0; j < 480; ++j) assert(samples[j] == 0);
    }
    for(unsigned i = 0; i < 9; ++i) for(unsigned j = 0; j < i; ++j)
        assert(fabs(powers[i] - powers[j]) > 1000);
}

int main(void)
{
    synthesis();
    audio_playback_t playback;
    assert(audio_playback_start(&playback, "null", "/nonexistent.wav", 0, 100, 1, NULL));
    binbot_audio_noise_state_t state = {.master = 70, .playing = 1, .timer_seconds = 1};
    state.levels[0] = 55; state.levels[4] = 35;
    state.master = 101; assert(!audio_playback_noise_control(&playback, &state)); state.master = 70;
    state.levels[0] = 101; assert(!audio_playback_noise_control(&playback, &state)); state.levels[0] = 55;
    assert(audio_playback_noise_control(&playback, &state));
    binbot_audio_music_control_t word_stop = {.command = BINBOT_AUDIO_MUSIC_STOP,
        .reserved = BINBOT_AUDIO_MEDIA_PRONUNCIATION};
    assert(audio_playback_music_control(&playback, &word_stop));
    binbot_audio_noise_state_t unaffected;
    audio_playback_noise_snapshot(&playback, &unaffected); assert(unaffected.playing);
    usleep(250000);
    state.playing = 0; assert(audio_playback_noise_control(&playback, &state));
    binbot_audio_noise_state_t snapshot;
    audio_playback_noise_snapshot(&playback, &snapshot);
    assert(!snapshot.playing && snapshot.remaining_seconds == 1);
    usleep(1100000);
    audio_playback_noise_snapshot(&playback, &snapshot);
    assert(!snapshot.playing && snapshot.remaining_seconds == 1);
    state.playing = 1; assert(audio_playback_noise_control(&playback, &state));
    usleep(1000000);
    audio_playback_noise_snapshot(&playback, &snapshot);
    assert(!snapshot.playing && !snapshot.remaining_seconds && !snapshot.timer_seconds);
    state.timer_seconds = 0;
    assert(audio_playback_noise_control(&playback, &state));
    usleep(100000);
    binbot_audio_playback_begin_t begin = {.stream_id = {1}, .channels = 1,
        .sample_rate = BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, .frame_ms = BINBOT_AUDIO_FRAME_MS};
    assert(audio_playback_begin(&playback, &begin));
    binbot_audio_music_control_t word_play = {.command = BINBOT_AUDIO_MUSIC_PLAY,
        .reserved = BINBOT_AUDIO_MEDIA_PRONUNCIATION, .path = "/not-used.mp3"};
    assert(!audio_playback_music_control(&playback, &word_play));
    assert(audio_playback_dialog_active(&playback));
    audio_playback_noise_snapshot(&playback, &snapshot); assert(!snapshot.playing);
    assert(audio_playback_noise_control(&playback, &state));
    assert(!audio_playback_dialog_active(&playback));
    usleep(100000);
    audio_playback_request(&playback);
    audio_playback_noise_snapshot(&playback, &snapshot); assert(!snapshot.playing);
    memset(state.levels, 0, sizeof(state.levels));
    assert(audio_playback_noise_control(&playback, &state));
    audio_playback_noise_snapshot(&playback, &snapshot); assert(!snapshot.playing);
    audio_playback_stop(&playback);
    assert(audio_playback_start(&playback, "binbot-missing-device", "/nonexistent.wav", 0, 100, 1, NULL));
    state.levels[0] = 55; assert(audio_playback_noise_control(&playback, &state));
    usleep(200000);
    audio_playback_noise_snapshot(&playback, &snapshot); assert(!snapshot.playing);
    audio_playback_stop(&playback);
    puts("white noise synthesis, timer, pause, preemption, validation and playback failure verified");
    return 0;
}
