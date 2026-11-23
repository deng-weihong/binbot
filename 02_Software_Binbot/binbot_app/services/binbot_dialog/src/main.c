#include "bbvp.h"
#include "binbot/agent/ipc.h"
#include "binbot/audio/client.h"
#include "retry_policy.h"
#include "camera_publish.h"
#include "vad.h"

#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <opus.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    OUTBOUND_CAPACITY = 32,
    OUTBOUND_DATA_CAPACITY = BBVP_AUDIO_HEADER_SIZE + BBVP_MAX_AUDIO_PAYLOAD,
    CONTROL_CAPACITY = 16384,
    HELLO_CAPACITY = 2048,
    AUDIO_SERVICE_INTERVAL_MS = 5,
    PING_INTERVAL_MS = 15000,
    PONG_TIMEOUT_MS = 45000,
};

typedef struct {
    bool binary;
    size_t size;
    uint8_t data[LWS_PRE + OUTBOUND_DATA_CAPACITY];
} outbound_item_t;

typedef struct {
    outbound_item_t items[OUTBOUND_CAPACITY];
    size_t head;
    size_t count;
} outbound_queue_t;

typedef struct {
    char type[32];
    char state[24];
    char turn_id[37];
    char stream_id[37];
    char code[64];
    char message[256];
    char command_id[37];
    char name[32];
    char index[16];
    char position_seconds[16];
    char track_index[16];
    char done[8];
    char text[CONTROL_CAPACITY];
    char asset_path[512];
    char calendar_revision[16];
    char component_fields[13][96];
    char display_profile[8];
    char input_endpoint[32];
    char reason[64];
    char camera_lease_seconds[8];
    char message_id[37];
} control_event_t;

typedef struct dialog dialog_t;

struct dialog {
    struct lws_context *context;
    struct lws *wsi;
    lws_sorted_usec_list_t reconnect_sul;
    lws_sorted_usec_list_t service_sul;
    size_t reconnect_attempt;
    bool reconnect_scheduled;
    bool ready;
    bool server_endpoint;
    bool realtime_voice;

    char url[512];
    char parsed_url[512];
    const char *scheme;
    const char *address;
    const char *path;
    int port;
    bool use_tls;
    char ca_file[256];
    char device_id[65];
    char token[513];
    char client_version[33];
    char agent_socket[108];
    uint32_t agent_timeout_ms;
    bool device_agent_enabled;
    binbot_camera_publisher_t camera_publisher;

    binbot_audio_client_t audio;
    bool audio_was_connected;
    uint64_t ipc_sequence;
    binbot_dialog_state_value_t state;

    outbound_queue_t outbound;
    uint8_t receive_buffer[CONTROL_CAPACITY];
    size_t receive_size;
    bool receive_binary;

    OpusEncoder *encoder;
    OpusDecoder *decoder;
    binbot_vad_t vad;
    uint32_t capture_delay_ms;
    uint64_t wake_timestamp_ms;
    uint64_t capture_base_ms;
    uint32_t uplink_sequence;

    bool turn_active;
    bool input_ended;
    bool turn_finishing;
    uint8_t turn_id[16];
    uint8_t message_id[16];
    uint8_t uplink_stream_id[16];
    char turn_id_text[37];
    char uplink_stream_id_text[37];
    char assistant_text[BINBOT_DIALOG_TEXT_MAX + 1];
    char recognized_text[BINBOT_DIALOG_TEXT_MAX + 1];

    bool downlink_active;
    uint8_t downlink_stream_id[16];
    uint32_t downlink_sequence;
    uint64_t downlink_timestamp_ms;

    uint64_t last_ping_ms;
    uint64_t last_pong_ms;

    binbot_audio_remote_state_t remote_state;
    bool remote_state_valid;
    binbot_audio_calendar_state_t calendar_state;
    bool calendar_state_valid;

    pthread_mutex_t agent_mutex;
    pthread_t agent_thread;
    bool agent_thread_active;
    bool agent_result_ready;
    bool agent_result_success;
    char agent_turn_id[37];
    char agent_response[BINBOT_AGENT_RESPONSE_MAX + 1];
    char agent_error[192];
};

typedef struct {
    dialog_t *dialog;
    char turn_id[37];
    char text[BINBOT_AGENT_REQUEST_MAX + 1];
} agent_job_t;

static volatile sig_atomic_t running = 1;
static dialog_t *active_dialog;

static bool outbound_push(dialog_t *dialog, const void *data, size_t size,
                          bool binary);
static bool turn_matches(const dialog_t *dialog, const control_event_t *event);
static void cancel_current_turn(dialog_t *dialog, const char *reason,
                                binbot_dialog_state_value_t final_state);
static bool parse_u32(const char *text, uint32_t maximum, uint32_t *value);

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL + (uint64_t)value.tv_nsec / 1000000ULL;
}

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static uint32_t environment_u32(const char *name, uint32_t fallback,
                                uint32_t minimum, uint32_t maximum)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if(!end || *end || value < minimum || value > maximum) {
        fprintf(stderr, "dialog: invalid %s, using %u\n", name, fallback);
        return fallback;
    }
    return (uint32_t)value;
}

static bool valid_device_id(const char *value)
{
    size_t length = strlen(value);
    if(length == 0 || length > 64) return false;
    for(size_t i = 0; i < length; ++i) {
        unsigned char character = (unsigned char)value[i];
        if(!isalnum(character) && character != '.' && character != '_' &&
           character != ':' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool json_escape(const char *input, char *output, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    size_t position = 0;
    for(size_t i = 0; input[i]; ++i) {
        unsigned char value = (unsigned char)input[i];
        if(value == '"' || value == '\\') {
            if(position + 2 >= capacity) return false;
            output[position++] = '\\';
            output[position++] = (char)value;
        }
        else if(value < 0x20) {
            if(position + 6 >= capacity) return false;
            memcpy(output + position, "\\u00", 4);
            output[position + 4] = hex[value >> 4];
            output[position + 5] = hex[value & 0x0fU];
            position += 6;
        }
        else {
            if(position + 1 >= capacity) return false;
            output[position++] = (char)value;
        }
    }
    if(position >= capacity) return false;
    output[position] = '\0';
    return true;
}

static void *agent_request_worker(void *opaque)
{
    agent_job_t *job = opaque;
    dialog_t *dialog = job->dialog;
    char response[BINBOT_AGENT_RESPONSE_MAX + 1] = {0};
    char error[192] = {0};
    bool success = binbot_agent_request(
        dialog->agent_socket, job->text, response, sizeof(response),
        dialog->agent_timeout_ms, error, sizeof(error));
    pthread_mutex_lock(&dialog->agent_mutex);
    snprintf(dialog->agent_turn_id, sizeof(dialog->agent_turn_id), "%s",
             job->turn_id);
    snprintf(dialog->agent_response, sizeof(dialog->agent_response), "%s",
             response);
    snprintf(dialog->agent_error, sizeof(dialog->agent_error), "%s",
             error[0] ? error : "device agent request failed");
    dialog->agent_result_success = success;
    dialog->agent_result_ready = true;
    pthread_mutex_unlock(&dialog->agent_mutex);
    lws_cancel_service(dialog->context);
    free(job);
    return NULL;
}

static bool start_agent_request(dialog_t *dialog,
                                const control_event_t *event)
{
    if(!dialog->device_agent_enabled || !turn_matches(dialog, event) ||
       !event->text[0] || strlen(event->text) > BINBOT_AGENT_REQUEST_MAX) return false;
    agent_job_t *job = calloc(1, sizeof(*job));
    if(!job) return false;
    job->dialog = dialog;
    snprintf(job->turn_id, sizeof(job->turn_id), "%s", event->turn_id);
    memcpy(job->text, event->text, strlen(event->text) + 1);

    pthread_mutex_lock(&dialog->agent_mutex);
    if(dialog->agent_thread_active) {
        pthread_mutex_unlock(&dialog->agent_mutex);
        free(job);
        return false;
    }
    dialog->agent_thread_active = true;
    dialog->agent_result_ready = false;
    int result = pthread_create(&dialog->agent_thread, NULL,
                                agent_request_worker, job);
    if(result != 0) dialog->agent_thread_active = false;
    pthread_mutex_unlock(&dialog->agent_mutex);
    if(result != 0) {
        fprintf(stderr, "dialog: cannot start device agent worker: %s\n",
                strerror(result));
        free(job);
        return false;
    }
    fprintf(stdout, "dialog: device agent request started\n");
    return true;
}

static bool send_agent_response_control(dialog_t *dialog, const char *turn_id,
                                        const char *response)
{
    char escaped[OUTBOUND_DATA_CAPACITY];
    char message[OUTBOUND_DATA_CAPACITY];
    if(!json_escape(response, escaped, sizeof(escaped))) return false;
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"agent.response\",\"turn_id\":\"%s\","
                          "\"text\":\"%s\"}", turn_id, escaped);
    return length > 0 && (size_t)length < sizeof(message) &&
           outbound_push(dialog, message, (size_t)length, false);
}

static void service_agent_result(dialog_t *dialog)
{
    char turn_id[37];
    char response[BINBOT_AGENT_RESPONSE_MAX + 1];
    char error[192];
    bool success;
    pthread_t thread;
    pthread_mutex_lock(&dialog->agent_mutex);
    if(!dialog->agent_result_ready) {
        pthread_mutex_unlock(&dialog->agent_mutex);
        return;
    }
    snprintf(turn_id, sizeof(turn_id), "%s", dialog->agent_turn_id);
    snprintf(response, sizeof(response), "%s", dialog->agent_response);
    snprintf(error, sizeof(error), "%s", dialog->agent_error);
    success = dialog->agent_result_success;
    thread = dialog->agent_thread;
    dialog->agent_result_ready = false;
    pthread_mutex_unlock(&dialog->agent_mutex);

    (void)pthread_join(thread, NULL);
    pthread_mutex_lock(&dialog->agent_mutex);
    dialog->agent_thread_active = false;
    pthread_mutex_unlock(&dialog->agent_mutex);

    if(!dialog->turn_active || strcmp(dialog->turn_id_text, turn_id) != 0) {
        fprintf(stdout, "dialog: discarded stale device agent response\n");
        return;
    }
    if(success && send_agent_response_control(dialog, turn_id, response)) {
        fprintf(stdout, "dialog: device agent response sent\n");
        return;
    }
    fprintf(stderr, "dialog: device agent failed: %s\n",
            success ? "response is too large" : error);
    cancel_current_turn(dialog, "device_agent_failed",
                        BINBOT_DIALOG_STATE_ERROR);
}

static void outbound_clear(dialog_t *dialog)
{
    dialog->outbound.head = 0;
    dialog->outbound.count = 0;
}

static bool outbound_push(dialog_t *dialog, const void *data, size_t size, bool binary)
{
    if(!data || size == 0 || size > OUTBOUND_DATA_CAPACITY ||
       dialog->outbound.count >= OUTBOUND_CAPACITY) {
        return false;
    }
    size_t tail = (dialog->outbound.head + dialog->outbound.count) %
                  OUTBOUND_CAPACITY;
    outbound_item_t *item = &dialog->outbound.items[tail];
    item->binary = binary;
    item->size = size;
    memcpy(item->data + LWS_PRE, data, size);
    dialog->outbound.count++;
    if(dialog->wsi) lws_callback_on_writable(dialog->wsi);
    return true;
}

static bool outbound_control(dialog_t *dialog, const char *format, ...)
{
    char message[HELLO_CAPACITY];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    return length > 0 && (size_t)length < sizeof(message) &&
           outbound_push(dialog, message, (size_t)length, false);
}

static void send_dialog_state(dialog_t *dialog)
{
    if(binbot_audio_client_fd(&dialog->audio) < 0) return;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_STATE,
                             sizeof(packet.payload.dialog_state),
                             dialog->ipc_sequence++, monotonic_ns());
    packet.payload.dialog_state.state = (uint16_t)dialog->state;
    packet.payload.dialog_state.error_code = 0;
    (void)binbot_audio_client_send(&dialog->audio, &packet);
}

static void set_state(dialog_t *dialog, binbot_dialog_state_value_t state)
{
    if(dialog->state == state) return;
    dialog->state = state;
    fprintf(stdout, "dialog: state=%u\n", (unsigned)state);
    send_dialog_state(dialog);
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if(length >= capacity) {
        length = capacity - 1;
        while(length && ((unsigned char)source[length] & 0xc0) == 0x80) --length;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void send_dialog_text_flags(dialog_t *dialog, binbot_dialog_text_role_t role,
                                    bool final, const char *text, uint32_t flags)
{
    if(binbot_audio_client_fd(&dialog->audio) < 0) return;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_TEXT,
                             sizeof(packet.payload.dialog_text),
                             dialog->ipc_sequence++, monotonic_ns());
    memcpy(packet.payload.dialog_text.turn_id,
           dialog->realtime_voice ? dialog->message_id : dialog->turn_id, 16);
    packet.payload.dialog_text.role = role;
    packet.payload.dialog_text.final = final;
    packet.header.flags = flags;
    copy_text(packet.payload.dialog_text.text, sizeof(packet.payload.dialog_text.text), text);
    (void)binbot_audio_client_send(&dialog->audio, &packet);
}

static void send_dialog_text(dialog_t *dialog, binbot_dialog_text_role_t role,
                              bool final, const char *text)
{
    send_dialog_text_flags(dialog, role, final, text, 0);
}

static bool send_playback_control(dialog_t *dialog,
                                  binbot_audio_message_type_t type,
                                  const uint8_t stream_id[16])
{
    if(binbot_audio_client_fd(&dialog->audio) < 0) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, type,
                             sizeof(packet.payload.playback_control),
                             dialog->ipc_sequence++, monotonic_ns());
    memcpy(packet.payload.playback_control.stream_id, stream_id, 16);
    return binbot_audio_client_send(&dialog->audio, &packet);
}

static void cancel_playback(dialog_t *dialog)
{
    static const uint8_t empty_id[16] = {0};
    if(dialog->downlink_active || (dialog->realtime_voice &&
       memcmp(dialog->downlink_stream_id, empty_id, 16))) {
        bool sent = send_playback_control(dialog, BINBOT_AUDIO_MSG_PLAYBACK_CANCEL,
                                          dialog->downlink_stream_id);
        fprintf(sent ? stdout : stderr,
                "dialog: playback cancel sent=%u frames=%u at_ms=%llu\n",
                (unsigned)sent, dialog->downlink_sequence,
                (unsigned long long)monotonic_ms());
    }
    dialog->downlink_active = false;
    dialog->downlink_sequence = 0;
    memset(dialog->downlink_stream_id, 0, sizeof(dialog->downlink_stream_id));
}

static bool turn_matches(const dialog_t *dialog, const control_event_t *event)
{
    return dialog->turn_active && event->turn_id[0] &&
           strcmp(dialog->turn_id_text, event->turn_id) == 0;
}

static void clear_turn(dialog_t *dialog)
{
    dialog->turn_active = false;
    dialog->input_ended = false;
    dialog->turn_finishing = false;
    dialog->turn_id_text[0] = '\0';
    dialog->uplink_stream_id_text[0] = '\0';
    dialog->assistant_text[0] = '\0';
    memset(dialog->turn_id, 0, sizeof(dialog->turn_id));
    memset(dialog->message_id, 0, sizeof(dialog->message_id));
    memset(dialog->uplink_stream_id, 0, sizeof(dialog->uplink_stream_id));
}

static void cancel_current_turn(dialog_t *dialog, const char *reason,
                                binbot_dialog_state_value_t final_state)
{
    if(!dialog->turn_active) {
        set_state(dialog, final_state);
        return;
    }
    char turn_id[37];
    memcpy(turn_id, dialog->turn_id_text, sizeof(turn_id));
    outbound_clear(dialog);
    set_state(dialog, BINBOT_DIALOG_STATE_CANCELLING);
    (void)outbound_control(
        dialog,
        "{\"type\":\"turn.cancel\",\"turn_id\":\"%s\",\"reason\":\"%s\"}",
        turn_id, reason);
    cancel_playback(dialog);
    clear_turn(dialog);
    set_state(dialog, final_state);
}

static void start_turn(dialog_t *dialog)
{
    if(!dialog->ready) {
        set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        return;
    }

    char cancelled_turn[37] = {0};
    if(dialog->turn_active) {
        memcpy(cancelled_turn, dialog->turn_id_text, sizeof(cancelled_turn));
        outbound_clear(dialog);
        set_state(dialog, BINBOT_DIALOG_STATE_CANCELLING);
        (void)outbound_control(
            dialog,
            "{\"type\":\"turn.cancel\",\"turn_id\":\"%s\","
            "\"reason\":\"user_interrupt\"}",
            cancelled_turn);
        cancel_playback(dialog);
        clear_turn(dialog);
    }

    if(!bbvp_uuid_generate(dialog->turn_id) ||
       !bbvp_uuid_generate(dialog->uplink_stream_id)) {
        fprintf(stderr, "dialog: cannot generate turn identifiers\n");
        set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    bbvp_uuid_format(dialog->turn_id, dialog->turn_id_text);
    memcpy(dialog->message_id, dialog->turn_id, sizeof(dialog->message_id));
    bbvp_uuid_format(dialog->uplink_stream_id, dialog->uplink_stream_id_text);
    dialog->turn_active = true;
    dialog->input_ended = false;
    dialog->turn_finishing = false;
    dialog->uplink_sequence = 0;
    dialog->capture_base_ms = 0;
    dialog->wake_timestamp_ms = monotonic_ms();
    binbot_vad_reset(&dialog->vad);
    dialog->recognized_text[0] = '\0';
    if(dialog->realtime_voice) {
        fprintf(stdout, "dialog: endpoint=seeduplex continuous microphone enabled\n");
    }
    else if(dialog->server_endpoint) {
        fprintf(stdout, "dialog: endpoint=server_silero local_max_ms=%u\n",
                dialog->vad.config.max_utterance_ms);
    }
    else {
        fprintf(stdout, "dialog: VAD engine=webrtc mode=%u silence_ms=%u transcript_grace_ms=%u max_ms=%u\n",
                dialog->vad.config.mode, dialog->vad.config.end_silence_ms,
                dialog->vad.config.transcript_grace_ms, dialog->vad.config.max_utterance_ms);
    }
    opus_encoder_ctl(dialog->encoder, OPUS_RESET_STATE);

    if(!outbound_control(
           dialog,
           "{\"type\":\"turn.start\",\"turn_id\":\"%s\","
           "\"stream_id\":\"%s\",\"wake_word\":\"你好米雅\"}",
           dialog->turn_id_text, dialog->uplink_stream_id_text)) {
        clear_turn(dialog);
        set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    set_state(dialog, BINBOT_DIALOG_STATE_LISTENING);
}

static void finish_input(dialog_t *dialog)
{
    if(!dialog->turn_active || dialog->input_ended) return;
    if(!outbound_control(
           dialog,
           "{\"type\":\"audio.end\",\"turn_id\":\"%s\","
           "\"stream_id\":\"%s\"}",
           dialog->turn_id_text, dialog->uplink_stream_id_text)) {
        cancel_current_turn(dialog, "uplink_backpressure",
                            BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    dialog->input_ended = true;
    set_state(dialog, BINBOT_DIALOG_STATE_THINKING);
}

static void process_microphone_frame(dialog_t *dialog,
                                     const binbot_audio_packet_t *packet)
{
    if(dialog->state == BINBOT_DIALOG_STATE_IDLE && !dialog->turn_active)
        binbot_vad_observe_noise(&dialog->vad, packet->payload.pcm.samples, BINBOT_AUDIO_FRAME_SAMPLES);
    if(!dialog->turn_active || dialog->input_ended ||
       (!dialog->realtime_voice && dialog->state != BINBOT_DIALOG_STATE_LISTENING)) {
        return;
    }
    uint64_t now = monotonic_ms();
    if(now - dialog->wake_timestamp_ms < dialog->capture_delay_ms) return;

    binbot_vad_result_t vad_result;
    if(dialog->realtime_voice) {
        vad_result = BINBOT_VAD_WAITING;
    }
    else if(dialog->server_endpoint) {
        dialog->vad.elapsed_ms += BINBOT_AUDIO_FRAME_MS;
        vad_result = dialog->vad.elapsed_ms >= dialog->vad.config.max_utterance_ms
            ? BINBOT_VAD_MAX_DURATION : BINBOT_VAD_WAITING;
    }
    else {
        vad_result = binbot_vad_process(
            &dialog->vad, packet->payload.pcm.samples, BINBOT_AUDIO_FRAME_SAMPLES);
    }
    if(vad_result == BINBOT_VAD_SPEECH_STARTED) {
        fprintf(stdout,
                "dialog: VAD speech started rms=%.1f elapsed_ms=%u\n",
                dialog->vad.last_rms, dialog->vad.elapsed_ms);
    }
    uint8_t encoded[BBVP_MAX_AUDIO_PAYLOAD];
    int encoded_size = opus_encode(
        dialog->encoder, packet->payload.pcm.samples, BINBOT_AUDIO_FRAME_SAMPLES,
        encoded, sizeof(encoded));
    if(encoded_size < 0) {
        fprintf(stderr, "dialog: Opus encode failed: %s\n",
                opus_strerror(encoded_size));
        cancel_current_turn(dialog, "opus_encode_failed",
                            BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    if(dialog->capture_base_ms == 0) {
        dialog->capture_base_ms = packet->header.timestamp_ns / 1000000ULL;
        if(dialog->capture_base_ms == 0) dialog->capture_base_ms = now;
    }
    bbvp_audio_frame_t frame = {
        .kind = BBVP_AUDIO_UPLINK_OPUS,
        .sequence = dialog->uplink_sequence,
        .timestamp_ms = dialog->capture_base_ms +
                        (uint64_t)dialog->uplink_sequence * BINBOT_AUDIO_FRAME_MS,
        .payload = encoded,
        .payload_size = (uint32_t)encoded_size,
    };
    memcpy(frame.stream_id, dialog->uplink_stream_id, 16);
    uint8_t wire[BBVP_AUDIO_HEADER_SIZE + BBVP_MAX_AUDIO_PAYLOAD];
    size_t wire_size = 0;
    if(!bbvp_audio_encode(wire, sizeof(wire), &frame, &wire_size) ||
       !outbound_push(dialog, wire, wire_size, true)) {
        fprintf(stderr, "dialog: uplink queue full; cancelling turn\n");
        cancel_current_turn(dialog, "uplink_backpressure",
                            BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    dialog->uplink_sequence++;

    if(vad_result == BINBOT_VAD_SPEECH_ENDED ||
       vad_result == BINBOT_VAD_MAX_DURATION) {
        fprintf(stdout,
                "dialog: VAD input complete result=%s frames=%u "
                "speech_ms=%u silence_ms=%u transcript_idle_ms=%u rms=%.1f peak_rms=%.1f\n",
                vad_result == BINBOT_VAD_MAX_DURATION ? "max_duration" : "speech_ended",
                dialog->uplink_sequence, dialog->vad.speech_ms,
                dialog->vad.silence_ms,
                dialog->vad.elapsed_ms - dialog->vad.last_transcript_ms,
                dialog->vad.last_rms, dialog->vad.peak_rms);
        finish_input(dialog);
    }
    else if(vad_result == BINBOT_VAD_START_TIMEOUT) {
        fprintf(stdout,
                "dialog: VAD initial capture complete frames=%u rms=%.1f peak_rms=%.1f\n",
                dialog->uplink_sequence, dialog->vad.last_rms,
                dialog->vad.peak_rms);
        finish_input(dialog);
    }
}

static bool append_value(char *destination, size_t capacity,
                         const char *value, size_t length)
{
    size_t used = strlen(destination);
    if(used + length >= capacity) return false;
    memcpy(destination + used, value, length);
    destination[used + length] = '\0';
    return true;
}

static const char * const control_paths[] = {
    "type", "state", "turn_id", "stream_id", "code", "message",
    "command_id", "name", "payload.index", "payload.position_seconds",
    "payload.track_index", "payload.done", "payload.text", "payload.asset_path",
    /* Transcripts and agent requests carry text at the top level. */
    "text",
    "revision",
    "payload.component_id", "payload.version", "payload.size_bytes",
    "payload.sha256", "payload.elf_sha256", "payload.before_boot_id",
    "payload.before_version", "payload.before_elf_sha256", "payload.allow_downgrade",
    "payload.cancel_requested", "payload.job_id",
    "payload.reconcile_only",
    "payload.file_name",
    "payload.display_profile",
    "input_endpoint",
    "reason",
    "lease_seconds",
    "message_id",
};

static signed char control_json_callback(struct lejp_ctx *context, char reason)
{
    control_event_t *event = context->user;
    if(!context->path_match) return 0;
    size_t index = (size_t)context->path_match - 1U;
    char *destination = NULL;
    size_t capacity = 0;
    switch(index) {
        case 0: destination = event->type; capacity = sizeof(event->type); break;
        case 1: destination = event->state; capacity = sizeof(event->state); break;
        case 2: destination = event->turn_id; capacity = sizeof(event->turn_id); break;
        case 3: destination = event->stream_id; capacity = sizeof(event->stream_id); break;
        case 4: destination = event->code; capacity = sizeof(event->code); break;
        case 5: destination = event->message; capacity = sizeof(event->message); break;
        case 6: destination = event->command_id; capacity = sizeof(event->command_id); break;
        case 7: destination = event->name; capacity = sizeof(event->name); break;
        case 8: destination = event->index; capacity = sizeof(event->index); break;
        case 9: destination = event->position_seconds;
                capacity = sizeof(event->position_seconds); break;
        case 10: destination = event->track_index;
                capacity = sizeof(event->track_index); break;
        case 11: destination = event->done; capacity = sizeof(event->done); break;
        case 12: destination = event->text; capacity = sizeof(event->text); break;
        case 13: destination = event->asset_path;
                 capacity = sizeof(event->asset_path); break;
        case 14: destination = event->text; capacity = sizeof(event->text); break;
        case 15: destination = event->calendar_revision;
                 capacity = sizeof(event->calendar_revision); break;
        case 29: destination = event->display_profile;
                 capacity = sizeof(event->display_profile); break;
        case 30: destination = event->input_endpoint;
                 capacity = sizeof(event->input_endpoint); break;
        case 31: destination = event->reason; capacity = sizeof(event->reason); break;
        case 32: destination = event->camera_lease_seconds;
                 capacity = sizeof(event->camera_lease_seconds); break;
        case 33: destination = event->message_id; capacity = sizeof(event->message_id); break;
        default:
            if(index >= 16 && index < 29) {
                destination = event->component_fields[index - 16];
                capacity = sizeof(event->component_fields[0]);
            } else return 0;
    }
    if(reason == LEJPCB_VAL_STR_START || reason == LEJPCB_VAL_NUM_INT ||
       reason == LEJPCB_VAL_TRUE || reason == LEJPCB_VAL_FALSE) {
        destination[0] = '\0';
    }
    if(reason == LEJPCB_VAL_TRUE) return append_value(destination, capacity, "true", 4) ? 0 : -1;
    if(reason == LEJPCB_VAL_FALSE) return append_value(destination, capacity, "false", 5) ? 0 : -1;
    if(reason & LEJP_FLAG_CB_IS_VALUE) {
        if(!append_value(destination, capacity, context->buf, context->npos)) return -1;
    }
    return 0;
}

static bool calendar_string(const cJSON *object, const char *key, char *output,
                            size_t capacity, bool truncate)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!item || cJSON_IsNull(item)) return true;
    const char *value = cJSON_GetStringValue(item);
    if(!value) return false;
    size_t length = strlen(value);
    if(length >= capacity) {
        if(!truncate) return false;
        length = capacity - 1;
        while(length && ((unsigned char)value[length] & 0xc0) == 0x80) --length;
    }
    memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

static bool send_calendar_state(dialog_t *dialog,
                                const binbot_audio_calendar_state_t *state)
{
    if(!dialog || !state || binbot_audio_client_fd(&dialog->audio) < 0) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_CALENDAR_STATE,
                             sizeof(packet.payload.calendar_state),
                             dialog->ipc_sequence++, monotonic_ns());
    packet.payload.calendar_state = *state;
    return binbot_audio_client_send(&dialog->audio, &packet);
}

static bool parse_calendar_state(dialog_t *dialog, const uint8_t *data, size_t size)
{
    if(!dialog || !data || size == 0 || size >= CONTROL_CAPACITY) return false;
    const char *end = NULL, *limit = (const char *)data + size;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)data, size, &end, false);
    if(!json) return false;
    while(end < limit && isspace((unsigned char)*end)) ++end;
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(json, "revision");
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(json, "events");
    bool valid = end == limit && cJSON_IsObject(json) && cJSON_IsNumber(revision) &&
                 revision->valuedouble >= 0 && revision->valuedouble <= UINT32_MAX &&
                 revision->valuedouble == (uint32_t)revision->valuedouble && cJSON_IsArray(events);
    binbot_audio_calendar_state_t state = {0};
    if(valid) state.revision = (uint32_t)revision->valuedouble;
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(json, "changes");
    if(valid && changes) {
        const char *names[] = {"added", "updated", "removed"};
        uint8_t *counts[] = {&state.changes_added, &state.changes_updated, &state.changes_removed};
        valid = cJSON_IsObject(changes);
        for(unsigned i = 0; valid && i < 3; ++i) {
            const cJSON *count = cJSON_GetObjectItemCaseSensitive(changes, names[i]);
            valid = cJSON_IsNumber(count) && count->valuedouble >= 0 &&
                    count->valuedouble <= UINT32_MAX && count->valuedouble == (uint32_t)count->valuedouble;
            if(valid) {
                unsigned maximum = i ? UINT8_MAX : BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT - 1;
                *counts[i] = count->valuedouble > maximum ? maximum : (uint8_t)count->valuedouble;
            }
        }
        state.changes_added |= BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT;
    }
    const cJSON *object;
    cJSON_ArrayForEach(object, events) {
        if(!valid || state.event_count == BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY) break;
        binbot_audio_calendar_event_t *event = &state.events[state.event_count];
        memset(event, 0, sizeof(*event));
        valid = cJSON_IsObject(object);
#define CALENDAR_FIELD(field, truncate) \
        valid = valid && calendar_string(object, #field, event->field, sizeof(event->field), truncate)
        CALENDAR_FIELD(event_id, false);
        CALENDAR_FIELD(summary, true);
        CALENDAR_FIELD(start_timestamp, false);
        CALENDAR_FIELD(end_timestamp, false);
        CALENDAR_FIELD(start_date, false);
        CALENDAR_FIELD(end_date, false);
        CALENDAR_FIELD(timezone, true);
        CALENDAR_FIELD(location, true);
        CALENDAR_FIELD(meeting_url, true);
        CALENDAR_FIELD(status, true);
#undef CALENDAR_FIELD
        if(valid && event->event_id[0]) ++state.event_count;
    }
    cJSON_Delete(json);
    if(!valid) return false;
    dialog->calendar_state = state;
    dialog->calendar_state_valid = true;
    (void)send_calendar_state(dialog, &state);
    fprintf(stdout, "dialog: received calendar revision=%u events=%u\n",
            state.revision, (unsigned)state.event_count);
    return true;
}

static bool parse_control_event(const uint8_t *data, size_t size,
                                control_event_t *event)
{
    memset(event, 0, sizeof(*event));
    struct lejp_ctx parser;
    lejp_construct(&parser, control_json_callback, event,
                   control_paths, LWS_ARRAY_SIZE(control_paths));
    int result = lejp_parse(&parser, data, (int)size);
    lejp_destruct(&parser);
    return result >= 0 && event->type[0] != '\0';
}

static bool parse_u32(const char *text, uint32_t maximum, uint32_t *value)
{
    if(!text || !text[0] || !value) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if(!end || *end || parsed > maximum) return false;
    *value = (uint32_t)parsed;
    return true;
}

static void send_remote_result_control(dialog_t *dialog,
                                       const uint8_t command_id[16],
                                       bool success,
                                       const char *message)
{
    char command_text[37];
    char escaped_message[257];
    bbvp_uuid_format(command_id, command_text);
    if(!json_escape(message ? message : "", escaped_message,
                    sizeof(escaped_message))) {
        snprintf(escaped_message, sizeof(escaped_message), "%s", "invalid result");
    }
    (void)outbound_control(
        dialog,
        "{\"type\":\"device.command.result\",\"command_id\":\"%s\","
        "\"success\":%s,\"message\":\"%s\"}",
        command_text, success ? "true" : "false", escaped_message);
}

static bool send_remote_state_control(dialog_t *dialog,
                                      const binbot_audio_remote_state_t *state)
{
    if(!dialog->ready || !state) return false;
    char message[HELLO_CAPACITY];
    int length = snprintf(
        message, sizeof(message),
        "{\"type\":\"device.state\",\"revision\":%u,\"todos\":[",
        state->revision);
    if(length < 0 || (size_t)length >= sizeof(message)) return false;
    size_t used = (size_t)length;
    size_t todo_count = state->todo_count <= 8U ? state->todo_count : 8U;
    for(size_t i = 0; i < todo_count; ++i) {
        char escaped[145];
        if(!json_escape(state->todos[i].text, escaped, sizeof(escaped))) return false;
        length = snprintf(
            message + used, sizeof(message) - used,
            "%s{\"text\":\"%s\",\"done\":%s}",
            i ? "," : "", escaped, state->todos[i].done ? "true" : "false");
        if(length < 0 || (size_t)length >= sizeof(message) - used) return false;
        used += (size_t)length;
    }
    char escaped_display[257];
    if(!json_escape(state->display_asset, escaped_display,
                    sizeof(escaped_display))) return false;
    length = snprintf(
        message + used, sizeof(message) - used,
        "],\"music\":{\"track_index\":%u,\"position_seconds\":%u,"
        "\"active\":%s,\"playing\":%s},\"display\":{\"gif\":\"%s\"}}",
        state->music_track_index, state->music_position_seconds,
        state->music_active ? "true" : "false",
        state->music_playing ? "true" : "false", escaped_display);
    return length > 0 && (size_t)length < sizeof(message) - used &&
           outbound_push(dialog, message, used + (size_t)length, false);
}

static bool remote_action(const char *name, binbot_audio_remote_action_t *action)
{
    if(strcmp(name, "music.play") == 0) *action = BINBOT_REMOTE_MUSIC_PLAY;
    else if(strcmp(name, "music.pause") == 0) *action = BINBOT_REMOTE_MUSIC_PAUSE;
    else if(strcmp(name, "music.stop") == 0) *action = BINBOT_REMOTE_MUSIC_STOP;
    else if(strcmp(name, "music.previous") == 0) *action = BINBOT_REMOTE_MUSIC_PREVIOUS;
    else if(strcmp(name, "music.next") == 0) *action = BINBOT_REMOTE_MUSIC_NEXT;
    else if(strcmp(name, "music.seek") == 0) *action = BINBOT_REMOTE_MUSIC_SEEK;
    else if(strcmp(name, "music.select") == 0) *action = BINBOT_REMOTE_MUSIC_SELECT;
    else if(strcmp(name, "todo.add") == 0) *action = BINBOT_REMOTE_TODO_ADD;
    else if(strcmp(name, "todo.set_done") == 0) *action = BINBOT_REMOTE_TODO_SET_DONE;
    else if(strcmp(name, "todo.edit") == 0) *action = BINBOT_REMOTE_TODO_EDIT;
    else if(strcmp(name, "todo.delete") == 0) *action = BINBOT_REMOTE_TODO_DELETE;
    else if(strcmp(name, "display.gif") == 0) *action = BINBOT_REMOTE_DISPLAY_GIF;
    else if(strcmp(name, "modules.image") == 0) *action = BINBOT_REMOTE_MODULE_IMAGE;
    else if(strcmp(name, "voice.wake") == 0) *action = BINBOT_REMOTE_VOICE_WAKE;
    else return false;
    return true;
}

static bool dispatch_remote_command(dialog_t *dialog,
                                    const control_event_t *event)
{
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_REMOTE_COMMAND,
                             sizeof(packet.payload.remote_command),
                             dialog->ipc_sequence++, monotonic_ns());
    binbot_audio_remote_command_t *command = &packet.payload.remote_command;
    binbot_audio_remote_action_t action;
    if(!bbvp_uuid_parse(event->command_id, command->command_id) ||
       !remote_action(event->name, &action)) {
        return false;
    }
    command->action = (uint16_t)action;
    if(action == BINBOT_REMOTE_VOICE_WAKE) {
        binbot_audio_packet_t wake;
        binbot_audio_packet_init(&wake, BINBOT_AUDIO_MSG_WAKE_REQUEST,
                                 sizeof(wake.payload.wake),
                                 dialog->ipc_sequence++, monotonic_ns());
        wake.payload.wake.hotword_index = 0;
        bool sent = binbot_audio_client_send(&dialog->audio, &wake);
        send_remote_result_control(dialog, command->command_id, sent,
                                   sent ? "voice wake requested"
                                        : "audio service is unavailable");
        return true;
    }
    if(action == BINBOT_REMOTE_TODO_ADD || action == BINBOT_REMOTE_TODO_EDIT) {
        if(!event->text[0]) return false;
        size_t text_size = strlen(event->text);
        if(text_size >= sizeof(command->text)) return false;
        memcpy(command->text, event->text, text_size + 1U);
    }
    if(action == BINBOT_REMOTE_TODO_EDIT ||
       action == BINBOT_REMOTE_TODO_SET_DONE ||
       action == BINBOT_REMOTE_TODO_DELETE) {
        if(!parse_u32(event->index, 7, &command->index)) return false;
    }
    if(action == BINBOT_REMOTE_TODO_SET_DONE) {
        if(strcmp(event->done, "true") == 0) command->value = 1;
        else if(strcmp(event->done, "false") != 0) return false;
    }
    if(action == BINBOT_REMOTE_MUSIC_SEEK &&
       !parse_u32(event->position_seconds, 86400, &command->value)) return false;
    if(action == BINBOT_REMOTE_MUSIC_SELECT &&
       !parse_u32(event->track_index, 255, &command->value)) return false;
    if(action == BINBOT_REMOTE_DISPLAY_GIF || action == BINBOT_REMOTE_MODULE_IMAGE) {
        if(!event->asset_path[0] || event->asset_path[0] != '/') return false;
        const char *scheme = dialog->use_tls ? "https" : "http";
        int length = snprintf(command->resource, sizeof(command->resource),
                              "%s://%s:%d%s", scheme, dialog->address,
                              dialog->port, event->asset_path);
        if(length < 0 || (size_t)length >= sizeof(command->resource)) return false;
        snprintf(command->text, sizeof(command->text), "%s",
                 strrchr(event->asset_path, '/') + 1);
        if(action == BINBOT_REMOTE_MODULE_IMAGE && event->display_profile[0] &&
           !parse_u32(event->display_profile, 2, &command->value)) return false;
    }
    if(!binbot_audio_client_send(&dialog->audio, &packet)) {
        send_remote_result_control(dialog, command->command_id, false,
                                   "device application is unavailable");
    }
    return true;
}

static bool dispatch_component_command(dialog_t *dialog, const control_event_t *event)
{
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_COMPONENT_COMMAND,
                             sizeof(packet.payload.component_command),
                             dialog->ipc_sequence++, monotonic_ns());
    binbot_component_command_t *command = &packet.payload.component_command;
    if(!bbvp_uuid_parse(event->command_id, command->request_id)) return false;
    if(!strcmp(event->component_fields[0], "slave-1")) command->reserved[3] = 1;
    else if(strcmp(event->component_fields[0], "component-1")) return false;
    if(!strcmp(event->name, "component.probe")) command->action = BINBOT_COMPONENT_PROBE;
    else if(!strcmp(event->name, "component.ota")) {
        command->action = BINBOT_COMPONENT_OTA;
        if(strcmp(event->command_id, event->component_fields[10]) ||
           !parse_u32(event->component_fields[2], 2 * 1024 * 1024, &command->size) ||
           strlen(event->component_fields[1]) >= sizeof(command->version) ||
           strlen(event->component_fields[6]) >= sizeof(command->before_version) ||
           strlen(event->component_fields[3]) != 64 || strlen(event->component_fields[4]) != 64 ||
           strlen(event->component_fields[7]) != 64 || strlen(event->component_fields[5]) != 16 ||
           strncmp(event->asset_path, "/v1/device/firmware/", strlen("/v1/device/firmware/"))) return false;
        char *end;
        command->before_boot_id = strtoull(event->component_fields[5], &end, 16);
        if(*end) return false;
        memcpy(command->version, event->component_fields[1], strlen(event->component_fields[1]) + 1);
        memcpy(command->before_version, event->component_fields[6], strlen(event->component_fields[6]) + 1);
        memcpy(command->sha256, event->component_fields[3], 65);
        memcpy(command->elf_sha256, event->component_fields[4], 65);
        memcpy(command->before_elf_sha256, event->component_fields[7], 65);
        command->allow_downgrade = !strcmp(event->component_fields[8], "true");
        command->cancel = !strcmp(event->component_fields[9], "true");
        command->reconcile_only = !strcmp(event->component_fields[11], "true");
        int length = snprintf(command->url, sizeof(command->url), "%s://%s:%d%s",
                              dialog->use_tls ? "https" : "http", dialog->address,
                              dialog->port, event->asset_path);
        if(length < 0 || (size_t)length >= sizeof(command->url)) return false;
    } else if(!strcmp(event->name, "component.file")) {
        if(command->reserved[3]) return false;
        command->action = BINBOT_COMPONENT_FILE_UPDATE;
        bool valid = strcmp(event->command_id, event->component_fields[10]) == 0 &&
                     strcmp(event->component_fields[12], "display.gif") == 0 &&
                     parse_u32(event->component_fields[2], 512 * 1024, &command->size) &&
                     strlen(event->component_fields[3]) == 64 &&
                     strncmp(event->asset_path, "/v1/device/component-assets/",
                             strlen("/v1/device/component-assets/")) == 0;
        if(!valid) {
            return false;
        }
        memcpy(command->file_name, event->component_fields[12],
               strlen(event->component_fields[12]) + 1);
        memcpy(command->sha256, event->component_fields[3], 65);
        command->cancel = !strcmp(event->component_fields[9], "true");
        int length = snprintf(command->url, sizeof(command->url), "%s://%s:%d%s",
                              dialog->use_tls ? "https" : "http", dialog->address,
                              dialog->port, event->asset_path);
        if(length < 0 || (size_t)length >= sizeof(command->url)) return false;
    } else return false;
    return binbot_audio_client_send(&dialog->audio, &packet);
}

static bool send_component_event(dialog_t *dialog, const binbot_component_event_t *event)
{
    static const char *states[] = {"", "preparing", "downloading", "transferring", "validating",
        "activating", "verifying", "reconciling", "succeeded", "failed", "cancelled", "probe"};
    static const char *profiles[] = {"", "round_screen", "holographic_screen", "showcase_turntable", "slave_mcu", "ambient_light"};
    if(!dialog->ready || event->status == 0 || event->status >= LWS_ARRAY_SIZE(states)) return false;
    char request[37], escaped[321], info[512] = "null";
    bbvp_uuid_format(event->request_id, request);
    if(!json_escape(event->message, escaped, sizeof(escaped))) return false;
    if(event->has_info) {
        char version[65], project[65];
        if(event->profile == 0 || event->profile >= LWS_ARRAY_SIZE(profiles) ||
           !json_escape(event->version, version, sizeof(version)) ||
           !json_escape(event->project, project, sizeof(project))) return false;
        snprintf(info, sizeof(info),
                 "{\"profile\":\"%s\",\"state\":%u,\"healthy\":%s,\"boot_id\":\"%016llx\","
                 "\"capacity\":%u,\"version\":\"%s\",\"project\":\"%s\",\"elf_sha256\":\"%s\"}",
                 profiles[event->profile], event->state, event->healthy ? "true" : "false",
                 (unsigned long long)event->boot_id, event->capacity, version, project, event->elf_sha256);
    }
    return outbound_control(dialog,
        "{\"type\":\"component.event\",\"request_id\":\"%s\",\"component_id\":\"%s\","
        "\"event_seq\":%llu,\"status\":\"%s\",\"bytes_written\":%u,\"total_bytes\":%u,"
        "\"info\":%s,\"message\":\"%s\"}", request, event->profile == 4 ? "slave-1" : "component-1",
        (unsigned long long)event->event_seq,
        states[event->status], event->bytes_written, event->total_bytes, info, escaped);
}

static bool playback_begin(dialog_t *dialog, const uint8_t stream_id[16])
{
    if(binbot_audio_client_fd(&dialog->audio) < 0) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN,
                             sizeof(packet.payload.playback_begin),
                             dialog->ipc_sequence++, monotonic_ns());
    memcpy(packet.payload.playback_begin.stream_id, stream_id, 16);
    packet.payload.playback_begin.sample_rate = BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE;
    packet.payload.playback_begin.channels = 1;
    packet.payload.playback_begin.frame_ms = BINBOT_AUDIO_FRAME_MS;
    return binbot_audio_client_send(&dialog->audio, &packet);
}

static void handle_control(dialog_t *dialog, const uint8_t *data, size_t size)
{
    control_event_t event;
    if(!parse_control_event(data, size, &event)) {
        fprintf(stderr, "dialog: rejected malformed server control frame\n");
        set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        return;
    }
    if(strcmp(event.type, "camera.publish") == 0) {
        uint32_t seconds;
        if(!dialog->ready || !parse_u32(event.camera_lease_seconds, 90, &seconds) ||
           !binbot_camera_publisher_lease(&dialog->camera_publisher, seconds, monotonic_ms()))
            fprintf(stderr, "dialog: rejected camera publishing lease\n");
        return;
    }
    if(strcmp(event.type, "hello.ack") == 0) {
        dialog->server_endpoint = !strcmp(event.input_endpoint, "server_vad_v1");
        dialog->realtime_voice = !strcmp(event.input_endpoint, "realtime_voice_v1");
        dialog->ready = true;
        dialog->reconnect_attempt = 0;
        dialog->last_ping_ms = monotonic_ms();
        dialog->last_pong_ms = dialog->last_ping_ms;
        set_state(dialog, BINBOT_DIALOG_STATE_IDLE);
        fprintf(stdout, "dialog: BBVP handshake complete input_endpoint=%s\n",
                dialog->realtime_voice ? "seeduplex" : (
                    dialog->server_endpoint ? "server_silero" : "local_webrtc"));
        if(dialog->remote_state_valid) {
            (void)send_remote_state_control(dialog, &dialog->remote_state);
        }
        if(dialog->calendar_state_valid) {
            (void)send_calendar_state(dialog, &dialog->calendar_state);
        }
        return;
    }
    if(strcmp(event.type, "input.started") == 0) {
        uint8_t message_id[16];
        if(dialog->realtime_voice && turn_matches(dialog, &event) &&
           bbvp_uuid_parse(event.message_id, message_id)) {
            fprintf(stdout, "dialog: realtime input started state=%u at_ms=%llu\n",
                    (unsigned)dialog->state, (unsigned long long)monotonic_ms());
            cancel_playback(dialog);
            memcpy(dialog->message_id, message_id, sizeof(dialog->message_id));
            dialog->assistant_text[0] = '\0';
            dialog->recognized_text[0] = '\0';
            set_state(dialog, BINBOT_DIALOG_STATE_LISTENING);
        }
        return;
    }
    if(strcmp(event.type, "audio.cancel") == 0) {
        if(dialog->realtime_voice && turn_matches(dialog, &event)) cancel_playback(dialog);
        return;
    }
    if(strcmp(event.type, "audio.input_ended") == 0) {
        if(dialog->server_endpoint && !dialog->input_ended && turn_matches(dialog, &event) &&
           !strcmp(event.stream_id, dialog->uplink_stream_id_text)) {
            fprintf(stdout, "dialog: server ended input reason=%s capture_ms=%u frames=%u\n",
                    event.reason, dialog->vad.elapsed_ms, dialog->uplink_sequence);
            dialog->input_ended = true;
            set_state(dialog, BINBOT_DIALOG_STATE_THINKING);
        }
        return;
    }
    if(strcmp(event.type, "device.calendar") == 0) {
        if(!parse_calendar_state(dialog, data, size)) {
            fprintf(stderr, "dialog: rejected malformed calendar snapshot\n");
        }
        return;
    }
    if(strcmp(event.type, "pong") == 0) {
        dialog->last_pong_ms = monotonic_ms();
        return;
    }
    if(strcmp(event.type, "device.command") == 0) {
        if(!strncmp(event.name, "component.", 10)) {
            if(!dispatch_component_command(dialog, &event))
                fprintf(stderr, "dialog: rejected %s command_id=%s component=%s\n",
                        event.name, event.command_id, event.component_fields[0]);
            return;
        }
        if(!dispatch_remote_command(dialog, &event)) {
            uint8_t command_id[16];
            if(bbvp_uuid_parse(event.command_id, command_id)) {
                send_remote_result_control(dialog, command_id, false,
                                           "invalid device command");
            }
        }
        return;
    }
    if(strcmp(event.type, "agent.request") == 0) {
        if(!start_agent_request(dialog, &event)) {
            fprintf(stderr, "dialog: rejected device agent request\n");
            if(turn_matches(dialog, &event)) {
                cancel_current_turn(dialog, "device_agent_unavailable",
                                    BINBOT_DIALOG_STATE_ERROR);
            }
        }
        return;
    }
    if(strcmp(event.type, "asr.partial") == 0 || strcmp(event.type, "asr.final") == 0) {
        if(turn_matches(dialog, &event)) {
            if(!dialog->input_ended && event.text[0] &&
               strcmp(dialog->recognized_text, event.text)) {
                copy_text(dialog->recognized_text, sizeof(dialog->recognized_text), event.text);
                binbot_vad_note_transcript(&dialog->vad);
                fprintf(stdout, "dialog: ASR progress capture_ms=%u silence_ms=%u\n",
                        dialog->vad.elapsed_ms, dialog->vad.silence_ms);
            }
            send_dialog_text(dialog, BINBOT_DIALOG_TEXT_USER,
                               !strcmp(event.type, "asr.final"), event.text);
        }
        return;
    }
    if(strcmp(event.type, "assistant.playback") == 0) {
        uint8_t stream_id[16];
        size_t length = strlen(event.text);
        if(turn_matches(dialog, &event) && dialog->state == BINBOT_DIALOG_STATE_SPEAKING &&
           bbvp_uuid_parse(event.stream_id, stream_id) &&
           !memcmp(stream_id, dialog->downlink_stream_id, 16) && length &&
           length <= strlen(dialog->assistant_text) &&
           !memcmp(event.text, dialog->assistant_text, length)) {
            send_dialog_text_flags(dialog, BINBOT_DIALOG_TEXT_ASSISTANT, false,
                                     event.text, BINBOT_AUDIO_FLAG_DIALOG_PLAYBACK);
        }
        return;
    }
    if(strcmp(event.type, "assistant.delta") == 0) {
        if(turn_matches(dialog, &event)) {
            size_t used = strlen(dialog->assistant_text);
            copy_text(dialog->assistant_text + used, sizeof(dialog->assistant_text) - used, event.text);
            send_dialog_text(dialog, BINBOT_DIALOG_TEXT_ASSISTANT, false, dialog->assistant_text);
        }
        return;
    }
    if(strcmp(event.type, "error") == 0) {
        if(!event.turn_id[0] || turn_matches(dialog, &event)) {
            fprintf(stderr, "dialog: server error code=%s message=%s\n",
                    event.code[0] ? event.code : "unknown",
                    event.message[0] ? event.message : "(none)");
            cancel_playback(dialog);
            clear_turn(dialog);
            /* A provider reporting no recognized speech is a recoverable
             * input miss, not a server outage.  Return to idle without
             * making the UI show the service-unavailable toast. */
            if(strcmp(event.code, "provider_failure") == 0 &&
               strcmp(event.message, "ASR returned no speech") == 0) {
                set_state(dialog, BINBOT_DIALOG_STATE_IDLE);
            }
            else {
                set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
            }
        }
        return;
    }
    if(strcmp(event.type, "turn.cancelled") == 0) {
        if(turn_matches(dialog, &event)) {
            cancel_playback(dialog);
            clear_turn(dialog);
            set_state(dialog, BINBOT_DIALOG_STATE_IDLE);
        }
        return;
    }
    if(strcmp(event.type, "audio.start") == 0) {
        uint8_t stream_id[16];
        if(turn_matches(dialog, &event) &&
           bbvp_uuid_parse(event.stream_id, stream_id)) {
            cancel_playback(dialog);
            /* The server creates a fresh Opus encoder for every downlink
             * stream. Reset the decoder too so its prediction state cannot
             * leak across independent responses. */
            int decoder_reset = opus_decoder_ctl(dialog->decoder,
                                                  OPUS_RESET_STATE);
            if(decoder_reset != OPUS_OK) {
                fprintf(stderr, "dialog: cannot reset Opus decoder: %s\n",
                        opus_strerror(decoder_reset));
                cancel_current_turn(dialog, "opus_decoder_reset_failed",
                                    BINBOT_DIALOG_STATE_ERROR);
                return;
            }
            memcpy(dialog->downlink_stream_id, stream_id, 16);
            dialog->downlink_sequence = 0;
            dialog->downlink_timestamp_ms = 0;
            if(playback_begin(dialog, stream_id)) {
                dialog->downlink_active = true;
            }
            else {
                cancel_current_turn(dialog, "playback_unavailable",
                                    BINBOT_DIALOG_STATE_ERROR);
            }
        }
        return;
    }
    if(strcmp(event.type, "audio.end") == 0) {
        uint8_t stream_id[16];
        if(turn_matches(dialog, &event) && dialog->downlink_active &&
           bbvp_uuid_parse(event.stream_id, stream_id) &&
           memcmp(stream_id, dialog->downlink_stream_id, 16) == 0) {
            (void)send_playback_control(dialog, BINBOT_AUDIO_MSG_PLAYBACK_END,
                                        stream_id);
            dialog->downlink_active = false;
        }
        return;
    }
    if(strcmp(event.type, "assistant.final") == 0) {
        if(turn_matches(dialog, &event)) {
            if(event.text[0]) copy_text(dialog->assistant_text, sizeof(dialog->assistant_text), event.text);
            send_dialog_text(dialog, BINBOT_DIALOG_TEXT_ASSISTANT, true,
                               event.text[0] ? event.text : dialog->assistant_text);
            dialog->turn_finishing = !dialog->realtime_voice;
        }
        return;
    }
    if(strcmp(event.type, "state") != 0) return;

    if(strcmp(event.state, "idle") == 0) {
        /* The server emits an error state followed by idle after a provider
         * failure.  The error handler clears the local turn before that idle
         * event arrives, so do not leave the UI latched in ERROR. */
        if(!dialog->turn_active || dialog->turn_finishing ||
           dialog->state == BINBOT_DIALOG_STATE_ERROR) {
            cancel_playback(dialog);
            clear_turn(dialog);
            set_state(dialog, BINBOT_DIALOG_STATE_IDLE);
        }
    }
    else if(turn_matches(dialog, &event)) {
        if(strcmp(event.state, "listening") == 0) {
            set_state(dialog, BINBOT_DIALOG_STATE_LISTENING);
        }
        else if(strcmp(event.state, "thinking") == 0) {
            set_state(dialog, BINBOT_DIALOG_STATE_THINKING);
        }
        else if(strcmp(event.state, "speaking") == 0) {
            set_state(dialog, BINBOT_DIALOG_STATE_SPEAKING);
        }
        else if(strcmp(event.state, "error") == 0) {
            set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        }
    }
}

static bool send_playback_frame(dialog_t *dialog, const int16_t *samples,
                                uint16_t sample_count)
{
    if(binbot_audio_client_fd(&dialog->audio) < 0) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_FRAME,
                             sizeof(packet.payload.playback_frame),
                             dialog->ipc_sequence++, monotonic_ns());
    memcpy(packet.payload.playback_frame.stream_id,
           dialog->downlink_stream_id, 16);
    packet.payload.playback_frame.sample_count = sample_count;
    memcpy(packet.payload.playback_frame.samples, samples,
           (size_t)sample_count * sizeof(samples[0]));
    return binbot_audio_client_send(&dialog->audio, &packet);
}

static bool handle_downlink_audio(dialog_t *dialog, const uint8_t *data, size_t size)
{
    bbvp_audio_frame_t frame;
    if(!bbvp_audio_decode(data, size, &frame) ||
       frame.kind != BBVP_AUDIO_DOWNLINK_OPUS) {
        fprintf(stderr, "dialog: invalid BBVP downlink frame\n");
        return false;
    }
    if(!dialog->turn_active || !dialog->downlink_active ||
       memcmp(frame.stream_id, dialog->downlink_stream_id, 16) != 0) {
        return true;
    }
    uint64_t expected_timestamp = dialog->downlink_sequence == 0
        ? frame.timestamp_ms
        : dialog->downlink_timestamp_ms + BINBOT_AUDIO_FRAME_MS;
    if(frame.sequence != dialog->downlink_sequence ||
       frame.timestamp_ms != expected_timestamp) {
        fprintf(stderr, "dialog: non-contiguous downlink audio\n");
        return false;
    }
    int16_t pcm[BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES];
    int samples = opus_decode(dialog->decoder, frame.payload,
                              (opus_int32)frame.payload_size, pcm,
                              BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES, 0);
    if(samples != BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES) {
        fprintf(stderr, "dialog: Opus decode returned %d samples\n", samples);
        return false;
    }
    if(!send_playback_frame(dialog, pcm, (uint16_t)samples)) {
        fprintf(stderr, "dialog: playback IPC backpressure\n");
        cancel_current_turn(dialog, "playback_backpressure",
                            BINBOT_DIALOG_STATE_ERROR);
        return true;
    }
    dialog->downlink_sequence++;
    dialog->downlink_timestamp_ms = frame.timestamp_ms;
    return true;
}

static void handle_complete_message(dialog_t *dialog)
{
    bool accepted = dialog->receive_binary
        ? handle_downlink_audio(dialog, dialog->receive_buffer, dialog->receive_size)
        : (handle_control(dialog, dialog->receive_buffer, dialog->receive_size), true);
    dialog->receive_size = 0;
    if(!accepted && dialog->wsi) {
        set_state(dialog, BINBOT_DIALOG_STATE_ERROR);
        lws_set_timeout(dialog->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
    }
}

static void schedule_reconnect(dialog_t *dialog);

static void handle_disconnected(dialog_t *dialog)
{
    dialog->wsi = NULL;
    dialog->ready = false;
    dialog->server_endpoint = false;
    dialog->receive_size = 0;
    outbound_clear(dialog);
    cancel_playback(dialog);
    dialog->realtime_voice = false;
    clear_turn(dialog);
    set_state(dialog, BINBOT_DIALOG_STATE_DISCONNECTED);
}

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *input, size_t length)
{
    dialog_t *dialog = user;
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            if(!dialog) return -1;
            dialog->wsi = wsi;
            dialog->reconnect_scheduled = false;
            outbound_clear(dialog);
            char escaped_device[129];
            char escaped_token[1025];
            char escaped_version[65];
            dialog->device_agent_enabled = access(dialog->agent_socket, F_OK) == 0;
            const char *agent_capability = dialog->device_agent_enabled
                ? ",\"device_agent_v1\"" : "";
            if(!json_escape(dialog->device_id, escaped_device, sizeof(escaped_device)) ||
               !json_escape(dialog->token, escaped_token, sizeof(escaped_token)) ||
               !json_escape(dialog->client_version, escaped_version,
                            sizeof(escaped_version)) ||
               !outbound_control(
                   dialog,
                   "{\"type\":\"hello\",\"protocol_version\":1,"
                   "\"device_id\":\"%s\",\"client_version\":\"%s\","
                   "\"token\":\"%s\","
                   "\"uplink\":{\"codec\":\"opus\",\"sample_rate\":16000,"
                   "\"channels\":1,\"frame_ms\":20},"
                   "\"downlink\":{\"codec\":\"opus\",\"sample_rate\":24000,"
                   "\"channels\":1,\"frame_ms\":20},"
                   "\"capabilities\":[\"turn_cancel\",\"remote_control_v1\",\"calendar_v1\",\"component_ota_v1\",\"server_vad_v1\",\"realtime_voice_v1\"%s%s],"
                   "\"camera\":{\"transport\":\"rtsp\",\"port\":554,"
                   "\"path\":\"/live/binbot\"}}",
                   escaped_device, escaped_version, escaped_token,
                   agent_capability,
                   binbot_camera_publisher_available(&dialog->camera_publisher)
                       ? ",\"camera_publish_v1\"" : "")) {
                return -1;
            }
            fprintf(stdout, "dialog: WebSocket connected, sending hello%s\n",
                    dialog->device_agent_enabled ? " with device agent" : "");
            return 0;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            if(!dialog || dialog->outbound.count == 0) return 0;
            outbound_item_t *item = &dialog->outbound.items[dialog->outbound.head];
            enum lws_write_protocol protocol = item->binary
                ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            int written = lws_write(wsi, item->data + LWS_PRE, item->size, protocol);
            if(written != (int)item->size) return -1;
            dialog->outbound.head =
                (dialog->outbound.head + 1U) % OUTBOUND_CAPACITY;
            dialog->outbound.count--;
            if(dialog->outbound.count) lws_callback_on_writable(wsi);
            return 0;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if(!dialog || !input || length == 0) return -1;
            bool binary = lws_frame_is_binary(wsi) != 0;
            if(dialog->receive_size == 0) dialog->receive_binary = binary;
            if(dialog->receive_binary != binary ||
               dialog->receive_size + length > sizeof(dialog->receive_buffer)) {
                return -1;
            }
            memcpy(dialog->receive_buffer + dialog->receive_size, input, length);
            dialog->receive_size += length;
            if(lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                handle_complete_message(dialog);
            }
            return 0;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            if(dialog) {
                fprintf(stderr, "dialog: WebSocket connection failed: %s\n",
                        input ? (const char *)input : "unknown error");
                handle_disconnected(dialog);
                schedule_reconnect(dialog);
            }
            return 0;
        case LWS_CALLBACK_CLIENT_CLOSED:
            if(dialog) {
                fprintf(stderr, "dialog: WebSocket closed\n");
                handle_disconnected(dialog);
                schedule_reconnect(dialog);
            }
            return 0;
        default:
            return lws_callback_http_dummy(wsi, reason, user, input, length);
    }
}

static const struct lws_protocols protocols[] = {
    {"binbot-bbvp", websocket_callback, 0, CONTROL_CAPACITY, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM,
};

static void connect_client(lws_sorted_usec_list_t *scheduled)
{
    dialog_t *dialog = lws_container_of(
        scheduled, dialog_t, reconnect_sul);
    dialog->reconnect_scheduled = false;
    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));
    info.context = dialog->context;
    info.address = dialog->address;
    info.port = dialog->port;
    info.path = dialog->path;
    info.host = dialog->address;
    info.origin = dialog->address;
    info.ssl_connection = dialog->use_tls ? LCCSCF_USE_SSL : 0;
    info.protocol = protocols[0].name;
    info.local_protocol_name = protocols[0].name;
    info.pwsi = &dialog->wsi;
    info.userdata = dialog;
    if(!lws_client_connect_via_info(&info)) schedule_reconnect(dialog);
}

static void schedule_reconnect(dialog_t *dialog)
{
    if(!running || dialog->reconnect_scheduled) return;
    uint32_t delay = binbot_retry_delay_ms(dialog->reconnect_attempt++);
    dialog->reconnect_scheduled = true;
    fprintf(stderr, "dialog: reconnecting in %u ms\n", delay);
    lws_sul_schedule(dialog->context, 0, &dialog->reconnect_sul,
                     connect_client, (lws_usec_t)delay * LWS_US_PER_MS);
}

static void service_audio_ipc(dialog_t *dialog)
{
    bool was_connected = binbot_audio_client_fd(&dialog->audio) >= 0;
    (void)binbot_audio_client_connect(&dialog->audio, monotonic_ms());
    bool connected = binbot_audio_client_fd(&dialog->audio) >= 0;
    if(!was_connected && connected) {
        fprintf(stdout, "dialog: audio IPC connected\n");
        send_dialog_state(dialog);
        if(dialog->calendar_state_valid) {
            (void)send_calendar_state(dialog, &dialog->calendar_state);
        }
    }
    while(binbot_audio_client_fd(&dialog->audio) >= 0) {
        binbot_audio_packet_t packet;
        int received = binbot_audio_client_receive(&dialog->audio, &packet);
        if(received <= 0) break;
        if(packet.header.type == BINBOT_AUDIO_MSG_WAKE_DETECTED &&
           packet.header.payload_size == sizeof(packet.payload.wake)) {
            if(dialog->turn_active) {
                /* A second hotword while listening is usually an acoustic
                 * echo or a repeated wake attempt.  Restarting here drops
                 * the active audio turn before ASR can finish. */
                fprintf(stdout, "dialog: wake ignored while turn active\n");
            } else {
                fprintf(stdout, "dialog: wake detected\n");
                start_turn(dialog);
            }
        }
        else if(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END &&
                packet.header.payload_size == 0 && dialog->turn_active) {
            if(dialog->realtime_voice || dialog->state != BINBOT_DIALOG_STATE_LISTENING) {
                cancel_current_turn(dialog, "user_stop", BINBOT_DIALOG_STATE_IDLE);
            } else finish_input(dialog);
        }
        else if(packet.header.type == BINBOT_AUDIO_MSG_PCM &&
                packet.header.payload_size == sizeof(packet.payload.pcm)) {
            process_microphone_frame(dialog, &packet);
        }
        else if(packet.header.type == BINBOT_AUDIO_MSG_REMOTE_STATE &&
                packet.header.payload_size == sizeof(packet.payload.remote_state)) {
            dialog->remote_state = packet.payload.remote_state;
            dialog->remote_state_valid = true;
            (void)send_remote_state_control(dialog, &dialog->remote_state);
        }
        else if(packet.header.type == BINBOT_AUDIO_MSG_COMPONENT_EVENT &&
                packet.header.payload_size == sizeof(packet.payload.component_event)) {
            (void)send_component_event(dialog, &packet.payload.component_event);
        }
        else if(packet.header.type == BINBOT_AUDIO_MSG_REMOTE_RESULT &&
                packet.header.payload_size == sizeof(packet.payload.remote_result)) {
            char result_message[sizeof(packet.payload.remote_result.message) + 1U];
            memcpy(result_message, packet.payload.remote_result.message,
                   sizeof(packet.payload.remote_result.message));
            result_message[sizeof(packet.payload.remote_result.message)] = '\0';
            if(dialog->ready) {
                send_remote_result_control(
                    dialog, packet.payload.remote_result.command_id,
                    packet.payload.remote_result.success != 0, result_message);
            }
        }
    }
    connected = binbot_audio_client_fd(&dialog->audio) >= 0;
    if(dialog->audio_was_connected && !connected) {
        fprintf(stderr, "dialog: audio IPC disconnected\n");
        cancel_current_turn(dialog, "audio_service_lost",
                            dialog->ready ? BINBOT_DIALOG_STATE_ERROR
                                          : BINBOT_DIALOG_STATE_DISCONNECTED);
    }
    dialog->audio_was_connected = connected;
}

static void service_heartbeat(dialog_t *dialog)
{
    if(!dialog->ready || !dialog->wsi) return;
    uint64_t now = monotonic_ms();
    if(now - dialog->last_ping_ms >= PING_INTERVAL_MS) {
        if(outbound_control(dialog,
                            "{\"type\":\"ping\",\"nonce\":\"%llu\"}",
                            (unsigned long long)now)) {
            dialog->last_ping_ms = now;
        }
    }
    if(now - dialog->last_pong_ms >= PONG_TIMEOUT_MS) {
        fprintf(stderr, "dialog: BBVP pong timed out\n");
        lws_set_timeout(dialog->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
        dialog->last_pong_ms = now;
    }
}

static void service_periodic_io(lws_sorted_usec_list_t *scheduled)
{
    dialog_t *dialog = lws_container_of(scheduled, dialog_t, service_sul);
    service_agent_result(dialog);
    service_audio_ipc(dialog);
    service_heartbeat(dialog);
    binbot_camera_publisher_poll(&dialog->camera_publisher, monotonic_ms());
    if(running) {
        lws_sul_schedule(dialog->context, 0, &dialog->service_sul,
                         service_periodic_io,
                         AUDIO_SERVICE_INTERVAL_MS * LWS_US_PER_MS);
    }
}

static bool initialize_config(dialog_t *dialog)
{
    const char *url = getenv("BINBOT_DIALOG_URL");
    if(!url || !url[0]) url = "ws://127.0.0.1:8080/v1/device/ws";
    const char *device_id = getenv("BINBOT_DEVICE_ID");
    if(!device_id || !device_id[0]) device_id = "binbot-001";
    const char *token = getenv("BINBOT_DEVICE_TOKEN");
    if(!token) token = "";
    const char *client_version = getenv("BINBOT_CLIENT_VERSION");
    if(!client_version || !client_version[0]) client_version = "0.3.0";
    const char *agent_socket = getenv("BINBOT_AGENT_SOCKET");
    if(!agent_socket || !agent_socket[0]) agent_socket = "/tmp/binbot_agent.sock";
    if(strlen(url) >= sizeof(dialog->url) || !valid_device_id(device_id) ||
       strlen(token) >= sizeof(dialog->token) ||
       strlen(client_version) >= sizeof(dialog->client_version) ||
       strlen(agent_socket) >= sizeof(dialog->agent_socket)) {
        fprintf(stderr, "dialog: invalid URL, device id, or token configuration\n");
        return false;
    }
    snprintf(dialog->url, sizeof(dialog->url), "%s", url);
    snprintf(dialog->parsed_url, sizeof(dialog->parsed_url), "%s", url);
    snprintf(dialog->device_id, sizeof(dialog->device_id), "%s", device_id);
    snprintf(dialog->token, sizeof(dialog->token), "%s", token);
    snprintf(dialog->client_version, sizeof(dialog->client_version), "%s",
             client_version);
    snprintf(dialog->agent_socket, sizeof(dialog->agent_socket), "%s",
             agent_socket);
    dialog->agent_timeout_ms = environment_u32(
        "BINBOT_AGENT_IPC_TIMEOUT_MS", 90000, 5000, 180000);
    if(lws_parse_uri(dialog->parsed_url, &dialog->scheme, &dialog->address,
                     &dialog->port, &dialog->path) != 0 ||
       (strcmp(dialog->scheme, "ws") != 0 && strcmp(dialog->scheme, "wss") != 0)) {
        fprintf(stderr, "dialog: BINBOT_DIALOG_URL must be ws:// or wss://\n");
        return false;
    }
    dialog->use_tls = strcmp(dialog->scheme, "wss") == 0;
    const char *ca_file = getenv("BINBOT_DIALOG_CA_FILE");
    if(ca_file && ca_file[0]) {
        if(strlen(ca_file) >= sizeof(dialog->ca_file)) return false;
        snprintf(dialog->ca_file, sizeof(dialog->ca_file), "%s", ca_file);
    }
    dialog->capture_delay_ms = environment_u32(
        /* The wake notification is played through the same speaker that is
         * acoustically coupled to the microphone.  The shipped notification
         * is about two seconds long; starting VAD after 250 ms makes the
         * notification itself look like user speech and leaves the turn
         * stuck until max-utterance. */
        "BINBOT_DIALOG_CAPTURE_DELAY_MS", 950, 0, 5000);
    binbot_vad_config_t vad_config = {
        .start_timeout_ms = environment_u32(
            "BINBOT_DIALOG_START_TIMEOUT_MS", 5000, 500, 30000),
        .end_silence_ms = environment_u32(
            "BINBOT_DIALOG_END_SILENCE_MS", 1200, 200, 5000),
        .transcript_grace_ms = environment_u32(
            "BINBOT_DIALOG_TRANSCRIPT_GRACE_MS", 2400, 200, 5000),
        .max_utterance_ms = environment_u32(
            "BINBOT_DIALOG_MAX_UTTERANCE_MS", 30000, 1000, 30000),
        .frame_ms = BINBOT_AUDIO_FRAME_MS,
        .mode = (uint16_t)environment_u32("BINBOT_DIALOG_VAD_MODE", 3, 0, 3),
        .start_frames = (uint16_t)environment_u32(
            "BINBOT_DIALOG_VAD_START_FRAMES", 2, 1, 20),
    };
    if(!binbot_vad_init(&dialog->vad, &vad_config)) {
        fprintf(stderr, "dialog: cannot initialize WebRTC VAD\n");
        return false;
    }
    return true;
}

static void stop_requested(int signal_number)
{
    (void)signal_number;
    running = 0;
}

int main(int argc, char **argv)
{
    if(argc > 1 && strcmp(argv[1], "--camera-publisher") == 0)
        return binbot_camera_publisher_exec(argc - 2, argv + 2);
    signal(SIGINT, stop_requested);
    signal(SIGTERM, stop_requested);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    static dialog_t dialog;
    memset(&dialog, 0, sizeof(dialog));
    binbot_camera_publisher_init(&dialog.camera_publisher);
    dialog.state = BINBOT_DIALOG_STATE_DISCONNECTED;
    active_dialog = &dialog;
    if(pthread_mutex_init(&dialog.agent_mutex, NULL) != 0) {
        fprintf(stderr, "dialog: cannot initialize agent worker mutex\n");
        return EXIT_FAILURE;
    }
    if(!initialize_config(&dialog)) {
        pthread_mutex_destroy(&dialog.agent_mutex);
        return EXIT_FAILURE;
    }

    int opus_error = OPUS_OK;
    dialog.encoder = opus_encoder_create(
        BINBOT_AUDIO_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if(!dialog.encoder || opus_error != OPUS_OK) {
        fprintf(stderr, "dialog: cannot create Opus encoder: %s\n",
                opus_strerror(opus_error));
        binbot_vad_destroy(&dialog.vad);
        pthread_mutex_destroy(&dialog.agent_mutex);
        return EXIT_FAILURE;
    }
    (void)opus_encoder_ctl(dialog.encoder, OPUS_SET_BITRATE(24000));
    (void)opus_encoder_ctl(dialog.encoder, OPUS_SET_COMPLEXITY(5));
    dialog.decoder = opus_decoder_create(
        BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE, 1, &opus_error);
    if(!dialog.decoder || opus_error != OPUS_OK) {
        fprintf(stderr, "dialog: cannot create Opus decoder: %s\n",
                opus_strerror(opus_error));
        opus_encoder_destroy(dialog.encoder);
        binbot_vad_destroy(&dialog.vad);
        pthread_mutex_destroy(&dialog.agent_mutex);
        return EXIT_FAILURE;
    }

    binbot_audio_client_init(
        &dialog.audio, BINBOT_AUDIO_ROLE_DIALOG,
        BINBOT_AUDIO_CAP_PCM_LIVE | BINBOT_AUDIO_CAP_EVENTS |
            BINBOT_AUDIO_CAP_REMOTE_EVENTS,
        0, getenv("BINBOT_AUDIO_SOCKET"));

    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
    struct lws_context_creation_info context_info;
    memset(&context_info, 0, sizeof(context_info));
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    context_info.fd_limit_per_thread = 4;
    if(dialog.ca_file[0]) context_info.client_ssl_ca_filepath = dialog.ca_file;
    dialog.context = lws_create_context(&context_info);
    if(!dialog.context) {
        fprintf(stderr, "dialog: cannot initialize libwebsockets\n");
        binbot_audio_client_close(&dialog.audio);
        opus_decoder_destroy(dialog.decoder);
        opus_encoder_destroy(dialog.encoder);
        binbot_vad_destroy(&dialog.vad);
        pthread_mutex_destroy(&dialog.agent_mutex);
        return EXIT_FAILURE;
    }

    fprintf(stdout, "dialog: starting device_id=%s endpoint=%s\n",
            dialog.device_id, dialog.url);
    /* lws_service() ignores its timeout argument on modern libwebsockets.  A
       scheduled callback keeps the external audio IPC draining promptly. */
    lws_sul_schedule(dialog.context, 0, &dialog.service_sul,
                     service_periodic_io, 1);
    lws_sul_schedule(dialog.context, 0, &dialog.reconnect_sul,
                     connect_client, 1);
    while(running) {
        int service_result = lws_service(dialog.context, 0);
        if(service_result < 0) {
            fprintf(stderr, "dialog: libwebsockets service failed: %d\n",
                    service_result);
            break;
        }
    }

    if(dialog.turn_active && dialog.ready) {
        cancel_current_turn(&dialog, "client_shutdown",
                            BINBOT_DIALOG_STATE_DISCONNECTED);
    }
    cancel_playback(&dialog);
    binbot_camera_publisher_close(&dialog.camera_publisher);
    set_state(&dialog, BINBOT_DIALOG_STATE_DISCONNECTED);
    pthread_mutex_lock(&dialog.agent_mutex);
    bool join_agent = dialog.agent_thread_active;
    pthread_t agent_thread = dialog.agent_thread;
    pthread_mutex_unlock(&dialog.agent_mutex);
    if(join_agent) (void)pthread_join(agent_thread, NULL);
    binbot_audio_client_close(&dialog.audio);
    lws_context_destroy(dialog.context);
    opus_decoder_destroy(dialog.decoder);
    opus_encoder_destroy(dialog.encoder);
    binbot_vad_destroy(&dialog.vad);
    pthread_mutex_destroy(&dialog.agent_mutex);
    active_dialog = NULL;
    fprintf(stdout, "dialog: stopped cleanly\n");
    return EXIT_SUCCESS;
}
