#ifndef BINBOT_AUDIO_PROTOCOL_H
#define BINBOT_AUDIO_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_AUDIO_PROTOCOL_MAGIC = 0x42424155U, /* "BBAU" */
    BINBOT_AUDIO_PROTOCOL_VERSION = 2,
    BINBOT_AUDIO_SAMPLE_RATE = 16000,
    BINBOT_AUDIO_FRAME_MS = 20,
    BINBOT_AUDIO_FRAME_SAMPLES = 320,
    BINBOT_AUDIO_STEREO_CHANNELS = 2,
    BINBOT_AUDIO_DEFAULT_PREROLL_MS = 2000,
    BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE = 24000,
    BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES = 480,
    BINBOT_DIALOG_TEXT_MAX = 3072,
};

#define BINBOT_AUDIO_DEFAULT_SOCKET "/tmp/binbot_audio.sock"

typedef enum {
    BINBOT_AUDIO_MSG_HELLO = 1,
    BINBOT_AUDIO_MSG_PCM = 2,
    BINBOT_AUDIO_MSG_WAKE_DETECTED = 3,
    BINBOT_AUDIO_MSG_LEVEL = 4,
    BINBOT_AUDIO_MSG_PCM_STEREO = 5,
    BINBOT_AUDIO_MSG_PLAYBACK_BEGIN = 6,
    BINBOT_AUDIO_MSG_PLAYBACK_FRAME = 7,
    BINBOT_AUDIO_MSG_PLAYBACK_END = 8,
    BINBOT_AUDIO_MSG_PLAYBACK_CANCEL = 9,
    BINBOT_AUDIO_MSG_DIALOG_STATE = 10,
    BINBOT_AUDIO_MSG_MUSIC_CONTROL = 11,
    BINBOT_AUDIO_MSG_VOLUME_SET = 12,
    BINBOT_AUDIO_MSG_REMOTE_COMMAND = 13,
    BINBOT_AUDIO_MSG_REMOTE_STATE = 14,
    BINBOT_AUDIO_MSG_REMOTE_RESULT = 15,
    /* A local UI (or another trusted client) asks the audio service to
     * synthesize the same event as a hotword hit.  Keeping this separate from
     * WAKE_DETECTED prevents an arbitrary client from impersonating the wake
     * detector on the wire. */
    BINBOT_AUDIO_MSG_WAKE_REQUEST = 16,
    BINBOT_AUDIO_MSG_CALENDAR_STATE = 17,
    BINBOT_AUDIO_MSG_COMPONENT_COMMAND = 18,
    BINBOT_AUDIO_MSG_COMPONENT_EVENT = 19,
    BINBOT_AUDIO_MSG_DIALOG_TEXT = 20,
    BINBOT_AUDIO_MSG_DIALOG_INPUT_END = 21,
    BINBOT_AUDIO_MSG_NOISE_CONTROL = 22,
    BINBOT_AUDIO_MSG_NOISE_STATE = 23,
    /* Opt-in raw capture for an engine which owns its complete DSP chain. */
    BINBOT_AUDIO_MSG_PCM_FRONTEND = 24,
    /* Additive v2 message: actual mixer result, echoing request sequence. */
    BINBOT_AUDIO_MSG_VOLUME_RESULT = 25,
    BINBOT_AUDIO_MSG_MICROPHONE_SET = 26,
    BINBOT_AUDIO_MSG_MICROPHONE_RESULT = 27,
    BINBOT_AUDIO_MSG_NOISE_RESULT = 28,
} binbot_audio_message_type_t;

typedef enum {
    BINBOT_AUDIO_ROLE_UNKNOWN = 0,
    BINBOT_AUDIO_ROLE_WAKE = 1,
    BINBOT_AUDIO_ROLE_APP = 2,
    BINBOT_AUDIO_ROLE_ASR = 3,
    BINBOT_AUDIO_ROLE_RECORDER = 4,
    BINBOT_AUDIO_ROLE_DIALOG = 5,
} binbot_audio_client_role_t;

typedef enum {
    BINBOT_AUDIO_CAP_PCM_LIVE = 1U << 0,
    BINBOT_AUDIO_CAP_PCM_PREROLL = 1U << 1,
    BINBOT_AUDIO_CAP_EVENTS = 1U << 2,
    BINBOT_AUDIO_CAP_LEVEL = 1U << 3,
    BINBOT_AUDIO_CAP_PCM_STEREO_LIVE = 1U << 4,
    BINBOT_AUDIO_CAP_DIALOG_STATE = 1U << 5,
    BINBOT_AUDIO_CAP_REMOTE_COMMAND = 1U << 6,
    BINBOT_AUDIO_CAP_REMOTE_EVENTS = 1U << 7,
    BINBOT_AUDIO_CAP_DIALOG_TEXT = 1U << 8,
    BINBOT_AUDIO_CAP_CALENDAR_STATE = 1U << 9,
    BINBOT_AUDIO_CAP_NOISE_STATE = 1U << 10,
    /* A dedicated APP connection owns a hardware-test session until close. */
    BINBOT_AUDIO_CAP_DIAGNOSTICS = 1U << 11,
    BINBOT_AUDIO_CAP_PCM_FRONTEND = 1U << 12,
} binbot_audio_capability_t;

enum {
    /* DIALOG_TEXT with this flag is the estimated spoken prefix of an
     * existing assistant message, not a transcript update. The payload stays
     * compatible with the audio service's existing text relay. */
    BINBOT_AUDIO_FLAG_DIALOG_PLAYBACK = 1U << 0,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint32_t flags;
    uint64_t sequence;
    uint64_t timestamp_ns;
} binbot_audio_header_t;

typedef struct {
    uint16_t role;
    uint16_t reserved;
    uint32_t capabilities;
    uint32_t preroll_ms;
    uint32_t reserved2;
} binbot_audio_hello_t;

typedef struct {
    int16_t samples[BINBOT_AUDIO_FRAME_SAMPLES];
} binbot_audio_pcm_t;

/* Interleaved LRLR... samples from the same ALSA capture period. */
typedef struct {
    int16_t samples[BINBOT_AUDIO_FRAME_SAMPLES * BINBOT_AUDIO_STEREO_CHANNELS];
} binbot_audio_pcm_stereo_t;

/* Unfiltered ALSA mic0/mic1 plus the synchronized 16 kHz render reference.
 * Each frame is interleaved [mic0,mic1,reference] x 320. Zero reference means
 * no rendered sample at that capture time. Existing streams stay unchanged. */
typedef struct {
    int16_t samples[BINBOT_AUDIO_FRAME_SAMPLES * 3];
} binbot_audio_pcm_frontend_t;

typedef struct {
    int32_t hotword_index;
    int32_t reserved;
} binbot_audio_wake_t;

typedef struct {
    uint16_t rms;
    uint16_t peak;
    uint16_t scale;
    uint16_t reserved;
} binbot_audio_level_t;

typedef struct {
    uint8_t stream_id[16];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frame_ms;
} binbot_audio_playback_begin_t;

typedef struct {
    uint8_t stream_id[16];
    uint16_t sample_count;
    uint16_t reserved;
    int16_t samples[BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES];
} binbot_audio_playback_frame_t;

typedef struct {
    uint8_t stream_id[16];
} binbot_audio_playback_control_t;

typedef enum {
    BINBOT_AUDIO_MUSIC_PLAY = 1,
    BINBOT_AUDIO_MUSIC_PAUSE = 2,
    BINBOT_AUDIO_MUSIC_STOP = 3,
    BINBOT_AUDIO_MUSIC_SEEK = 4,
} binbot_audio_music_command_t;

/* Keep the v2 packet layout. Pronunciation is a one-shot MP3 in the music
 * worker; a targeted STOP must not stop a later music/noise request. */
typedef enum {
    BINBOT_AUDIO_MEDIA_MUSIC = 0,
    BINBOT_AUDIO_MEDIA_PRONUNCIATION = 1,
} binbot_audio_media_kind_t;

typedef struct {
    uint16_t command;
    uint16_t reserved; /* binbot_audio_media_kind_t; legacy clients send 0 */
    uint32_t position_ms;
    char path[256];
} binbot_audio_music_control_t;

typedef struct {
    uint8_t volume;
    uint8_t reserved[3];
} binbot_audio_volume_set_t;

typedef struct {
    uint8_t volume;
    uint8_t success;
    uint8_t reserved[2];
} binbot_audio_volume_result_t;

enum { BINBOT_AUDIO_NOISE_COUNT = 9 };

/* Sound order: rain, waves, fire, forest, cafe, wind, storm, stream, typing. */
typedef struct {
    uint8_t levels[BINBOT_AUDIO_NOISE_COUNT];
    uint8_t master;
    uint8_t playing;
    uint8_t reserved;
    uint32_t timer_seconds;
    uint32_t remaining_seconds;
} binbot_audio_noise_state_t;

static inline int binbot_audio_noise_is_valid(const binbot_audio_noise_state_t *state)
{
    if(!state || state->master > 100 || state->playing > 1 ||
       state->timer_seconds > 86400U) return 0;
    for(unsigned i = 0; i < BINBOT_AUDIO_NOISE_COUNT; ++i)
        if(state->levels[i] > 100) return 0;
    return 1;
}

typedef enum {
    BINBOT_REMOTE_MUSIC_PLAY = 1,
    BINBOT_REMOTE_MUSIC_PAUSE = 2,
    BINBOT_REMOTE_MUSIC_STOP = 3,
    BINBOT_REMOTE_MUSIC_PREVIOUS = 4,
    BINBOT_REMOTE_MUSIC_NEXT = 5,
    BINBOT_REMOTE_MUSIC_SEEK = 6,
    BINBOT_REMOTE_MUSIC_SELECT = 7,
    BINBOT_REMOTE_TODO_ADD = 20,
    BINBOT_REMOTE_TODO_SET_DONE = 21,
    BINBOT_REMOTE_TODO_EDIT = 22,
    BINBOT_REMOTE_TODO_DELETE = 23,
    BINBOT_REMOTE_DISPLAY_GIF = 40,
    BINBOT_REMOTE_MODULE_IMAGE = 41,
    BINBOT_REMOTE_VOICE_WAKE = 50,
} binbot_audio_remote_action_t;

typedef struct {
    uint8_t command_id[16];
    uint16_t action;
    uint16_t reserved;
    uint32_t index;
    uint32_t value;
    char text[72];
    char resource[512];
} binbot_audio_remote_command_t;

typedef struct {
    char text[72];
    uint8_t done;
    uint8_t reserved[3];
} binbot_audio_remote_todo_t;

typedef struct {
    uint32_t revision;
    uint8_t todo_count;
    uint8_t music_track_index;
    uint8_t music_active;
    uint8_t music_playing;
    uint32_t music_position_seconds;
    binbot_audio_remote_todo_t todos[8];
    char display_asset[128];
} binbot_audio_remote_state_t;

typedef struct {
    uint8_t command_id[16];
    uint8_t success;
    uint8_t reserved[3];
    char message[128];
} binbot_audio_remote_result_t;

enum {
    BINBOT_COMPONENT_PROBE = 1,
    BINBOT_COMPONENT_OTA = 2,
    BINBOT_COMPONENT_FILE_UPDATE = 3,
    BINBOT_COMPONENT_MOTOR_CONTROL = 4,
    BINBOT_COMPONENT_LIGHT_CONTROL = 5,
    BINBOT_COMPONENT_PEN_CONTROL = 6,
    BINBOT_COMPONENT_PREPARING = 1,
    BINBOT_COMPONENT_DOWNLOADING = 2,
    BINBOT_COMPONENT_TRANSFERRING = 3,
    BINBOT_COMPONENT_VALIDATING = 4,
    BINBOT_COMPONENT_ACTIVATING = 5,
    BINBOT_COMPONENT_VERIFYING = 6,
    BINBOT_COMPONENT_RECONCILING = 7,
    BINBOT_COMPONENT_SUCCEEDED = 8,
    BINBOT_COMPONENT_FAILED = 9,
    BINBOT_COMPONENT_CANCELLED = 10,
    BINBOT_COMPONENT_PROBED = 11,
};

typedef struct {
    uint8_t request_id[16];
    uint32_t action;
    uint32_t size;
    uint64_t before_boot_id;
    uint8_t cancel;
    uint8_t allow_downgrade;
    uint8_t reconcile_only;
    /* [0]: profile, [1..2]: motor, [3]: target (0=component-1, 1=slave-1). */
    uint8_t reserved[5];
    char version[32];
    char before_version[32];
    char sha256[65];
    char elf_sha256[65];
    char before_elf_sha256[65];
    /* PEN_CONTROL uses the first 24 bytes for its binary state. */
    char url[768];
    /* LIGHT_CONTROL stores its nine-byte settings here; otherwise a file name. */
    char file_name[16];
} binbot_component_command_t;

typedef struct {
    uint8_t request_id[16];
    uint64_t event_seq;
    uint32_t status;
    uint32_t bytes_written;
    uint32_t total_bytes;
    uint8_t has_info;
    uint8_t profile;
    uint8_t healthy;
    uint8_t state;
    uint64_t boot_id;
    uint32_t capacity;
    char version[32];
    char project[32];
    char elf_sha256[65];
    char message[160];
    /* Reuses the original structure's three tail padding bytes. */
    uint8_t motor_supported;
    uint8_t motor_direction;
    uint8_t motor_speed;
} binbot_component_event_t;

enum {
    BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY = 8,
    BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT = 0x80,
};

typedef struct {
    char event_id[64];
    char summary[128];
    char start_timestamp[24];
    char end_timestamp[24];
    char start_date[16];
    char end_date[16];
    char timezone[64];
    char location[96];
    char meeting_url[256];
    char status[20];
} binbot_audio_calendar_event_t;

typedef struct {
    uint32_t revision;
    uint8_t event_count;
    /* Former reserved bytes; the high bit marks authoritative change counts.
     * Retain packet size and event offsets for installed audio-service caches. */
    uint8_t changes_added;
    uint8_t changes_updated;
    uint8_t changes_removed;
    binbot_audio_calendar_event_t events[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
} binbot_audio_calendar_state_t;

typedef enum {
    BINBOT_DIALOG_STATE_DISCONNECTED = 0,
    BINBOT_DIALOG_STATE_IDLE = 1,
    BINBOT_DIALOG_STATE_LISTENING = 2,
    BINBOT_DIALOG_STATE_THINKING = 3,
    BINBOT_DIALOG_STATE_SPEAKING = 4,
    BINBOT_DIALOG_STATE_CANCELLING = 5,
    BINBOT_DIALOG_STATE_ERROR = 6,
} binbot_dialog_state_value_t;

typedef struct {
    uint16_t state;
    uint16_t error_code;
    uint32_t reserved;
} binbot_audio_dialog_state_t;

typedef enum {
    BINBOT_DIALOG_TEXT_USER = 1,
    BINBOT_DIALOG_TEXT_ASSISTANT = 2,
} binbot_dialog_text_role_t;

/* Partial events contain the complete text so far; final events replace it. */
typedef struct {
    uint8_t turn_id[16];
    uint16_t role;
    uint16_t final;
    char text[BINBOT_DIALOG_TEXT_MAX + 1];
} binbot_audio_dialog_text_t;

static inline int binbot_audio_dialog_text_is_valid(const binbot_audio_dialog_text_t *text)
{
    static const uint8_t empty_turn[16] = {0};
    return text && memcmp(text->turn_id, empty_turn, sizeof(empty_turn)) != 0 &&
           (text->role == BINBOT_DIALOG_TEXT_USER || text->role == BINBOT_DIALOG_TEXT_ASSISTANT) &&
           text->final <= 1 && memchr(text->text, '\0', sizeof(text->text)) != NULL;
}

typedef struct {
    binbot_audio_header_t header;
    union {
        binbot_audio_hello_t hello;
        binbot_audio_pcm_t pcm;
        binbot_audio_pcm_stereo_t pcm_stereo;
        binbot_audio_pcm_frontend_t pcm_frontend;
        binbot_audio_wake_t wake;
        binbot_audio_level_t level;
        binbot_audio_playback_begin_t playback_begin;
        binbot_audio_playback_frame_t playback_frame;
        binbot_audio_playback_control_t playback_control;
        binbot_audio_music_control_t music_control;
        binbot_audio_volume_set_t volume_set;
        binbot_audio_volume_result_t volume_result;
        binbot_audio_noise_state_t noise;
        binbot_audio_dialog_state_t dialog_state;
        binbot_audio_dialog_text_t dialog_text;
        binbot_audio_remote_command_t remote_command;
        binbot_audio_remote_state_t remote_state;
        binbot_audio_remote_result_t remote_result;
        binbot_audio_calendar_state_t calendar_state;
        binbot_component_command_t component_command;
        binbot_component_event_t component_event;
    } payload;
} binbot_audio_packet_t;

static inline void binbot_audio_packet_init(binbot_audio_packet_t *packet,
                                            binbot_audio_message_type_t type,
                                            uint32_t payload_size,
                                            uint64_t sequence,
                                            uint64_t timestamp_ns)
{
    memset(packet, 0, sizeof(*packet));
    packet->header.magic = BINBOT_AUDIO_PROTOCOL_MAGIC;
    packet->header.version = BINBOT_AUDIO_PROTOCOL_VERSION;
    packet->header.type = (uint16_t)type;
    packet->header.payload_size = payload_size;
    packet->header.sequence = sequence;
    packet->header.timestamp_ns = timestamp_ns;
}

static inline uint32_t binbot_audio_packet_size(const binbot_audio_packet_t *packet)
{
    return (uint32_t)sizeof(packet->header) + packet->header.payload_size;
}

static inline int binbot_audio_packet_is_valid(const binbot_audio_packet_t *packet,
                                               uint32_t received_size)
{
    if(received_size < sizeof(packet->header)) return 0;
    if(packet->header.magic != BINBOT_AUDIO_PROTOCOL_MAGIC) return 0;
    if(packet->header.version != BINBOT_AUDIO_PROTOCOL_VERSION) return 0;
    if(packet->header.payload_size > sizeof(packet->payload)) return 0;
    return received_size == binbot_audio_packet_size(packet);
}

#ifdef __cplusplus
}
#endif

#endif
