#include "device_handshake_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool version_valid(const char *version)
{
    if (!version || !*version || strlen(version) > 64) return false;
    for (const unsigned char *p = (const unsigned char *)version; *p; ++p)
        if (*p <= 32 || *p >= 127) return false;
    return true;
}

static bool mac_valid(const char *mac)
{
    if (!mac || strlen(mac) != 17) return false;
    for (unsigned i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (mac[i] != ':') return false; }
        else if (!((mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'a' && mac[i] <= 'f') ||
                   (mac[i] >= 'A' && mac[i] <= 'F'))) return false;
    }
    return true;
}

void device_handshake_exchange(const device_handshake_http_t *http,
                               const device_handshake_config_t *config,
                               const char *version, const char *mac,
                               device_handshake_result_t *output)
{
    memset(output, 0, sizeof(*output));
    output->reason = "transport_failed";
    char url[sizeof(config->server_url) + sizeof(DEVICE_HANDSHAKE_PATH)];
    char authorization[sizeof(config->psk) + 8] = {0};
    char body[64];
    char *response = NULL;
    size_t received = 0;
    if (!config->enabled || !version_valid(version) || !mac_valid(mac)) {
        output->status = -1;
        output->reason = "invalid_request_metadata";
        goto done;
    }
    if (http->cancelled(http->context)) { output->reason = "network_changed"; goto done; }
    response = device_handshake_alloc(DEVICE_HANDSHAKE_RESPONSE_MAX + 1);
    if (!response) { output->reason = "allocation_failed"; goto done; }
    snprintf(url, sizeof(url), "%s%s", config->server_url, DEVICE_HANDSHAKE_PATH);
    snprintf(authorization, sizeof(authorization), "Bearer %s", config->psk);
    snprintf(body, sizeof(body), "{\"mac_address\":\"%s\"}", mac);
    const device_handshake_header_t headers[] = {
        { "device-id", config->device_id }, { "device-model", config->device_model },
        { "device-version", version }, { "authorization", authorization },
        { "content-type", "application/json" }, { "accept", "application/json" },
        { "connection", "close" }
    };
    int64_t declared_size = -1;
    if (!http->begin(http->context, url, headers, sizeof(headers) / sizeof(headers[0]), body,
                     &output->status, &declared_size)) goto done;
    if (output->status >= 300 && output->status < 400) {
        output->reason = "redirect_rejected";
        goto done;
    }
    if (declared_size > DEVICE_HANDSHAKE_RESPONSE_MAX) {
        output->reason = "response_too_large";
        goto done;
    }
    const int64_t deadline = http->now_ms(http->context) + 15000;
    char chunk[512];
    while (!http->cancelled(http->context) && http->now_ms(http->context) < deadline) {
        int count = http->read(http->context, chunk, sizeof(chunk));
        if (count < 0 || (count == 0 && !http->complete(http->context))) {
            if (output->status == 200) output->status = 0;
            output->reason = "response_read_failed";
            goto done;
        }
        if (count == 0) {
            if (declared_size >= 0 && (int64_t)received != declared_size) {
                if (output->status == 200) output->status = 0;
                output->reason = "response_truncated";
                goto done;
            }
            if (output->status == 200) {
                output->success = device_handshake_parse_response(response, received,
                    config->device_id, &output->response, &output->reason);
            } else output->reason = device_handshake_error_code(response, received);
            goto done;
        }
        if ((size_t)count > sizeof(chunk) || (size_t)count > DEVICE_HANDSHAKE_RESPONSE_MAX - received) {
            output->reason = "response_too_large";
            goto done;
        }
        memcpy(response + received, chunk, (size_t)count);
        received += (size_t)count;
    }
    if (output->status == 200) output->status = 0;
    output->reason = http->cancelled(http->context) ? "network_changed" : "response_timeout";
done:
    http->close(http->context);
    device_handshake_wipe(authorization, sizeof(authorization));
    if (response) {
        device_handshake_wipe(response, DEVICE_HANDSHAKE_RESPONSE_MAX + 1);
        free(response);
    }
}
