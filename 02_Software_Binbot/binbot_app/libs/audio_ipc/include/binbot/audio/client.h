#ifndef BINBOT_AUDIO_CLIENT_H
#define BINBOT_AUDIO_CLIENT_H

#include "binbot/audio/protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;
    binbot_audio_client_role_t role;
    uint32_t capabilities;
    uint32_t preroll_ms;
    uint64_t last_connect_attempt_ms;
    char socket_path[108];
} binbot_audio_client_t;

void binbot_audio_client_init(binbot_audio_client_t *client,
                              binbot_audio_client_role_t role,
                              uint32_t capabilities,
                              uint32_t preroll_ms,
                              const char *socket_path);
void binbot_audio_client_close(binbot_audio_client_t *client);
bool binbot_audio_client_connect(binbot_audio_client_t *client, uint64_t monotonic_ms);
int binbot_audio_client_receive(binbot_audio_client_t *client,
                                binbot_audio_packet_t *packet);
bool binbot_audio_client_send(binbot_audio_client_t *client,
                              const binbot_audio_packet_t *packet);
bool binbot_audio_client_music_control(binbot_audio_client_t *client,
                                       binbot_audio_music_command_t command,
                                       uint32_t position_ms,
                                       const char *path);
bool binbot_audio_client_pronunciation(binbot_audio_client_t *client,
                                       bool play, const char *path);
bool binbot_audio_client_set_volume(binbot_audio_client_t *client, uint8_t volume);
bool binbot_audio_client_request_volume(binbot_audio_client_t *client, uint8_t volume,
                                        uint64_t request_sequence);
int binbot_audio_client_fd(const binbot_audio_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
