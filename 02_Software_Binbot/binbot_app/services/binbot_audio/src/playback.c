#include "playback.h"
#include "noise.h"
#include "pcm_clock.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef BINBOT_HAVE_RKAUDIO_MP3
#include "mp3dec.h"
#endif

enum {
    MUSIC_MAX_FILE_BYTES = 64 * 1024 * 1024,
    MUSIC_MAX_OUTPUT_SAMPLES = 1152 * 2,
};

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
    long data_offset;
} wave_format_t;

typedef struct {
    audio_echo_render_t render;
    int16_t pending[16384];
    size_t pending_frames;
} playback_reference_t;

static void playback_reference_close(playback_reference_t *reference)
{
    audio_echo_render_close(&reference->render);
    reference->pending_frames = 0;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool wave_read_format(FILE *file, wave_format_t *format)
{
    uint8_t riff[12];
    if(fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
       memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_format = false;
    bool have_data = false;
    while(!have_data) {
        uint8_t header[8];
        if(fread(header, 1, sizeof(header), file) != sizeof(header)) break;
        uint32_t chunk_size = read_le32(header + 4);
        long chunk_start = ftell(file);
        if(chunk_start < 0) return false;

        if(memcmp(header, "fmt ", 4) == 0 && chunk_size >= 16U) {
            uint8_t value[16];
            if(fread(value, 1, sizeof(value), file) != sizeof(value)) return false;
            format->format = read_le16(value);
            format->channels = read_le16(value + 2);
            format->sample_rate = read_le32(value + 4);
            format->bits_per_sample = read_le16(value + 14);
            have_format = true;
        }
        else if(memcmp(header, "data", 4) == 0) {
            format->data_offset = chunk_start;
            format->data_size = chunk_size;
            have_data = true;
            break;
        }

        long next = chunk_start + (long)chunk_size + (long)(chunk_size & 1U);
        if(fseek(file, next, SEEK_SET) != 0) return false;
    }
    return have_format && have_data && format->format == 1 &&
           format->bits_per_sample == 16 && format->channels > 0 &&
           format->channels <= 2 && fseek(file, format->data_offset, SEEK_SET) == 0;
}

static bool playback_should_stop(audio_playback_t *playback)
{
    bool stop;
    pthread_mutex_lock(&playback->mutex);
    stop = playback->stop;
    pthread_mutex_unlock(&playback->mutex);
    return stop;
}

static uint64_t playback_clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static snd_pcm_sframes_t playback_write(audio_playback_t *playback,
                                         playback_reference_t *reference,
                                         snd_pcm_t *pcm, const int16_t *samples,
                                         snd_pcm_uframes_t frames, unsigned rate,
                                         unsigned channels)
{
    if(playback->echo && !reference->render.started && !reference->pending_frames)
        (void)audio_pcm_clock_enable(pcm);
    snd_pcm_sframes_t written = snd_pcm_writei(pcm, samples, frames);
    if(written > 0 && playback->echo) {
        /* Before ALSA starts, queued samples have no playback time yet. Keep
         * them until the DMA clock can timestamp the entire startup buffer. */
        if(snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) {
            size_t capacity = sizeof(reference->pending) / sizeof(reference->pending[0]) / channels;
            if(reference->pending_frames + (size_t)written <= capacity) {
                memcpy(reference->pending + reference->pending_frames * channels,
                    samples, (size_t)written * channels * sizeof(*samples));
                reference->pending_frames += (size_t)written;
                return written;
            }
            /* Unusually large externally configured buffers still get a
             * bounded reference, rather than overrunning the startup cache. */
            reference->pending_frames = 0;
        }
        snd_pcm_sframes_t delay = 0;
        uint64_t position_ns;
        if(audio_pcm_clock_read(pcm, playback_clock_ns(), &position_ns, &delay)) {
            /* Match the microphone to samples reaching the DAC, not to the
             * network queue or the time a blocking ALSA write started. */
            int64_t first_ns = (int64_t)position_ns +
                ((int64_t)delay - written - (int64_t)reference->pending_frames) *
                1000000000LL / rate;
            /* Keep the reference causal through plug/dmix timestamp rounding
             * and the DAC/ADC period boundary. The filter absorbs this lead. */
            first_ns -= BINBOT_AUDIO_FRAME_MS * 1000000LL;
            if(reference->pending_frames) {
                audio_echo_render(playback->echo, &reference->render, reference->pending,
                    reference->pending_frames, rate, channels,
                    first_ns > 0 ? (uint64_t)first_ns : 0);
                first_ns += (int64_t)reference->pending_frames * 1000000000LL / rate;
                reference->pending_frames = 0;
            }
            audio_echo_render(playback->echo, &reference->render, samples, (size_t)written,
                rate, channels, first_ns > 0 ? (uint64_t)first_ns : 0);
        }
    }
    return written;
}

static void playback_drop(audio_playback_t *playback, snd_pcm_t *pcm,
                            playback_reference_t *reference)
{
    snd_pcm_drop(pcm);
    audio_echo_discard(playback->echo, playback_clock_ns());
    playback_reference_close(reference);
}

static void play_wave(audio_playback_t *playback)
{
    pthread_mutex_lock(&playback->device_mutex);
    FILE *file = fopen(playback->sound_path, "rb");
    if(!file) {
        fprintf(stderr, "audio: cannot open wake sound %s: %s\n",
                playback->sound_path, strerror(errno));
        pthread_mutex_unlock(&playback->device_mutex);
        return;
    }
    wave_format_t format = {0};
    if(!wave_read_format(file, &format)) {
        fprintf(stderr, "audio: wake sound must be PCM S16_LE WAV\n");
        fclose(file);
        pthread_mutex_unlock(&playback->device_mutex);
        return;
    }

    snd_pcm_t *pcm = NULL;
    int error = snd_pcm_open(&pcm, playback->device, SND_PCM_STREAM_PLAYBACK, 0);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open playback device %s: %s\n",
                playback->device, snd_strerror(error));
        fclose(file);
        pthread_mutex_unlock(&playback->device_mutex);
        return;
    }
    error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               format.channels, format.sample_rate,
                               1, 80000);
    if(error < 0) {
        fprintf(stderr, "audio: cannot configure playback: %s\n", snd_strerror(error));
        snd_pcm_close(pcm);
        fclose(file);
        pthread_mutex_unlock(&playback->device_mutex);
        return;
    }

    fprintf(stdout, "audio: playing wake sound max_ms=%u\n",
            playback->wake_max_ms);
    int16_t samples[512 * 2];
    playback_reference_t reference = {0};
    uint32_t bytes_left = format.data_size;
    if(playback->wake_max_ms > 0) {
        uint64_t maximum_bytes =
            (uint64_t)format.sample_rate * format.channels * sizeof(int16_t) *
            playback->wake_max_ms / 1000U;
        if(maximum_bytes < bytes_left) bytes_left = (uint32_t)maximum_bytes;
    }
    while(bytes_left > 0 && !playback_should_stop(playback)) {
        size_t wanted = sizeof(samples);
        if(wanted > bytes_left) wanted = bytes_left;
        size_t bytes_read = fread(samples, 1, wanted, file);
        if(bytes_read == 0) break;
        bytes_left -= (uint32_t)bytes_read;

        size_t sample_count = bytes_read / sizeof(samples[0]);
        for(size_t index = 0; index < sample_count; ++index) {
            samples[index] = (int16_t)((float)samples[index] * playback->wake_volume);
        }

        snd_pcm_sframes_t frames_left =
            (snd_pcm_sframes_t)(bytes_read / (sizeof(int16_t) * format.channels));
        int16_t *position = samples;
        while(frames_left > 0 && !playback_should_stop(playback)) {
            snd_pcm_sframes_t written = playback_write(playback, &reference, pcm,
                position, (snd_pcm_uframes_t)frames_left, format.sample_rate, format.channels);
            if(written < 0 && snd_pcm_recover(pcm, (int)written, 1) >= 0) {
                playback_reference_close(&reference);
                continue;
            }
            if(written < 0) {
                fprintf(stderr, "audio: playback failed: %s\n", snd_strerror((int)written));
                frames_left = 0;
                bytes_left = 0;
                break;
            }
            position += written * format.channels;
            frames_left -= written;
        }
    }
    if(playback_should_stop(playback)) playback_drop(playback, pcm, &reference);
    else snd_pcm_drain(pcm);
    playback_reference_close(&reference);
    snd_pcm_close(pcm);
    fclose(file);
    pthread_mutex_unlock(&playback->device_mutex);
}

#ifdef BINBOT_HAVE_RKAUDIO_MP3
static bool music_snapshot(audio_playback_t *playback,
                           uint64_t generation,
                           bool *should_stop,
                           bool *should_pause)
{
    bool changed = false;
    pthread_mutex_lock(&playback->mutex);
    *should_stop = playback->music_stop || !playback->music_active ||
                   playback->music_generation != generation;
    *should_pause = playback->music_paused;
    changed = *should_stop || *should_pause || playback->stop;
    pthread_mutex_unlock(&playback->mutex);
    return changed;
}

static bool music_read_file(const char *path, unsigned char **data_out, size_t *size_out)
{
    *data_out = NULL;
    *size_out = 0;
    FILE *file = fopen(path, "rb");
    if(!file) {
        fprintf(stderr, "audio: cannot open music %s: %s\n", path, strerror(errno));
        return false;
    }
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if(length <= 0 || (unsigned long)length > MUSIC_MAX_FILE_BYTES ||
       fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "audio: invalid music size for %s\n", path);
        fclose(file);
        return false;
    }
    unsigned char *data = malloc((size_t)length);
    if(!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "audio: cannot read music %s\n", path);
        free(data);
        fclose(file);
        return false;
    }
    fclose(file);
    *data_out = data;
    *size_out = (size_t)length;
    return true;
}

static bool music_write_frames(audio_playback_t *playback, snd_pcm_t *pcm,
                               const short *samples, size_t sample_count,
                               unsigned rate, unsigned channels, uint64_t generation,
                               playback_reference_t *reference)
{
    snd_pcm_sframes_t frames_left = (snd_pcm_sframes_t)(sample_count / channels);
    const short *position = samples;
    while(frames_left > 0) {
        bool stop = false;
        bool pause = false;
        if(music_snapshot(playback, generation, &stop, &pause) && (stop || pause)) {
            playback_drop(playback, pcm, reference);
            return false;
        }
        snd_pcm_sframes_t written = playback_write(playback, reference, pcm, position,
            (snd_pcm_uframes_t)frames_left, rate, channels);
        if(written < 0 && snd_pcm_recover(pcm, (int)written, 1) >= 0) {
            playback_reference_close(reference);
            continue;
        }
        if(written < 0) {
            fprintf(stderr, "audio: music playback failed: %s\n",
                    snd_strerror((int)written));
            return false;
        }
        position += written * channels;
        frames_left -= written;
    }
    return true;
}

static bool music_play_file(audio_playback_t *playback, const char *path,
                            uint32_t seek_ms, uint64_t generation)
{
    unsigned char *data = NULL;
    size_t data_size = 0;
    if(!music_read_file(path, &data, &data_size)) return false;

    HMP3Decoder decoder = MP3InitDecoder();
    if(!decoder) {
        fprintf(stderr, "audio: cannot initialize MP3 decoder\n");
        free(data);
        return false;
    }

    int sync = MP3FindSyncWord(data, (int)data_size);
    if(sync < 0) {
        fprintf(stderr, "audio: no MP3 frame found in %s\n", path);
        MP3FreeDecoder(decoder);
        free(data);
        return false;
    }

    snd_pcm_t *pcm = NULL;
    unsigned char *cursor = data + sync;
    int bytes_left = (int)(data_size - (size_t)sync);
    short decoded[MUSIC_MAX_OUTPUT_SAMPLES];
    uint64_t decoded_ms = 0;
    bool configured = false;
    playback_reference_t reference = {0};
    while(bytes_left > 0) {
        bool stop = false;
        bool pause = false;
        if(music_snapshot(playback, generation, &stop, &pause) && (stop || pause)) break;

        unsigned char *before = cursor;
        int available = bytes_left;
        int result = MP3Decode(decoder, &cursor, &bytes_left, decoded, 0);
        if(result != ERR_MP3_NONE) {
            int next = MP3FindSyncWord(before + 1, available > 1 ? available - 1 : 0);
            if(next < 0) break;
            cursor = before + 1 + next;
            bytes_left = available - 1 - next;
            continue;
        }

        MP3FrameInfo info;
        memset(&info, 0, sizeof(info));
        MP3GetLastFrameInfo(decoder, &info);
        if(info.nChans <= 0 || info.nChans > 2 || info.samprate <= 0 ||
           info.outputSamps <= 0 || info.outputSamps > MUSIC_MAX_OUTPUT_SAMPLES) {
            fprintf(stderr, "audio: invalid MP3 frame info\n");
            break;
        }
        uint32_t frame_ms = (uint32_t)(((uint64_t)(info.outputSamps / info.nChans) *
                                        1000ULL) / (uint32_t)info.samprate);
        if(decoded_ms + frame_ms <= seek_ms) {
            decoded_ms += frame_ms;
            continue;
        }

        if(!configured) {
            pthread_mutex_lock(&playback->device_mutex);
            int error = snd_pcm_open(&pcm, playback->device, SND_PCM_STREAM_PLAYBACK, 0);
            if(error >= 0) {
                error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                            SND_PCM_ACCESS_RW_INTERLEAVED,
                                            (unsigned)info.nChans,
                                            (unsigned)info.samprate, 1, 80000);
            }
            if(error < 0) {
                fprintf(stderr, "audio: cannot configure music playback: %s\n",
                        snd_strerror(error));
                if(pcm) snd_pcm_close(pcm);
                pcm = NULL;
                pthread_mutex_unlock(&playback->device_mutex);
                break;
            }
            configured = true;
            fprintf(stdout, "audio: playing MP3 %s (%d Hz, %d ch)\n",
                    path, info.samprate, info.nChans);
        }
        if(!music_write_frames(playback, pcm, decoded, (size_t)info.outputSamps,
                               (unsigned)info.samprate, (unsigned)info.nChans,
                               generation, &reference)) break;
        decoded_ms += frame_ms;
    }
    if(pcm) {
        bool stop = false;
        bool pause = false;
        if(music_snapshot(playback, generation, &stop, &pause) && (stop || pause)) {
            playback_drop(playback, pcm, &reference);
        } else {
            snd_pcm_drain(pcm);
        }
        snd_pcm_close(pcm);
        pthread_mutex_unlock(&playback->device_mutex);
    }
    playback_reference_close(&reference);
    MP3FreeDecoder(decoder);
    free(data);
    return configured;
}
#endif

static uint64_t noise_clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void noise_play(audio_playback_t *playback, uint64_t generation)
{
    audio_noise_t noise;
    audio_noise_init(&noise);
    snd_pcm_t *pcm = NULL;
    pthread_mutex_lock(&playback->device_mutex);
    int error = snd_pcm_open(&pcm, playback->device, SND_PCM_STREAM_PLAYBACK, 0);
    if(error >= 0) error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, 1, BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, 1, 240000);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open white noise playback: %s\n", snd_strerror(error));
        if(pcm) snd_pcm_close(pcm);
        pthread_mutex_unlock(&playback->device_mutex);
        return;
    }
    bool pace_null = snd_pcm_type(pcm) == SND_PCM_TYPE_NULL;
    unsigned recovered_count = 0;
    uint64_t blocks = 0, rendered_ns = 0;
    fprintf(stdout, "audio: white noise playback started\n");
    uint64_t started = noise_clock_ns();
    uint64_t previous = noise_clock_ns();
    playback_reference_t reference = {0};
    while(true) {
        pthread_mutex_lock(&playback->mutex);
        bool active = !playback->music_stop && playback->music_active &&
                      playback->noise_mode && playback->music_generation == generation;
        binbot_audio_noise_state_t state = playback->noise;
        pthread_mutex_unlock(&playback->mutex);
        if(!active || !state.playing) break;
        int16_t samples[BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES];
        uint64_t render_started = noise_clock_ns();
        audio_noise_render(&noise, &state, samples, BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES);
        rendered_ns += noise_clock_ns() - render_started;
        blocks++;
        snd_pcm_sframes_t offset = 0;
        while(offset < BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES) {
            snd_pcm_sframes_t written = playback_write(playback, &reference, pcm,
                samples + offset, BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES - offset,
                BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, 1);
            if(written < 0) {
                int recovered = snd_pcm_recover(pcm, (int)written, 1);
                if(recovered < 0) { error = recovered; break; }
                playback_reference_close(&reference);
                recovered_count++;
                if(recovered_count == 1 || recovered_count % 100 == 0)
                    fprintf(stderr, "audio: white noise buffer recovered (%u)\n", recovered_count);
                continue;
            }
            if(written == 0) { error = -EIO; break; }
            offset += written;
        }
        if(error < 0) {
            fprintf(stderr, "audio: white noise playback failed: %s\n", snd_strerror(error));
            break;
        }
        /* Blocking ALSA writes already pace hardware, including dmix's larger
         * periods. Sleeping again starves its buffer. Only null needs a clock. */
        uint64_t now = noise_clock_ns(), deadline = previous + 20000000ULL;
        if(pace_null && now < deadline) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)(deadline - now)};
            while(nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
            now = noise_clock_ns();
        }
        pthread_mutex_lock(&playback->mutex);
        if(playback->music_generation == generation && playback->noise.timer_seconds) {
            uint64_t elapsed = now - previous;
            if(elapsed >= playback->noise_remaining_ns) {
                playback->noise_remaining_ns = 0;
                playback->noise.timer_seconds = 0;
                playback->noise.remaining_seconds = 0;
                playback->noise.playing = 0;
                playback->music_active = false;
                playback->music_playing = false;
            }
            else {
                playback->noise_remaining_ns -= elapsed;
                playback->noise.remaining_seconds =
                    (uint32_t)((playback->noise_remaining_ns + 999999999ULL) / 1000000000ULL);
            }
        }
        pthread_mutex_unlock(&playback->mutex);
        previous = now;
    }
    playback_drop(playback, pcm, &reference);
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&playback->device_mutex);
    fprintf(stdout, "audio: white noise playback stopped blocks=%llu elapsed_ms=%llu render_us=%llu recoveries=%u\n",
        (unsigned long long)blocks, (unsigned long long)((noise_clock_ns() - started) / 1000000),
        (unsigned long long)(blocks ? rendered_ns / blocks / 1000 : 0), recovered_count);
}

static void *music_worker(void *user_data)
{
    audio_playback_t *playback = user_data;
    while(true) {
        char path[sizeof(playback->music_path)];
        uint32_t seek_ms = 0;
        uint64_t generation = 0;
        bool is_noise = false;
        pthread_mutex_lock(&playback->mutex);
        while(!playback->music_stop &&
              (!playback->music_active || playback->music_paused)) {
            pthread_cond_wait(&playback->condition, &playback->mutex);
        }
        if(playback->music_stop) {
            pthread_mutex_unlock(&playback->mutex);
            break;
        }
        snprintf(path, sizeof(path), "%s", playback->music_path);
        seek_ms = playback->music_seek_ms;
        playback->music_seek_ms = 0;
        generation = playback->music_generation;
        is_noise = playback->noise_mode;
        pthread_mutex_unlock(&playback->mutex);
        if(is_noise) noise_play(playback, generation);
        else {
#ifdef BINBOT_HAVE_RKAUDIO_MP3
        (void)music_play_file(playback, path, seek_ms, generation);
#else
        (void)seek_ms;
        fprintf(stderr, "audio: MP3 support is not built\n");
#endif
        }
        pthread_mutex_lock(&playback->mutex);
        if(!playback->music_stop && playback->music_generation == generation &&
           !playback->music_paused) {
            playback->music_active = false;
            playback->music_playing = false;
            if(is_noise) playback->noise.playing = 0;
        }
        pthread_mutex_unlock(&playback->mutex);
    }
    return NULL;
}

static void *playback_worker(void *user_data)
{
    audio_playback_t *playback = user_data;
    snd_pcm_t *dialog_pcm = NULL;
    playback_reference_t reference = {0};
    uint64_t observed_epoch = 0;
    while(true) {
        bool play_wake = false;
        bool finish_dialog = false;
        bool close_dialog = false;
        binbot_playback_queue_frame_t frame;
        bool have_frame = false;

        pthread_mutex_lock(&playback->mutex);
        while(!playback->stop && !playback->wake_pending &&
              playback->dialog_epoch == observed_epoch &&
              playback->dialog_queue.count == 0 &&
              !binbot_playback_queue_should_finish(&playback->dialog_queue)) {
            pthread_cond_wait(&playback->condition, &playback->mutex);
        }
        if(playback->stop) {
            pthread_mutex_unlock(&playback->mutex);
            break;
        }
        if(playback->dialog_epoch != observed_epoch) {
            observed_epoch = playback->dialog_epoch;
            close_dialog = true;
        }
        if(playback->wake_pending) {
            playback->wake_pending = false;
            playback->wake_playing = true;
            play_wake = true;
            close_dialog = true;
        }
        else if(binbot_playback_queue_pop(&playback->dialog_queue, &frame)) {
            have_frame = true;
        }
        else if(binbot_playback_queue_should_finish(&playback->dialog_queue)) {
            binbot_playback_queue_finish(&playback->dialog_queue);
            finish_dialog = true;
        }
        pthread_mutex_unlock(&playback->mutex);

        if(close_dialog && dialog_pcm) {
            playback_drop(playback, dialog_pcm, &reference);
            snd_pcm_close(dialog_pcm);
            dialog_pcm = NULL;
        }
        if(play_wake) {
            play_wave(playback);
            pthread_mutex_lock(&playback->mutex);
            playback->wake_playing = false;
            pthread_mutex_unlock(&playback->mutex);
            continue;
        }
        if(finish_dialog && dialog_pcm) {
            snd_pcm_drain(dialog_pcm);
            playback_reference_close(&reference);
            snd_pcm_close(dialog_pcm);
            dialog_pcm = NULL;
            continue;
        }
        if(!have_frame) continue;

        if(!dialog_pcm) {
            int error = snd_pcm_open(
                &dialog_pcm, playback->device, SND_PCM_STREAM_PLAYBACK, 0);
            if(error >= 0) {
                error = snd_pcm_set_params(
                    dialog_pcm, SND_PCM_FORMAT_S16_LE,
                    SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                    BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, 1, 80000);
            }
            if(error < 0) {
                fprintf(stderr, "audio: cannot open dialog playback: %s\n",
                        snd_strerror(error));
                if(dialog_pcm) snd_pcm_close(dialog_pcm);
                dialog_pcm = NULL;
                pthread_mutex_lock(&playback->mutex);
                binbot_playback_queue_cancel(&playback->dialog_queue, NULL);
                playback->dialog_epoch++;
                observed_epoch = playback->dialog_epoch;
                pthread_mutex_unlock(&playback->mutex);
                continue;
            }
            fprintf(stdout, "audio: dialog playback started at 24000 Hz mono\n");
        }

        for(unsigned i = 0; i < frame.sample_count; ++i)
            frame.samples[i] = (int16_t)(frame.samples[i] * playback->dialog_volume);
        snd_pcm_sframes_t left = frame.sample_count;
        int16_t *position = frame.samples;
        while(left > 0) {
            snd_pcm_sframes_t written = playback_write(playback, &reference, dialog_pcm,
                position, (snd_pcm_uframes_t)left, BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, 1);
            if(written < 0 && snd_pcm_recover(dialog_pcm, (int)written, 1) >= 0) {
                playback_reference_close(&reference);
                continue;
            }
            if(written < 0) {
                fprintf(stderr, "audio: dialog playback failed: %s\n",
                        snd_strerror((int)written));
                playback_drop(playback, dialog_pcm, &reference);
                snd_pcm_close(dialog_pcm);
                dialog_pcm = NULL;
                break;
            }
            position += written;
            left -= written;
        }
    }
    if(dialog_pcm) {
        playback_drop(playback, dialog_pcm, &reference);
        snd_pcm_close(dialog_pcm);
    }
    playback_reference_close(&reference);
    return NULL;
}

bool audio_playback_start(audio_playback_t *playback,
                          const char *device,
                          const char *sound_path,
                          float wake_volume,
                          uint32_t wake_max_ms,
                          float dialog_volume,
                          audio_echo_t *echo)
{
    memset(playback, 0, sizeof(*playback));
    playback->echo = echo;
    snprintf(playback->device, sizeof(playback->device), "%s",
             device && device[0] ? device : "plug:dmix");
    snprintf(playback->sound_path, sizeof(playback->sound_path), "%s",
             sound_path && sound_path[0] ? sound_path :
             "/oem/assets/audio/wake_notification.wav");
    playback->wake_volume = wake_volume >= 0.0f && wake_volume <= 1.0f
                                ? wake_volume : 0.35f;
    playback->wake_max_ms = wake_max_ms;
    playback->dialog_volume = dialog_volume >= 0.0f && dialog_volume <= 1.0f
                                  ? dialog_volume : 0.25f;
    playback->noise.master = 70;
    playback->noise.levels[0] = 55;
    playback->noise.levels[4] = 35;
    if(pthread_mutex_init(&playback->mutex, NULL) != 0) return false;
    if(pthread_mutex_init(&playback->device_mutex, NULL) != 0) {
        pthread_mutex_destroy(&playback->mutex);
        return false;
    }
    if(pthread_cond_init(&playback->condition, NULL) != 0) {
        pthread_mutex_destroy(&playback->device_mutex);
        pthread_mutex_destroy(&playback->mutex);
        return false;
    }
    /* Initialize shared queue/state before the worker can observe them. */
    binbot_playback_queue_init(&playback->dialog_queue);
    playback->initialized = true;
    if(pthread_create(&playback->thread, NULL, playback_worker, playback) != 0) {
        playback->initialized = false;
        pthread_cond_destroy(&playback->condition);
        pthread_mutex_destroy(&playback->device_mutex);
        pthread_mutex_destroy(&playback->mutex);
        return false;
    }
    if(pthread_create(&playback->music_thread, NULL, music_worker, playback) != 0) {
        pthread_mutex_lock(&playback->mutex);
        playback->stop = true;
        pthread_cond_broadcast(&playback->condition);
        pthread_mutex_unlock(&playback->mutex);
        pthread_join(playback->thread, NULL);
        playback->initialized = false;
        pthread_cond_destroy(&playback->condition);
        pthread_mutex_destroy(&playback->device_mutex);
        pthread_mutex_destroy(&playback->mutex);
        return false;
    }
    playback->music_thread_started = true;
    return true;
}

bool audio_playback_music_control(audio_playback_t *playback,
                                  const binbot_audio_music_control_t *control)
{
    if(!playback || !playback->initialized || !control) return false;
    if(control->command < BINBOT_AUDIO_MUSIC_PLAY ||
       control->command > BINBOT_AUDIO_MUSIC_SEEK) return false;
    if(control->path[sizeof(control->path) - 1U] != '\0') return false;
    if(control->reserved > BINBOT_AUDIO_MEDIA_PRONUNCIATION) return false;
    const bool pronunciation = control->reserved == BINBOT_AUDIO_MEDIA_PRONUNCIATION;
    if(pronunciation && control->command != BINBOT_AUDIO_MUSIC_PLAY &&
       control->command != BINBOT_AUDIO_MUSIC_STOP) return false;
    if(pronunciation && control->command == BINBOT_AUDIO_MUSIC_PLAY &&
       (!control->path[0] || control->position_ms)) return false;

    pthread_mutex_lock(&playback->mutex);
    if(pronunciation && control->command == BINBOT_AUDIO_MUSIC_STOP &&
       playback->music_kind != BINBOT_AUDIO_MEDIA_PRONUNCIATION) {
        pthread_mutex_unlock(&playback->mutex);
        return true;
    }
    if(pronunciation && control->command == BINBOT_AUDIO_MUSIC_PLAY &&
       (playback->dialog_queue.active || playback->wake_pending || playback->wake_playing)) {
        pthread_mutex_unlock(&playback->mutex);
        return false;
    }
    switch((binbot_audio_music_command_t)control->command) {
        case BINBOT_AUDIO_MUSIC_PLAY:
            if(control->path[0]) {
                snprintf(playback->music_path, sizeof(playback->music_path),
                         "%s", control->path);
            } else if(!playback->music_path[0]) {
                pthread_mutex_unlock(&playback->mutex);
                return false;
            }
            /* PLAY is also resume.  Refresh the position and invalidate any
             * decoder that is still unwinding from a previous pause. */
            playback->music_seek_ms = control->position_ms;
            playback->music_kind = control->reserved;
            playback->noise_mode = false;
            playback->noise.playing = 0;
            playback->music_generation++;
            playback->music_active = true;
            playback->music_playing = true;
            playback->music_paused = false;
            binbot_playback_queue_cancel(&playback->dialog_queue, NULL);
            playback->dialog_epoch++;
            pthread_cond_broadcast(&playback->condition);
            break;
        case BINBOT_AUDIO_MUSIC_PAUSE:
            if(!playback->music_active) {
                pthread_mutex_unlock(&playback->mutex);
                return false;
            }
            playback->music_paused = true;
            playback->music_playing = false;
            /* Stop the current decoder.  The generation check prevents its
             * cleanup from clearing a later resume request. */
            playback->music_generation++;
            pthread_cond_broadcast(&playback->condition);
            break;
        case BINBOT_AUDIO_MUSIC_STOP:
            playback->music_active = false;
            playback->music_playing = false;
            playback->music_paused = false;
            playback->music_seek_ms = 0;
            playback->music_generation++;
            pthread_cond_broadcast(&playback->condition);
            break;
        case BINBOT_AUDIO_MUSIC_SEEK:
            if(!playback->music_path[0]) {
                pthread_mutex_unlock(&playback->mutex);
                return false;
            }
            playback->music_seek_ms = control->position_ms;
            playback->music_generation++;
            playback->music_active = true;
            playback->music_playing = !playback->music_paused;
            pthread_cond_broadcast(&playback->condition);
            break;
        default:
            pthread_mutex_unlock(&playback->mutex);
            return false;
    }
    pthread_mutex_unlock(&playback->mutex);
    return true;
}

bool audio_playback_noise_control(audio_playback_t *playback,
                                  const binbot_audio_noise_state_t *state)
{
    if(!playback || !playback->initialized || !binbot_audio_noise_is_valid(state)) return false;
    unsigned active = 0;
    for(unsigned i = 0; i < BINBOT_AUDIO_NOISE_COUNT; ++i) active += state->levels[i] > 0;
    pthread_mutex_lock(&playback->mutex);
    if(state->timer_seconds != playback->noise.timer_seconds)
        playback->noise_remaining_ns = (uint64_t)state->timer_seconds * 1000000000ULL;
    uint32_t remaining = (uint32_t)((playback->noise_remaining_ns + 999999999ULL) / 1000000000ULL);
    playback->noise = *state;
    playback->noise.reserved = 0;
    playback->noise.remaining_seconds = remaining;
    playback->noise.playing = state->playing && active;
    if(playback->noise.playing) {
        playback->music_kind = BINBOT_AUDIO_MEDIA_MUSIC;
        if(!playback->noise_mode || !playback->music_active) {
            playback->music_generation++;
            binbot_playback_queue_cancel(&playback->dialog_queue, NULL);
            playback->dialog_epoch++;
        }
        playback->noise_mode = true;
        playback->music_active = true;
        playback->music_playing = true;
        playback->music_paused = false;
    }
    else if(playback->noise_mode) {
        playback->music_active = false;
        playback->music_playing = false;
        playback->music_generation++;
    }
    pthread_cond_broadcast(&playback->condition);
    pthread_mutex_unlock(&playback->mutex);
    return true;
}

void audio_playback_noise_snapshot(audio_playback_t *playback,
                                   binbot_audio_noise_state_t *state)
{
    if(!state) return;
    if(!playback || !playback->initialized) {
        memset(state, 0, sizeof(*state));
        return;
    }
    pthread_mutex_lock(&playback->mutex);
    *state = playback->noise;
    state->playing = playback->noise_mode && playback->music_active &&
                     !playback->music_paused && playback->noise.playing;
    pthread_mutex_unlock(&playback->mutex);
}

void audio_playback_request(audio_playback_t *playback)
{
    if(!playback || !playback->initialized) return;
    pthread_mutex_lock(&playback->mutex);
    playback->music_active = false;
    playback->music_playing = false;
    playback->music_paused = false;
    playback->music_generation++;
    pthread_cond_broadcast(&playback->condition);
    if(!playback->wake_playing && !playback->wake_pending) {
        binbot_playback_queue_cancel(&playback->dialog_queue, NULL);
        playback->dialog_epoch++;
        playback->wake_pending = true;
        pthread_cond_broadcast(&playback->condition);
    }
    pthread_mutex_unlock(&playback->mutex);
}

bool audio_playback_active(audio_playback_t *playback)
{
    if(!playback || !playback->initialized) return false;
    pthread_mutex_lock(&playback->mutex);
    bool active = playback->wake_playing || playback->wake_pending;
    pthread_mutex_unlock(&playback->mutex);
    return active;
}

bool audio_playback_dialog_active(audio_playback_t *playback)
{
    if(!playback || !playback->initialized) return false;
    pthread_mutex_lock(&playback->mutex);
    bool active = playback->dialog_queue.active;
    pthread_mutex_unlock(&playback->mutex);
    return active;
}

bool audio_playback_begin(audio_playback_t *playback,
                          const binbot_audio_playback_begin_t *begin)
{
    if(!playback || !playback->initialized || !begin ||
       begin->sample_rate != BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE ||
       begin->channels != 1 || begin->frame_ms != BINBOT_AUDIO_FRAME_MS) {
        return false;
    }
    pthread_mutex_lock(&playback->mutex);
    playback->music_active = false;
    playback->music_playing = false;
    playback->music_paused = false;
    playback->music_generation++;
    pthread_cond_broadcast(&playback->condition);
    playback->dialog_epoch++;
    binbot_playback_queue_begin(&playback->dialog_queue, begin->stream_id);
    pthread_cond_broadcast(&playback->condition);
    pthread_mutex_unlock(&playback->mutex);
    return true;
}

bool audio_playback_enqueue(audio_playback_t *playback,
                            const binbot_audio_playback_frame_t *frame)
{
    if(!playback || !playback->initialized || !frame) return false;
    pthread_mutex_lock(&playback->mutex);
    bool accepted = binbot_playback_queue_push(&playback->dialog_queue, frame);
    if(accepted) pthread_cond_broadcast(&playback->condition);
    pthread_mutex_unlock(&playback->mutex);
    return accepted;
}

bool audio_playback_end(audio_playback_t *playback,
                        const uint8_t stream_id[16])
{
    if(!playback || !playback->initialized || !stream_id) return false;
    pthread_mutex_lock(&playback->mutex);
    bool accepted = binbot_playback_queue_end(&playback->dialog_queue, stream_id);
    if(accepted) pthread_cond_broadcast(&playback->condition);
    pthread_mutex_unlock(&playback->mutex);
    return accepted;
}

bool audio_playback_cancel(audio_playback_t *playback,
                           const uint8_t stream_id[16])
{
    if(!playback || !playback->initialized) return false;
    pthread_mutex_lock(&playback->mutex);
    bool accepted = binbot_playback_queue_cancel(
        &playback->dialog_queue, stream_id);
    if(accepted) {
        playback->music_active = false;
        playback->music_playing = false;
        playback->music_paused = false;
        playback->music_generation++;
        playback->dialog_epoch++;
        pthread_cond_broadcast(&playback->condition);
    }
    pthread_mutex_unlock(&playback->mutex);
    return accepted;
}

size_t audio_playback_queued(audio_playback_t *playback)
{
    if(!playback || !playback->initialized) return 0;
    pthread_mutex_lock(&playback->mutex);
    size_t count = binbot_playback_queue_count(&playback->dialog_queue);
    pthread_mutex_unlock(&playback->mutex);
    return count;
}

void audio_playback_stop(audio_playback_t *playback)
{
    if(!playback || !playback->initialized) return;
    pthread_mutex_lock(&playback->mutex);
    playback->stop = true;
    playback->music_stop = true;
    playback->music_active = false;
    playback->music_playing = false;
    playback->music_generation++;
    binbot_playback_queue_cancel(&playback->dialog_queue, NULL);
    playback->dialog_epoch++;
    pthread_cond_broadcast(&playback->condition);
    pthread_mutex_unlock(&playback->mutex);
    if(playback->music_thread_started) {
        pthread_join(playback->music_thread, NULL);
        playback->music_thread_started = false;
    }
    pthread_join(playback->thread, NULL);
    pthread_cond_destroy(&playback->condition);
    pthread_mutex_destroy(&playback->device_mutex);
    pthread_mutex_destroy(&playback->mutex);
    playback->initialized = false;
}
