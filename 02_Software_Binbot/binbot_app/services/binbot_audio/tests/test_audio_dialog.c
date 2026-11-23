/* Check event routing against real packet handlers using isolated sockets. */
#define main audio_service_main
#include "../src/service.c"
#undef main

#include <assert.h>

static void volume_receipts(void)
{
    audio_server_t *server=calloc(1,sizeof(*server)); assert(server);
    int app[2]; assert(socketpair(AF_UNIX,SOCK_SEQPACKET,0,app)==0);
    server->clients[0].fd=app[0];
    binbot_audio_packet_t command, result;
    binbot_audio_packet_init(&command,BINBOT_AUDIO_MSG_HELLO,sizeof(command.payload.hello),0,0);
    command.payload.hello.role=BINBOT_AUDIO_ROLE_APP;
    command.payload.hello.capabilities=0;
    server_handle_packet(server,&server->clients[0],&command);
    binbot_audio_packet_init(&command,BINBOT_AUDIO_MSG_VOLUME_SET,sizeof(command.payload.volume_set),123,0);
    command.payload.volume_set.volume=67;
    /* No physical mixer is touched: the nonexistent card must report failure. */
    assert(setenv("BINBOT_AUDIO_MIXER_CARD","binbot-test-missing-mixer",1)==0);
    server_handle_packet(server,&server->clients[0],&command);
    ssize_t size=recv(app[1],&result,sizeof(result),MSG_DONTWAIT);
    assert(size>0 && binbot_audio_packet_is_valid(&result,(uint32_t)size));
    assert(result.header.type==BINBOT_AUDIO_MSG_VOLUME_RESULT && result.header.sequence==123);
    assert(result.payload.volume_result.volume==67 && !result.payload.volume_result.success);
    command.header.sequence=124; command.payload.volume_set.volume=101;
    server_handle_packet(server,&server->clients[0],&command);
    size=recv(app[1],&result,sizeof(result),MSG_DONTWAIT);
    assert(size>0 && result.header.sequence==124 && !result.payload.volume_result.success);
    unsetenv("BINBOT_AUDIO_MIXER_CARD"); close(app[0]); close(app[1]); free(server);
}

static void expect_state(int fd, unsigned value)
{
    binbot_audio_packet_t packet;
    ssize_t size = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
    assert(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE);
    assert(packet.payload.dialog_state.state == value);
}

static void expect_empty(int fd)
{
    char buffer[32];
    assert(recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) < 0);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void expect_calendar(int fd, const binbot_audio_calendar_state_t *expected)
{
    binbot_audio_packet_t packet;
    ssize_t size = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
    assert(packet.header.type == BINBOT_AUDIO_MSG_CALENDAR_STATE);
    assert(packet.header.payload_size == sizeof(packet.payload.calendar_state));
    assert(!memcmp(&packet.payload.calendar_state, expected, sizeof(*expected)));
}

static binbot_audio_noise_state_t expect_noise(int fd)
{
    binbot_audio_packet_t packet;
    ssize_t size = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
    assert(packet.header.type == BINBOT_AUDIO_MSG_NOISE_STATE);
    assert(packet.header.payload_size == sizeof(packet.payload.noise));
    assert(binbot_audio_noise_is_valid(&packet.payload.noise));
    return packet.payload.noise;
}

static void microphone_receipts(void)
{
    char path[] = "/tmp/binbot-microphone-XXXXXX";
    int file = mkstemp(path); assert(file >= 0); close(file);
    assert(setenv("BINBOT_MICROPHONE_STATE", path, 1) == 0);
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server);
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    server->clients[0].fd = sockets[0]; server->clients[0].ready = true;
    server->clients[0].role = BINBOT_AUDIO_ROLE_APP;
    binbot_audio_packet_t packet, result;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_MICROPHONE_SET, sizeof(packet.payload.volume_set), 81, 0);
    packet.payload.volume_set.volume = 0;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(recv(sockets[1], &result, sizeof(result), MSG_DONTWAIT) > 0);
    assert(result.header.type == BINBOT_AUDIO_MSG_MICROPHONE_RESULT && result.header.sequence == 81);
    assert(result.payload.volume_result.success && server->microphone_muted);
    FILE *saved = fopen(path, "r"); assert(saved && fgetc(saved) == '1'); fclose(saved);
    packet.payload.volume_set.volume = 2;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(recv(sockets[1], &result, sizeof(result), MSG_DONTWAIT) > 0);
    assert(!result.payload.volume_result.success && server->microphone_muted);
    packet.payload.volume_set.volume = 1;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(recv(sockets[1], &result, sizeof(result), MSG_DONTWAIT) > 0);
    assert(result.payload.volume_result.success && !server->microphone_muted);
    close(sockets[0]); close(sockets[1]); free(server); unlink(path);
    unsetenv("BINBOT_MICROPHONE_STATE");
}

static void noise_routing(void)
{
    char path[] = "/tmp/binbot-audio-noise-routing-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0); close(temporary); unlink(path);
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server && server_start(server, path));
    audio_playback_t playback;
    assert(audio_playback_start(&playback, "null", "/nonexistent.wav", 0, 100, 1, NULL));
    server->playback = &playback;
    int app[2], legacy[2], dialog[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, app));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, legacy));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, dialog));
    server->clients[0].fd = app[0]; server->clients[1].fd = legacy[0];
    server->clients[2].fd = dialog[0];
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_HELLO, sizeof(packet.payload.hello), 0, 0);
    packet.payload.hello.role = BINBOT_AUDIO_ROLE_APP;
    packet.payload.hello.capabilities = BINBOT_AUDIO_CAP_NOISE_STATE;
    server_handle_packet(server, &server->clients[0], &packet);
    binbot_audio_noise_state_t state = expect_noise(app[1]);
    assert(!state.playing && state.master == 70 && state.levels[0] == 55 && state.levels[4] == 35);
    packet.payload.hello.capabilities = 0;
    server_handle_packet(server, &server->clients[1], &packet);
    expect_empty(legacy[1]);
    packet.payload.hello.role = BINBOT_AUDIO_ROLE_DIALOG;
    packet.payload.hello.capabilities = BINBOT_AUDIO_CAP_NOISE_STATE;
    server_handle_packet(server, &server->clients[2], &packet);
    expect_empty(dialog[1]);
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_NOISE_CONTROL, sizeof(packet.payload.noise), 0, 0);
    packet.payload.noise = state;
    packet.payload.noise.master = 42;
    server_handle_packet(server, &server->clients[2], &packet);
    audio_playback_noise_snapshot(&playback, &state); assert(state.master == 70);
    server_handle_packet(server, &server->clients[0], &packet);
    binbot_audio_packet_t result;
    assert(recv(app[1], &result, sizeof(result), MSG_DONTWAIT) > 0);
    assert(result.header.type == BINBOT_AUDIO_MSG_NOISE_RESULT && result.payload.noise.reserved == 1);
    assert(result.payload.noise.master == 42);
    server_service(server);
    state = expect_noise(app[1]); assert(state.master == 42);
    expect_empty(legacy[1]); expect_empty(dialog[1]);
    packet.payload.noise.levels[0] = 101;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(recv(app[1], &result, sizeof(result), MSG_DONTWAIT) > 0);
    assert(result.header.type == BINBOT_AUDIO_MSG_NOISE_RESULT && result.payload.noise.reserved == 0);
    audio_playback_noise_snapshot(&playback, &state); assert(state.levels[0] == 55);
    /* A reconnect replays the authoritative mix even if it has not changed. */
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_HELLO, sizeof(packet.payload.hello), 0, 0);
    packet.payload.hello.role = BINBOT_AUDIO_ROLE_APP;
    packet.payload.hello.capabilities = BINBOT_AUDIO_CAP_NOISE_STATE;
    server_handle_packet(server, &server->clients[0], &packet);
    state = expect_noise(app[1]); assert(state.master == 42);
    server_stop(server); free(server); audio_playback_stop(&playback);
    close(app[1]); close(legacy[1]); close(dialog[1]);
}

static void diagnostic_routing(void)
{
    char path[] = "/tmp/binbot-audio-diagnostics-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0); close(temporary); unlink(path);
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server && server_start(server, path));
    audio_playback_t playback;
    assert(audio_playback_start(&playback, "null", "/nonexistent.wav", 0, 100, 1, NULL));
    server->playback = &playback;
    int diag[2], app[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, diag));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, app));
    server->clients[0] = (audio_client_slot_t){.fd = diag[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_APP, .capabilities = BINBOT_AUDIO_CAP_DIAGNOSTICS};
    server->clients[1] = (audio_client_slot_t){.fd = app[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_APP, .capabilities = BINBOT_AUDIO_CAP_EVENTS};
    server_emit_wake(server, 1, 7, 0); expect_empty(app[1]);
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN,
                             sizeof(packet.payload.playback_begin), 0, 0);
    packet.payload.playback_begin.stream_id[0] = 27;
    packet.payload.playback_begin.sample_rate = 24000;
    packet.payload.playback_begin.channels = 1;
    packet.payload.playback_begin.frame_ms = 20;
    server_handle_packet(server, &server->clients[1], &packet);
    assert(!audio_playback_dialog_active(&playback));
    server->dialog_state.state = BINBOT_DIALOG_STATE_LISTENING;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(!audio_playback_dialog_active(&playback));
    server->dialog_state.state = BINBOT_DIALOG_STATE_IDLE;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(audio_playback_dialog_active(&playback) && server->diagnostic_playback);
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_STATE,
                             sizeof(packet.payload.dialog_state), 0, 0);
    packet.payload.dialog_state.state = BINBOT_DIALOG_STATE_SPEAKING;
    server_handle_packet(server, &server->clients[0], &packet);
    assert(server->dialog_state.state == BINBOT_DIALOG_STATE_IDLE);
    client_close(&server->clients[0]); server_service(server);
    assert(!audio_playback_dialog_active(&playback) && !server->diagnostic_playback);
    server_emit_wake(server, 1, 8, 0);
    ssize_t size = recv(app[1], &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && packet.header.type == BINBOT_AUDIO_MSG_WAKE_DETECTED);
    assert(packet.header.sequence == 8);
    server_stop(server); free(server); audio_playback_stop(&playback);
    close(diag[1]); close(app[1]);
}

static void wake_pcm_during_dialog(void)
{
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server);
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) server->clients[i].fd = -1;
    int wake[2], dialog[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, wake));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, dialog));
    server->clients[0] = (audio_client_slot_t){.fd = wake[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_WAKE, .capabilities = BINBOT_AUDIO_CAP_PCM_LIVE};
    server->clients[1] = (audio_client_slot_t){.fd = dialog[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_DIALOG, .capabilities = BINBOT_AUDIO_CAP_PCM_LIVE};
    binbot_audio_packet_t packet, received;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PCM, sizeof(packet.payload.pcm), 1, 0);
    for(unsigned state = BINBOT_DIALOG_STATE_LISTENING; state <= BINBOT_DIALOG_STATE_CANCELLING; ++state) {
        server->dialog_state.state = state;
        server_broadcast(server, &packet, BINBOT_AUDIO_CAP_PCM_LIVE, true);
        expect_empty(wake[1]);
        assert(recv(dialog[1], &received, sizeof(received), MSG_DONTWAIT) > 0);
        assert(received.header.type == BINBOT_AUDIO_MSG_PCM);
    }
    server->dialog_state.state = BINBOT_DIALOG_STATE_IDLE;
    server_broadcast(server, &packet, BINBOT_AUDIO_CAP_PCM_LIVE, true);
    assert(recv(wake[1], &received, sizeof(received), MSG_DONTWAIT) > 0);
    assert(received.header.type == BINBOT_AUDIO_MSG_PCM);
    close(wake[0]); close(wake[1]); close(dialog[0]); close(dialog[1]); free(server);
}

static void raw_frontend_stream_is_opt_in(void)
{
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server);
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) server->clients[i].fd = -1;
    int wake[2], legacy[2], recorder[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, wake));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, legacy));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, recorder));
    server->clients[0] = (audio_client_slot_t){.fd = wake[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_WAKE, .capabilities = BINBOT_AUDIO_CAP_PCM_FRONTEND};
    server->clients[1] = (audio_client_slot_t){.fd = legacy[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_DIALOG, .capabilities = BINBOT_AUDIO_CAP_PCM_LIVE};
    server->clients[2] = (audio_client_slot_t){.fd = recorder[0], .ready = true,
        .role = BINBOT_AUDIO_ROLE_RECORDER, .capabilities = BINBOT_AUDIO_CAP_PCM_FRONTEND};
    binbot_audio_packet_t packet, received;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PCM_FRONTEND,
        sizeof(packet.payload.pcm_frontend), 42, 123000);
    for(unsigned i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES * 3; ++i)
        packet.payload.pcm_frontend.samples[i] = (int16_t)(i - 400);
    server_broadcast(server, &packet, BINBOT_AUDIO_CAP_PCM_FRONTEND, true);
    for(unsigned i = 0; i < 2; ++i) {
        int fd = i == 0 ? wake[1] : recorder[1];
        ssize_t size = recv(fd, &received, sizeof(received), MSG_DONTWAIT);
        assert(size > 0 && binbot_audio_packet_is_valid(&received, size));
        assert(received.header.sequence == 42);
        assert(!memcmp(&received.payload.pcm_frontend, &packet.payload.pcm_frontend,
                       sizeof(packet.payload.pcm_frontend)));
    }
    expect_empty(legacy[1]);
    server->dialog_state.state = BINBOT_DIALOG_STATE_SPEAKING;
    server_broadcast(server, &packet, BINBOT_AUDIO_CAP_PCM_FRONTEND, true);
    expect_empty(wake[1]); expect_empty(legacy[1]);
    assert(recv(recorder[1], &received, sizeof(received), MSG_DONTWAIT) > 0);
    close(wake[0]); close(wake[1]); close(legacy[0]); close(legacy[1]);
    close(recorder[0]); close(recorder[1]); free(server);
}

int main(void)
{
    volume_receipts();
    raw_frontend_stream_is_opt_in();
    wake_pcm_during_dialog();
    microphone_receipts();
    noise_routing();
    diagnostic_routing();
    char path[] = "/tmp/binbot-audio-dialog-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0); close(temporary); unlink(path);
    audio_server_t *server = calloc(1, sizeof(*server)); assert(server && server_start(server, path));
    int app[2], dialog[2], second[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, app));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, dialog));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, second));
    server->clients[0].fd = app[0]; server->clients[1].fd = dialog[0];
    server->clients[2].fd = second[0];
    binbot_audio_packet_t hello;
    binbot_audio_packet_init(&hello, BINBOT_AUDIO_MSG_HELLO, sizeof(hello.payload.hello), 0, 0);
    hello.payload.hello.role = BINBOT_AUDIO_ROLE_APP;
    hello.payload.hello.capabilities = BINBOT_AUDIO_CAP_DIALOG_STATE |
        BINBOT_AUDIO_CAP_DIALOG_TEXT | BINBOT_AUDIO_CAP_CALENDAR_STATE;
    server_handle_packet(server, &server->clients[0], &hello);
    expect_state(app[1], BINBOT_DIALOG_STATE_DISCONNECTED);
    expect_empty(app[1]);
    hello.payload.hello.role = BINBOT_AUDIO_ROLE_DIALOG;
    hello.payload.hello.capabilities = 0;
    server_handle_packet(server, &server->clients[1], &hello);
    binbot_audio_packet_t state;
    binbot_audio_packet_init(&state, BINBOT_AUDIO_MSG_DIALOG_STATE, sizeof(state.payload.dialog_state), 0, 0);
    state.payload.dialog_state.state = BINBOT_DIALOG_STATE_IDLE;
    server_handle_packet(server, &server->clients[1], &state);
    expect_state(app[1], BINBOT_DIALOG_STATE_IDLE);
    hello.payload.hello.role = BINBOT_AUDIO_ROLE_APP;
    hello.payload.hello.capabilities = BINBOT_AUDIO_CAP_DIALOG_STATE;
    server_handle_packet(server, &server->clients[2], &hello);
    expect_state(second[1], BINBOT_DIALOG_STATE_IDLE);

    binbot_audio_packet_t text;
    binbot_audio_packet_init(&text, BINBOT_AUDIO_MSG_DIALOG_TEXT, sizeof(text.payload.dialog_text), 0, 0);
    text.payload.dialog_text.turn_id[0] = 1;
    text.payload.dialog_text.role = BINBOT_DIALOG_TEXT_USER;
    text.payload.dialog_text.final = 1;
    strcpy(text.payload.dialog_text.text, "真实识别事件");
    server_handle_packet(server, &server->clients[0], &text);
    expect_empty(app[1]);
    server_handle_packet(server, &server->clients[1], &text);
    binbot_audio_packet_t received;
    ssize_t size = recv(app[1], &received, sizeof(received), MSG_DONTWAIT);
    assert(size == (ssize_t)binbot_audio_packet_size(&text));
    assert(!memcmp(&received, &text, (size_t)size));
    expect_empty(second[1]);
    text.header.flags = BINBOT_AUDIO_FLAG_DIALOG_PLAYBACK;
    text.payload.dialog_text.role = BINBOT_DIALOG_TEXT_ASSISTANT;
    text.payload.dialog_text.final = 0;
    server_handle_packet(server, &server->clients[1], &text);
    size = recv(app[1], &received, sizeof(received), MSG_DONTWAIT);
    assert(size == (ssize_t)binbot_audio_packet_size(&text));
    assert(!memcmp(&received, &text, (size_t)size));
    expect_empty(second[1]);
    memset(text.payload.dialog_text.text, 'a', sizeof(text.payload.dialog_text.text));
    server_handle_packet(server, &server->clients[1], &text);
    expect_empty(app[1]);
    text.payload.dialog_text.text[0] = '\0'; text.payload.dialog_text.role = 99;
    server_handle_packet(server, &server->clients[1], &text);
    expect_empty(app[1]);
    server->clients[1].capabilities = BINBOT_AUDIO_CAP_EVENTS;
    binbot_audio_packet_t end;
    binbot_audio_packet_init(&end, BINBOT_AUDIO_MSG_DIALOG_INPUT_END, 0, 0, 0);
    server_handle_packet(server, &server->clients[1], &end);
    expect_empty(dialog[1]);
    server_handle_packet(server, &server->clients[0], &end);
    size = recv(dialog[1], &received, sizeof(received), MSG_DONTWAIT);
    assert(size == (ssize_t)sizeof(received.header));
    assert(received.header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END && !received.header.payload_size);
    expect_empty(app[1]);

    binbot_audio_packet_t calendar;
    binbot_audio_packet_init(&calendar, BINBOT_AUDIO_MSG_CALENDAR_STATE,
                             sizeof(calendar.payload.calendar_state), 0, 0);
    calendar.payload.calendar_state.revision = 7;
    calendar.payload.calendar_state.event_count = 1;
    strcpy(calendar.payload.calendar_state.events[0].event_id, "calendar-test-event");
    strcpy(calendar.payload.calendar_state.events[0].summary, "Calendar sync test");
    strcpy(calendar.payload.calendar_state.events[0].start_timestamp, "1792198800");
    server_handle_packet(server, &server->clients[0], &calendar);
    expect_empty(app[1]);
    assert(!server->calendar_state_valid);
    server_handle_packet(server, &server->clients[1], &calendar);
    expect_calendar(app[1], &calendar.payload.calendar_state);
    expect_empty(second[1]);
    assert(server->calendar_state_valid);

    hello.payload.hello.capabilities = BINBOT_AUDIO_CAP_DIALOG_STATE |
        BINBOT_AUDIO_CAP_CALENDAR_STATE;
    server_handle_packet(server, &server->clients[2], &hello);
    expect_state(second[1], BINBOT_DIALOG_STATE_IDLE);
    expect_calendar(second[1], &calendar.payload.calendar_state);

    calendar.payload.calendar_state.event_count = BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY + 1;
    server_handle_packet(server, &server->clients[1], &calendar);
    expect_empty(app[1]);
    expect_empty(second[1]);
    assert(server->calendar_state.event_count == 1);
    calendar.payload.calendar_state.event_count = 1;
    calendar.header.payload_size--;
    server_handle_packet(server, &server->clients[1], &calendar);
    expect_empty(app[1]);
    expect_empty(second[1]);

    binbot_audio_packet_init(&calendar, BINBOT_AUDIO_MSG_CALENDAR_STATE,
                             sizeof(calendar.payload.calendar_state), 0, 0);
    calendar.payload.calendar_state.revision = 8;
    server_handle_packet(server, &server->clients[1], &calendar);
    expect_calendar(app[1], &calendar.payload.calendar_state);
    expect_calendar(second[1], &calendar.payload.calendar_state);
    server_handle_packet(server, &server->clients[2], &hello);
    expect_state(second[1], BINBOT_DIALOG_STATE_IDLE);
    expect_calendar(second[1], &calendar.payload.calendar_state);

    server->dialog_connected = true; client_close(&server->clients[1]);
    server_service(server);
    expect_state(app[1], BINBOT_DIALOG_STATE_DISCONNECTED);
    expect_state(second[1], BINBOT_DIALOG_STATE_DISCONNECTED);
    server_stop(server); free(server);
    close(app[1]); close(dialog[1]); close(second[1]);
    puts("audio transcript and calendar routing, role checks, subscription, replay and disconnect passed");
    return 0;
}
