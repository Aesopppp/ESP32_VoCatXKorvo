/* 运行实际 service 函数；只替换 RTOS、MQTT、NVS 和时钟，不复制生产策略。 */
#include "../main/device_mqtt_service.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_NOT_FOUND 5
#define ESP_FAIL (-1)
#define ESP_EVENT_ANY_ID (-1)
#ifndef TEST_NO_CA
#define DEVICE_MQTT_HAS_CA 1
#endif
#ifndef DEVICE_MQTT_FORWARD_TASK_TO_WS
#define DEVICE_MQTT_FORWARD_TASK_TO_WS 1
#endif
#define __has_include(x) 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 20
typedef bool atomic_bool;
typedef unsigned atomic_uint;
#define atomic_load(p) (*(p))
#define atomic_store(p,v) ((*(p))=(v))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
static bool atomic_exchange(atomic_bool *p, bool v) { bool old=*p; *p=v; return old; }
typedef int BaseType_t;
typedef int SemaphoreHandle_t;
typedef int esp_event_base_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define configASSERT(x) CHECK(x)
typedef struct { unsigned size, capacity, count; unsigned char *data; } *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned n, unsigned size) {
    QueueHandle_t q = calloc(1, sizeof(*q)); q->size=size; q->capacity=n; q->data=calloc(n,size); return q;
}
static int xQueueSend(QueueHandle_t q, const void *item, int wait) {
    (void)wait; if (q->count==q->capacity) return 0;
    memcpy(q->data+q->count++*q->size,item,q->size); return 1;
}
static int xQueueReceive(QueueHandle_t q, void *item, int wait) {
    (void)wait; if (!q->count) return 0; memcpy(item,q->data,q->size);
    --q->count; memmove(q->data,q->data+q->size,q->count*q->size); return 1;
}
static void xQueueReset(QueueHandle_t q) { q->count=0; }
static void vQueueDelete(QueueHandle_t q) { free(q->data); free(q); }
static int xSemaphoreCreateMutex(void) { return 1; }
static int xSemaphoreTake(int s,int t) { (void)s; (void)t; return 1; }
static void xSemaphoreGive(int s) { (void)s; }
static void vSemaphoreDelete(int s) { (void)s; }
static void *worker_argument;
static int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *task,int core) {
    (void)fn;(void)name;(void)stack;(void)priority;(void)task;(void)core; worker_argument=arg; return 1;
}
static void vTaskDelay(int ms) { (void)ms; }
static int64_t fake_now = 1000;
static int64_t esp_timer_get_time(void) { return fake_now * 1000; }
static uint32_t esp_random(void) { return 20; }
static bool wifi_service_get_ip_str(char *p,size_t n) { snprintf(p,n,"192.0.2.1"); return true; }
static int wifi_service_get_rssi(void) { return -50; }
typedef struct { const char *version; } mock_description_t;
static const mock_description_t *esp_app_get_description(void) { static mock_description_t d={"test-version"}; return &d; }
typedef unsigned nvs_handle_t;
static bool have_blob, fail_commit;
static unsigned commits;
static device_mqtt_history_t stored_history, staged_history;
static int nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)mode; CHECK(!strcmp(name,"device_tasks")); *h=1; return ESP_OK; }
static int nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {
    (void)h; CHECK(!strcmp(key,"played_v1")); if (!have_blob) return ESP_ERR_NVS_NOT_FOUND;
    CHECK(*n>=sizeof(stored_history)); memcpy(p,&stored_history,sizeof(stored_history)); *n=sizeof(stored_history); return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {
    (void)h; (void)key; CHECK(n==sizeof(staged_history)); memcpy(&staged_history,p,n); return ESP_OK;
}
static int nvs_commit(nvs_handle_t h) { (void)h; if(fail_commit)return ESP_FAIL; stored_history=staged_history; have_blob=true; ++commits; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }

enum { MQTT_EVENT_CONNECTED=1, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_SUBSCRIBED,
       MQTT_EVENT_ERROR, MQTT_EVENT_DATA };
enum { MQTT_ERROR_TYPE_CONNECTION_REFUSED=2, MQTT_PROTOCOL_V_3_1_1=4 };
typedef struct { int error_type, connect_return_code, esp_tls_stack_err, esp_tls_cert_verify_flags; } mock_error_t;
typedef struct { int retain,qos,topic_len,total_data_len,current_data_offset,data_len,msg_id;
    char *topic,*data; mock_error_t *error_handle; } mock_event_t, *esp_mqtt_event_handle_t;
typedef void *esp_mqtt_client_handle_t;
typedef struct {
    struct { struct { const char *uri; } address;
             struct { const char *certificate; bool skip_cert_common_name_check; } verification; } broker;
    struct { const char *client_id,*username; struct { const char *password; } authentication; } credentials;
    struct { int protocol_ver,keepalive; bool disable_clean_session; } session;
    struct { bool disable_auto_reconnect; int timeout_ms; } network;
    struct { int priority,stack_size; } task;
    struct { int size,out_size; } buffer;
    struct { int limit; } outbox;
} esp_mqtt_client_config_t;
static unsigned starts, stops, subscriptions, publishes, acks;
static int next_id=10;
static esp_mqtt_client_config_t observed;
static char last_topic[129], last_payload[400];
static esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config) { observed=*config; return (void *)1; }
static int esp_mqtt_client_register_event(void *client,int id,void (*cb)(void*,esp_event_base_t,int32_t,void*),void *arg) {
    (void)client;(void)id;(void)cb;(void)arg; return ESP_OK;
}
static int esp_mqtt_client_start(void *c) { CHECK(c!=NULL); ++starts; return ESP_OK; }
static int esp_mqtt_client_stop(void *c) { CHECK(c!=NULL); ++stops; return ESP_OK; }
static int esp_mqtt_client_destroy(void *c) { CHECK(c!=NULL); return ESP_OK; }
static int esp_mqtt_client_subscribe(void *c,const char *topic,int qos) {
    CHECK(c!=NULL && qos==1 && !strchr(topic,'#') && !strchr(topic,'+')); ++subscriptions; return next_id++;
}
static int esp_mqtt_client_enqueue(void *c,const char *topic,const char *payload,int len,int qos,int retain,bool store) {
    (void)len; CHECK(c!=NULL && qos==1 && retain==0 && store); ++publishes;
    snprintf(last_topic,sizeof(last_topic),"%s",topic); snprintf(last_payload,sizeof(last_payload),"%s",payload);
    if (strstr(topic,"/ack")) ++acks;
    return next_id++;
}

#include "device_mqtt_service.inc"

static mqtt_worker_t *worker;
static mqtt_settings_t scratch;
static device_handshake_response_t config;
static void step(void) { mqtt_worker_step(worker,&scratch); }
static void signal_event(int id, int msg_id, int code) {
    mqtt_notice_t event={.id=id,.msg_id=msg_id,.code=code};
    CHECK(xQueueSend(s_control,&event,0)); step();
}
static void receive_task(const char *id) {
    char payload[400];
    snprintf(payload,sizeof(payload),"{\"task_id\":\"%s\",\"kind\":\"reminder\",\"content\":\"原文播报\",\"fire_at\":\"2020-01-01T01:02:03.000Z\"}",id);
    mock_event_t e={.topic=config.mqtt.subscribe_topic,.topic_len=(int)strlen(config.mqtt.subscribe_topic),
        .qos=1,.data=payload,.data_len=(int)strlen(payload),.total_data_len=(int)strlen(payload)};
    mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step();
}
static void connect_ok(void) {
    signal_event(MQTT_EVENT_CONNECTED,0,0);
    signal_event(MQTT_EVENT_SUBSCRIBED,worker->subscribe_id,1);
    CHECK(worker->subscribed);
    signal_event(MQTT_EVENT_PUBLISHED,worker->status_id,0);
}
int main(void)
{
#ifdef TEST_NO_CA
    CHECK(device_mqtt_service_init()==ESP_ERR_NOT_FOUND);
    device_mqtt_service_poll(true,1,&config);
    CHECK(!device_mqtt_service_voice_enabled() && !device_mqtt_service_take_task() && !starts);
    printf("Device MQTT missing-CA guard: %u checks passed\n",checks);
    return 0;
#endif
    strcpy(config.mqtt.endpoint,"mqtts://broker.example:8883");
    strcpy(config.mqtt.username,"dev-1");
    strcpy(config.mqtt.client_id,"issued-id");
    strcpy(config.mqtt.password,"test-only");
    strcpy(config.mqtt.publish_topic,"devices/dev-1/status");
    strcpy(config.mqtt.subscribe_topic,"devices/dev-1/task");
    CHECK(device_mqtt_service_init()==ESP_OK);
    worker=worker_argument;
    device_mqtt_service_poll(true,1,&config); step();
    CHECK(starts==1 && worker->storage_ready);
    CHECK(observed.session.protocol_ver==MQTT_PROTOCOL_V_3_1_1 && observed.session.keepalive==60);
    CHECK(!observed.session.disable_clean_session && observed.network.disable_auto_reconnect);
    CHECK(observed.broker.verification.certificate && !observed.broker.verification.skip_cert_common_name_check);
    CHECK(!strcmp(observed.credentials.client_id,"issued-id"));
    connect_ok(); CHECK(publishes==1 && subscriptions==1);
#if !DEVICE_MQTT_FORWARD_TASK_TO_WS
    receive_task("protocol-pending");
    receive_task("protocol-pending");
    CHECK(worker->pending_count==1 && !worker->active);
    CHECK(!device_mqtt_service_voice_enabled() && !device_mqtt_service_take_task());
    CHECK(acks==0 && commits==0);
    printf("Device MQTT undefined-voice-protocol guard: %u checks passed\n",checks);
    return 0;
#endif
    CHECK(!strcmp(last_topic,config.mqtt.publish_topic));
    cJSON *status=cJSON_Parse(last_payload); CHECK(cJSON_IsNumber(cJSON_GetObjectItem(status,"rssi"))); cJSON_Delete(status);
    device_mqtt_service_poll(true,1,NULL); step(); CHECK(stops==0 && starts==1); /* WS 续期不重连 MQTT */
    fake_now+=60000; step(); CHECK(publishes==2);
    step(); CHECK(publishes==2); /* 未收到 PUBACK，不堆积状态。 */
    signal_event(MQTT_EVENT_PUBLISHED,worker->status_id,0);
    receive_task("first"); device_mqtt_delivery_t *task=device_mqtt_service_take_task();
    CHECK(task && !strcmp(task->message.id,"first") && acks==0 && commits==0);
    receive_task("first"); CHECK(worker->pending_count==0 && acks==0);
    device_mqtt_service_finish_task(task,false); step(); CHECK(acks==0 && commits==0);
    receive_task("first"); task=device_mqtt_service_take_task(); CHECK(task);
    fail_commit=true; device_mqtt_service_finish_task(task,true); step();
    CHECK(worker->dirty && acks==0 && !have_blob);
    receive_task("first"); CHECK(!worker->active && !worker->pending_count && acks==0);
    fail_commit=false; fake_now+=5000; step(); CHECK(commits==1 && acks==1 && !worker->dirty);
    CHECK(!strcmp(last_topic,"devices/dev-1/ack") && strstr(last_payload,"first"));
    int index=device_mqtt_history_find(&worker->history,"first");
    CHECK(index>=0); signal_event(MQTT_EVENT_PUBLISHED,worker->ack_ids[index],0);
    fake_now+=100; receive_task("first"); CHECK(acks==2 && !worker->active);
    int expired_ack=worker->ack_ids[index];
    fake_now+=45000; step(); CHECK(acks==3 && worker->ack_ids[index]!=expired_ack);
    signal_event(MQTT_EVENT_PUBLISHED,expired_ack,0); CHECK(worker->ack_pending[index]);
    signal_event(MQTT_EVENT_PUBLISHED,worker->ack_ids[index],0);
    mqtt_worker_t reboot={0}; reboot.settings=worker->settings;
    CHECK(load_history(&reboot) && device_mqtt_history_find(&reboot.history,"first")>=0);
    /* 原始 SUBACK 128 不触发断线重连；0 是实际 QoS0。 */
    mock_event_t e={.msg_id=worker->subscribe_id,.data="\200",.data_len=1};
    mqtt_event(worker,0,MQTT_EVENT_SUBSCRIBED,&e); step();
    CHECK(worker->subscription_rejected && !worker->subscribed && stops==0);
    e.data="\0"; mqtt_event(worker,0,MQTT_EVENT_SUBSCRIBED,&e); step();
    CHECK(worker->subscribed && !worker->subscription_rejected && stops==0);
    /* retained/别人的 topic 不得入队；积压/借出 ID 继续去重。 */
    char *payload="{\"task_id\":\"bad\",\"kind\":\"reminder\",\"content\":\"x\",\"fire_at\":\"2020-01-01T00:00:00Z\"}";
    e=(mock_event_t){.topic=config.mqtt.subscribe_topic,.topic_len=(int)strlen(config.mqtt.subscribe_topic),
        .data=payload,.data_len=(int)strlen(payload),.total_data_len=(int)strlen(payload),.retain=1,.qos=1};
    mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step(); CHECK(!worker->pending_count && !worker->active);
    receive_task("active"); task=device_mqtt_service_take_task(); CHECK(task);
    for(unsigned i=0;i<9;++i){char id[20];snprintf(id,sizeof(id),"pending-%u",i);receive_task(id);}
    CHECK(worker->pending_count==8);
    receive_task("pending-0"); CHECK(worker->pending_count==8);
    device_mqtt_service_finish_task(task,false); step();
    task=device_mqtt_service_take_task(); CHECK(task && !strcmp(task->message.id,"pending-0"));
    device_mqtt_service_finish_task(task,false); step();
    /* 掉网会停 MQTT，恢复重新订阅；鉴权失败不会因 Wi-Fi 变化解除上限。 */
    device_mqtt_service_poll(false,2,NULL); step(); CHECK(!worker->client);
    device_mqtt_service_poll(true,3,&config); step(); CHECK(worker->client);
    unsigned before=subscriptions; connect_ok(); CHECK(subscriptions==before+1);
    for(unsigned i=1;i<=5;++i){
        signal_event(MQTT_EVENT_ERROR,0,5);
        CHECK(worker->retry.auth_failures==i && worker->retry.blocked==(i==5));
        signal_event(MQTT_EVENT_DISCONNECTED,0,0); CHECK(worker->retry.auth_failures==i);
        fake_now=worker->retry.retry_at_ms; step();
    }
    CHECK(!worker->client);
    device_mqtt_service_poll(false,4,NULL); step();
    device_mqtt_service_poll(true,5,&config); step(); CHECK(!worker->client);
    device_mqtt_service_on_wake(); step(); CHECK(worker->client && !worker->retry.blocked);
    printf("Device MQTT service: %u checks passed\n",checks);
    return 0;
}
