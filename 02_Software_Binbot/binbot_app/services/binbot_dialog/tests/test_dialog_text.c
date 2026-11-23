/* Exercise the service's JSON parser and IPC sender without a microphone,
 * credentials, or a network connection. */
#define main dialog_service_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>

static const char *turn = "00112233-4455-4677-8899-aabbccddeeff";

static void raw_control(dialog_t *dialog, const char *json)
{
    handle_control(dialog, (const uint8_t *)json, strlen(json));
}

static void control(dialog_t *dialog, const char *type, const char *text)
{
    char json[CONTROL_CAPACITY];
    int size = snprintf(json, sizeof(json),
                        "{\"type\":\"%s\",\"turn_id\":\"%s\",\"text\":\"%s\"}", type, turn, text);
    assert(size > 0 && (size_t)size < sizeof(json));
    handle_control(dialog, (const uint8_t *)json, (size_t)size);
}

static void expect_text(int fd, unsigned role, bool final, const char *text)
{
    binbot_audio_packet_t packet;
    ssize_t size = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
    assert(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_TEXT);
    assert(packet.header.payload_size == sizeof(packet.payload.dialog_text));
    assert(binbot_audio_dialog_text_is_valid(&packet.payload.dialog_text));
    uint8_t id[16]; assert(bbvp_uuid_parse(turn, id));
    assert(!memcmp(packet.payload.dialog_text.turn_id, id, sizeof(id)));
    assert(packet.payload.dialog_text.role == role && packet.payload.dialog_text.final == final);
    assert(!strcmp(packet.payload.dialog_text.text, text));
}

static void expect_empty(int fd)
{
    char buffer[32];
    assert(recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) < 0);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void calendar_json(void)
{
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    dialog_t dialog = {0}; dialog.audio.fd = sockets[0];
    const char *escaped =
        "{\"type\":\"device.calendar\",\"revision\":7,\"events\":[{"
        "\"event_id\":\"t1\",\"summary\":\"\\u6d4b\\u8bd5\\u65e5\\u7a0b 1\\uff1a\\u65e5\\u7a0b\\u540c\\u6b65\","
        "\"start_timestamp\":\"1792198800\",\"end_timestamp\":\"1792200000\","
        "\"location\":\"研发 \\\"A\\\" {会议室}\\n二楼\","
        "\"meeting_url\":\"https:\\/\\/example.com\\/meeting\"},"
        "{\"event_id\":\"t2\",\"summary\":\"下午讨论 \\ud834\\udd1e\"}],"
        "\"changes\":{\"added\":1,\"updated\":2,\"removed\":3}}";
    raw_control(&dialog, escaped);
    binbot_audio_packet_t packet;
    ssize_t size = recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
    assert(packet.header.type == BINBOT_AUDIO_MSG_CALENDAR_STATE);
    assert(packet.payload.calendar_state.revision == 7 && packet.payload.calendar_state.event_count == 2);
    assert(packet.payload.calendar_state.changes_added == (BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT | 1));
    assert(packet.payload.calendar_state.changes_updated == 2 && packet.payload.calendar_state.changes_removed == 3);
    assert(!strcmp(packet.payload.calendar_state.events[0].summary, "测试日程 1：日程同步"));
    assert(!strcmp(packet.payload.calendar_state.events[0].location, "研发 \"A\" {会议室}\n二楼"));
    assert(!strcmp(packet.payload.calendar_state.events[0].meeting_url, "https://example.com/meeting"));
    assert(!strcmp(packet.payload.calendar_state.events[1].summary, "下午讨论 \xf0\x9d\x84\x9e"));

    const char *invalid[] = {
        "{\"revision\":8,\"events\":[{\"event_id\":\"t3\",\"summary\":\"unfinished}",
        "{\"revision\":8,\"events\":[{\"event_id\":\"t3\",\"summary\":{}}]}",
        "{\"revision\":8.5,\"events\":[]}",
        "{\"revision\":-1,\"events\":[]}",
        "{\"revision\":8,\"events\":{}}",
        "{\"revision\":8,\"events\":[]} trailing",
        "{\"revision\":8,\"events\":[],\"changes\":[]}",
        "{\"revision\":8,\"events\":[],\"changes\":{\"added\":-1,\"updated\":0,\"removed\":0}}",
        "{\"revision\":8,\"events\":[],\"changes\":{\"added\":0,\"updated\":0.5,\"removed\":0}}",
        "{\"revision\":8,\"events\":[],\"changes\":{\"added\":0,\"updated\":\"1\",\"removed\":0}}",
        "{\"revision\":8,\"events\":[],\"changes\":{\"added\":0,\"updated\":0}}",
    };
    for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!parse_calendar_state(&dialog, (const uint8_t *)invalid[i], strlen(invalid[i])));
        assert(dialog.calendar_state.revision == 7 && dialog.calendar_state.event_count == 2);
        expect_empty(sockets[1]);
    }

    char title[181], json[1024];
    for(size_t i = 0; i < 60; ++i) memcpy(title + i * 3, "测", 3);
    title[180] = 0;
    snprintf(json, sizeof(json), "{\"type\":\"device.calendar\",\"revision\":8,\"events\":["
             "{\"summary\":\"ignored\",\"location\":\"ignored\"},{\"event_id\":\"long\",\"summary\":\"%s\"}]}", title);
    raw_control(&dialog, json);
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.payload.calendar_state.event_count == 1);
    assert(strlen(packet.payload.calendar_state.events[0].summary) == 126);
    assert(!memcmp(packet.payload.calendar_state.events[0].summary, title, 126));
    assert(!packet.payload.calendar_state.events[0].location[0]);

    raw_control(&dialog, "{\"type\":\"device.calendar\",\"revision\":9,\"events\":[]}");
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.payload.calendar_state.revision == 9 && !packet.payload.calendar_state.event_count);
    assert(!packet.payload.calendar_state.changes_added);
    raw_control(&dialog, "{\"type\":\"device.calendar\",\"revision\":10,\"events\":[],"
                "\"changes\":{\"added\":300,\"updated\":400,\"removed\":5}}");
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.payload.calendar_state.changes_added == UINT8_MAX);
    assert(packet.payload.calendar_state.changes_updated == UINT8_MAX && packet.payload.calendar_state.changes_removed == 5);
    close(sockets[0]); close(sockets[1]);
}

static void camera_publish_lease(void)
{
    dialog_t dialog = {0};
    dialog.ready = true;
    binbot_camera_publisher_init(&dialog.camera_publisher);
    dialog.camera_publisher.binary = "/bin/true";
    dialog.camera_publisher.destination = "rtsp://example.invalid/live/test";
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        signal(SIGTERM, SIG_DFL);
        for(;;) pause();
    }
    dialog.camera_publisher.pid = child;
    raw_control(&dialog, "{\"type\":\"camera.publish\",\"lease_seconds\":45}");
    uint64_t until = dialog.camera_publisher.lease_until_ms;
    assert(until > monotonic_ms() && until <= monotonic_ms() + 45000);
    raw_control(&dialog, "{\"type\":\"camera.publish\",\"lease_seconds\":91}");
    raw_control(&dialog, "{\"type\":\"camera.publish\",\"lease_seconds\":4}");
    assert(dialog.camera_publisher.lease_until_ms == until);
    raw_control(&dialog, "{\"type\":\"camera.publish\",\"lease_seconds\":0}");
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    binbot_camera_publisher_poll(&dialog.camera_publisher, monotonic_ms());
    assert(dialog.camera_publisher.pid == 0);
    binbot_camera_publisher_close(&dialog.camera_publisher);
}

static void stop_continuous_voice(void)
{
    for(unsigned state = BINBOT_DIALOG_STATE_LISTENING; state <= BINBOT_DIALOG_STATE_SPEAKING; ++state) {
        int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
        dialog_t dialog = {0};
        dialog.audio.fd = sockets[0];
        dialog.ready = dialog.turn_active = dialog.realtime_voice = true;
        dialog.state = (binbot_dialog_state_value_t)state;
        strcpy(dialog.turn_id_text, turn);
        assert(bbvp_uuid_parse(turn, dialog.turn_id));
        dialog.downlink_active = true;
        dialog.downlink_stream_id[0] = 3;
        binbot_audio_packet_t packet;
        binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_INPUT_END, 0, 0, 0);
        assert(send(sockets[1], &packet, sizeof(packet.header), 0) == (ssize_t)sizeof(packet.header));
        service_audio_ipc(&dialog);
        assert(!dialog.turn_active && !dialog.input_ended && !dialog.downlink_active);
        assert(dialog.state == BINBOT_DIALOG_STATE_IDLE);
        bool cancelled_playback = false;
        while(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0)
            if(packet.header.type == BINBOT_AUDIO_MSG_PLAYBACK_CANCEL) cancelled_playback = true;
        assert(cancelled_playback);
        assert(dialog.outbound.count == 1);
        assert(strstr((const char *)dialog.outbound.items[dialog.outbound.head].data + LWS_PRE, "turn.cancel"));
        assert(strstr((const char *)dialog.outbound.items[dialog.outbound.head].data + LWS_PRE, "user_stop"));
        close(sockets[0]); close(sockets[1]);
    }
}

static void barge_in_cancels_playback(void)
{
    for(unsigned ended = 0; ended < 2; ++ended) {
        int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
        dialog_t dialog = {0};
        dialog.audio.fd = sockets[0];
        dialog.turn_active = dialog.realtime_voice = true;
        dialog.state = BINBOT_DIALOG_STATE_SPEAKING;
        strcpy(dialog.turn_id_text, turn);
        assert(bbvp_uuid_parse(turn, dialog.turn_id));
        uint8_t stream_id[16] = {3};
        memcpy(dialog.downlink_stream_id, stream_id, sizeof(stream_id));
        dialog.downlink_active = !ended;
        strcpy(dialog.assistant_text, "old reply");

        raw_control(&dialog, "{\"type\":\"input.started\",\"turn_id\":\"11112233-4455-4677-8899-aabbccddeeff\","
                             "\"message_id\":\"11112233-4455-4677-8899-aabbccddeeff\"}");
        expect_empty(sockets[1]);
        assert(dialog.state == BINBOT_DIALOG_STATE_SPEAKING);
        raw_control(&dialog, "{\"type\":\"input.started\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                             "\"message_id\":\"invalid\"}");
        expect_empty(sockets[1]);

        raw_control(&dialog, "{\"type\":\"input.started\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                             "\"message_id\":\"11112233-4455-4677-8899-aabbccddeeff\"}");
        binbot_audio_packet_t packet;
        assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
        assert(packet.header.type == BINBOT_AUDIO_MSG_PLAYBACK_CANCEL);
        assert(!memcmp(packet.payload.playback_control.stream_id, stream_id, sizeof(stream_id)));
        assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
        assert(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE);
        assert(packet.payload.dialog_state.state == BINBOT_DIALOG_STATE_LISTENING);
        expect_empty(sockets[1]);
        assert(dialog.turn_active && !dialog.input_ended && !dialog.downlink_active);
        assert(!dialog.assistant_text[0] && !dialog.outbound.count);
        close(sockets[0]); close(sockets[1]);
    }
}

static void playback_text_progress(void)
{
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    dialog_t dialog = {0}; dialog.audio.fd = sockets[0];
    dialog.turn_active = dialog.realtime_voice = true;
    dialog.state = BINBOT_DIALOG_STATE_SPEAKING;
    strcpy(dialog.turn_id_text, turn);
    assert(bbvp_uuid_parse(turn, dialog.turn_id));
    memcpy(dialog.message_id, dialog.turn_id, 16);
    assert(bbvp_uuid_parse("11112233-4455-4677-8899-aabbccddeeff", dialog.downlink_stream_id));
    strcpy(dialog.assistant_text, "先完成工作，再喝一杯水。");
    const char *cue = "{\"type\":\"assistant.playback\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                      "\"stream_id\":\"11112233-4455-4677-8899-aabbccddeeff\",\"text\":\"先完成工作，\"}";
    raw_control(&dialog, cue);
    binbot_audio_packet_t packet;
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_TEXT);
    assert(packet.header.flags == BINBOT_AUDIO_FLAG_DIALOG_PLAYBACK);
    assert(binbot_audio_dialog_text_is_valid(&packet.payload.dialog_text));
    assert(!strcmp(packet.payload.dialog_text.text, "先完成工作，"));
    assert(!strcmp(dialog.assistant_text, "先完成工作，再喝一杯水。"));
    dialog.downlink_stream_id[0] ^= 1;
    raw_control(&dialog, cue); expect_empty(sockets[1]);
    dialog.downlink_stream_id[0] ^= 1;
    dialog.state = BINBOT_DIALOG_STATE_LISTENING;
    raw_control(&dialog, cue); expect_empty(sockets[1]);
    dialog.state = BINBOT_DIALOG_STATE_SPEAKING;
    strcpy(dialog.assistant_text, "另一轮回复");
    raw_control(&dialog, cue); expect_empty(sockets[1]);
    close(sockets[0]); close(sockets[1]);
}

int main(void)
{
    playback_text_progress();
    stop_continuous_voice();
    barge_in_cancels_playback();
    camera_publish_lease();
    calendar_json();
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    dialog_t dialog = {0}; dialog.audio.fd = sockets[0];
    dialog.turn_active = true;
    strcpy(dialog.turn_id_text, turn);
    assert(bbvp_uuid_parse(turn, dialog.turn_id));
    control(&dialog, "asr.partial", "今天");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_USER, false, "今天");
    assert(dialog.vad.transcript_received);
    dialog.vad.silence_ms = 300;
    dialog.vad.elapsed_ms = 1800;
    control(&dialog, "asr.partial", "今天");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_USER, false, "今天");
    assert(dialog.vad.silence_ms == 300);
    assert(dialog.vad.last_transcript_ms == 0);
    control(&dialog, "asr.partial", "今天深圳");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_USER, false, "今天深圳");
    assert(dialog.vad.silence_ms == 300 && dialog.vad.last_transcript_ms == 1800);
    control(&dialog, "asr.final", "今天深圳天气怎么样？");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_USER, true, "今天深圳天气怎么样？");
    control(&dialog, "assistant.delta", "深圳");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, false, "深圳");
    control(&dialog, "assistant.delta", "晴天");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, false, "深圳晴天");
    control(&dialog, "assistant.final", "深圳今天晴天。");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, true, "深圳今天晴天。");
    assert(dialog.turn_finishing);

    char long_reply[3001];
    for(size_t i = 0; i < 1000; ++i) memcpy(long_reply + i * 3, "晴", 3);
    long_reply[3000] = '\0';
    control(&dialog, "assistant.final", long_reply);
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, true, long_reply);

    memset(dialog.assistant_text, 'a', BINBOT_DIALOG_TEXT_MAX - 1);
    dialog.assistant_text[BINBOT_DIALOG_TEXT_MAX - 1] = '\0';
    control(&dialog, "assistant.delta", "晴天");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, false, dialog.assistant_text);
    assert(strlen(dialog.assistant_text) == BINBOT_DIALOG_TEXT_MAX - 1);
    control(&dialog, "assistant.final", "");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, true, dialog.assistant_text);

    dialog.turn_id_text[0] = 'f';
    control(&dialog, "asr.final", "旧对话");
    control(&dialog, "assistant.delta", "旧回复");
    control(&dialog, "assistant.final", "旧回复");
    expect_empty(sockets[1]);
    clear_turn(&dialog);
    assert(!dialog.assistant_text[0]);
    control(&dialog, "asr.partial", "已取消");
    expect_empty(sockets[1]);

    binbot_vad_config_t config = {
        .start_timeout_ms = 500, .end_silence_ms = 1200, .transcript_grace_ms = 2400,
        .max_utterance_ms = 30000,
        .frame_ms = 20, .mode = 3, .start_frames = 2,
    };
    assert(binbot_vad_init(&dialog.vad, &config));
    int opus_error;
    dialog.encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &opus_error);
    assert(dialog.encoder && opus_error == OPUS_OK);
    dialog.turn_active = true;
    dialog.state = BINBOT_DIALOG_STATE_LISTENING;
    binbot_audio_packet_t frame;
    binbot_audio_packet_init(&frame, BINBOT_AUDIO_MSG_PCM, sizeof(frame.payload.pcm), 0, 0);
    for(unsigned i = 0; i < 25; ++i) process_microphone_frame(&dialog, &frame);
    assert(dialog.turn_active && dialog.input_ended && dialog.state == BINBOT_DIALOG_STATE_THINKING);
    assert(dialog.outbound.count == 26);
    outbound_item_t *last = &dialog.outbound.items[25];
    assert(!last->binary && !memcmp(last->data + LWS_PRE, "{\"type\":\"audio.end\"", 19));
    binbot_audio_packet_t state;
    assert(recv(sockets[1], &state, sizeof(state), MSG_DONTWAIT) > 0);
    assert(state.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE &&
           state.payload.dialog_state.state == BINBOT_DIALOG_STATE_THINKING);

    dialog.state = BINBOT_DIALOG_STATE_IDLE;
    raw_control(&dialog, "{\"type\":\"hello.ack\",\"input_endpoint\":\"server_vad_v1\"}");
    assert(dialog.ready && dialog.server_endpoint);
    dialog.state = BINBOT_DIALOG_STATE_LISTENING;
    dialog.turn_active = true;
    dialog.input_ended = false;
    strcpy(dialog.turn_id_text, turn);
    strcpy(dialog.uplink_stream_id_text, "11112233-4455-4677-8899-aabbccddeeff");
    binbot_vad_reset(&dialog.vad);
    binbot_vad_note_transcript(&dialog.vad);
    for(unsigned i = 0; i < 180; ++i) {
        outbound_clear(&dialog);
        process_microphone_frame(&dialog, &frame);
        assert(!dialog.input_ended && dialog.state == BINBOT_DIALOG_STATE_LISTENING);
        assert(dialog.outbound.count == 1 && dialog.outbound.items[0].binary);
    }
    outbound_clear(&dialog);
    assert(dialog.vad.elapsed_ms == 3600);
    raw_control(&dialog, "{\"type\":\"audio.input_ended\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                         "\"stream_id\":\"00112233-4455-4677-8899-aabbccddeeff\",\"reason\":\"speech_ended\"}");
    assert(!dialog.input_ended);
    const char *ended = "{\"type\":\"audio.input_ended\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                        "\"stream_id\":\"11112233-4455-4677-8899-aabbccddeeff\",\"reason\":\"speech_ended\"}";
    raw_control(&dialog, ended);
    assert(dialog.input_ended && dialog.state == BINBOT_DIALOG_STATE_THINKING);
    assert(dialog.outbound.count == 0);
    assert(recv(sockets[1], &state, sizeof(state), MSG_DONTWAIT) > 0);
    assert(state.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE &&
           state.payload.dialog_state.state == BINBOT_DIALOG_STATE_THINKING);
    raw_control(&dialog, ended);
    process_microphone_frame(&dialog, &frame);
    assert(dialog.vad.elapsed_ms == 3600 && dialog.outbound.count == 0);
    expect_empty(sockets[1]);
    dialog.state = BINBOT_DIALOG_STATE_IDLE;
    raw_control(&dialog, "{\"type\":\"hello.ack\"}");
    assert(dialog.ready && !dialog.server_endpoint);

    raw_control(&dialog, "{\"type\":\"hello.ack\",\"input_endpoint\":\"realtime_voice_v1\"}");
    assert(dialog.realtime_voice && !dialog.server_endpoint);
    dialog.turn_active = true;
    dialog.input_ended = false;
    dialog.turn_finishing = false;
    dialog.state = BINBOT_DIALOG_STATE_SPEAKING;
    strcpy(dialog.turn_id_text, turn);
    assert(bbvp_uuid_parse(turn, dialog.turn_id));
    memcpy(dialog.message_id, dialog.turn_id, sizeof(dialog.message_id));
    dialog.vad.elapsed_ms = dialog.vad.config.max_utterance_ms + 200;
    for(unsigned i = 0; i < 3; ++i) {
        outbound_clear(&dialog);
        process_microphone_frame(&dialog, &frame);
        assert(!dialog.input_ended && dialog.state == BINBOT_DIALOG_STATE_SPEAKING);
        assert(dialog.outbound.count == 1 && dialog.outbound.items[0].binary);
    }
    control(&dialog, "assistant.final", "第一轮回答");
    expect_text(sockets[1], BINBOT_DIALOG_TEXT_ASSISTANT, true, "第一轮回答");
    assert(!dialog.turn_finishing && dialog.turn_active);
    raw_control(&dialog, "{\"type\":\"input.started\",\"turn_id\":\"00112233-4455-4677-8899-aabbccddeeff\","
                         "\"message_id\":\"11112233-4455-4677-8899-aabbccddeeff\"}");
    assert(dialog.turn_active && dialog.state == BINBOT_DIALOG_STATE_LISTENING);
    assert(!dialog.assistant_text[0] && !dialog.recognized_text[0]);
    assert(recv(sockets[1], &state, sizeof(state), MSG_DONTWAIT) > 0);
    assert(state.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE);
    control(&dialog, "asr.final", "第二轮问题");
    assert(recv(sockets[1], &state, sizeof(state), MSG_DONTWAIT) > 0);
    assert(state.header.type == BINBOT_AUDIO_MSG_DIALOG_TEXT);
    uint8_t message_id[16];
    assert(bbvp_uuid_parse("11112233-4455-4677-8899-aabbccddeeff", message_id));
    assert(!memcmp(state.payload.dialog_text.turn_id, message_id, sizeof(message_id)));
    assert(!memcmp(dialog.turn_id_text, turn, 37));
    opus_encoder_destroy(dialog.encoder);
    binbot_vad_destroy(&dialog.vad);
    close(sockets[0]); close(sockets[1]);
    puts("dialog text parsing, stream accumulation, final replacement, UTF-8 bounds and stale turns passed");
    return 0;
}
