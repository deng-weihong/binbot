#ifndef BINBOT_AUDIO_PCM_CLOCK_H
#define BINBOT_AUDIO_PCM_CLOCK_H

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Read the DMA position and its timestamp together. Pairing snd_pcm_delay()
 * with a later userspace clock read turns scheduling stalls into AEC delay. */
static inline bool audio_pcm_clock_enable(snd_pcm_t *pcm)
{
    snd_pcm_sw_params_t *params;
    snd_pcm_sw_params_alloca(&params);
    return snd_pcm_sw_params_current(pcm, params) >= 0 &&
           snd_pcm_sw_params_set_tstamp_mode(pcm, params, SND_PCM_TSTAMP_ENABLE) >= 0 &&
           snd_pcm_sw_params_set_tstamp_type(pcm, params, SND_PCM_TSTAMP_TYPE_MONOTONIC) >= 0 &&
           snd_pcm_sw_params(pcm, params) >= 0;
}

static inline bool audio_pcm_clock_read(snd_pcm_t *pcm, uint64_t now_ns,
                                         uint64_t *position_ns,
                                         snd_pcm_sframes_t *delay)
{
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);
    if(snd_pcm_status(pcm, status) >= 0) {
        snd_htimestamp_t timestamp;
        snd_pcm_status_get_htstamp(status, &timestamp);
        uint64_t ns = (uint64_t)timestamp.tv_sec * 1000000000ULL + timestamp.tv_nsec;
        /* Reject disabled timestamps and an unsupported clock domain. */
        if(ns && ns <= now_ns + 100000000ULL && now_ns <= ns + 500000000ULL) {
            *position_ns = ns;
            *delay = snd_pcm_status_get_delay(status);
            return true;
        }
    }
    *position_ns = now_ns;
    return snd_pcm_delay(pcm, delay) >= 0;
}

#endif
