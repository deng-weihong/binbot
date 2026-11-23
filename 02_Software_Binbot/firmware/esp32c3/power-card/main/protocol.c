#include "protocol.h"

#include <string.h>
#include "cJSON.h"
#include "http_parser.h"

int power_card_frame_feed(power_card_frame_t *frame, unsigned char byte)
{
    if (byte == '\n') {
        frame->data[frame->used] = '\0';
        return 1;
    }
    if (byte == 0 || frame->used == POWER_CARD_MAX_FRAME) return -1;
    frame->data[frame->used++] = (char)byte;
    return 0;
}

bool power_card_valid_ota_url(const char *url)
{
    if (!url) return false;
    const size_t length = strlen(url);
    if (!length || length > POWER_CARD_MAX_URL) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] >= 127) return false;
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, length, 0, &parsed) != 0) return false;
    return (parsed.field_set & (1 << UF_SCHEMA)) &&
           parsed.field_data[UF_SCHEMA].len == 4 &&
           strncmp(url + parsed.field_data[UF_SCHEMA].off, "http", 4) == 0 &&
           (parsed.field_set & (1 << UF_HOST)) &&
           parsed.field_data[UF_HOST].len > 0 &&
           !(parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_FRAGMENT))) &&
           (!(parsed.field_set & (1 << UF_PORT)) || parsed.port != 0);
}

static bool token_matches(const char *a, const char *b)
{
    if (!a || !b || strlen(a) != strlen(b)) return false;
    unsigned diff = 0;
    for (size_t i = 0; a[i]; ++i) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

const char *power_card_parse_request(const char *line, const char *token, power_card_request_t *request)
{
    memset(request, 0, sizeof(*request));
    // cJSON strings cannot represent embedded NULs. Reject the escape before parsing.
    if (!line || strlen(line) > POWER_CARD_MAX_FRAME || strstr(line, "\\u0000"))
        return "invalid_request";
    cJSON *root = cJSON_ParseWithOpts(line, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return "invalid_json";
    }
    const char *error = "invalid_request";
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id) {
        if (!cJSON_IsString(id) || strlen(id->valuestring) > POWER_CARD_MAX_ID) goto done;
        request->has_id = true;
        strcpy(request->id, id->valuestring);
    }
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(auth) || !token_matches(auth->valuestring, token)) {
        error = "unauthorized";
        goto done;
    }
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(command)) goto done;
    unsigned seen = 0;
    const cJSON *field;
    cJSON_ArrayForEach(field, root) {
        unsigned bit = 0;
        if (strcmp(field->string, "id") == 0) bit = 1;
        else if (strcmp(field->string, "token") == 0) bit = 2;
        else if (strcmp(field->string, "cmd") == 0) bit = 4;
        else if (strcmp(field->string, "url") == 0) bit = 8;
        if (!bit || (seen & bit)) goto done;
        seen |= bit;
    }
    if (strcmp(command->valuestring, "status") == 0) request->command = POWER_CARD_CMD_STATUS;
    else if (strcmp(command->valuestring, "power") == 0) request->command = POWER_CARD_CMD_POWER;
    else if (strcmp(command->valuestring, "reset") == 0) request->command = POWER_CARD_CMD_RESET;
    else if (strcmp(command->valuestring, "ota_status") == 0) request->command = POWER_CARD_CMD_OTA_STATUS;
    else if (strcmp(command->valuestring, "ota") == 0) request->command = POWER_CARD_CMD_OTA;
    else {
        error = "unknown_command";
        goto done;
    }
    if (request->command == POWER_CARD_CMD_OTA) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (!cJSON_IsString(url) || !power_card_valid_ota_url(url->valuestring)) {
            error = "invalid_url";
            goto done;
        }
        strcpy(request->url, url->valuestring);
    } else if (seen & 8) goto done;
    error = NULL;
done:
    cJSON_Delete(root);
    return error;
}
