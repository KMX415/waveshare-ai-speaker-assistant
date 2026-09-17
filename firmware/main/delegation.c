#include "delegation.h"
#include <string.h>
#include <stdbool.h>

typedef struct {char delegation[128],response[128];cJSON *calls;} pending_t;
static pending_t pending[4];
static const char *string(const cJSON *obj,const char *key) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    return cJSON_IsString(v)?v->valuestring:"";
}
void delegation_reset(void){for(unsigned i=0;i<4;i++){cJSON_Delete(pending[i].calls);memset(&pending[i],0,sizeof(pending[i]));}}
cJSON *delegation_event(const cJSON *envelope) {
    const cJSON *e=cJSON_GetObjectItemCaseSensitive(envelope,"event");
    const char *type=string(e,"type"),*d=string(envelope,"delegation_id");
    const char *r=string(cJSON_GetObjectItemCaseSensitive(e,"response"),"id");
    if(!r[0])r=string(e,"response_id");
    if(!d[0]||strlen(d)>=128||strlen(r)>=128)return NULL;
    pending_t *p=NULL;
    for(unsigned i=0;i<4;i++)if(!strcmp(pending[i].delegation,d) && (!r[0]||!strcmp(pending[i].response,r))){p=&pending[i];break;}
    if(!strcmp(type,"response.created")) {
        if(p||!r[0])return NULL;
        for(unsigned i=0;i<4;i++)if(!pending[i].delegation[0]){p=&pending[i];break;}
        if(!p)return NULL;
        strcpy(p->delegation,d);strcpy(p->response,r);p->calls=cJSON_CreateArray();
    } else if(p && !strcmp(type,"response.output_item.done")) {
        const cJSON *item=cJSON_GetObjectItemCaseSensitive(e,"item");
        if(strcmp(string(item,"type"),"function_call"))return NULL;
        const char *id=string(item,"call_id"),*name=string(item,"name"),*args=string(item,"arguments");
        if(!id[0]||strlen(id)>=128||!name[0]||strlen(name)>64||!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"arguments"))||strlen(args)>1024)return NULL;
        cJSON *call;
        cJSON_ArrayForEach(call,p->calls)if(!strcmp(string(call,"call_id"),id))return NULL;
        if(cJSON_GetArraySize(p->calls)<4)cJSON_AddItemToArray(p->calls,cJSON_Duplicate(item,true));
    } else if(p && (!strcmp(type,"response.completed")||!strcmp(type,"response.failed")||!strcmp(type,"response.cancelled")||!strcmp(type,"response.incomplete"))) {
        cJSON *calls=p->calls;p->calls=NULL;memset(p,0,sizeof(*p));
        if(!strcmp(type,"response.completed")&&cJSON_GetArraySize(calls))return calls;
        cJSON_Delete(calls);
    }
    return NULL;
}
static cJSON *feed(const char *text){cJSON *j=cJSON_Parse(text),*result=delegation_event(j);cJSON_Delete(j);return result;}
unsigned delegation_selftest(void) {
    delegation_reset();unsigned passed=0;cJSON *r;
    feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.created\",\"response\":{\"id\":\"r\"}}}");
    const char *call="{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"c\",\"name\":\"set_volume\",\"arguments\":\"{\\\"volume\\\":40}\"}}}";
    r=feed(call);if(!r)passed|=1;cJSON_Delete(r);feed(call);
    r=feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.completed\",\"response\":{\"id\":\"wrong\",\"output\":[]}}}");
    if(!r)passed|=2;
    cJSON_Delete(r);
    r=feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"output\":[]}}}");
    if(cJSON_GetArraySize(r)==1)passed|=4;
    cJSON_Delete(r);
    r=feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.completed\",\"response\":{\"id\":\"r\"}}}");
    if(!r)passed|=8;
    cJSON_Delete(r);
    feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.created\",\"response\":{\"id\":\"r\"}}}");feed(call);
    r=feed("{\"delegation_id\":\"d\",\"event\":{\"type\":\"response.cancelled\",\"response\":{\"id\":\"r\"}}}");
    if(!r)passed|=16;
    cJSON_Delete(r);delegation_reset();return passed;
}
