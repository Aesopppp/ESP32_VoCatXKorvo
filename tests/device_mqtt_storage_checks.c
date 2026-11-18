/* 使用生产编码器验证持久格式与旧记录兼容，不依赖 NVS 硬件。 */
#include "../main/device_mqtt_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void)
{
    uint8_t bytes[DEVICE_MQTT_STORAGE_MAX];
    device_mqtt_history_t h, decoded;
    mqtt_pending_store_t pending={0}, restored;
    bool legacy;
    device_mqtt_history_init(&h,"dev-1"); device_mqtt_history_add(&h,"000021");
    size_t n=device_mqtt_storage_encode_history(&h,bytes,sizeof(bytes));
    CHECK(n>0 && n<100);
    CHECK(device_mqtt_storage_decode_history(bytes,n,&decoded,&legacy) && !legacy);
    CHECK(!strcmp(decoded.ids[0],"000021") && decoded.next==h.next);
    for(size_t i=0;i<n;++i) CHECK(!device_mqtt_storage_decode_history(bytes,i,&decoded,&legacy));
    CHECK(!device_mqtt_storage_decode_history(bytes,n+1,&decoded,&legacy));
    CHECK(!device_mqtt_storage_encode_history(&h,bytes,n-1));
    CHECK(device_mqtt_storage_decode_history(&h,sizeof(h),&decoded,&legacy) && legacy);
    CHECK(!memcmp(&h,&decoded,sizeof(h)));
    h.version=99; CHECK(!device_mqtt_storage_decode_history(&h,sizeof(h),&decoded,&legacy)); h.version=1;
    for(unsigned i=0;i<DEVICE_TASK_HISTORY_SIZE;++i) {
        memset(h.ids[i],'9',DEVICE_TASK_ID_MAX); h.ids[i][0]=(char)('0'+i%10);
        h.ids[i][1]=(char)('0'+i/10); h.ids[i][DEVICE_TASK_ID_MAX]=0;
    }
    h.next=31;
    n=device_mqtt_storage_encode_history(&h,bytes,sizeof(bytes)); CHECK(n>4000);
    CHECK(device_mqtt_storage_decode_history(bytes,n,&decoded,&legacy) && !legacy);
    CHECK(!memcmp(&h,&decoded,sizeof(h)));
    bytes[4]=32; CHECK(!device_mqtt_storage_decode_history(bytes,n,&decoded,&legacy));
    memset(h.ids[31],'9',sizeof(h.ids[31])); CHECK(!device_mqtt_storage_encode_history(&h,bytes,sizeof(bytes)));

    pending.version=MQTT_PENDING_VERSION; strcpy(pending.device_id,"dev-1");
    n=device_mqtt_storage_encode_pending(&pending,bytes,sizeof(bytes)); CHECK(n>0 && n<30);
    CHECK(device_mqtt_storage_decode_pending(bytes,n,&restored,&legacy) && !legacy && !restored.count);
    pending.count=DEVICE_TASK_QUEUE_SIZE;
    for(unsigned i=0;i<pending.count;++i) {
        mqtt_pending_entry_t *p=&pending.entries[i];
        memset(p->id,'0',DEVICE_TASK_ID_MAX); p->id[127]=(char)('0'+i); p->id[128]=0;
        p->received_at=i==7 ? INT64_MAX : 1790000000+i;
        p->state=i%3; p->failure_pending=i%2;
    }
    n=device_mqtt_storage_encode_pending(&pending,bytes,sizeof(bytes)); CHECK(n>1000 && n<sizeof(pending));
    CHECK(device_mqtt_storage_decode_pending(bytes,n,&restored,&legacy) && !legacy);
    CHECK(!memcmp(&pending,&restored,sizeof(pending)));
    for(size_t i=0;i<n;++i) CHECK(!device_mqtt_storage_decode_pending(bytes,i,&restored,&legacy));
    CHECK(!device_mqtt_storage_decode_pending(bytes,n+1,&restored,&legacy));
    CHECK(!device_mqtt_storage_encode_pending(&pending,bytes,n-1));
    CHECK(device_mqtt_storage_decode_pending(&pending,sizeof(pending),&restored,&legacy) && legacy);
    CHECK(!memcmp(&pending,&restored,sizeof(pending)));
    n=device_mqtt_storage_encode_pending(&pending,bytes,sizeof(bytes)); bytes[n-3]=0xff;
    CHECK(!device_mqtt_storage_decode_pending(bytes,n,&restored,&legacy)); /* Unix 秒不能溢出 int64。 */
    pending.entries[0].received_at=-1; CHECK(!device_mqtt_storage_encode_pending(&pending,bytes,sizeof(bytes)));
    pending.entries[0].received_at=0; pending.count=9;
    CHECK(!device_mqtt_storage_encode_pending(&pending,bytes,sizeof(bytes)));
    printf("Device MQTT storage: %u checks passed\n",checks); return 0;
}
