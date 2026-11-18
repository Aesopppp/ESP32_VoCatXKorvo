#include "../main/device_mqtt_protocol.h"
#include "../main/device_handshake.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const char *message = "{\"task_id\":\"42\",\"kind\":\"reminder\",\"content\":\"该吃午饭了\",\"fire_at\":\"2020-11-03T06:00:00.000Z\"}";

static void parser_checks(void)
{
    device_mqtt_task_t task;
    CHECK(device_mqtt_parse_task(message, strlen(message), &task));
    CHECK(!strcmp(task.id, "42") && !strcmp(task.payload, message));
    /* 迟到、转义内容和 Unicode 文本必须保留原报文。 */
    const char *invalid[] = { "", "{}", "[]", "null", "{\"a\":{}}", "{\"a\":[]}",
        "{\"content\":\"bad\\u0000text\"}", "{\"task_id\":\"a\",\"task_id\":\"b\"}",
        "{\"task_id\":\"task-1\",\"kind\":\"reminder\",\"content\":\"x\",\"fire_at\":\"2020-11-03T06:00:00Z\"}",
        "{\"content\":\"a\n\"}" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(!device_mqtt_parse_task(invalid[i], strlen(invalid[i]), &task));
    for (size_t n = 0; n < strlen(message); ++n) CHECK(!device_mqtt_parse_task(message, n, &task));
    CHECK(!device_mqtt_parse_task(message, DEVICE_TASK_PAYLOAD_MAX + 1, &task));
    const char *keys[] = { "task_id", "kind", "content", "fire_at" };
    for (unsigned i = 0; i < 4; ++i) {
        cJSON *json = cJSON_Parse(message);
        cJSON_DeleteItemFromObjectCaseSensitive(json, keys[i]);
        char *text = cJSON_PrintUnformatted(json);
        CHECK(!device_mqtt_parse_task(text, strlen(text), &task));
        free(text); cJSON_Delete(json);
    }
    cJSON *json = cJSON_Parse(message);
    cJSON_ReplaceItemInObjectCaseSensitive(json, "content", cJSON_CreateString("引号\"、换行\n、\\路径"));
    char *text = cJSON_PrintUnformatted(json);
    CHECK(device_mqtt_parse_task(text, strlen(text), &task));
    CHECK(!strcmp(task.payload, text)); free(text); cJSON_Delete(json);
    char trailing[512]; snprintf(trailing, sizeof(trailing), "%s true", message);
    CHECK(!device_mqtt_parse_task(trailing, strlen(trailing), &task));
    snprintf(trailing, sizeof(trailing), "%s \r\n", message);
    CHECK(device_mqtt_parse_task(trailing, strlen(trailing), &task));
}
static void fragment_checks(void)
{
    device_mqtt_assembly_t s = {0};
    size_t total = strlen(message);
    for (size_t split = 1; split < total; ++split) {
        CHECK(device_mqtt_assemble(&s, true, total, 0, message, split) == DEVICE_MQTT_FRAGMENT_MORE);
        CHECK(device_mqtt_assemble(&s, false, total, split, message + split, total - split) == DEVICE_MQTT_FRAGMENT_COMPLETE);
        CHECK(!strcmp(s.data, message));
    }
    CHECK(device_mqtt_assemble(&s, false, total, 0, message, total) == DEVICE_MQTT_FRAGMENT_REJECTED);
    CHECK(device_mqtt_assemble(&s, true, 4097, 0, message, 1) == DEVICE_MQTT_FRAGMENT_REJECTED);
    CHECK(device_mqtt_assemble(&s, true, total, 0, message, 20) == DEVICE_MQTT_FRAGMENT_MORE);
    CHECK(device_mqtt_assemble(&s, true, total, 19, message + 19, 2) == DEVICE_MQTT_FRAGMENT_REJECTED);
    CHECK(device_mqtt_assemble(&s, true, total, 0, message, 20) == DEVICE_MQTT_FRAGMENT_MORE);
    CHECK(device_mqtt_assemble(&s, true, total + 1, 20, message + 20, 2) == DEVICE_MQTT_FRAGMENT_REJECTED);
    CHECK(device_mqtt_assemble(&s, true, 1, 0, "\0", 1) == DEVICE_MQTT_FRAGMENT_REJECTED);
    CHECK(device_mqtt_assemble(&s, true, total, 0, message, total) == DEVICE_MQTT_FRAGMENT_COMPLETE);
}
static void history_checks(void)
{
    device_mqtt_history_t h;
    device_mqtt_history_init(&h, "device-1");
    CHECK(device_mqtt_history_valid(&h, "device-1"));
    CHECK(!device_mqtt_history_valid(&h, "device-2"));
    for (unsigned i = 0; i < 64; ++i) {
        char id[32]; snprintf(id, sizeof(id), "%u", i);
        CHECK(device_mqtt_history_find(&h, id) < 0);
        unsigned slot = device_mqtt_history_add(&h, id);
        CHECK(device_mqtt_history_add(&h, id) == slot);
        CHECK(device_mqtt_history_find(&h, id) == (int)slot);
    }
    device_mqtt_history_t reboot = h;
    for (unsigned i = 32; i < 64; ++i) {
        char id[32]; snprintf(id, sizeof(id), "%u", i);
        CHECK(device_mqtt_history_find(&reboot, id) >= 0);
    }
    CHECK(device_mqtt_history_find(&reboot, "0") < 0);
    CHECK(device_mqtt_history_valid(&reboot, "device-1")); /* prior history format remains readable */
    reboot.next = 32; CHECK(!device_mqtt_history_valid(&reboot, "device-1"));
    reboot = h; memset(reboot.ids[0], 'x', sizeof(reboot.ids[0]));
    CHECK(!device_mqtt_history_valid(&reboot, "device-1"));
}
static void retry_checks(void)
{
    for (unsigned seed = 0; seed < 41; ++seed) {
        device_mqtt_retry_t s = {0};
        for (unsigned i = 0; i < 100; ++i) {
            device_mqtt_retry_failed(&s, 0, 10000, seed);
            CHECK(!s.blocked && s.retry_at_ms >= 10800 && s.retry_at_ms <= 70000);
        }
    }
    device_mqtt_retry_t s = {0};
    for (unsigned i = 1; i <= 5; ++i) {
        device_mqtt_retry_failed(&s, 5, 100, 20);
        CHECK(s.blocked == (i == 5) && s.auth_failures == i);
    }
    device_mqtt_retry_wake(&s); CHECK(!s.blocked && !s.auth_failures && !s.retry_at_ms);
    unsigned char codes[] = {0,1,2,128,3,255};
    for (unsigned i = 0; i < 6; ++i) CHECK(device_mqtt_suback(codes+i, 1) == (i < 3 ? (int)i : -1));
    CHECK(device_mqtt_suback(codes, 2) == -1 && device_mqtt_suback(NULL, 1) == -1);
}
int main(void)
{
    parser_checks(); fragment_checks(); history_checks(); retry_checks();
    printf("Device MQTT protocol: %u checks passed\n", checks);
    return 0;
}
