#include "binbot/agent/ipc.h"

#include <cjson/cJSON.h>
#include <libwebsockets.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_HTTP_TIMEOUT_MS = 60000,
    DEFAULT_SNAPSHOT_WAIT_MS = 1800,
    DEFAULT_MAX_IMAGE_BYTES = 512 * 1024,
    HTTP_RESPONSE_CAPACITY = 128 * 1024,
    HTTP_WRITE_CHUNK = 16 * 1024,
    HISTORY_CAPACITY = 12,
};

typedef struct {
    char role[10];
    char content[BINBOT_AGENT_RESPONSE_MAX + 1];
} history_message_t;

typedef struct {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char endpoint[512];
    char scheme[8];
    char address[256];
    char path[512];
    int port;
    char api_key[512];
    char model[160];
    char system_prompt[1024];
    char ca_file[256];
    char image_path[256];
    char capture_request_path[256];
    uint32_t http_timeout_ms;
    uint32_t snapshot_wait_ms;
    uint32_t max_image_bytes;
    size_t history_limit;
} agent_config_t;

typedef struct {
    history_message_t messages[HISTORY_CAPACITY];
    size_t count;
} conversation_t;

typedef struct http_transaction http_transaction_t;

struct http_transaction {
    struct lws_context *context;
    struct lws *wsi;
    lws_sorted_usec_list_t timeout_sul;
    const agent_config_t *config;
    const char *request_body;
    size_t request_size;
    size_t request_offset;
    char *response_body;
    size_t response_capacity;
    size_t response_size;
    int status;
    bool done;
    bool failed;
    bool timed_out;
    char error[192];
};

static volatile sig_atomic_t running = 1;

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL +
           (uint64_t)value.tv_nsec / 1000000ULL;
}

static uint32_t environment_u32(const char *name, uint32_t fallback,
                                uint32_t minimum, uint32_t maximum)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if(errno || !end || *end || value < minimum || value > maximum) {
        fprintf(stderr, "agent: invalid %s, using %u\n", name, fallback);
        return fallback;
    }
    return (uint32_t)value;
}

static bool copy_environment(char *destination, size_t capacity,
                             const char *name, const char *fallback,
                             bool required)
{
    const char *value = getenv(name);
    if(!value || !value[0]) value = fallback;
    if(!value) value = "";
    if(strlen(value) >= capacity || (required && !value[0])) {
        fprintf(stderr, "agent: invalid or missing %s\n", name);
        return false;
    }
    snprintf(destination, capacity, "%s", value);
    return true;
}

static bool parse_endpoint(agent_config_t *config, const char *base_url)
{
    size_t base_size = strlen(base_url);
    const char suffix[] = "/chat/completions";
    bool already_complete = base_size >= strlen(suffix) &&
        strcmp(base_url + base_size - strlen(suffix), suffix) == 0;
    int length = snprintf(config->endpoint, sizeof(config->endpoint), "%s%s%s",
                          base_url,
                          (!already_complete && base_size &&
                           base_url[base_size - 1] == '/') ? "" :
                          (already_complete ? "" : "/"),
                          already_complete ? "" : "chat/completions");
    if(length <= 0 || (size_t)length >= sizeof(config->endpoint)) return false;

    char parsed[sizeof(config->endpoint)];
    snprintf(parsed, sizeof(parsed), "%s", config->endpoint);
    const char *scheme = NULL;
    const char *address = NULL;
    const char *path = NULL;
    int port = 0;
    if(lws_parse_uri(parsed, &scheme, &address, &port, &path) != 0 ||
       !scheme || !address || !path ||
       (strcmp(scheme, "https") != 0 && strcmp(scheme, "http") != 0)) {
        return false;
    }
    if(strlen(scheme) >= sizeof(config->scheme) ||
       strlen(address) >= sizeof(config->address) ||
       strlen(path) + 2 > sizeof(config->path)) return false;
    snprintf(config->scheme, sizeof(config->scheme), "%s", scheme);
    snprintf(config->address, sizeof(config->address), "%s", address);
    if(path[0] == '/') snprintf(config->path, sizeof(config->path), "%s", path);
    else snprintf(config->path, sizeof(config->path), "/%s", path);
    config->port = port;
    return true;
}

static bool load_config(agent_config_t *config)
{
    memset(config, 0, sizeof(*config));
    const char *api_key = getenv("BINBOT_AGENT_API_KEY");
    if(!api_key || !api_key[0]) api_key = getenv("ARK_API_KEY");
    const char *base_url = getenv("BINBOT_AGENT_BASE_URL");
    if(!base_url || !base_url[0]) {
        base_url = "https://ark.cn-beijing.volces.com/api/v3";
    }
    if(!parse_endpoint(config, base_url) ||
       !copy_environment(config->socket_path, sizeof(config->socket_path),
                         "BINBOT_AGENT_SOCKET", "/tmp/binbot_agent.sock", true) ||
       !copy_environment(config->api_key, sizeof(config->api_key),
                         "BINBOT_AGENT_API_KEY", api_key, true) ||
       !copy_environment(config->model, sizeof(config->model),
                         "BINBOT_AGENT_MODEL",
                         "doubao-seed-2-0-lite-260428", true) ||
       !copy_environment(
           config->system_prompt, sizeof(config->system_prompt),
           "BINBOT_AGENT_SYSTEM_PROMPT",
           "你是 BinBot，一个运行在桌面智能终端上的多模态语音助手。"
           "你可以结合当前摄像头画面回答。回答要自然、准确、简洁，"
           "适合直接语音播报；不要声称看到画面中不存在的内容。", true) ||
       !copy_environment(config->ca_file, sizeof(config->ca_file),
                         "BINBOT_AGENT_CA_FILE", "", false) ||
       !copy_environment(config->image_path, sizeof(config->image_path),
                         "BINBOT_AGENT_IMAGE_PATH",
                         "/tmp/binbot_agent_frame.jpg", true) ||
       !copy_environment(config->capture_request_path,
                         sizeof(config->capture_request_path),
                         "BINBOT_AGENT_CAPTURE_REQUEST",
                         "/tmp/binbot_agent_capture.request", true)) {
        fprintf(stderr, "agent: invalid configuration\n");
        return false;
    }
    config->http_timeout_ms = environment_u32(
        "BINBOT_AGENT_TIMEOUT_MS", DEFAULT_HTTP_TIMEOUT_MS, 5000, 180000);
    config->snapshot_wait_ms = environment_u32(
        "BINBOT_AGENT_SNAPSHOT_WAIT_MS", DEFAULT_SNAPSHOT_WAIT_MS, 100, 5000);
    config->max_image_bytes = environment_u32(
        "BINBOT_AGENT_MAX_IMAGE_BYTES", DEFAULT_MAX_IMAGE_BYTES,
        16 * 1024, 2 * 1024 * 1024);
    config->history_limit = environment_u32(
        "BINBOT_AGENT_HISTORY_MESSAGES", 8, 0, HISTORY_CAPACITY);
    return true;
}

static void signal_handler(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static bool write_capture_request(const agent_config_t *config)
{
    int fd = open(config->capture_request_path,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if(fd < 0) return false;
    static const char request[] = "capture\n";
    bool ok = write(fd, request, sizeof(request) - 1) ==
              (ssize_t)(sizeof(request) - 1);
    close(fd);
    return ok;
}

static unsigned char *capture_image(const agent_config_t *config, size_t *size)
{
    *size = 0;
    /* Keep the last good frame as a fallback while the camera produces the
       next one.  This avoids making a voice turn text-only at the VI/VENC
       handoff boundary. */
    bool request_written = write_capture_request(config);
    uint64_t deadline = monotonic_ms() + config->snapshot_wait_ms;
    struct stat metadata;
    while(running && monotonic_ms() < deadline) {
        if(stat(config->image_path, &metadata) == 0 && metadata.st_size > 0 &&
           (uint64_t)metadata.st_size <= config->max_image_bytes) break;
        usleep(50000);
    }
    (void)request_written;
    if(stat(config->image_path, &metadata) != 0 || metadata.st_size <= 0 ||
       (uint64_t)metadata.st_size > config->max_image_bytes) return NULL;
    FILE *stream = fopen(config->image_path, "rb");
    if(!stream) return NULL;
    unsigned char *data = malloc((size_t)metadata.st_size);
    if(!data) {
        fclose(stream);
        return NULL;
    }
    size_t count = fread(data, 1, (size_t)metadata.st_size, stream);
    fclose(stream);
    if(count != (size_t)metadata.st_size) {
        free(data);
        return NULL;
    }
    *size = count;
    return data;
}

static char *image_data_url(const unsigned char *image, size_t image_size)
{
    if(!image || image_size == 0 || image_size > (size_t)INT32_MAX) return NULL;
    /* libwebsockets checks for both the final NUL and one spare byte. */
    size_t encoded_capacity = ((image_size + 2U) / 3U) * 4U + 2U;
    const char prefix[] = "data:image/jpeg;base64,";
    if(encoded_capacity > SIZE_MAX - sizeof(prefix)) return NULL;
    char *url = malloc(sizeof(prefix) - 1U + encoded_capacity);
    if(!url) return NULL;
    memcpy(url, prefix, sizeof(prefix) - 1U);
    int result = lws_b64_encode_string(
        (const char *)image, (int)image_size, url + sizeof(prefix) - 1U,
        (int)encoded_capacity);
    if(result < 0) {
        free(url);
        return NULL;
    }
    return url;
}

static cJSON *message_object(const char *role, const char *content)
{
    cJSON *message = cJSON_CreateObject();
    if(!message || !cJSON_AddStringToObject(message, "role", role) ||
       !cJSON_AddStringToObject(message, "content", content)) {
        cJSON_Delete(message);
        return NULL;
    }
    return message;
}

static char *build_request_body(const agent_config_t *config,
                                const conversation_t *conversation,
                                const char *user_text, const char *data_url)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    if(!root || !messages ||
       !cJSON_AddStringToObject(root, "model", config->model) ||
       !cJSON_AddBoolToObject(root, "stream", false) ||
       !cJSON_AddNumberToObject(root, "max_tokens", 500) ||
       !cJSON_AddItemToObject(root, "messages", messages)) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        return NULL;
    }
    cJSON *system = message_object("system", config->system_prompt);
    if(!system || !cJSON_AddItemToArray(messages, system)) {
        cJSON_Delete(system);
        cJSON_Delete(root);
        return NULL;
    }
    for(size_t i = 0; i < conversation->count; ++i) {
        cJSON *history = message_object(conversation->messages[i].role,
                                        conversation->messages[i].content);
        if(!history || !cJSON_AddItemToArray(messages, history)) {
            cJSON_Delete(history);
            cJSON_Delete(root);
            return NULL;
        }
    }

    cJSON *user = cJSON_CreateObject();
    if(!user || !cJSON_AddStringToObject(user, "role", "user")) {
        cJSON_Delete(user);
        cJSON_Delete(root);
        return NULL;
    }
    if(data_url) {
        cJSON *content = cJSON_CreateArray();
        cJSON *text_part = cJSON_CreateObject();
        cJSON *image_part = cJSON_CreateObject();
        cJSON *image_url = cJSON_CreateObject();
        if(!content || !text_part || !image_part || !image_url ||
           !cJSON_AddStringToObject(text_part, "type", "text") ||
           !cJSON_AddStringToObject(text_part, "text", user_text) ||
           !cJSON_AddStringToObject(image_part, "type", "image_url") ||
           !cJSON_AddStringToObject(image_url, "url", data_url)) {
            cJSON_Delete(content);
            cJSON_Delete(text_part);
            cJSON_Delete(image_part);
            cJSON_Delete(image_url);
            cJSON_Delete(user);
            cJSON_Delete(root);
            return NULL;
        }
        if(!cJSON_AddItemToObject(image_part, "image_url", image_url)) {
            cJSON_Delete(content);
            cJSON_Delete(text_part);
            cJSON_Delete(image_part);
            cJSON_Delete(image_url);
            cJSON_Delete(user);
            cJSON_Delete(root);
            return NULL;
        }
        image_url = NULL;
        if(!cJSON_AddItemToArray(content, text_part)) {
            cJSON_Delete(content);
            cJSON_Delete(text_part);
            cJSON_Delete(image_part);
            cJSON_Delete(user);
            cJSON_Delete(root);
            return NULL;
        }
        text_part = NULL;
        if(!cJSON_AddItemToArray(content, image_part)) {
            cJSON_Delete(content);
            cJSON_Delete(image_part);
            cJSON_Delete(user);
            cJSON_Delete(root);
            return NULL;
        }
        image_part = NULL;
        if(!cJSON_AddItemToObject(user, "content", content)) {
            cJSON_Delete(content);
            cJSON_Delete(user);
            cJSON_Delete(root);
            return NULL;
        }
    }
    else if(!cJSON_AddStringToObject(user, "content", user_text)) {
        cJSON_Delete(user);
        cJSON_Delete(root);
        return NULL;
    }
    if(!cJSON_AddItemToArray(messages, user)) {
        cJSON_Delete(user);
        cJSON_Delete(root);
        return NULL;
    }
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return serialized;
}

static void http_timeout(lws_sorted_usec_list_t *scheduled)
{
    http_transaction_t *transaction = lws_container_of(
        scheduled, http_transaction_t, timeout_sul);
    transaction->timed_out = true;
    transaction->failed = true;
    transaction->done = true;
    snprintf(transaction->error, sizeof(transaction->error),
             "model request timed out");
    if(transaction->wsi) {
        lws_set_timeout(transaction->wsi, PENDING_TIMEOUT_USER_OK,
                        LWS_TO_KILL_ASYNC);
    }
    if(transaction->context) lws_cancel_service(transaction->context);
}

static int http_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *input, size_t length)
{
    http_transaction_t *transaction = user;
    if(!transaction) return lws_callback_http_dummy(wsi, reason, user, input, length);
    switch(reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char **cursor = input;
            unsigned char *end = *cursor + length;
            char authorization[sizeof(transaction->config->api_key) + 8U];
            int auth_length = snprintf(authorization, sizeof(authorization),
                                       "Bearer %s", transaction->config->api_key);
            static const unsigned char content_type[] = "application/json";
            if(auth_length <= 7 || (size_t)auth_length >= sizeof(authorization) ||
               lws_add_http_header_by_token(
                   wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                   (const unsigned char *)authorization, auth_length,
                   cursor, end) ||
               lws_add_http_header_by_token(
                   wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, content_type,
                   sizeof(content_type) - 1U, cursor, end) ||
               lws_add_http_header_content_length(
                   wsi, (lws_filepos_t)transaction->request_size, cursor, end)) {
                snprintf(transaction->error, sizeof(transaction->error),
                         "cannot construct model request headers");
                transaction->failed = true;
                return -1;
            }
            lws_client_http_body_pending(wsi, 1);
            lws_callback_on_writable(wsi);
            return 0;
        }
        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE: {
            if(transaction->request_offset >= transaction->request_size) return 0;
            uint8_t buffer[LWS_PRE + HTTP_WRITE_CHUNK];
            size_t remaining = transaction->request_size - transaction->request_offset;
            size_t chunk = remaining > HTTP_WRITE_CHUNK ? HTTP_WRITE_CHUNK : remaining;
            memcpy(buffer + LWS_PRE,
                   transaction->request_body + transaction->request_offset, chunk);
            int written = lws_write(wsi, buffer + LWS_PRE, chunk, LWS_WRITE_HTTP);
            if(written != (int)chunk) {
                snprintf(transaction->error, sizeof(transaction->error),
                         "model request body write failed");
                transaction->failed = true;
                return -1;
            }
            transaction->request_offset += chunk;
            if(transaction->request_offset == transaction->request_size) {
                lws_client_http_body_pending(wsi, 0);
            }
            else lws_callback_on_writable(wsi);
            return 0;
        }
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            transaction->status = (int)lws_http_client_http_response(wsi);
            return 0;
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP: {
            uint8_t buffer[LWS_PRE + 4096];
            char *cursor = (char *)buffer + LWS_PRE;
            int available = 4096;
            return lws_http_client_read(wsi, &cursor, &available) < 0 ? -1 : 0;
        }
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            if(transaction->response_size + length >= transaction->response_capacity) {
                snprintf(transaction->error, sizeof(transaction->error),
                         "model response exceeded %u bytes",
                         (unsigned)transaction->response_capacity);
                transaction->failed = true;
                return -1;
            }
            memcpy(transaction->response_body + transaction->response_size,
                   input, length);
            transaction->response_size += length;
            transaction->response_body[transaction->response_size] = '\0';
            return 0;
        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            transaction->done = true;
            transaction->wsi = NULL;
            lws_cancel_service(transaction->context);
            return 0;
        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
            transaction->wsi = NULL;
            if(!transaction->done) {
                transaction->done = true;
                lws_cancel_service(transaction->context);
            }
            return 0;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            transaction->wsi = NULL;
            if(!transaction->error[0]) {
                snprintf(transaction->error, sizeof(transaction->error),
                         "model connection failed: %s",
                         input ? (const char *)input : "unknown error");
            }
            transaction->failed = true;
            transaction->done = true;
            lws_cancel_service(transaction->context);
            return 0;
        default:
            return lws_callback_http_dummy(wsi, reason, user, input, length);
    }
}

static const struct lws_protocols http_protocols[] = {
    {"binbot-agent-http", http_callback, 0, 0, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM,
};

static bool model_request(const agent_config_t *config, const char *request_body,
                          char *response, size_t response_capacity,
                          char *error, size_t error_capacity)
{
    struct lws_context_creation_info context_info;
    memset(&context_info, 0, sizeof(context_info));
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = http_protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    context_info.fd_limit_per_thread = 4;
    if(config->ca_file[0]) context_info.client_ssl_ca_filepath = config->ca_file;
    struct lws_context *context = lws_create_context(&context_info);
    if(!context) {
        snprintf(error, error_capacity, "cannot initialize HTTPS client");
        return false;
    }
    http_transaction_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    transaction.context = context;
    transaction.config = config;
    transaction.request_body = request_body;
    transaction.request_size = strlen(request_body);
    transaction.response_body = response;
    transaction.response_capacity = response_capacity;
    response[0] = '\0';

    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));
    info.context = context;
    info.address = config->address;
    info.port = config->port;
    info.path = config->path;
    info.host = config->address;
    info.origin = config->address;
    info.method = "POST";
    info.protocol = http_protocols[0].name;
    info.local_protocol_name = http_protocols[0].name;
    info.alpn = "http/1.1";
    info.ssl_connection = strcmp(config->scheme, "https") == 0
        ? LCCSCF_USE_SSL : 0;
    info.userdata = &transaction;
    info.pwsi = &transaction.wsi;
    lws_sul_schedule(context, 0, &transaction.timeout_sul, http_timeout,
                     (lws_usec_t)config->http_timeout_ms * LWS_US_PER_MS);
    if(!lws_client_connect_via_info(&info)) {
        lws_sul_cancel(&transaction.timeout_sul);
        lws_context_destroy(context);
        snprintf(error, error_capacity, "cannot start model connection");
        return false;
    }
    while(!transaction.done && running) {
        int result = lws_service(context, 0);
        if(result < 0) {
            transaction.failed = true;
            snprintf(transaction.error, sizeof(transaction.error),
                     "HTTPS service failed: %d", result);
            break;
        }
    }
    lws_sul_cancel(&transaction.timeout_sul);
    lws_context_destroy(context);
    if(!running && !transaction.done) {
        snprintf(error, error_capacity, "device agent is stopping");
        return false;
    }
    if(transaction.failed || transaction.timed_out) {
        snprintf(error, error_capacity, "%s",
                 transaction.error[0] ? transaction.error : "model request failed");
        return false;
    }
    if(transaction.status < 200 || transaction.status >= 300) {
        snprintf(error, error_capacity, "model API returned HTTP %d",
                 transaction.status);
        return false;
    }
    if(transaction.response_size == 0) {
        snprintf(error, error_capacity, "model API returned an empty response");
        return false;
    }
    return true;
}

static void utf8_copy_truncated(char *destination, size_t capacity,
                                const char *source)
{
    if(capacity == 0) return;
    size_t source_size = strlen(source);
    size_t count = source_size < capacity - 1U ? source_size : capacity - 1U;
    while(count > 0 && ((unsigned char)source[count] & 0xc0U) == 0x80U) --count;
    memcpy(destination, source, count);
    destination[count] = '\0';
}

static bool parse_model_response(const char *body, char *reply,
                                 size_t reply_capacity, char *error,
                                 size_t error_capacity)
{
    cJSON *root = cJSON_Parse(body);
    if(!root) {
        snprintf(error, error_capacity, "model API returned invalid JSON");
        return false;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if(!cJSON_IsString(content) || !content->valuestring || !content->valuestring[0]) {
        cJSON *api_error = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *api_message = api_error
            ? cJSON_GetObjectItemCaseSensitive(api_error, "message") : NULL;
        snprintf(error, error_capacity, "%s",
                 cJSON_IsString(api_message) && api_message->valuestring
                    ? api_message->valuestring
                    : "model response did not contain assistant text");
        cJSON_Delete(root);
        return false;
    }
    utf8_copy_truncated(reply, reply_capacity, content->valuestring);
    cJSON_Delete(root);
    return reply[0] != '\0';
}

static void append_history(conversation_t *conversation, const char *role,
                           const char *content, size_t limit)
{
    if(limit == 0) return;
    if(conversation->count >= limit) {
        memmove(&conversation->messages[0], &conversation->messages[1],
                (conversation->count - 1U) * sizeof(conversation->messages[0]));
        conversation->count--;
    }
    history_message_t *message = &conversation->messages[conversation->count++];
    snprintf(message->role, sizeof(message->role), "%s", role);
    utf8_copy_truncated(message->content, sizeof(message->content), content);
}

/* Camera capture is deliberately opt-in.  Taking a frame for every voice
 * turn adds latency and needlessly sends an image to the multimodal model.
 * Keep this heuristic local so ordinary conversation stays fast; explicit
 * visual words still route the turn through the camera. */
static bool request_needs_vision(const char *text)
{
    static const char * const keywords[] = {
        "看一下", "看下", "看看", "看一看", "你看到", "看到什么",
        "面前", "眼前", "这里有什么", "这是什么", "这是谁", "识别",
        "辨认", "读一下", "读取", "文字", "图像", "图片", "照片",
        "画面", "摄像头", "镜头", "颜色", "数量", "几个", "多少个",
        "手势", "姿势", "人脸", "物体", "桌面", "桌上", "屏幕",
        "look", "see", "what is in", "what's in", "camera", "image",
        "picture", "photo", "identify", "read", "color", "how many",
        "who is",
    };
    if(!text || !text[0]) return false;
    for(size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if(strstr(text, keywords[i]) != NULL) return true;
    }
    return false;
}

static bool answer_request(const agent_config_t *config,
                           conversation_t *conversation, const char *user_text,
                           char *reply, size_t reply_capacity,
                           char *error, size_t error_capacity)
{
    size_t image_size = 0;
    char *data_url = NULL;
    if(request_needs_vision(user_text)) {
        unsigned char *image = capture_image(config, &image_size);
        data_url = image_data_url(image, image_size);
        free(image);
        if(data_url) {
            fprintf(stdout, "agent: vision request; attached camera snapshot (%u bytes)\n",
                    (unsigned)image_size);
        } else {
            fprintf(stdout, "agent: vision request; camera snapshot unavailable; using text only\n");
        }
    } else {
        fprintf(stdout, "agent: text-only request; camera capture skipped\n");
    }

    char *request_body = build_request_body(config, conversation, user_text, data_url);
    free(data_url);
    if(!request_body) {
        snprintf(error, error_capacity, "cannot build model request");
        return false;
    }
    char *response_body = malloc(HTTP_RESPONSE_CAPACITY);
    if(!response_body) {
        cJSON_free(request_body);
        snprintf(error, error_capacity, "not enough memory for model response");
        return false;
    }
    bool ok = model_request(config, request_body, response_body,
                            HTTP_RESPONSE_CAPACITY, error, error_capacity);
    cJSON_free(request_body);
    if(ok) ok = parse_model_response(response_body, reply, reply_capacity,
                                     error, error_capacity);
    free(response_body);
    if(ok) {
        append_history(conversation, "user", user_text, config->history_limit);
        append_history(conversation, "assistant", reply, config->history_limit);
    }
    return ok;
}

static int create_server_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    (void)unlink(path);
    if(bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
       chmod(path, 0660) != 0 || listen(fd, 4) != 0) {
        close(fd);
        (void)unlink(path);
        return -1;
    }
    return fd;
}

static void serve_client(int fd, const agent_config_t *config,
                         conversation_t *conversation)
{
    char request[BINBOT_AGENT_REQUEST_MAX + 1];
    char error[256] = {0};
    size_t request_size = 0;
    binbot_agent_message_type_t type = BINBOT_AGENT_MSG_ERROR;
    if(!binbot_agent_ipc_read(fd, &type, request, sizeof(request), &request_size,
                              5000, error, sizeof(error)) ||
       type != BINBOT_AGENT_MSG_REQUEST || request_size == 0) {
        if(!error[0]) snprintf(error, sizeof(error), "invalid agent request");
        (void)binbot_agent_ipc_write(fd, BINBOT_AGENT_MSG_ERROR,
                                     error, strlen(error), 1000, NULL, 0);
        return;
    }
    fprintf(stdout, "agent: processing request (%u bytes)\n", (unsigned)request_size);
    char reply[BINBOT_AGENT_RESPONSE_MAX + 1];
    bool ok = answer_request(config, conversation, request,
                             reply, sizeof(reply), error, sizeof(error));
    const char *payload = ok ? reply : error;
    if(!payload[0]) payload = ok ? "抱歉，我暂时无法回答。" : "device agent failed";
    if(!binbot_agent_ipc_write(fd,
                               ok ? BINBOT_AGENT_MSG_RESPONSE
                                  : BINBOT_AGENT_MSG_ERROR,
                               payload, strlen(payload), 5000, NULL, 0)) {
        fprintf(stderr, "agent: dialog disconnected before response\n");
    } else if(ok) {
        fprintf(stdout, "agent: request completed\n");
    } else {
        fprintf(stderr, "agent: request failed: %s\n",
                error[0] ? error : "unknown error");
    }
}

int main(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    agent_config_t config;
    if(!load_config(&config)) return EXIT_FAILURE;
    int server_fd = create_server_socket(config.socket_path);
    if(server_fd < 0) {
        fprintf(stderr, "agent: cannot listen on %s: %s\n",
                config.socket_path, strerror(errno));
        return EXIT_FAILURE;
    }
    fprintf(stdout, "agent: ready model=%s endpoint=%s\n",
            config.model, config.endpoint);
    conversation_t conversation;
    memset(&conversation, 0, sizeof(conversation));
    while(running) {
        struct pollfd descriptor = {.fd = server_fd, .events = POLLIN};
        int result = poll(&descriptor, 1, 500);
        if(result < 0) {
            if(errno == EINTR) continue;
            fprintf(stderr, "agent: poll failed: %s\n", strerror(errno));
            break;
        }
        if(result == 0 || !(descriptor.revents & POLLIN)) continue;
        int client = accept(server_fd, NULL, NULL);
        if(client < 0) {
            if(errno != EINTR) fprintf(stderr, "agent: accept failed: %s\n",
                                      strerror(errno));
            continue;
        }
        (void)fcntl(client, F_SETFD, FD_CLOEXEC);
        serve_client(client, &config, &conversation);
        close(client);
    }
    close(server_fd);
    (void)unlink(config.socket_path);
    (void)unlink(config.capture_request_path);
    fprintf(stdout, "agent: stopped cleanly\n");
    return EXIT_SUCCESS;
}
