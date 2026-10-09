#include "telemetry_client.h"

#include "telemetry_json.h"
#include "dual_dispense_controller.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "job_queue.h"
#include "mqtt_link_core.h"
#include "nvs_config.h"
#include "run_log.h"
#include "safety_manager.h"
#include "sdkconfig.h"
#include "weight_receiver.h"
#include "websocket_client.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "TELEMETRY"
#define CAPTURE_DEPTH 64U
#define SAMPLE_DEPTH 48U
#define UPLOAD_MS 500U
#define BODY_CAP 4800U
/* Server hands out <=10 commands per poll, each <= TELEMETRY_MQTT_RX_MAX (1024)
 * on the wire: 10 pinned JOBs + separators fit. Overflow is handled (prefix). */
#define COMMAND_RESPONSE_CAP 12288U
#define COMMAND_OBJECT_CAP (TELEMETRY_MQTT_RX_MAX+1U) /* same ceiling as MQTT */
#define PROFILE_RESPONSE_CAP 768U
#define MQTT_STATUS_MS 1000U      /* HTTP status stays 2000 ms; the live UI polls 1 s */
#define MQTT_COMPLETE_GRACE_MS 1500U /* runs/complete (HTTP) trails the last MQTT publish */
#define URGENT_DEPTH 4U
#define URGENT_CAP 512U
#define NORMAL_DEPTH 6U
#define CMD_SEEN_MAX 32U

#ifndef CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP
#define CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP ""
#endif
#ifndef CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_PORT
#define CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_PORT 8000
#endif
#ifndef CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY
#define CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY ""
#endif
#ifndef CONFIG_WEIGHT_DEMO_TELEMETRY_DEVICE_ID
#define CONFIG_WEIGHT_DEMO_TELEMETRY_DEVICE_ID ""
#endif

typedef struct {
    weight_msg_t msg;
    uint32_t received_ms;
    dual_channel_snapshot_t channels[2];
} capture_t;
typedef struct {
    uint32_t idx, elapsed_ms, uptime_ms, seq, age_ms;
    int32_t weight_g, target_g, error_g;
    float p, i, d, output;
    bool has_seq, stable, relay;
    char state[24];
    char stage[16];
    char scale_owner[12];
} sample_t;
typedef struct {
    uint32_t job_id, started_ms, sample_idx, event_idx;
    uint32_t batch_seq, dropped_samples;   /* MQTT run lifecycle (CONTRACT 9.2) */
    uint8_t material_id;
    int32_t target_g, start_g, max_weight_g;
    bool allocated, started, completion_pending, terminal_event_sent, mqtt_used;
    uint32_t last_mqtt_ms;
    char run_id[65];
    char last_event_state[24];
    dispense_job_t finished;
    sample_t ring[SAMPLE_DEPTH];
    uint8_t head, count;
} run_t;

static QueueHandle_t s_capture_q;
static run_t *s_runs;   /* [2], heap: allocated by run_state_alloc() so an unused client costs no static RAM */
static _Atomic uint32_t s_dropped;
static _Atomic uint32_t s_capture_peak;
static char s_device_id[65];
static char *s_body;    /* BODY_CAP, heap (see run_state_alloc) */
/* The uploader task has a 7 KiB stack. Keep the JSON work buffers in static
 * storage: the status body is over 6 KiB and the command response alone is
 * 8 KiB, so automatic arrays here corrupt the task stack as soon as network
 * traffic arrives. These helpers are called only by telemetry_task. */
static char s_status_json[3400];
static char s_status_body[3600];
static char s_command_response[COMMAND_RESPONSE_CAP];
static char s_command_object[COMMAND_OBJECT_CAP];
static uint32_t s_last_command_poll;
static bool upload_event(run_t *r,uint8_t ch,const char *event,const char *state,uint32_t now,int32_t grams,bool terminal);
static esp_err_t request(const char *method, const char *path, const char *body,
                         char *response, size_t response_cap);

static bool run_state_alloc(void)
{
    if(!s_runs)s_runs=calloc(2U,sizeof(run_t));
    if(!s_body)s_body=malloc(BODY_CAP);
    return s_runs&&s_body;
}

static bool enabled(void) { return CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP[0] != '\0'; }
static uint32_t ticks_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* MQTT transport (registered by main). Null ops = HTTP only. */
static telemetry_mqtt_ops_t s_ops;
void telemetry_client_set_mqtt_ops(const telemetry_mqtt_ops_t *ops)
{
    if (ops) s_ops = *ops; else memset(&s_ops, 0, sizeof(s_ops));
}
const char *telemetry_client_device_id(void) { return s_device_id; }
static bool mqtt_up(void) { return s_ops.connected && s_ops.publish && s_ops.connected(); }
static bool mqtt_pub(const char *suffix, const char *json, int qos)
{
    return mqtt_up() && s_ops.publish(suffix, json, qos, false);
}

/* Both flags default OFF (NVS run_mqtt / req_pin_prof): with them off the run
 * lifecycle stays HTTP + QoS 0 and a pin-less JOB keeps its legacy fallback. */
static bool s_run_mqtt;      /* CONTRACT 9.2: run/start|samples|events|complete over MQTT QoS 1 */
static bool s_require_pin;   /* CONTRACT 9.3: refuse a JOB without a pinned profile */
/* NVS hash_check, default OFF: profile_hash verification, its canonicalisation buffer and
 * applied_profile in the ACK. Off = a JOB is handled exactly as before 9.3 (hash ignored). */
static bool s_hash_check;
static bool s_run_track;     /* run history state (s_runs/s_body/capture queue) is available */
static uint32_t s_run_alloc_failed;
bool telemetry_client_run_mqtt_enabled(void) { return s_run_mqtt; }
bool telemetry_client_require_pin(void) { return s_require_pin; }
bool telemetry_client_hash_check_enabled(void) { return s_hash_check; }
void telemetry_client_test_set_flags(bool run_mqtt, bool require_pin)
{
    s_run_mqtt = run_mqtt;
    s_require_pin = require_pin;
}
void telemetry_client_test_set_hash_check(bool on) { s_hash_check = on; }
static bool run_publish(const char *suffix, const char *json, int qos, bool retain)
{
    return s_ops.publish != NULL && s_ops.publish(suffix, json, qos, retain);
}
static void next_message_id(char *out, size_t cap)
{
    static uint32_t n;
    snprintf(out, cap, "%s-%lu", mqtt_link_boot_id(), (unsigned long)++n);
}
void telemetry_client_run_ack(const char *json, size_t len)
{
    if (s_run_mqtt) runlog_on_ack(json, len);
}
/* run_mqtt task only (never mqtt_cmd, never a control task): acks come from the link's own
 * ack queue, so the run ring lock is taken here and nowhere near the command path. */
size_t telemetry_client_run_ack_drain(void)
{
    char buf[MQTT_LINK_ARX_PAYLOAD_MAX + 1U];
    size_t len = 0U, n = 0U;
    while (s_ops.run_ack_pop != NULL && n < 6U && s_ops.run_ack_pop(buf, sizeof(buf), &len, 0U)) {
        telemetry_client_run_ack(buf, len);
        n++;
    }
    return n;
}

/* One lock for the small shared decision state (dedupe rings, high-water
 * marks, READY ledger, cancel ledger) now that two tasks dispatch commands.
 * Held only for short RAM/controller work, never across network I/O. Created
 * statically before any task runs so the QEMU suite needs no start-up. */
static StaticSemaphore_t s_state_lock_buf;
static SemaphoreHandle_t s_state_lock;
static void __attribute__((constructor)) state_lock_init(void)
{
    s_state_lock = xSemaphoreCreateMutexStatic(&s_state_lock_buf);
}
static void state_lock(void) { if (s_state_lock) (void)xSemaphoreTake(s_state_lock, portMAX_DELAY); }
static void state_unlock(void) { if (s_state_lock) (void)xSemaphoreGive(s_state_lock); }

static int build_device_status(char *status,size_t cap,uint32_t now)
{
    size_t used=(size_t)snprintf(status,cap,"{\"channels\":[");
    for(uint8_t ch=1;ch<=2;ch++){
        dual_channel_snapshot_t s;
        if(!dual_dispense_controller_snapshot(ch,now,&s))return -1;
        int n=snprintf(status+used,cap-used,
            "%s{\"material_id\":\"M%u\",\"pump_id\":\"Pump %u\",\"channel_id\":\"CH%u\",\"relay_id\":\"Relay %u\",\"scale_id\":\"Scale %u\",\"active_job_id\":%lu,\"target_g\":%ld,\"weight_g\":%ld,\"error_g\":%ld,"
            "\"weight_valid\":%s,\"weight_age_ms\":%lu,\"stable\":%s,\"state\":\"%s\",\"stage\":\"%s\",\"scale_owner\":\"%s\",\"relay_on\":%s,\"manual\":%s,\"output\":%.6f,"
            "\"kp\":%.7g,\"ki\":%.7g,\"kd\":%.7g,\"profile_id\":\"%s\",\"profile_version\":%lu,"
            "\"queue_depth\":%lu,\"elapsed_ms\":%lu,\"fault\":\"%s\"}",
            ch==1?"":",",ch,ch,ch,ch,ch,(unsigned long)s.active_job_id,(long)s.target_g,(long)s.current_weight_g,(long)s.error_g,
            s.have_weight&&s.weight_age_ms<=WEIGHT_MESSAGE_TIMEOUT_MS?"true":"false",(unsigned long)s.weight_age_ms,s.stable?"true":"false",
            dual_dispense_state_name(s.state),dispense_stage_name(s.stage),scale_owner_name(s.scale_owner),
            s.relay_on?"true":"false",s.manual?"true":"false",(double)s.output,
            (double)s.kp,(double)s.ki,(double)s.kd,s.profile_id,(unsigned long)s.profile_version,
            (unsigned long)s.queue_depth,(unsigned long)s.elapsed_ms,s.fault?s.fault:"");
        if(n<0||(size_t)n>=cap-used)return -1;
        used+=(size_t)n;
    }
    int n=snprintf(status+used,cap-used,"],\"queue\":[");
    if(n<0||(size_t)n>=cap-used)return -1;
    used+=(size_t)n;
    dispense_job_t jobs[JOB_QUEUE_MAX];uint32_t count=job_queue_snapshot(jobs,JOB_QUEUE_MAX);
    for(uint32_t i=0;i<count;i++){
        /* held/promoted ride along so the operator UI can show a parked row and
         * the line order without asking a second endpoint. Serialisation only â€”
         * the flag semantics themselves are covered by the job_queue QEMU tests. */
        n=snprintf(status+used,cap-used,
            "%s{\"id\":%lu,\"material_id\":\"M%u\",\"channel_id\":%u,\"target_g\":%ld,\"priority\":%u,\"enqueue_sequence\":%lu,\"state\":\"%s\",\"held\":%s,\"promoted\":%s,\"queued_ms\":%lu}",
            i?",":"",(unsigned long)jobs[i].id,(unsigned)jobs[i].material_id,(unsigned)jobs[i].channel_id,(long)jobs[i].target_g,
            (unsigned)jobs[i].priority,(unsigned long)jobs[i].seq,job_state_name(jobs[i].state),
            jobs[i].held?"true":"false",jobs[i].promoted?"true":"false",(unsigned long)jobs[i].requested_ms);
        if(n<0||(size_t)n>=cap-used)return -1;
        used+=(size_t)n;
    }
    /* Single-scale state (CONTRACT 9.11): which job waits for the scale to be moved,
     * where it must go, which sender boot the READY has to quote, and the interlock
     * the server mirrors for ZERO/TARE. Observability only. */
    dual_scale_info_t si;dual_dispense_controller_scale_info(now,&si);
    char aw[64]="null",ns[8]="null",sid[24]="null",sbid[16]="null",sw[16]="null";
    if(si.await_job_id)snprintf(aw,sizeof(aw),"{\"job_id\":%lu,\"channel\":\"CH%u\"}",
        (unsigned long)si.await_job_id,(unsigned)si.await_channel);
    if(si.needs_station)snprintf(ns,sizeof(ns),"\"CH%u\"",(unsigned)si.needs_station);
    if(si.scale_id[0])snprintf(sid,sizeof(sid),"\"%s\"",si.scale_id);
    if(si.boot_id[0])snprintf(sbid,sizeof(sbid),"\"%s\"",si.boot_id);
    if(si.online)snprintf(sw,sizeof(sw),"%ld",(long)si.weight_g);
    n=snprintf(status+used,cap-used,
        "],\"safety_fault\":\"%s\",\"role\":\"relay_controller\",\"boot_id\":\"%s\","
        "\"weight_link\":\"%s\",\"ws_connected\":%s,"
        "\"awaiting_scale_move\":%s,\"needs_station\":%s,\"scale_id\":%s,\"scale_boot_id\":%s,"
        "\"scale_in_transit\":%s,\"scale_online\":%s,\"scale_weight_g\":%s,\"scale_stable\":%s,"
        "\"uptime_ms\":%lu}",
        safety_fault_name(safety_manager_fault()),mqtt_link_boot_id(),
        weight_link_state_name(weight_receiver_link_state()),
        websocket_client_is_connected()?"true":"false",
        aw,ns,sid,sbid,si.in_transit?"true":"false",si.online?"true":"false",sw,si.stable?"true":"false",
        (unsigned long)now);
    if(n<0||(size_t)n>=cap-used)return -1;
    return (int)(used+(size_t)n);
}

/* Test seam: the status object exactly as it is published; its length, or -1 if it does not fit. */
int telemetry_client_status_json(char *buf,size_t cap,uint32_t now)
{
    return buf&&cap?build_device_status(buf,cap,now):-1;
}

static void upload_device_status(uint32_t now)
{
    char *status=s_status_json;
    char *body=s_status_body;
    int n=build_device_status(status,sizeof(s_status_json),now);
    if(n<0)return;
    /* MQTT telemetry/status body = the inner status object (QoS 0). It reaches
     * the same server code as POST /device/status, so HTTP is skipped while the
     * link is up and resumes the moment it is not. */
    if(mqtt_pub("telemetry/status",status,0))return;
    n=snprintf(body,sizeof(s_status_body),"{\"device_id\":\"%s\",\"status\":%s}",s_device_id,status);
    if(n>0&&(size_t)n<sizeof(s_status_body))(void)request("POST","/api/v1/device/status",body,NULL,0);
}

static esp_err_t request(const char *method, const char *path, const char *body,
                         char *response, size_t response_cap)
{
    char url[220];
    int n = snprintf(url, sizeof(url), "http://%s:%d%s",
        CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP,
        CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_PORT, path);
    if (n < 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t cfg = {.url=url,.method=method[0]=='G'?HTTP_METHOD_GET:HTTP_METHOD_POST,
        .timeout_ms=1600,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    if (response && response_cap) { response[0]='\0'; esp_http_client_set_user_data(client, response); }
    esp_http_client_set_header(client,"Content-Type","application/json");
    if (CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY[0])
        esp_http_client_set_header(client,"X-API-Key",CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY);
    if (body) esp_http_client_set_post_field(client,body,(int)strlen(body));
    esp_err_t err=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err==ESP_OK && status>=200 && status<300 ? ESP_OK : (err!=ESP_OK?err:ESP_FAIL);
}

typedef struct { char *buf; size_t cap; bool truncated; } response_buf_t;
static void response_append(response_buf_t *response,const void *data,size_t add)
{
    /* First overflow latches `truncated` and nothing is appended after it: a
     * dropped middle chunk followed by a small later chunk would otherwise join
     * the head of one command to the tail of another into a balanced object. */
    if (response->truncated) return;
    char *buf=response->buf;
    size_t have=strlen(buf);
    if (have+add<response->cap) { memcpy(buf+have,data,add); buf[have+add]='\0'; }
    else response->truncated=true;
}
bool telemetry_client_test_response_append(char *buf,size_t cap,bool *truncated,
                                           const char *data,size_t len)
{
    response_buf_t r={.buf=buf,.cap=cap,.truncated=truncated?*truncated:false};
    response_append(&r,data,len);
    if(truncated)*truncated=r.truncated;
    return !r.truncated;
}
static esp_err_t profile_event(esp_http_client_event_t *evt)
{
    if (evt->event_id==HTTP_EVENT_ON_DATA && evt->user_data && evt->data && evt->data_len>0)
        response_append((response_buf_t *)evt->user_data,evt->data,(size_t)evt->data_len);
    return ESP_OK;
}

/* `truncated` (optional) reports a body that overflowed `cap`; the buffer then
 * holds a clean prefix of the body ending on a chunk boundary. */
static esp_err_t get_json(const char *path, char *response, size_t cap, bool *truncated)
{
    if(truncated)*truncated=false;
    char url[220];
    int n=snprintf(url,sizeof(url),"http://%s:%d%s",CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP,
                   CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_PORT,path);
    if(n<0||(size_t)n>=sizeof(url)) return ESP_ERR_INVALID_SIZE;
    response[0]='\0';
    response_buf_t buffer={.buf=response,.cap=cap};
    esp_http_client_config_t cfg={.url=url,.method=HTTP_METHOD_GET,.timeout_ms=1200,
                                  .event_handler=profile_event,.user_data=&buffer};
    esp_http_client_handle_t c=esp_http_client_init(&cfg); if(!c)return ESP_ERR_NO_MEM;
    if(CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY[0]) esp_http_client_set_header(c,"X-API-Key",CONFIG_WEIGHT_DEMO_TELEMETRY_API_KEY);
    esp_err_t e=esp_http_client_perform(c); int status=esp_http_client_get_status_code(c); esp_http_client_cleanup(c);
    if(truncated)*truncated=buffer.truncated;
    return e==ESP_OK&&status==200?ESP_OK:(e!=ESP_OK?e:ESP_FAIL);
}

/* Value start for "key": tolerating whitespace after the colon. The HTTP
 * route emits compact JSON, but the MQTT bridge uses json.dumps defaults
 * (`"key": value`), so both must parse identically. */
static const char *field_value_in(const char *json,const char *to,const char *key)
{
    char needle[48]; snprintf(needle,sizeof(needle),"\"%s\":",key);
    const char *p=strstr(json,needle); if(!p||(to&&p>=to))return NULL; p+=strlen(needle);
    while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
    return p;
}
static const char *field_value(const char *json,const char *key){return field_value_in(json,NULL,key);}
static bool number_value(const char *p,double *out)
{
    if(!p||!strncmp(p,"null",4))return false;
    char *end=NULL; double v=strtod(p,&end); if(end==p||!isfinite(v))return false; *out=v; return true;
}
static bool uint_value(const char *p,uint32_t *out)
{ double d; if(!number_value(p,&d)||d<0||d>4294967295.0)return false; *out=(uint32_t)d; return true; }
static bool string_value(const char *p,char *out,size_t cap)
{
    if(!p||!cap||*p!='\"')return false;
    p++;
    const char *end=strchr(p,'\"');if(!end||(size_t)(end-p)>=cap)return false;
    memcpy(out,p,(size_t)(end-p));out[end-p]='\0';return true;
}
static bool number_field(const char *json,const char *key,double *out)
{ return number_value(field_value(json,key),out); }
static bool uint_field(const char *j,const char *key,uint32_t *out)
{ return uint_value(field_value(j,key),out); }
static bool string_field(const char *json,const char *key,char *out,size_t cap)
{ return string_value(field_value(json,key),out,cap); }
/* End ('}') of the brace-balanced object starting at *p, or NULL if unterminated. */
static const char *object_end(const char *p)
{
    int depth=0;bool quoted=false,escaped=false;
    for(;*p;p++){
        char ch=*p;
        if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='\"')quoted=false;}
        else if(ch=='\"')quoted=true;
        else if(ch=='{')depth++;
        else if(ch=='}'&&--depth==0)return p;
    }
    return NULL;
}
/* M4: range-checked BEFORE the cast (a double outside long's range is UB to
 * cast); out of range or non-numeric yields `fallback`. 32-bit long on the target. */
static long long_field(const char *json,const char *key,long fallback)
{
    double value;
    if(!number_field(json,key,&value))return fallback;
    if(!(value>=-2147483648.0&&value<=2147483647.0))return fallback;
    return (long)value;
}

static bool fetch_profile(uint8_t material_id,uint8_t ch,int32_t target_g,uint32_t now);

/* Optional staged-dispense fields on a PROFILE payload. Missing fields fall
 * back to the Kconfig defaults; present fields override them so each target
 * profile carries its own COARSE/FINE/MICRO/SETTLING behaviour. */
typedef struct {
    int32_t coarse_threshold_g, fine_threshold_g, micro_threshold_g;
    uint32_t coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms;
    uint32_t settle_time_ms;
    int32_t inflight_comp_g;
    /* OPTIONAL NO_PROGRESS thresholds (CONTRACT 9.11), [0] COARSE [1] FINE [2] MICRO;
     * 0 = absent, the controller learns its bound. Pinned jobs only. */
    uint32_t np_window_ms[3];
    int32_t np_min_rise_g[3];
} staged_profile_fields_t;

static void staged_profile_fields_default(staged_profile_fields_t *s)
{
    memset(s->np_window_ms, 0, sizeof(s->np_window_ms));
    memset(s->np_min_rise_g, 0, sizeof(s->np_min_rise_g));
    s->coarse_threshold_g = CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G;
    s->fine_threshold_g   = CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G;
    s->micro_threshold_g  = CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G;
    s->coarse_min_on_ms   = CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS;
    s->fine_min_on_ms     = CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS;
    s->micro_min_on_ms    = CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS;
    s->settle_time_ms     = CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS;
    s->inflight_comp_g    = CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G;
}

/* Legacy PROFILE command / profile fetch path (the values go on to the store
 * path's own range checks). The float-to-integer cast is range-guarded here: an
 * out-of-range or non-finite value becomes -1 / 0, which the store path refuses,
 * instead of an undefined cast. */
static int32_t legacy_i32(double v){return (isfinite(v)&&v>=0.0&&v<=1e9)?(int32_t)v:-1;}
static uint32_t legacy_u32(double v){return (isfinite(v)&&v>=0.0&&v<=4e9)?(uint32_t)v:0U;}
/* Gains: out of float range or non-finite becomes NaN, which the store path
 * refuses (!isfinite). */
static float legacy_f32(double v){return (isfinite(v)&&fabs(v)<=1e30)?(float)v:NAN;}
/* Target weight: 0 (never valid) unless finite and in 1..1e9. */
static int32_t legacy_target(double v){return (isfinite(v)&&v>=1.0&&v<=1e9)?(int32_t)v:0;}
/* DELIBERATE difference from the pin path: legacy values are TRUNCATED to
 * integers (60.5 -> 60), the pin path refuses a fraction (PIN_NOT_INTEGER). The
 * range/ordering/pulse/saturation/target rules are the same function in both. */
/* A staged key that is ABSENT keeps its default; one that is PRESENT but not a
 * finite number (string, null, NaN, 1e999) refuses the whole command instead of
 * silently running the device default in its place. Returns false on refusal. */
#define STG_LEGACY(key,conv,field) do{ \
    if(field_value(json,key)){ \
        if(!number_field(json,key,&v))return false; \
        s->field=conv(v); \
    } \
}while(0)
static bool staged_profile_fields_parse(const char *json, staged_profile_fields_t *s)
{
    double v;
    STG_LEGACY("coarse_threshold_g",legacy_i32,coarse_threshold_g);
    STG_LEGACY("fine_threshold_g",legacy_i32,fine_threshold_g);
    STG_LEGACY("micro_threshold_g",legacy_i32,micro_threshold_g);
    STG_LEGACY("coarse_min_on_ms",legacy_u32,coarse_min_on_ms);
    STG_LEGACY("fine_min_on_ms",legacy_u32,fine_min_on_ms);
    STG_LEGACY("micro_min_on_ms",legacy_u32,micro_min_on_ms);
    STG_LEGACY("settle_time_ms",legacy_u32,settle_time_ms);
    STG_LEGACY("inflight_comp_g",legacy_i32,inflight_comp_g);
    return true;
}
#undef STG_LEGACY

/* ---- PRE-02: strict pin validation (JOB command, nested "profile" object) ----
 * Every numeric is range-checked as a double BEFORE any integer cast, taken only
 * from the nested object's own members, and refused on duplicate keys. Bounds
 * mirror the store path (set_pending_profile_staged) and the server schema. */
/* PLACEHOLDER pending operator decision: equals the server schema upper bound
 * (TuningProfileFields.max_duration_ms le=3_600_000). */
#define ABS_MAX_DURATION_CAP_MS 3600000U
#define PIN_GAIN_MAX 1.0
#define PIN_G_MAX 100000.0          /* tolerance / overshoot / thresholds */
#define PIN_WINDOW_MIN_MS 50.0
#define PIN_WINDOW_MAX_MS 10000.0
#define PIN_MIN_MS_MAX 10000.0      /* min_on / min_off / stage min_on pre-cast cap */
#define PIN_SETTLE_MAX_MS 60000.0
#define PIN_INFLIGHT_MAX_G 10000.0

static bool json_ws(char ch){return ch==' '||ch=='\t'||ch=='\r'||ch=='\n';}
static bool json_digit(char ch){return ch>='0'&&ch<='9';}

/* Strict RFC 8259 number at p (no hex, inf, nan, leading '+', or trailing junk). */
static bool strict_number(const char *p,double *out)
{
    if(!p)return false;
    const char *s=p;
    if(*s=='-')s++;
    if(*s=='0')s++;
    else if(*s>='1'&&*s<='9'){while(json_digit(*s))s++;}
    else return false;
    if(*s=='.'){s++;if(!json_digit(*s))return false;while(json_digit(*s))s++;}
    if(*s=='e'||*s=='E'){s++;if(*s=='+'||*s=='-')s++;if(!json_digit(*s))return false;while(json_digit(*s))s++;}
    if(*s&&*s!=','&&*s!='}'&&!json_ws(*s))return false;
    char *e=NULL;double v=strtod(p,&e);
    if(e!=s||!isfinite(v))return false;
    *out=v;return true;
}

/* Skip one JSON value starting at p (bounded by end); returns the byte after it. */
static const char *skip_value(const char *p,const char *end)
{
    if(p<end&&*p=='\"'){
        p++;
        while(p<end&&*p!='\"'){if(*p=='\\'&&p+1<end)p++;p++;}
        return p<end?p+1:end;
    }
    if(p<end&&(*p=='{'||*p=='[')){
        int depth=0;bool quoted=false,escaped=false;
        for(;p<end;p++){
            char ch=*p;
            if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='\"')quoted=false;}
            else if(ch=='\"')quoted=true;
            else if(ch=='{'||ch=='[')depth++;
            else if((ch=='}'||ch==']')&&--depth==0)return p+1;
        }
        return end;
    }
    while(p<end&&*p!=','&&*p!='}')p++;
    return p;
}

/* Members named `key` directly inside the object [obj..end] ('{' .. '}'): returns
 * the count and the first one's value start. Nested objects/strings are skipped,
 * so a key spelled inside a string or a sub-object never matches. */
static int object_member(const char *obj,const char *end,const char *key,const char **val)
{
    int count=0;size_t klen=strlen(key);
    if(val)*val=NULL;
    const char *p=obj+1;
    while(p<end){
        while(p<end&&(json_ws(*p)||*p==','))p++;
        if(p>=end||*p!='\"')break;
        const char *ks=++p;
        while(p<end&&*p!='\"'){if(*p=='\\'&&p+1<end)p++;p++;}
        if(p>=end)break;
        size_t kl=(size_t)(p-ks);p++;
        while(p<end&&json_ws(*p))p++;
        if(p>=end||*p!=':')break;
        p++;
        while(p<end&&json_ws(*p))p++;
        if(kl==klen&&!memcmp(ks,key,klen)){if(!count&&val)*val=p;count++;}
        p=skip_value(p,end);
    }
    return count;
}

const char *tc_json_object_end(const char *p){return object_end(p);}
int tc_json_object_member(const char *obj,const char *end,const char *key,const char **val)
{ return object_member(obj,end,key,val); }

static void pin_refuse(char *error,size_t cap,const char *code,const char *key)
{
    if(error&&cap)snprintf(error,cap,"%s %s",code,key);
}

/* 1 = present and valid, 0 = absent, -1 = refused (error filled). */
static int pin_number(const char *obj,const char *end,const char *key,double lo,double hi,
                      bool integral,double *out,char *error,size_t cap)
{
    const char *v=NULL;
    int n=object_member(obj,end,key,&v);
    if(n>1){pin_refuse(error,cap,"PIN_DUPLICATE_KEY",key);return -1;}
    if(n==0)return 0;
    double d;
    if(!strict_number(v,&d)){pin_refuse(error,cap,"PIN_NOT_A_NUMBER",key);return -1;}
    if(d<lo||d>hi){pin_refuse(error,cap,"PIN_OUT_OF_RANGE",key);return -1;}
    if(integral&&d!=floor(d)){pin_refuse(error,cap,"PIN_NOT_INTEGER",key);return -1;}
    *out=d;return 1;
}

/* Identity version: a strict non-negative INTEGER (3 and 3.0 pass, 1.9 does not).
 * 1 = present and valid, 0 = absent, -1 = refused (error filled). */
static int pin_uint_member(const char *obj,const char *end,const char *key,uint32_t *out,
                           char *error,size_t cap)
{
    const char *v=NULL;
    int n=object_member(obj,end,key,&v);
    if(n>1){pin_refuse(error,cap,"PIN_DUPLICATE_KEY",key);return -1;}
    if(n==0)return 0;
    double d;
    if(!strict_number(v,&d)){pin_refuse(error,cap,"PIN_NOT_A_NUMBER",key);return -1;}
    if(d<0.0||d>4294967295.0){pin_refuse(error,cap,"PIN_OUT_OF_RANGE",key);return -1;}
    if(d!=floor(d)){pin_refuse(error,cap,"PIN_NOT_INTEGER",key);return -1;}
    *out=(uint32_t)d;return 1;
}

/* Optional staged fields from the nested object only; absent keys keep defaults. */
static bool staged_fields_parse_nested(const char *obj,const char *end,staged_profile_fields_t *s,
                                       char *error,size_t cap)
{
    double v;int r;
    #define STG(key,hi,lo,field,type) do{ \
        r=pin_number(obj,end,key,lo,hi,true,&v,error,cap); \
        if(r<0)return false; \
        if(r>0)s->field=(type)v; \
    }while(0);
    STG("coarse_threshold_g",PIN_G_MAX,0.0,coarse_threshold_g,int32_t)
    STG("fine_threshold_g",PIN_G_MAX,0.0,fine_threshold_g,int32_t)
    STG("micro_threshold_g",PIN_G_MAX,0.0,micro_threshold_g,int32_t)
    STG("coarse_min_on_ms",PIN_MIN_MS_MAX,1.0,coarse_min_on_ms,uint32_t)
    STG("fine_min_on_ms",PIN_MIN_MS_MAX,1.0,fine_min_on_ms,uint32_t)
    STG("micro_min_on_ms",PIN_MIN_MS_MAX,1.0,micro_min_on_ms,uint32_t)
    STG("settle_time_ms",PIN_SETTLE_MAX_MS,1.0,settle_time_ms,uint32_t)
    STG("inflight_comp_g",PIN_INFLIGHT_MAX_G,0.0,inflight_comp_g,int32_t)
    STG("np_coarse_window_ms",3600000.0,1.0,np_window_ms[0],uint32_t)
    STG("np_fine_window_ms",3600000.0,1.0,np_window_ms[1],uint32_t)
    STG("np_micro_window_ms",3600000.0,1.0,np_window_ms[2],uint32_t)
    STG("np_coarse_min_rise_g",PIN_G_MAX,1.0,np_min_rise_g[0],int32_t)
    STG("np_fine_min_rise_g",PIN_G_MAX,1.0,np_min_rise_g[1],int32_t)
    STG("np_micro_min_rise_g",PIN_G_MAX,1.0,np_min_rise_g[2],int32_t)
    #undef STG
    return true;
}

/* L1: structural validation of a whole JSON value, strict grammar (RFC 8259
 * scalars, balanced containers, comma-separated members, no trailing comma). The
 * per-key walkers above stop silently at the first malformed member; this is the
 * check that the REST of the object is well formed too. Bounded by `limit`. */
#define PIN_JSON_MAX_DEPTH 6
static const char *json_skip_ws(const char *p,const char *limit){while(p<limit&&json_ws(*p))p++;return p;}
static const char *json_string_end(const char *p,const char *limit)
{
    p++;
    while(p<limit){
        unsigned char ch=(unsigned char)*p;
        if(ch=='\"')return p+1;
        if(ch<0x20U)return NULL;
        if(ch=='\\'){
            if(++p>=limit)return NULL;
            if(*p=='u'){
                for(int i=0;i<4;i++){
                    if(++p>=limit||!isxdigit((unsigned char)*p))return NULL;
                }
            }else if(!strchr("\"\\/bfnrt",*p)||*p=='\0')return NULL;
        }
        p++;
    }
    return NULL;
}
static const char *json_value_end(const char *p,const char *limit,int depth)
{
    if(p>=limit)return NULL;
    if(*p=='\"')return json_string_end(p,limit);
    if(*p=='{'||*p=='['){
        if(depth>=PIN_JSON_MAX_DEPTH)return NULL;
        const char close=(*p=='{')?'}':']';
        const bool obj=(*p=='{');
        p=json_skip_ws(p+1,limit);
        if(p<limit&&*p==close)return p+1;
        for(;;){
            if(obj){
                if(p>=limit||*p!='\"')return NULL;
                p=json_string_end(p,limit);if(!p)return NULL;
                p=json_skip_ws(p,limit);
                if(p>=limit||*p!=':')return NULL;
                p=json_skip_ws(p+1,limit);
            }
            p=json_value_end(p,limit,depth+1);if(!p)return NULL;
            p=json_skip_ws(p,limit);
            if(p>=limit)return NULL;
            if(*p==close)return p+1;
            if(*p!=',')return NULL;
            p=json_skip_ws(p+1,limit);
        }
    }
    static const char *const lits[]={"true","false","null"};
    for(size_t i=0;i<3;i++){
        size_t n=strlen(lits[i]);
        if((size_t)(limit-p)>=n&&!memcmp(p,lits[i],n))return p+n;
    }
    const char *s=p;
    if(s<limit&&*s=='-')s++;
    if(s<limit&&*s=='0')s++;
    else if(s<limit&&*s>='1'&&*s<='9'){while(s<limit&&json_digit(*s))s++;}
    else return NULL;
    if(s<limit&&*s=='.'){s++;if(s>=limit||!json_digit(*s))return NULL;while(s<limit&&json_digit(*s))s++;}
    if(s<limit&&(*s=='e'||*s=='E')){
        s++;if(s<limit&&(*s=='+'||*s=='-'))s++;
        if(s>=limit||!json_digit(*s))return NULL;
        while(s<limit&&json_digit(*s))s++;
    }
    return s;
}
/* [obj..end] must be exactly one well-formed object ('{' .. '}'). */
static bool json_object_valid(const char *obj,const char *end)
{
    if(!obj||!end||*obj!='{'||*end!='}')return false;
    return json_value_end(obj,end+1,0)==end+1;
}

static void pin_error(char *error, size_t cap, const char *text)
{
    if(error&&cap){ snprintf(error,cap,"%s",text); }
}

job_pin_result_t telemetry_client_parse_job_pin(const char *object,
                                                uint8_t material_id,
                                                int32_t target_g,
                                                job_profile_t *out,
                                                char *error, size_t error_cap)
{
    if(!object||!out)return JOB_PIN_REFUSED;
    memset(out,0,sizeof(*out));

    /* A pin is "present" when the command names a profile at all. The server
     * sends profile_id + profile_version at the top level and the gains under
     * a nested "profile" object; a legacy command has neither. */
    /* The nested gains object is delimited once, and every lookup is confined to
     * its own region: top-level pin keys outside it, gains inside it. A command
     * spliced from two commands can then never borrow fields across the seam. */
    /* L2: the top-level members (profile, profile_id, profile_version) are found
     * by the same depth-1 walker as the nested ones, not by substring search, so
     * a key spelled inside a string or sub-object never matches, whitespace
     * before ':' is fine, and a top-level duplicate is refused. */
    const char *top=object;
    while(json_ws(*top))top++;
    const char *top_end=(*top=='{')?object_end(top):NULL;
    if(!top_end){
        pin_error(error,error_cap,"PIN_MALFORMED command is not one JSON object");
        return JOB_PIN_REFUSED;
    }
    const char *nested=NULL;
    const char *nend=NULL;
    int profile_n=object_member(top,top_end,"profile",&nested);
    if(profile_n>1){
        pin_refuse(error,error_cap,"PIN_DUPLICATE_KEY","profile");
        return JOB_PIN_REFUSED;
    }
    if(profile_n==1&&nested&&*nested=='{'){
        nend=object_end(nested);
        if(!nend){
            pin_error(error,error_cap,"pinned profile object is incomplete");
            return JOB_PIN_REFUSED;
        }
    }
    char pin_id[65]={0};
    uint32_t pin_version=0;
    const char *vp=NULL;
    int idn_top=object_member(top,top_end,"profile_id",&vp);
    if(idn_top>1){
        pin_refuse(error,error_cap,"PIN_DUPLICATE_KEY","profile_id");
        return JOB_PIN_REFUSED;
    }
    bool has_id=idn_top==1&&string_value(vp,pin_id,sizeof(pin_id));
    int ver_top=pin_uint_member(top,top_end,"profile_version",&pin_version,error,error_cap);
    if(ver_top<0)return JOB_PIN_REFUSED;
    bool has_version=ver_top==1;
    if(!has_id&&!has_version){
        /* Also treat a bare nested "profile" object as a pin: older payloads
         * may omit the top-level echo. */
        double probe;
        if(!number_field(object,"kp",&probe))return JOB_PIN_ABSENT;
    }
    if(has_id!=has_version||pin_version==0U){
        pin_error(error,error_cap,"pinned profile is missing profile_id or profile_version");
        return JOB_PIN_REFUSED;
    }

    /* Gains live only in the nested "profile" object. If that object is null
     * (the version was deleted after the job was queued) there is nothing to
     * stage â€” refuse the job rather than run someone else's tuning. */
    /* PRE-02: each value is range-checked (finite, typed, bounded, integral where
     * an integer) before it is cast; a duplicate key refuses the pin. */
    if(!nend){
        pin_error(error,error_cap,"pinned profile gains are missing (version deleted after queueing?)");
        return JOB_PIN_REFUSED;
    }
    double kp=0,ki=0,kd=0,tolerance=0,overshoot=0,duration=0,window=0,min_on=0,min_off=0;
    #define GAIN(key,lo,hi,integral,var) do{ \
        int gr_=pin_number(nested,nend,key,lo,hi,integral,&(var),error,error_cap); \
        if(gr_<0)return JOB_PIN_REFUSED; \
        if(gr_==0){pin_error(error,error_cap,"pinned profile gains are missing (version deleted after queueing?)"); \
                   return JOB_PIN_REFUSED;} \
    }while(0)
    GAIN("kp",0.0,PIN_GAIN_MAX,false,kp);
    GAIN("ki",0.0,PIN_GAIN_MAX,false,ki);
    GAIN("kd",0.0,PIN_GAIN_MAX,false,kd);
    GAIN("tolerance_g",0.0,PIN_G_MAX,true,tolerance);
    GAIN("max_overshoot_g",0.0,PIN_G_MAX,true,overshoot);
    GAIN("max_duration_ms",1.0,(double)ABS_MAX_DURATION_CAP_MS,true,duration);
    GAIN("window_ms",PIN_WINDOW_MIN_MS,PIN_WINDOW_MAX_MS,true,window);
    GAIN("min_on_ms",1.0,PIN_MIN_MS_MAX,true,min_on);
    GAIN("min_off_ms",1.0,PIN_MIN_MS_MAX,true,min_off);
    #undef GAIN
    /* Exact identity: the nested gains object must name the same profile id AND
     * version as the pin the job is addressed to. Missing or different means the
     * gains cannot be proven to belong to this job â€” refuse, never substitute. */
    char nested_id[65]={0};
    uint32_t nested_version=0;
    const char *idv=NULL;
    int idn=object_member(nested,nend,"profile_id",&idv);
    if(idn>1){
        pin_refuse(error,error_cap,"PIN_DUPLICATE_KEY","profile_id");
        return JOB_PIN_REFUSED;
    }
    int vern=pin_uint_member(nested,nend,"version",&nested_version,error,error_cap);
    if(vern<0)return JOB_PIN_REFUSED;
    if(!string_value(idv,nested_id,sizeof(nested_id))||vern!=1){
        pin_error(error,error_cap,"pinned profile gains carry no profile id/version");
        return JOB_PIN_REFUSED;
    }
    if(strcmp(nested_id,pin_id)!=0){
        pin_error(error,error_cap,"pinned profile id does not match its gains");
        return JOB_PIN_REFUSED;
    }
    if(nested_version!=pin_version){
        pin_error(error,error_cap,"pinned profile version does not match its gains");
        return JOB_PIN_REFUSED;
    }

    /* The pin is addressed to this job's material and target. The server
     * validated that at queue time; re-check the address here so a malformed
     * payload can never attach the wrong job's tuning. */
    if(material_id!=1U&&material_id!=2U){
        pin_error(error,error_cap,"pinned profile has an invalid material mapping");
        return JOB_PIN_REFUSED;
    }
    if(target_g!=5000&&target_g!=10000&&target_g!=15000&&target_g!=20000){
        pin_error(error,error_cap,"pinned profile target is not a canonical weight");
        return JOB_PIN_REFUSED;
    }

    staged_profile_fields_t staged;
    staged_profile_fields_default(&staged);
    if(!staged_fields_parse_nested(nested,nend,&staged,error,error_cap))return JOB_PIN_REFUSED;

    /* L1: the WHOLE nested object must be well-formed JSON. The member walkers
     * above stop at the first malformed member, so trailing junk after a number
     * ("0.5 x"), a missing comma ('"kp":0.1 "kp":0.9') or any other malformation
     * is caught here. Per-key checks run first so a bad value keeps its specific
     * PIN_* text. */
    if(!json_object_valid(nested,nend)){
        pin_error(error,error_cap,"PIN_MALFORMED profile object is not valid JSON");
        return JOB_PIN_REFUSED;
    }

    /* Consistency of the EFFECTIVE profile (staged defaults included), by the
     * same function the store path uses: min_on/min_off vs window, threshold
     * order, stage can pulse (REFUSE, not warn: a stage that never pulses stalls
     * the job to CHANNEL_TIMEOUT), min_off saturation, tolerance/overshoot vs
     * the target. */
    const dual_profile_limits_t limits={
        .target_g=target_g,.tolerance_g=(int32_t)tolerance,.max_overshoot_g=(int32_t)overshoot,
        .window_ms=(uint32_t)window,.min_on_ms=(uint32_t)min_on,.min_off_ms=(uint32_t)min_off,
        .coarse_threshold_g=staged.coarse_threshold_g,.fine_threshold_g=staged.fine_threshold_g,
        .micro_threshold_g=staged.micro_threshold_g,.coarse_min_on_ms=staged.coarse_min_on_ms,
        .fine_min_on_ms=staged.fine_min_on_ms,.micro_min_on_ms=staged.micro_min_on_ms,
    };
    const char *why=dual_dispense_profile_consistency_error(&limits);
    if(why){
        pin_error(error,error_cap,why);
        return JOB_PIN_REFUSED;
    }

    out->valid=true;
    snprintf(out->profile_id,sizeof(out->profile_id),"%s",pin_id);
    out->version=pin_version;
    out->kp=(float)kp; out->ki=(float)ki; out->kd=(float)kd;
    out->tolerance_g=(int32_t)tolerance;
    out->max_overshoot_g=(int32_t)overshoot;
    out->max_duration_ms=(uint32_t)duration;
    out->window_ms=(uint32_t)window;
    out->min_on_ms=(uint32_t)min_on;
    out->min_off_ms=(uint32_t)min_off;
    out->coarse_threshold_g=staged.coarse_threshold_g;
    out->fine_threshold_g=staged.fine_threshold_g;
    out->micro_threshold_g=staged.micro_threshold_g;
    out->coarse_min_on_ms=staged.coarse_min_on_ms;
    out->fine_min_on_ms=staged.fine_min_on_ms;
    out->micro_min_on_ms=staged.micro_min_on_ms;
    out->settle_time_ms=staged.settle_time_ms;
    out->inflight_comp_g=staged.inflight_comp_g;
    for(uint8_t i=0;i<3U;i++){
        out->np_window_ms[i]=staged.np_window_ms[i];
        out->np_min_rise_g[i]=staged.np_min_rise_g[i];
    }
    return JOB_PIN_PARSED;
}

/* ---- CONTRACT 9.3: profile_hash --------------------------------------------
 * SHA-256 over the canonical compact JSON of the JOB's nested profile{} (members
 * sorted by key, no whitespace outside strings, every value token verbatim as the
 * server sent it), first 16 hex. Own implementation: no dependency on the mbedtls
 * API generation, and a QEMU test pins it to the FIPS "abc" vector. */
static uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }

static void sha256_once(const uint8_t *msg, size_t len, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const size_t padded = ((len + 9U + 63U) / 64U) * 64U;
    const uint64_t bits = (uint64_t)len * 8U;
    for (size_t off = 0; off < padded; off += 64U) {
        uint32_t w[64];
        for (size_t t = 0; t < 16U; t++) {
            uint32_t v = 0;
            for (size_t b = 0; b < 4U; b++) {
                size_t i = off + t * 4U + b;
                uint8_t byte = i < len ? msg[i] : i == len ? 0x80U
                    : i >= padded - 8U ? (uint8_t)(bits >> (8U * (padded - 1U - i))) : 0U;
                v = (v << 8) | byte;
            }
            w[t] = v;
        }
        for (size_t t = 16; t < 64U; t++) {
            uint32_t s0 = rotr32(w[t-15], 7) ^ rotr32(w[t-15], 18) ^ (w[t-15] >> 3);
            uint32_t s1 = rotr32(w[t-2], 17) ^ rotr32(w[t-2], 19) ^ (w[t-2] >> 10);
            w[t] = w[t-16] + s0 + w[t-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (size_t t = 0; t < 64U; t++) {
            uint32_t t1 = hh + (rotr32(e,6)^rotr32(e,11)^rotr32(e,25)) + ((e&f)^(~e&g)) + K[t] + w[t];
            uint32_t t2 = (rotr32(a,2)^rotr32(a,13)^rotr32(a,22)) + ((a&b)^(a&c)^(b&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (size_t i = 0; i < 8U; i++) {
        out[i*4U] = (uint8_t)(h[i] >> 24); out[i*4U+1U] = (uint8_t)(h[i] >> 16);
        out[i*4U+2U] = (uint8_t)(h[i] >> 8); out[i*4U+3U] = (uint8_t)h[i];
    }
}
void telemetry_client_test_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    sha256_once(msg, len, out);
}

#define CANON_CAP 1024U
#define CANON_MEMBERS 32U
typedef struct { const char *k; size_t kl; const char *v; const char *ve; } canon_member_t;

static bool canon_put(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    if (*n + len >= cap) return false;
    memcpy(out + *n, s, len);
    *n += len;
    return true;
}

/* Whitespace outside strings dropped; everything else verbatim. */
static bool canon_compact(char *out, size_t cap, size_t *n, const char *p, const char *e)
{
    bool quoted = false, escaped = false;
    for (; p < e; p++) {
        char ch = *p;
        if (!quoted && json_ws(ch)) continue;
        if (quoted) { if (escaped) escaped = false; else if (ch == '\\') escaped = true; else if (ch == '\"') quoted = false; }
        else if (ch == '\"') quoted = true;
        if (!canon_put(out, cap, n, &ch, 1U)) return false;
    }
    return true;
}

static bool profile_canonical(const char *nested, const char *nend, char *out, size_t cap, size_t *outlen)
{
    canon_member_t m[CANON_MEMBERS];
    size_t count = 0;
    const char *p = nested + 1;
    while (p < nend) {
        while (p < nend && (json_ws(*p) || *p == ',')) p++;
        if (p >= nend || *p != '\"') break;
        const char *ks = ++p;
        while (p < nend && *p != '\"') { if (*p == '\\' && p + 1 < nend) p++; p++; }
        if (p >= nend) return false;
        size_t kl = (size_t)(p - ks);
        p++;
        while (p < nend && json_ws(*p)) p++;
        if (p >= nend || *p != ':') return false;
        p++;
        while (p < nend && json_ws(*p)) p++;
        const char *vs = p, *after = skip_value(p, nend), *ve = after;
        while (ve > vs && json_ws(ve[-1])) ve--;
        if (ve == vs || count == CANON_MEMBERS) return false;
        m[count++] = (canon_member_t){ks, kl, vs, ve};
        p = after;
    }
    for (size_t i = 1; i < count; i++) {   /* insertion sort by key bytes */
        canon_member_t cur = m[i];
        size_t j = i;
        while (j > 0) {
            size_t ml = m[j-1].kl < cur.kl ? m[j-1].kl : cur.kl;
            int c = memcmp(m[j-1].k, cur.k, ml);
            if (c < 0 || (c == 0 && m[j-1].kl <= cur.kl)) break;
            m[j] = m[j-1];
            j--;
        }
        m[j] = cur;
    }
    size_t n = 0;
    if (!canon_put(out, cap, &n, "{", 1U)) return false;
    for (size_t i = 0; i < count; i++) {
        if ((i && !canon_put(out, cap, &n, ",", 1U)) || !canon_put(out, cap, &n, "\"", 1U) ||
            !canon_put(out, cap, &n, m[i].k, m[i].kl) || !canon_put(out, cap, &n, "\":", 2U) ||
            !canon_compact(out, cap, &n, m[i].v, m[i].ve)) return false;
    }
    if (!canon_put(out, cap, &n, "}", 1U)) return false;
    *outlen = n;
    return true;
}

bool telemetry_client_job_profile_hash(const char *object, char out[17])
{
    if (!object || !out) return false;
    const char *top = object;
    while (json_ws(*top)) top++;
    const char *top_end = (*top == '{') ? object_end(top) : NULL;
    const char *nested = NULL;
    if (!top_end || object_member(top, top_end, "profile", &nested) != 1 || !nested || *nested != '{') return false;
    const char *nend = object_end(nested);
    /* Heap, not stack: 1 KiB would sit in the 6 KiB command task stacks. Only reached
     * with hash_check on. */
    char *canon = malloc(CANON_CAP);
    size_t n = 0;
    if (!canon) return false;
    if (!nend || !profile_canonical(nested, nend, canon, CANON_CAP, &n)) { free(canon); return false; }
    uint8_t d[32];
    sha256_once((const uint8_t *)canon, n, d);
    free(canon);
    for (size_t i = 0; i < 8U; i++) snprintf(out + i * 2U, 3U, "%02x", d[i]);
    return true;
}

/* Hash of the profile the JOB carries, checked against the server's profile_hash
 * when it sends one. `phash` always receives the computed hash. */
static bool job_pin_hash_check(const char *object, char phash[17], char *error, size_t cap)
{
    phash[0] = '\0';
    if (!telemetry_client_job_profile_hash(object, phash)) {
        pin_error(error, cap, "PROFILE_HASH_UNAVAILABLE profile object cannot be canonicalised");
        return false;
    }
    const char *top = object;
    while (json_ws(*top)) top++;
    const char *top_end = (*top == '{') ? object_end(top) : NULL;
    const char *v = NULL;
    int n = top_end ? object_member(top, top_end, "profile_hash", &v) : 0;
    if (n == 0) return true;   /* server does not send one yet: the relay still reports it */
    char sent[24];
    if (n > 1 || !string_value(v, sent, sizeof(sent)) || strlen(sent) != 16U) {
        pin_error(error, cap, "PROFILE_HASH_MISMATCH profile_hash is not 16 hex");
        return false;
    }
    for (size_t i = 0; i < 16U; i++) {
        char c = sent[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (c != phash[i]) {
            pin_error(error, cap, "PROFILE_HASH_MISMATCH profile_hash does not match the pinned profile");
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Manual pump command dedup
 *
 * Delivery is at-least-once: an un-ACKed command is re-served on every poll
 * until the ACK lands. For PUMP_START this is dangerous â€” a redelivered START
 * could re-energize a relay after the operator pressed Stop. Track the last
 * N executed manual command IDs and skip re-execution on duplicates.
 * ------------------------------------------------------------------------ */
#define MANUAL_CMD_DEDUP_MAX 16U
static uint32_t s_manual_cmd_ring[MANUAL_CMD_DEDUP_MAX];
static uint8_t s_manual_cmd_ring_pos;
static uint32_t s_manual_high_water[2];
static uint32_t s_ready_high_water[2];
#define READY_RING_MAX 8U
typedef struct { uint32_t id; uint8_t channel; bool ok; } ready_rec_t;
static ready_rec_t s_ready_ring[READY_RING_MAX];
static uint8_t s_ready_ring_pos;
static uint32_t s_remote_cancels[32];
static uint8_t s_remote_cancel_count;
static bool s_remote_cancel_full;

bool telemetry_client_note_remote_cancel(uint32_t id)
{
    if(!id)return false;
    bool ok=true;
    state_lock();
    bool known=false;
    for(uint8_t i=0;i<s_remote_cancel_count;i++)if(s_remote_cancels[i]==id)known=true;
    if(!known){
        if(s_remote_cancel_count==32){s_remote_cancel_full=true;ok=false;}
        else s_remote_cancels[s_remote_cancel_count++]=id;
    }
    state_unlock();
    return ok;
}

bool telemetry_client_remote_job_cancelled(uint32_t id)
{
    bool hit=false;
    state_lock();
    if(s_remote_cancel_full)hit=true; /* fail closed, never evict a cancellation */
    for(uint8_t i=0;i<s_remote_cancel_count;i++)if(s_remote_cancels[i]==id)hit=true;
    state_unlock();
    return hit;
}

static bool manual_command_already_executed(uint32_t command_id)
{
    for (uint8_t i = 0; i < MANUAL_CMD_DEDUP_MAX; ++i) {
        if (s_manual_cmd_ring[i] == command_id) return true;
    }
    return false;
}

static void mark_manual_command_executed(uint32_t command_id)
{
    s_manual_cmd_ring[s_manual_cmd_ring_pos] = command_id;
    s_manual_cmd_ring_pos = (uint8_t)((s_manual_cmd_ring_pos + 1U) % MANUAL_CMD_DEDUP_MAX);
}

void telemetry_client_test_reset_ready(void)
{
    memset(s_manual_cmd_ring,0,sizeof(s_manual_cmd_ring));
    s_manual_cmd_ring_pos=0;
    s_ready_high_water[0]=s_ready_high_water[1]=0;
    memset(s_ready_ring,0,sizeof(s_ready_ring));
    s_ready_ring_pos=0;
}

/* Every processed READY id (applied OR refused) is recorded with its outcome,
 * so a re-served id gets the original answer and is never applied later. */
static void ready_record(uint32_t id,uint8_t channel,bool ok)
{
    s_ready_ring[s_ready_ring_pos]=(ready_rec_t){id,channel,ok};
    s_ready_ring_pos=(uint8_t)((s_ready_ring_pos+1U)%READY_RING_MAX);
    if(id>s_ready_high_water[channel-1U])s_ready_high_water[channel-1U]=id;
}

/* ACK error text of a refused READY: the stable code first (the server and the app
 * key on it), then a short human reason. Static storage: *error points here. */
static const char *ready_error_text(dual_ready_result_t r)
{
    switch(r){
    case READY_BAD_CHANNEL:         return "READY_BAD_CHANNEL channel required";
    case READY_NOT_WAITING:         return "READY_NOT_WAITING channel is not awaiting operator ready";
    case READY_JOB_MISMATCH:        return "READY_JOB_MISMATCH job_id is not the job waiting on this channel";
    case READY_SCALE_BOOT_MISMATCH: return "READY_SCALE_BOOT_MISMATCH scale_boot_id is not the live scale boot_id";
    case READY_STALE:               return "READY_STALE READY is older than ready_max_age";
    case READY_SCALE_OFFLINE:       return "READY_SCALE_OFFLINE no fresh weight from the scale";
    case READY_SCALE_UNSTABLE:      return "READY_SCALE_UNSTABLE scale not stable long enough";
    case READY_SCALE_BUSY:          return "READY_SCALE_BUSY another job owns the scale";
    case READY_BLOCKED:             return "READY_BLOCKED E-Stop, safety fault, channel fault or manual hold";
    case READY_START_WEIGHT:        return "READY_START_WEIGHT start weight above start_max_g";
    case READY_NO_PROFILE:          return "READY_NO_PROFILE no profile for this target";
    case READY_LEGACY_DISABLED:     return "READY_LEGACY_DISABLED READY needs job_id and scale_boot_id";
    case READY_MALFORMED:           return "READY_MALFORMED job_id and scale_boot_id must both be valid";
    default:                        return "READY refused";
    }
}

static bool handle_ready_locked(uint32_t command_id,uint8_t channel,const telemetry_ready_t *rd,
                                uint32_t now,const char **error)
{
    if(!channel||channel>2U){*error="channel required";return false;}
    for(uint8_t i=0;i<READY_RING_MAX;i++){
        if(s_ready_ring[i].id==command_id&&s_ready_ring[i].channel==channel){
            if(s_ready_ring[i].ok){*error="duplicate (already executed)";return true;}
            *error="duplicate (previously refused)";return false;
        }
    }
    /* READY is not tied to a job: ids are server-monotonic, so anything at or
     * below the last processed id is a replay and must never release a later job. */
    if(command_id<=s_ready_high_water[channel-1U]){
        *error="stale READY (older than last processed)";return false;
    }
    dual_ready_result_t r;
    if(rd==NULL){
        r=dual_dispense_controller_legacy_ready(channel,now);   /* {channel} only: migration, logged */
    }else if(rd->malformed||rd->has_job_id!=rd->has_boot){
        r=READY_MALFORMED;
    }else if(!rd->has_job_id){
        r=dual_dispense_controller_legacy_ready(channel,now);
    }else{
        dual_ready_req_t req={.has_job_id=true,.has_boot=true,.job_id=rd->job_id,.age_ms=rd->age_ms};
        snprintf(req.scale_boot_id,sizeof(req.scale_boot_id),"%s",rd->scale_boot_id);
        r=dual_dispense_controller_scale_ready(channel,&req,now);
    }
    if(r!=READY_OK){
        ready_record(command_id,channel,false);
        *error=ready_error_text(r);return false;
    }
    ESP_LOGI(TAG,"READY ch=%u cmd=%lu%s",channel,(unsigned long)command_id,
             (rd&&rd->has_job_id&&!rd->malformed)?"":" (legacy: no job_id/scale_boot_id)");
    ready_record(command_id,channel,true);
    return true;
}

/* The ledger is touched by the HTTP command task and the MQTT command task. */
bool telemetry_client_handle_ready_ex(uint32_t command_id,uint8_t channel,const telemetry_ready_t *rd,
                                      uint32_t now,const char **error)
{
    state_lock();
    bool ok=handle_ready_locked(command_id,channel,rd,now,error);
    state_unlock();
    return ok;
}

bool telemetry_client_handle_ready(uint32_t command_id,uint8_t channel,uint32_t now,const char **error)
{
    return telemetry_client_handle_ready_ex(command_id,channel,NULL,now,error);
}

/* ---------------------------------------------------------------------------
 * Command dispatch: ONE path for the HTTP poll and the MQTT mqtt_cmd task, so
 * every command means exactly the same thing on both transports.
 * ------------------------------------------------------------------------ */
static void ack_http(void *ctx,uint32_t id,const char *state,uint32_t local_job,
                     uint8_t channel,const char *error)
{
    (void)ctx;
    char path[96],body[220];
    snprintf(path,sizeof(path),"/api/v1/device/commands/%lu/ack",(unsigned long)id);
    char job[16];
    if(local_job)snprintf(job,sizeof(job),"%lu",(unsigned long)local_job);else snprintf(job,sizeof(job),"null");
    snprintf(body,sizeof(body),"{\"state\":\"%s\",\"local_job_id\":%s,\"channel_id\":%s,\"error\":\"%s\"}",
        state,job,channel?(channel==1?"\"CH1\"":"\"CH2\""):"null",error?error:"");
    if(request("POST",path,body,NULL,0)!=ESP_OK)
        ESP_LOGW(TAG,"ACK for command %lu failed; will be re-served",(unsigned long)id);
}

/* ACK back on MQTT (QoS 1, cas/{device}/commands/ack). Enqueue only: this never
 * waits on the network, so an ACK can never delay the next command. A lost ACK
 * is repaired by the HTTP poll re-serving the command and the dedupe re-ACKing
 * the stored outcome. */
static void cmd_profile_json(uint32_t id,char *out,size_t cap);
static void ack_mqtt(void *ctx,uint32_t id,const char *state,uint32_t local_job,
                     uint8_t channel,const char *error)
{
    (void)ctx;
    char text[121];size_t o=0;
    for(const char *p=error?error:"";*p&&o<sizeof(text)-1U;p++)
        text[o++]=(*p=='\"'||*p=='\\'||(unsigned char)*p<0x20)?' ':*p;
    text[o]='\0';
    char job[16];
    if(local_job)snprintf(job,sizeof(job),"%lu",(unsigned long)local_job);else snprintf(job,sizeof(job),"null");
    /* applied_profile rides only on a JOB that was really staged (QUEUED/APPLIED). */
    char prof[160]="";
    if(s_hash_check&&(!strcmp(state,"QUEUED")||!strcmp(state,"APPLIED")))cmd_profile_json(id,prof,sizeof(prof));
    char body[640];
    int n=snprintf(body,sizeof(body),
        "{\"command_id\":%lu,\"state\":\"%s\",\"local_job_id\":%s,\"channel_id\":%s,\"error\":\"%s\",\"reason\":\"%s\",\"boot_id\":\"%s\"%s}",
        (unsigned long)id,state,job,channel?(channel==1?"\"CH1\"":"\"CH2\""):"null",text,text,mqtt_link_boot_id(),prof);
    if(n<=0||(size_t)n>=sizeof(body)||!s_ops.publish||!s_ops.publish("commands/ack",body,1,false))
        ESP_LOGW(TAG,"MQTT ACK for command %lu not queued; HTTP will re-serve it",(unsigned long)id);
}

/* Processed-command ledger shared by both transports. A slot is claimed
 * (INFLIGHT) before anything executes, so the same id arriving on the other
 * transport mid-run is dropped rather than run twice. */
enum { SEEN_FREE=0, SEEN_INFLIGHT, SEEN_DONE };
typedef enum { CMD_NEW, CMD_DUP_DONE, CMD_DUP_INFLIGHT, CMD_STALE } cmd_claim_t;
typedef struct {
    uint32_t id,local_job; uint8_t state,channel,lch; char result[8]; char error[96];
    /* CONTRACT 9.3: the profile this JOB was actually staged with (read back from
     * the queued job); empty when none. Replayed on a duplicate re-ACK. */
    bool has_prof; uint32_t prof_ver; char prof_id[65]; char prof_hash[17];
} cmd_seen_t;
static cmd_seen_t s_seen[CMD_SEEN_MAX];
static uint8_t s_seen_pos;
/* Monotonic floor: highest id ever pushed out of the ring (server ids are
 * monotonic). Once an id leaves the ledger its duplicate check is gone, so any
 * non-STOP command at or below the floor that is not in the ledger is refused
 * instead of run. Per channel for the channel-bound JOB/PUMP_START/READY (lch),
 * global for the rest. STOP-class is exempt: it is idempotent and must run. */
static uint32_t s_floor_global,s_floor_ch[2];
static void floor_raise(uint32_t *f,uint32_t id){if(id>*f)*f=id;}
static telemetry_cmd_stats_t s_stats;
#define STAT_INC(field) (void)__atomic_fetch_add(&s_stats.field,1U,__ATOMIC_RELAXED)

static cmd_claim_t cmd_claim(uint32_t id,uint8_t lch,bool stop,cmd_seen_t *copy)
{
    cmd_claim_t r=CMD_NEW;
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id){
        r=s_seen[i].state==SEEN_DONE?CMD_DUP_DONE:CMD_DUP_INFLIGHT;
        if(copy)*copy=s_seen[i];
        break;
    }
    if(r==CMD_NEW&&!stop&&id<=(lch?s_floor_ch[lch-1U]:s_floor_global))r=CMD_STALE;
    if(r==CMD_NEW){
        cmd_seen_t *s=&s_seen[s_seen_pos];
        if(s->state!=SEEN_FREE){
            floor_raise(&s_floor_global,s->id);
            if(s->lch)floor_raise(&s_floor_ch[s->lch-1U],s->id);
        }
        memset(s,0,sizeof(*s));s->id=id;s->lch=lch;s->state=SEEN_INFLIGHT;
        s_seen_pos=(uint8_t)((s_seen_pos+1U)%CMD_SEEN_MAX);
    }
    state_unlock();
    return r;
}

static void cmd_finish(uint32_t id,const char *result,const char *error,uint32_t local_job,uint8_t channel)
{
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id){
        s_seen[i].state=SEEN_DONE;s_seen[i].local_job=local_job;s_seen[i].channel=channel;
        snprintf(s_seen[i].result,sizeof(s_seen[i].result),"%s",result);
        snprintf(s_seen[i].error,sizeof(s_seen[i].error),"%s",error?error:"");
        break;
    }
    state_unlock();
}

static void cmd_set_profile(uint32_t id,const char *profile_id,uint32_t version,const char *hash)
{
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id){
        s_seen[i].has_prof=true;s_seen[i].prof_ver=version;
        snprintf(s_seen[i].prof_id,sizeof(s_seen[i].prof_id),"%s",profile_id);
        snprintf(s_seen[i].prof_hash,sizeof(s_seen[i].prof_hash),"%s",hash);
        break;
    }
    state_unlock();
}

/* `,"applied_profile":{...}` for an ACK, or "" when this command staged none. */
static void cmd_profile_json(uint32_t id,char *out,size_t cap)
{
    out[0]='\0';
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id){
        if(s_seen[i].has_prof){
            char pid[65];size_t o=0;
            for(const char *p=s_seen[i].prof_id;*p&&o<sizeof(pid)-1U;p++)
                pid[o++]=(*p=='\"'||*p=='\\'||(unsigned char)*p<0x20)?'_':*p;
            pid[o]='\0';
            snprintf(out,cap,",\"applied_profile\":{\"profile_id\":\"%s\",\"version\":%lu,\"hash\":\"%s\"}",
                pid,(unsigned long)s_seen[i].prof_ver,s_seen[i].prof_hash);
        }
        break;
    }
    state_unlock();
}

bool telemetry_client_test_applied_profile(uint32_t id,char *profile_id,size_t cap,uint32_t *version,char hash[17])
{
    bool found=false;
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id&&s_seen[i].has_prof){
        snprintf(profile_id,cap,"%s",s_seen[i].prof_id);*version=s_seen[i].prof_ver;
        snprintf(hash,17,"%s",s_seen[i].prof_hash);found=true;break;
    }
    state_unlock();
    return found;
}

static void cmd_forget(uint32_t id)
{
    state_lock();
    for(uint8_t i=0;i<CMD_SEEN_MAX;i++)if(s_seen[i].state!=SEEN_FREE&&s_seen[i].id==id)memset(&s_seen[i],0,sizeof(s_seen[i]));
    state_unlock();
}

/* "command_type" is what the HTTP route sends; MQTT also carries "type". */
static void cmd_type_of(const char *object,char *type,size_t cap)
{
    type[0]='\0';
    if(!string_field(object,"command_type",type,cap))(void)string_field(object,"type",type,cap);
}
static bool type_is_stop(const char *type){return !strcmp(type,"ESTOP")||!strcmp(type,"PUMP_STOP");}

/* M1->CH1/Pump 1/Relay 1/Scale 1, M2->CH2/Pump 2/Relay 2/Scale 2. A command may
 * omit the optional names, but never contradict the fixed mapping. */
static bool fixed_mapping_ok(const char *object,uint8_t n)
{
    static const char *const keys[3]={"pump_id","relay_id","scale_id"};
    static const char *const labels[3]={"Pump","Relay","Scale"};
    for(uint8_t i=0;i<3U;i++){
        char value[16],want[16];
        if(string_field(object,keys[i],value,sizeof(value))){
            snprintf(want,sizeof(want),"%s %u",labels[i],(unsigned)n);
            if(strcmp(value,want))return false;
        }
    }
    return true;
}

void telemetry_client_dispatch_command(const char *object,telemetry_cmd_transport_t via,
                                       uint32_t recv_ms,uint32_t now,
                                       telemetry_cmd_ack_fn ack,void *ctx)
{
    if(!object||!ack)return;
    long id_long=long_field(object,"command_id",0);
    if(id_long<1)return;
    uint32_t command_id=(uint32_t)id_long;
    char type[20]={0},channel_text[8]={0};
    cmd_type_of(object,type,sizeof(type));
    (void)string_field(object,"channel_id",channel_text,sizeof(channel_text));
    bool pump_stop=!strcmp(type,"PUMP_STOP");
    bool urgent=type_is_stop(type);

    uint8_t channel=0;
    if(!strcmp(channel_text,"CH1"))channel=1;else if(!strcmp(channel_text,"CH2"))channel=2;
    uint8_t lch=0; /* ledger channel: only the channel-bound, relay-affecting commands */
    if(!strcmp(type,"PUMP_START")||!strcmp(type,"READY"))lch=channel;
    else if(!strcmp(type,"JOB")){
        char mat[8]={0};(void)string_field(object,"material_id",mat,sizeof(mat));
        lch=!strcmp(mat,"M1")?1U:!strcmp(mat,"M2")?2U:channel;
    }
    cmd_seen_t prior;
    cmd_claim_t claim=cmd_claim(command_id,lch,urgent,&prior);
    if(claim==CMD_STALE&&!strcmp(type,"JOB")){
        /* The ledger record was evicted but the job itself may still be known:
         * if its ACK was lost, report the real state instead of "stale" so an
         * executed job is not mislabelled FAILED. Never re-queues anything. */
        job_view_t view;
        if(job_queue_find_by_remote(command_id,&view)){
            const char *st=view.state==JOB_COMPLETE?"APPLIED":
                (view.state==JOB_QUEUED||view.state==JOB_RUNNING)?"QUEUED":NULL;
            if(st){
                STAT_INC(dup_reacked);
                ack(ctx,command_id,st,view.id,view.channel_id,"");
                return;
            } /* cancelled/failed job: keep the refusal below */
        }
    }
    if(claim==CMD_STALE){
        /* Its dedupe record may have been evicted, so it cannot be proven new.
         * A JOB the queue cannot vouch for may already have run: say so
         * (STALE_UNKNOWN:) rather than a plain refusal. Never enqueued here. */
        STAT_INC(stale_refused);
        ESP_LOGW(TAG,"stale cmd %lu (%s) refused: older than ledger floor",(unsigned long)command_id,type);
        ack(ctx,command_id,"FAILED",0,channel,!strcmp(type,"JOB")?
            "STALE_UNKNOWN: id older than ledger floor, job no longer in queue; outcome unknown":
            "stale: older than ledger floor");
        return;
    }
    if(claim==CMD_DUP_INFLIGHT){STAT_INC(dup_inflight);return;} /* the first run will ACK */
    if(claim==CMD_DUP_DONE&&!pump_stop){
        /* QoS 1 redelivery, or the same id over the other transport: answer
         * with the original outcome and never run it again. */
        STAT_INC(dup_reacked);
        ack(ctx,command_id,prior.result,prior.local_job,prior.channel,prior.error);
        return;
    }

    uint32_t ttl_ms=0;(void)uint_field(object,"ttl_ms",&ttl_ms);
    uint32_t local_job=0;const char *result="APPLIED",*error="";
    char pin_error_text[80]={0};
    char pin_hash[17]={0};
    bool deferred=false;
    uint32_t age=now-recv_ms;
    if(!urgent&&ttl_ms>0U&&(int32_t)age>0&&age>ttl_ms){
        /* No wall clock on the device: expire if not applied within ttl_ms of
         * receipt. A late PUMP_START/READY must never act. STOP never expires. */
        result="FAILED";error="expired (ttl_ms elapsed before it could be applied)";
        STAT_INC(expired);
    }else if(!strcmp(type,"JOB")){
        STAT_INC(cmd_executed);
        if(telemetry_client_remote_job_cancelled(command_id)){
            result="FAILED";error="cancelled before delivery";
        }else{
            int32_t target_g=(int32_t)long_field(object,"target_g",0);
            char material_text[8]={0}; (void)string_field(object,"material_id",material_text,sizeof(material_text));
            uint8_t material_id=!strcmp(material_text,"M1")?1U:!strcmp(material_text,"M2")?2U:0U;
            if(!material_id){
                result="FAILED";error="invalid material_id (M1 or M2 required)";
            }else if(channel&&channel!=material_id){
                result="FAILED";error="material/channel mismatch (M1->CH1, M2->CH2)";
            }else if(!fixed_mapping_ok(object,material_id)){
                result="FAILED";error="material/pump/relay/scale mismatch";
            }else{
                /* The server pins the exact immutable tuning version the operator
                 * chose and ships its gains with the job. Stage THAT, never
                 * "whatever is currently active for this target": two queued jobs
                 * can share a target_g and still pin different versions. A pin
                 * that is present but unusable refuses the job outright â€” there is
                 * no silent fallback to another version. Only a legacy command
                 * with no pin at all falls back to the active-profile fetch. */
                job_profile_t pin;
                job_pin_result_t pin_res=telemetry_client_parse_job_pin(
                    object,material_id,target_g,&pin,pin_error_text,sizeof(pin_error_text));
                if(pin_res==JOB_PIN_REFUSED){
                    result="FAILED";
                    error=pin_error_text[0]?pin_error_text:"pinned profile refused";
                }else if(pin_res==JOB_PIN_ABSENT&&s_require_pin){
                    /* CONTRACT 9.3: never fall back to a device default or the active profile. */
                    result="FAILED";error="PROFILE_REQUIRED: JOB carries no pinned profile";
                }else if(pin_res==JOB_PIN_ABSENT&&via==TELEMETRY_CMD_MQTT){
                    /* The legacy fetch is blocking HTTP: never inside the MQTT
                     * command task, where it would stall a STOP. Leave the row
                     * un-ACKed; the HTTP poll delivers it and runs that path. */
                    deferred=true;
                }else if(pin_res==JOB_PIN_PARSED&&s_hash_check&&
                         !job_pin_hash_check(object,pin_hash,pin_error_text,sizeof(pin_error_text))){
                    result="FAILED";error=pin_error_text;
                }else{
                    if(pin_res==JOB_PIN_ABSENT)
                        (void)fetch_profile(material_id,material_id,target_g,now);
                    esp_err_t e=dual_dispense_controller_add_material_server_job_pinned(
                        material_id,target_g,(uint8_t)long_field(object,"priority",0),
                        command_id,(pin_res==JOB_PIN_PARSED)?&pin:NULL,&local_job);
                    if(e==ESP_OK){
                        result="QUEUED";
                        /* applied_profile is read back from the queued job, so it
                         * reports what was really staged, not what was parsed. */
                        dispense_job_t staged;
                        if(pin_res==JOB_PIN_PARSED&&s_hash_check&&job_queue_get(local_job,&staged)&&staged.pin.valid&&
                           !strcmp(staged.pin.profile_id,pin.profile_id)&&staged.pin.version==pin.version)
                            cmd_set_profile(command_id,staged.pin.profile_id,staged.pin.version,pin_hash);
                    }else{result="FAILED";error=e==ESP_ERR_NO_MEM?"queue full":"invalid job";}
                }
            }
        }
    }else if(!strcmp(type,"CANCEL")){
        STAT_INC(cmd_executed);
        /* Always single-job scoped when the operator named a job. The old
         * path called dual_dispense_controller_cancel(job.channel_id) for a
         * running job, which also wiped every other queued job on that
         * channel â€” cancelling one target weight cleared its siblings. */
        long requested=long_field(object,"local_job_id",0);
        long remote=long_field(object,"remote_command_id",0);
        bool ok=false;
        if(requested>0){
            ok=dual_dispense_controller_cancel_job((uint32_t)requested,now);
        }else if(remote>0){
            job_view_t view;
            if(job_queue_find_by_remote((uint32_t)remote,&view))
                ok=dual_dispense_controller_cancel_job(view.id,now);
            /* CANCEL sorts before JOB on the server. If the job payload
             * is still in flight, remember its unique remote identity. */
            if(!ok)ok=telemetry_client_note_remote_cancel((uint32_t)remote);
        }else if(channel)ok=dual_dispense_controller_cancel(channel,now);
        if(!ok){result="FAILED";error="job is no longer cancellable";}
    }else if(!strcmp(type,"HOLD")||!strcmp(type,"RELEASE")||!strcmp(type,"PROMOTE")){
        STAT_INC(cmd_executed);
        /* Queue order/parking only. These never touch a valve and never
         * interrupt a running job: the queue is non-preemptive. Refused
         * unless the target job is still waiting. */
        long requested=long_field(object,"local_job_id",0);
        long remote=long_field(object,"remote_command_id",0);
        uint32_t target=0U;
        if(requested>0)target=(uint32_t)requested;
        else if(remote>0){
            job_view_t view;
            if(job_queue_find_by_remote((uint32_t)remote,&view))target=view.id;
        }
        bool ok=false;
        if(target>0U){
            ok = !strcmp(type,"HOLD")    ? job_queue_hold(target)
               : !strcmp(type,"RELEASE") ? job_queue_release(target)
               :                            job_queue_promote(target);
        }
        if(!ok){result="FAILED";error="job is not waiting";}
    }else if(!strcmp(type,"PROFILE")){
        STAT_INC(cmd_executed);
        char profile_id[65]={0},profile_material[8]={0};uint32_t version=0;
        double kp,ki,kd,tolerance,overshoot,duration,window,min_on,min_off;
        double target_d=0;
        bool target_seen=number_field(object,"target_g",&target_d);
        int32_t target_g=legacy_target(target_d);
        /* Exact identity: id + version are both required and stored as given.
         * Nothing here ever swaps in a newer profile. */
        bool valid=channel&&target_seen&&target_g!=0&&
            string_field(object,"profile_id",profile_id,sizeof(profile_id))&&
            string_field(object,"material_id",profile_material,sizeof(profile_material))&&
            ((channel==1U&&!strcmp(profile_material,"M1"))||
             (channel==2U&&!strcmp(profile_material,"M2")))&&
            fixed_mapping_ok(object,channel)&&
            uint_field(object,"version",&version)&&version>0U&&number_field(object,"kp",&kp)&&
            number_field(object,"ki",&ki)&&number_field(object,"kd",&kd)&&
            number_field(object,"tolerance_g",&tolerance)&&number_field(object,"max_overshoot_g",&overshoot)&&
            number_field(object,"max_duration_ms",&duration)&&number_field(object,"window_ms",&window)&&
            number_field(object,"min_on_ms",&min_on)&&number_field(object,"min_off_ms",&min_off);
        if(!valid){result="FAILED";error="invalid profile configuration (id, version, target_g and material/channel mapping required)";}
        else{
            /* Staged fields are optional; when present they override the
             * derived defaults so each target profile carries its own
             * COARSE/FINE/MICRO/SETTLING behaviour. */
            staged_profile_fields_t staged;
            staged_profile_fields_default(&staged);
            if(!staged_profile_fields_parse(object,&staged)){
                result="FAILED";error="invalid staged profile field (not a finite number)";
            }else if(!dual_dispense_controller_set_pending_profile_staged(channel,profile_id,version,
                target_g,legacy_f32(kp),legacy_f32(ki),legacy_f32(kd),
                legacy_i32(tolerance),legacy_i32(overshoot),legacy_u32(duration),legacy_u32(window),
                legacy_u32(min_on),legacy_u32(min_off),
                staged.coarse_threshold_g,staged.fine_threshold_g,staged.micro_threshold_g,
                staged.coarse_min_on_ms,staged.fine_min_on_ms,staged.micro_min_on_ms,
                staged.settle_time_ms,staged.inflight_comp_g)){
                result="FAILED";error="profile refused (non-canonical target or invalid params)";
            }
        }
    }else if(!strcmp(type,"PROFILE_CLEAR")){
        STAT_INC(cmd_executed);
        char profile_material[8]={0};
        double target_d=0;
        bool target_seen=number_field(object,"target_g",&target_d);
        int32_t target_g=legacy_target(target_d);
        bool valid=channel&&target_seen&&target_g!=0&&
            string_field(object,"material_id",profile_material,sizeof(profile_material))&&
            ((channel==1U&&!strcmp(profile_material,"M1"))||
             (channel==2U&&!strcmp(profile_material,"M2")))&&
            fixed_mapping_ok(object,channel);
        if(!valid){result="FAILED";error="invalid profile material/channel mapping or missing target_g";}
        else if(!dual_dispense_controller_set_pending_default(channel,target_g)){
            result="FAILED";error="could not stage device default profile";
        }
    }else if(!strcmp(type,"PAUSE")){
        STAT_INC(cmd_executed);
        if(!channel||!dual_dispense_controller_pause(channel,now)){result="FAILED";error="channel is not running";}
    }else if(!strcmp(type,"RESUME")){
        STAT_INC(cmd_executed);
        if(!channel||!dual_dispense_controller_resume(channel,now)){result="FAILED";error="channel is not paused";}
    }else if(!strcmp(type,"ESTOP")){
        STAT_INC(cmd_executed);
        /* Idempotent: raising an already-latched E-Stop changes nothing. */
        safety_manager_raise(SAFETY_EMERGENCY_STOP,now);dual_dispense_controller_emergency_stop(now);
    }else if(!strcmp(type,"CLEAR")){
        STAT_INC(cmd_executed);
        bool ok=safety_manager_fault()==SAFETY_EMERGENCY_STOP?safety_manager_release_estop():safety_manager_clear();
        if(ok)dual_dispense_controller_clear_emergency_stop();else{result="FAILED";error="safety latch cannot be cleared";}
    }else if(!strcmp(type,"READY")){
        STAT_INC(cmd_executed);
        /* READY carries {channel_id, job_id, scale_boot_id} (CONTRACT 9.11). A READY
         * with neither identity field is the legacy form; one field alone, or one
         * that does not parse, is refused (READY_MALFORMED). */
        telemetry_ready_t rd;memset(&rd,0,sizeof(rd));
        const char *jv=field_value(object,"job_id"),*bv=field_value(object,"scale_boot_id");
        double jd=0;
        rd.has_job_id=jv!=NULL;rd.has_boot=bv!=NULL;
        if(jv&&!(number_value(jv,&jd)&&jd>=1.0&&jd<=4294967295.0&&jd==floor(jd)))rd.malformed=true;
        else if(jv)rd.job_id=(uint32_t)jd;
        if(bv&&!(string_value(bv,rd.scale_boot_id,sizeof(rd.scale_boot_id))&&strlen(rd.scale_boot_id)==8U))rd.malformed=true;
        rd.age_ms=(int32_t)(now-recv_ms)>0?now-recv_ms:0U;
        if(!telemetry_client_handle_ready_ex(command_id,channel,&rd,now,&error))result="FAILED";
    }else if(!strcmp(type,"PUMP_START")||pump_stop){
        STAT_INC(cmd_executed);
        bool on=!pump_stop;
        if(!channel){result="FAILED";error="channel required";}
        else{
            state_lock();
            if(on&&(manual_command_already_executed(command_id)||command_id<=s_manual_high_water[channel-1U])){
                /* At-least-once delivery: a redelivered START must not
                 * re-energize a relay (and a START older than the last manual
                 * command, e.g. one a later STOP overtook, must not run). */
                ESP_LOGI(TAG,"PUMP cmd %lu already executed; re-ACK only",(unsigned long)command_id);
                result="APPLIED";error="duplicate (already executed)";
            }else{
                /* STOP is always applied, even when the relay is already OFF
                 * or the id was seen: turning a pump off is idempotent. */
                ESP_LOGI(TAG,"PUMP_%s ch=%u cmd=%lu",on?"START":"STOP",channel,(unsigned long)command_id);
                if(on)STAT_INC(manual_start_exec);else STAT_INC(manual_stop_exec);
                if(dual_dispense_controller_manual_relay(channel,on,now)!=ESP_OK){
                    result="FAILED";error="manual pump refused (job active, E-Stop, safety fault, or other relay on)";
                }else{
                    mark_manual_command_executed(command_id);
                    if(command_id>s_manual_high_water[channel-1U])s_manual_high_water[channel-1U]=command_id;
                }
            }
            state_unlock();
        }
    }else{result="FAILED";error="unknown command";}

    if(deferred){
        cmd_forget(command_id);
        STAT_INC(deferred_to_http);
        return;
    }
    cmd_finish(command_id,result,error,local_job,channel);
    ack(ctx,command_id,result,local_job,channel,error);
}

void telemetry_client_http_process_response(const char *response,uint32_t recv_ms,uint32_t now_ms,
                                            telemetry_cmd_ack_fn ack,void *ctx)
{
    char *object=s_command_object;
    /* Pass 0 runs STOP-class commands only, pass 1 everything else, so a STOP
     * in the same response never waits behind earlier JOB/PROFILE work. */
    for(int pass=0;pass<2;pass++){
        const char *cursor=response;
        while((cursor=strchr(cursor,'{'))!=NULL){
            size_t len=0;int depth=0;bool quoted=false,escaped=false;
            const char *end=cursor;
            for(;*end;end++){
                char ch=*end;
                if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='\"')quoted=false;}
                else if(ch=='\"')quoted=true;
                else if(ch=='{')depth++;
                else if(ch=='}'&&--depth==0){len=(size_t)(end-cursor+1);break;}
            }
            if(!len)break; /* unterminated: nothing more to read */
            if(len>=sizeof(s_command_object)){
                /* Bigger than anything the server sends (a pinned JOB is under
                 * ~800 B, MQTT caps at 1024). Refuse this one alone; keep going. */
                if(pass==1){
                    memcpy(object,cursor,sizeof(s_command_object)-1U);object[sizeof(s_command_object)-1U]='\0';
                    long oid=long_field(object,"command_id",0);
                    if(oid>0)ack(ctx,(uint32_t)oid,"FAILED",0,0,"too large");
                }
                cursor=end+1;continue;
            }
            memcpy(object,cursor,len);object[len]='\0';cursor=end+1;
            char type[20]={0};
            cmd_type_of(object,type,sizeof(type));
            if(type_is_stop(type)!=(pass==0))continue;
            /* action time is taken after the GET and any preceding ACK waits */
            telemetry_client_dispatch_command(object,TELEMETRY_CMD_HTTP,recv_ms,
                now_ms==TELEMETRY_NOW_LIVE?ticks_ms():now_ms,ack,ctx);
        }
    }
}

static void poll_commands(uint32_t now)
{
    if(now-s_last_command_poll<1000U)return;
    s_last_command_poll=now;
    char path[180];
    snprintf(path,sizeof(path),"/api/v1/device/commands?device_id=%s",s_device_id);
    bool truncated=false;
    if(get_json(path,s_command_response,sizeof(s_command_response),&truncated)!=ESP_OK)return;
    /* A truncated body is a clean prefix: only objects fully inside it run (the
     * unterminated tail is discarded by the scanner and, never ACKed, is
     * re-served next poll). STOPs sort first so they are always inside it. */
    if(truncated)ESP_LOGW(TAG,"command response exceeded %u B; tail discarded, re-served next poll",(unsigned)COMMAND_RESPONSE_CAP);
    /* ttl counts from receipt of this response */
    telemetry_client_http_process_response(s_command_response,ticks_ms(),TELEMETRY_NOW_LIVE,ack_http,NULL);
}

/* ---------------------------------------------------------------------------
 * MQTT command intake. The mqtt_cmd task drains the link's RX queue into two
 * small staging queues: urgent (STOP-class) and normal. Staging is classified
 * here, in the task, never in the esp-mqtt callback. Urgent is executed before
 * anything normal and is never blocked by JOB/PROFILE work, telemetry
 * serialisation or publishing.
 * ------------------------------------------------------------------------ */
typedef struct { uint32_t recv_ms; int64_t recv_us; char data[URGENT_CAP+1U]; } urgent_slot_t;
typedef struct { uint32_t recv_ms; int64_t recv_us; char data[TELEMETRY_MQTT_RX_MAX+1U]; } normal_slot_t;
static urgent_slot_t s_urgent[URGENT_DEPTH];
static normal_slot_t s_normal[NORMAL_DEPTH];
static uint8_t s_urgent_head,s_urgent_n,s_normal_head,s_normal_n;
static char s_exec_buf[TELEMETRY_MQTT_RX_MAX+1U]; /* mqtt_cmd task only */

telemetry_intake_result_t telemetry_client_mqtt_intake(const char *payload,size_t len,bool retain,
                                                       uint32_t recv_ms,int64_t recv_us)
{
    if(retain){STAT_INC(drop_retained);return TELEMETRY_INTAKE_RETAINED;}
    /* payload[len] must be NUL (mqtt_link guarantees it) */
    if(!payload||!len||len>TELEMETRY_MQTT_RX_MAX||strlen(payload)!=len||
       long_field(payload,"command_id",0)<1){STAT_INC(drop_invalid);return TELEMETRY_INTAKE_INVALID;}
    char type[20]={0};
    cmd_type_of(payload,type,sizeof(type));
    bool urgent=type_is_stop(type)&&len<=URGENT_CAP;
    telemetry_intake_result_t r;
    state_lock();
    if(urgent){
        if(s_urgent_n==URGENT_DEPTH){STAT_INC(drop_full_urgent);r=TELEMETRY_INTAKE_FULL;}
        else{
            urgent_slot_t *s=&s_urgent[(s_urgent_head+s_urgent_n)%URGENT_DEPTH];
            s->recv_ms=recv_ms;s->recv_us=recv_us;memcpy(s->data,payload,len+1U);
            s_urgent_n++;STAT_INC(intake_urgent);r=TELEMETRY_INTAKE_URGENT;
        }
    }else{
        if(s_normal_n==NORMAL_DEPTH){STAT_INC(drop_full_normal);r=TELEMETRY_INTAKE_FULL;}
        else{
            normal_slot_t *s=&s_normal[(s_normal_head+s_normal_n)%NORMAL_DEPTH];
            s->recv_ms=recv_ms;s->recv_us=recv_us;memcpy(s->data,payload,len+1U);
            s_normal_n++;STAT_INC(intake_normal);r=TELEMETRY_INTAKE_NORMAL;
        }
    }
    state_unlock();
    return r;
}

size_t telemetry_client_mqtt_staged(void)
{
    state_lock();
    size_t n=(size_t)s_urgent_n+s_normal_n;
    state_unlock();
    return n;
}

static void note_latency(bool urgent,int64_t recv_us)
{
    if(recv_us<=0)return;
    int64_t d=esp_timer_get_time()-recv_us;
    uint32_t us=d<0?0U:d>4000000000LL?4000000000U:(uint32_t)d;
    if(urgent){s_stats.lat_urgent_last_us=us;if(us>s_stats.lat_urgent_max_us)s_stats.lat_urgent_max_us=us;}
    else{s_stats.lat_normal_last_us=us;if(us>s_stats.lat_normal_max_us)s_stats.lat_normal_max_us=us;}
}

bool telemetry_client_mqtt_step(uint32_t now)
{
    bool did=false;
    for(;;){ /* every staged STOP first */
        state_lock();
        if(!s_urgent_n){state_unlock();break;}
        urgent_slot_t *s=&s_urgent[s_urgent_head];
        uint32_t recv_ms=s->recv_ms;int64_t recv_us=s->recv_us;
        memcpy(s_exec_buf,s->data,sizeof(s->data));
        s_urgent_head=(uint8_t)((s_urgent_head+1U)%URGENT_DEPTH);s_urgent_n--;
        state_unlock();
        note_latency(true,recv_us);
        telemetry_client_dispatch_command(s_exec_buf,TELEMETRY_CMD_MQTT,recv_ms,now,ack_mqtt,NULL);
        STAT_INC(urgent_executed);
        did=true;
    }
    state_lock();
    if(s_normal_n){
        normal_slot_t *s=&s_normal[s_normal_head];
        uint32_t recv_ms=s->recv_ms;int64_t recv_us=s->recv_us;
        memcpy(s_exec_buf,s->data,sizeof(s->data));
        s_normal_head=(uint8_t)((s_normal_head+1U)%NORMAL_DEPTH);s_normal_n--;
        state_unlock();
        note_latency(false,recv_us);
        telemetry_client_dispatch_command(s_exec_buf,TELEMETRY_CMD_MQTT,recv_ms,now,ack_mqtt,NULL);
        STAT_INC(normal_executed);
        did=true;
    }else state_unlock();
    return did;
}

void telemetry_client_cmd_stats(telemetry_cmd_stats_t *out)
{
    if(out)*out=s_stats;
}

void telemetry_client_test_reset_commands(void)
{
    state_lock();
    memset(s_seen,0,sizeof(s_seen));s_seen_pos=0;
    s_floor_global=s_floor_ch[0]=s_floor_ch[1]=0;
    memset(&s_stats,0,sizeof(s_stats));
    s_urgent_head=s_urgent_n=s_normal_head=s_normal_n=0;
    memset(s_manual_cmd_ring,0,sizeof(s_manual_cmd_ring));s_manual_cmd_ring_pos=0;
    s_manual_high_water[0]=s_manual_high_water[1]=0;
    state_unlock();
}

static bool fetch_profile(uint8_t material_id,uint8_t ch,int32_t target_g,uint32_t now)
{
    if(ch<1U||ch>2U||material_id!=ch||target_g<=0)return false;
    char path[150], response[PROFILE_RESPONSE_CAP];
    snprintf(path,sizeof(path),"/api/v1/profiles/active?material_id=M%u&channel_id=CH%u&target_g=%ld",material_id,ch,(long)target_g);
    bool truncated=false;
    if(get_json(path,response,sizeof(response),&truncated)!=ESP_OK||truncated)return false;
    if(response[0]!='{'){
        bool accepted=dual_dispense_controller_set_pending_default(ch,target_g);
        if(accepted)
            ESP_LOGI(TAG,"CH%u has no server profile for %ld g; device default staged",
                     ch,(long)target_g);
        return accepted;
    }
    char id[65]={0}; const char *p=strstr(response,"\"profile_id\":\"");
    if(!p)return false;
    p+=14;
    const char *end=strchr(p,'\"');
    if(!end||(size_t)(end-p)>=sizeof(id))return false;
    memcpy(id,p,(size_t)(end-p)); id[end-p]='\0';
    uint32_t version=0; double kp,ki,kd,tol,over,duration,window,on,off;
    if(!uint_field(response,"version",&version) || version==0) return false;
    if(!number_field(response,"kp",&kp)||!number_field(response,"ki",&ki)||!number_field(response,"kd",&kd)||
       !number_field(response,"tolerance_g",&tol)||!number_field(response,"max_overshoot_g",&over)||
       !number_field(response,"max_duration_ms",&duration)||!number_field(response,"window_ms",&window)||
       !number_field(response,"min_on_ms",&on)||!number_field(response,"min_off_ms",&off))return false;
    /* Require an explicit non-zero target_g from the server. Never silently
     * fall back to a different target â€” that is the bleed bug this removes. */
    double target=0;
    if(!number_field(response,"target_g",&target)||target==0){
        ESP_LOGW(TAG,"CH%u profile %s: server response has no explicit target_g; refused",
                 ch,id);
        return false;
    }
    if(legacy_target(target)!=target_g){
        ESP_LOGW(TAG,"CH%u profile %s: server target_g=%g does not match requested %ld; refused",
                 ch,id,target,(long)target_g);
        return false;
    }
    staged_profile_fields_t staged;
    staged_profile_fields_default(&staged);
    if(!staged_profile_fields_parse(response,&staged)){
        ESP_LOGW(TAG,"CH%u profile %s: staged field is not a finite number; refused",ch,id);
        return false;
    }
    dual_channel_snapshot_t snap;
    if(dual_dispense_controller_snapshot(ch,now,&snap)&&
       (version>snap.profile_version || strcmp(id,snap.profile_id)!=0)) {
        bool accepted=dual_dispense_controller_set_pending_profile_staged(ch,id,version,
            target_g,
            legacy_f32(kp),legacy_f32(ki),legacy_f32(kd),legacy_i32(tol),legacy_i32(over),
            legacy_u32(duration),legacy_u32(window),legacy_u32(on),legacy_u32(off),
            staged.coarse_threshold_g,staged.fine_threshold_g,staged.micro_threshold_g,
            staged.coarse_min_on_ms,staged.fine_min_on_ms,staged.micro_min_on_ms,
            staged.settle_time_ms,staged.inflight_comp_g);
        if(accepted)
            ESP_LOGI(TAG,"CH%u profile %s v%lu accepted as pending",ch,id,(unsigned long)version);
        return accepted;
    }
    return true;
}

/* A new job takes over a run slot whose run was never closed (broker down for long): close the
 * old one as UNKNOWN (FAILED / RUN_SUPERSEDED, counted) instead of silently forgetting it. Its
 * unsent local samples are counted as dropped. COMPLETE has a reserved ring slot. */
static void run_supersede(run_t *r,uint32_t now)
{
    if(!s_run_mqtt||!r->allocated||!r->started)return;
    char body[560],mid[24];
    next_message_id(mid,sizeof(mid));
    uint32_t dropped=r->dropped_samples+r->count+runlog_dropped_samples(r->run_id);
    int m=snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"message_id\":\"%s\",\"status\":\"FAILED\","
        "\"final_weight_g\":%ld,\"max_weight_g\":%ld,\"duration_ms\":%lu,\"error\":\"RUN_SUPERSEDED\",\"end_seq\":%lu,"
        "\"total_batches\":%lu,\"total_samples\":%lu,\"total_events\":%lu,\"dropped_samples\":%lu}",
        r->run_id,mqtt_link_boot_id(),mid,(long)r->start_g,(long)r->max_weight_g,(unsigned long)(now-r->started_ms),
        (unsigned long)r->batch_seq,(unsigned long)r->batch_seq,(unsigned long)r->sample_idx,(unsigned long)r->event_idx,
        (unsigned long)dropped);
    bool ok=m>0&&(size_t)m<sizeof(body)&&runlog_add(RUNLOG_COMPLETE,r->run_id,r->batch_seq+1U,0,body,(size_t)m);
    runlog_note_superseded(ok);
    ESP_LOGW(TAG,"run %s superseded before it was closed (%s)",r->run_id,ok?"closed as UNKNOWN":"complete refused");
}

static void run_start(run_t *r,uint8_t ch,const dispense_job_t *job,uint32_t now,
                      const dual_channel_snapshot_t *s)
{
    if(r->allocated&&r->job_id==job->id&&r->started)return;
    if(!r->allocated||r->job_id!=job->id){
        run_supersede(r,now);
        memset(r,0,sizeof(*r)); r->allocated=true; r->job_id=job->id; r->material_id=job->material_id; r->target_g=job->target_g;
        r->started_ms=now; r->start_g=job->start_g; r->max_weight_g=job->start_g;
        snprintf(r->run_id,sizeof(r->run_id),"%.*s-ch%u-j%lu-%lu-%04lx",24,s_device_id,ch,
            (unsigned long)job->id,(unsigned long)(now/1000U),(unsigned long)(esp_random()&0xffffU));
    }
    char cfg[600],body[1200];
    snprintf(cfg,sizeof(cfg),"\"kp\":%.7g,\"ki\":%.7g,\"kd\":%.7g,\"tolerance_g\":%ld,"
        "\"max_overshoot_g\":%ld,\"max_duration_ms\":%lu,\"window_ms\":%lu,\"min_on_ms\":%lu,\"min_off_ms\":%lu,"
        "\"inflight_comp_g\":%ld,\"settle_time_ms\":%lu",
        (double)s->kp,(double)s->ki,(double)s->kd,(long)s->tolerance_g,(long)s->max_overshoot_g,
        (unsigned long)s->max_duration_ms,(unsigned long)s->window_ms,(unsigned long)s->min_on_ms,(unsigned long)s->min_off_ms,
        (long)s->inflight_comp_g,(unsigned long)s->settle_time_ms);
    if(s_run_mqtt){
        /* CONTRACT 9.2 run/start: kept in the RAM ring until the backend acks it. */
        char mid[24];next_message_id(mid,sizeof(mid));
        int m=snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"message_id\":\"%s\",\"material_id\":\"M%u\","
            "\"channel_id\":\"CH%u\",\"device_id\":\"%s\",\"job_id\":%lu,\"command_id\":%lu,\"target_g\":%ld,\"priority\":%u,"
            "\"firmware\":\"relay-controller 7.0-single-scale-staged\",\"start_weight_g\":%ld,\"profile_id\":\"%s\","
            "\"profile_version\":%lu,\"config\":{%s}}",
            r->run_id,mqtt_link_boot_id(),mid,(unsigned)job->material_id,ch,s_device_id,(unsigned long)job->id,
            (unsigned long)job->remote_command_id,(long)job->target_g,(unsigned)job->priority,(long)job->start_g,
            s->profile_id,(unsigned long)s->profile_version,cfg);
        if(m>0&&(size_t)m<sizeof(body)&&runlog_add(RUNLOG_START,r->run_id,0,0,body,(size_t)m)){
            r->started=true;ESP_LOGI(TAG,"CH%u run created %s (mqtt)",ch,r->run_id);
            (void)upload_event(r,ch,"JOB_ASSIGNED","QUEUED",now,job->start_g,false);
            (void)upload_event(r,ch,"JOB_STARTED","WAIT_WEIGHT",now,job->start_g,false);
        }
        return;
    }
    int n=snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"material_id\":\"M%u\",\"channel_id\":\"CH%u\",\"device_id\":\"%s\","
        "\"job_id\":%lu,\"target_g\":%ld,\"priority\":%u,\"firmware\":\"relay-controller 7.0-single-scale-staged\","
        "\"start_weight_g\":%ld,\"profile_id\":\"%s\",\"profile_version\":%lu,\"config\":{%s}}",
        r->run_id,(unsigned)job->material_id,ch,s_device_id,(unsigned long)job->id,(long)job->target_g,(unsigned)job->priority,
        (long)job->start_g,s->profile_id,(unsigned long)s->profile_version,cfg);
    if(n>0&&(size_t)n<sizeof(body)&&request("POST","/api/v1/runs/start",body,NULL,0)==ESP_OK) {
        r->started=true; ESP_LOGI(TAG,"CH%u run created %s",ch,r->run_id);
          (void)upload_event(r,ch,"JOB_ASSIGNED","QUEUED",now,job->start_g,false);
        (void)upload_event(r,ch,"JOB_STARTED","WAIT_WEIGHT",now,job->start_g,false);
    }
}

static bool append_sample(run_t *r,uint8_t ch,const capture_t *c,bool has,int32_t grams,bool stable)
{
    if(!r->allocated||!has||!c->msg.has_sequence)return false;
    dual_channel_snapshot_t snap=c->channels[ch-1];
    /* A backlog belongs to the job that acquired it, never the next job. */
    if(snap.active_job_id!=r->job_id)return false;
    if(r->count==SAMPLE_DEPTH){r->head=(r->head+1U)%SAMPLE_DEPTH;--r->count;++s_dropped;++r->dropped_samples;}
    sample_t *s=&r->ring[(r->head+r->count)%SAMPLE_DEPTH]; memset(s,0,sizeof(*s));
    s->idx=++r->sample_idx;s->uptime_ms=c->received_ms;s->elapsed_ms=c->received_ms-r->started_ms;
    s->seq=c->msg.sequence;s->has_seq=true;s->weight_g=grams;s->target_g=r->target_g;s->error_g=r->target_g-grams;
    s->stable=stable;s->age_ms=c->msg.age_ms;s->p=snap.p_term;s->i=snap.i_term;s->d=snap.d_term;s->output=snap.output;
    s->relay=snap.relay_on;snprintf(s->state,sizeof(s->state),"%s",dual_dispense_state_name(snap.state));
    snprintf(s->stage,sizeof(s->stage),"%s",dispense_stage_name(snap.stage));
    snprintf(s->scale_owner,sizeof(s->scale_owner),"%s",scale_owner_name(snap.scale_owner));
    if(grams>r->max_weight_g)r->max_weight_g=grams;
    if(strcmp(r->last_event_state,s->state)!=0){
        const char *event=!strcmp(s->state,"SETTLING")?"TARGET_REACHED":
            !strcmp(s->state,"PAUSED")?"PAUSE":
            !strcmp(r->last_event_state,"PAUSED")&&!strcmp(s->state,"DISPENSING")?"RESUME":"STATE_CHANGE";
        if(upload_event(r,ch,event,s->state,c->received_ms,grams,false))
            snprintf(r->last_event_state,sizeof(r->last_event_state),"%s",s->state);
    }
    ++r->count;return true;
}

static bool upload_samples(run_t *r,uint8_t ch)
{
    if(!r->started||!r->count)return true;
    /* MQTT lifecycle: drain the local ring into run/samples batches (5 samples keep
     * a body near 1.8 KB, so the 2-item send window stays inside the link outbox). */
    do{
    const uint8_t per_batch=s_run_mqtt?5U:6U;
    uint8_t count=r->count<per_batch?r->count:per_batch;
    size_t used=s_run_mqtt?
        (size_t)snprintf(s_body,BODY_CAP,"{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"batch_seq\":%lu,\"samples\":[",
            r->run_id,mqtt_link_boot_id(),(unsigned long)(r->batch_seq+1U)):
        (size_t)snprintf(s_body,BODY_CAP,"{\"run_id\":\"%s\",\"material_id\":\"M%u\",\"channel_id\":\"CH%u\",\"device_id\":\"%s\",\"samples\":[",
            r->run_id,(unsigned)r->material_id,ch,s_device_id);
    for(uint8_t i=0;i<count;i++){
        sample_t *s=&r->ring[(r->head+i)%SAMPLE_DEPTH];
        int n=snprintf(s_body+used,BODY_CAP-used,
          "%s{\"idx\":%lu,\"material_id\":\"M%u\",\"channel_id\":\"CH%u\",\"uptime_ms\":%lu,\"elapsed_ms\":%lu,\"seq\":%lu,\"source\":\"CAS\","
          "\"weight_g\":%ld,\"target_g\":%ld,\"error_g\":%ld,\"stable\":%s,\"weight_age_ms\":%lu,"
          "\"p_term\":%.6f,\"i_term\":%.6f,\"d_term\":%.6f,\"pid_output\":%.6f,\"relay1\":%s,\"relay2\":%s,\"state\":\"%s\",\"stage\":\"%s\",\"scale_owner\":\"%s\"}",
          i?",":"",(unsigned long)s->idx,(unsigned)r->material_id,ch,(unsigned long)s->uptime_ms,(unsigned long)s->elapsed_ms,(unsigned long)s->seq,
          (long)s->weight_g,(long)s->target_g,(long)s->error_g,s->stable?"true":"false",(unsigned long)s->age_ms,
          (double)s->p,(double)s->i,(double)s->d,(double)s->output,
          ch==1?(s->relay?"true":"false"):"false",ch==2?(s->relay?"true":"false"):"false",s->state,
          s->stage,s->scale_owner);
        if(n<0||(size_t)n>=BODY_CAP-used)return false;
        used+=(size_t)n;
    }
    if(used+3>=BODY_CAP)return false;
    snprintf(s_body+used,BODY_CAP-used,"]}");
    if(s_run_mqtt){
        /* Ring full of start/events/complete (refused): samples stay in the local ring. */
        if(!runlog_add(RUNLOG_SAMPLES,r->run_id,r->batch_seq+1U,count,s_body,used+2U))return false;
        r->batch_seq++;
    }
    /* MQTT first (QoS 0, same body as POST /telemetry/batch); HTTP when the link is down
     * or its queue refuses. The run exists server-side already (runs/start is HTTP). */
    else if(mqtt_pub("telemetry/samples",s_body,0)){r->mqtt_used=true;r->last_mqtt_ms=ticks_ms();}
    else if(request("POST","/api/v1/telemetry/batch",s_body,NULL,0)!=ESP_OK)return false;
    r->head=(r->head+count)%SAMPLE_DEPTH;r->count-=count;
    }while(s_run_mqtt&&r->count);
    return true;
}

static bool upload_event(run_t *r,uint8_t ch,const char *event,const char *state,uint32_t now,int32_t grams,bool terminal)
{
    if(!r->started)return false;
    char body[450];
    uint32_t idx=r->event_idx+1U;
    if(s_run_mqtt){
        /* CONTRACT 9.2 run/events; one event per batch, sharing the run's batch_seq counter. */
        int m=snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"batch_seq\":%lu,\"events\":[{\"idx\":%lu,"
            "\"event\":\"%s\",\"elapsed_ms\":%lu,\"state\":\"%s\",\"weight_g\":%ld}]}",
            r->run_id,mqtt_link_boot_id(),(unsigned long)(r->batch_seq+1U),(unsigned long)idx,event,
            (unsigned long)(now-r->started_ms),state,(long)grams);
        if(m<=0||(size_t)m>=sizeof(body)||!runlog_add(terminal?RUNLOG_TERMINAL:RUNLOG_EVENTS,r->run_id,r->batch_seq+1U,0,body,(size_t)m))return false;
        r->batch_seq++;r->event_idx=idx;
        return true;
    }
    snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"material_id\":\"M%u\",\"channel_id\":\"CH%u\",\"device_id\":\"%s\",\"events\":[{\"idx\":%lu,\"event\":\"%s\",\"elapsed_ms\":%lu,\"state\":\"%s\",\"weight_g\":%ld}]}",
        r->run_id,(unsigned)r->material_id,ch,s_device_id,(unsigned long)idx,event,(unsigned long)(now-r->started_ms),state,(long)grams);
    if(mqtt_pub("telemetry/events",body,0)){r->mqtt_used=true;r->last_mqtt_ms=ticks_ms();}
    else if(request("POST","/api/v1/events",body,NULL,0)!=ESP_OK)return false;
    r->event_idx=idx;
    return true;
}

/* The job reached a terminal state: send the terminal event, then close the run. */
static void run_finish(run_t *r,uint8_t ch,const dispense_job_t *jobp)
{
    const dispense_job_t job=*jobp;
    if(!r->completion_pending){r->finished=job;r->completion_pending=true;}
    if(r->started&&!r->terminal_event_sent){
        const char *event=job.state==JOB_COMPLETE?"JOB_COMPLETE":
            job.state==JOB_CANCELLED?"JOB_CANCELLED":
            !strcmp(job.error,"OVERWEIGHT")?"OVERWEIGHT":"JOB_FAILED";
        const char *state=job.state==JOB_COMPLETE?"COMPLETE":
            job.state==JOB_CANCELLED?"IDLE":"FAULT";
        r->terminal_event_sent=upload_event(r,ch,event,state,job.finish_ms,job.final_g,true);
    }
    const char *status=job.state==JOB_COMPLETE?"COMPLETE":job.state==JOB_CANCELLED?"CANCELLED":"FAILED";
    if(s_run_mqtt){
        /* CONTRACT 9.2 run/complete: only after the terminal event and the last sample
         * batch are in the ring. end_seq/total_batches count batches produced
         * (start excluded); total_samples counts samples produced, dropped ones
         * included, so the backend can name the missing idx ranges.
         * dropped_samples is a best-effort lower bound: the per-run drop table holds
         * 4 runs (an older run's count is recycled) and a release without a tombstone
         * (alloc failure) is not counted, so it can undercount. The backend's
         * sample_idx gaps are authoritative. */
        if(r->started&&r->terminal_event_sent&&r->count==0){
            char body[560],mid[24];
            next_message_id(mid,sizeof(mid));
            uint32_t dropped=r->dropped_samples+runlog_dropped_samples(r->run_id);
            int m=snprintf(body,sizeof(body),"{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"message_id\":\"%s\",\"status\":\"%s\","
                "\"final_weight_g\":%ld,\"max_weight_g\":%ld,\"duration_ms\":%lu,\"error\":\"%s\",\"end_seq\":%lu,"
                "\"total_batches\":%lu,\"total_samples\":%lu,\"total_events\":%lu,\"dropped_samples\":%lu}",
                r->run_id,mqtt_link_boot_id(),mid,status,(long)job.final_g,(long)r->max_weight_g,
                (unsigned long)(job.finish_ms-r->started_ms),job.error,(unsigned long)r->batch_seq,
                (unsigned long)r->batch_seq,(unsigned long)r->sample_idx,(unsigned long)r->event_idx,(unsigned long)dropped);
            if(m>0&&(size_t)m<sizeof(body)&&runlog_add(RUNLOG_COMPLETE,r->run_id,r->batch_seq+1U,0,body,(size_t)m)){
                ESP_LOGI(TAG,"CH%u run %s closed (%s), awaiting ack",ch,r->run_id,status);
                memset(r,0,sizeof(*r));
            }
        }
        return;
    }
    /* runs/{id}/complete stays HTTP. Hold it back until the last QoS 0 samples/events had time to land, so the server never sees a finished run first. */
    if(r->started&&r->count==0&&(!r->mqtt_used||ticks_ms()-r->last_mqtt_ms>=MQTT_COMPLETE_GRACE_MS)){
        char path[100],body[360];
        snprintf(path,sizeof(path),"/api/v1/runs/%s/complete",r->run_id);
        snprintf(body,sizeof(body),"{\"status\":\"%s\",\"final_weight_g\":%ld,\"max_weight_g\":%ld,\"duration_ms\":%lu,\"error\":\"%s\"}",status,(long)job.final_g,(long)r->max_weight_g,(unsigned long)(job.finish_ms-r->started_ms),job.error);
        if(request("POST",path,body,NULL,0)==ESP_OK){ESP_LOGI(TAG,"CH%u run %s saved (%s)",ch,r->run_id,status);memset(r,0,sizeof(*r));}
    }
}

/* Test-only: run_start alone (no memset), so a pending un-closed run meets the supersede path. */
void telemetry_client_test_run_begin(uint8_t ch,const dispense_job_t *job,const dual_channel_snapshot_t *snap,uint32_t now)
{
    if(ch<1U||ch>2U||!job||!snap||!run_state_alloc())return;
    run_start(&s_runs[ch-1U],ch,job,now,snap);
}

/* Test-only: one run end to end without the task. Starts the run for `job` on
 * channel ch, appends n synthetic samples, uploads them, then finishes with the
 * job's (terminal) state. Returns the run slot's batch_seq after the finish. */
uint32_t telemetry_client_test_run_cycle(uint8_t ch,const dispense_job_t *job,
                                         const dual_channel_snapshot_t *snap,uint32_t now,uint32_t n)
{
    if(ch<1U||ch>2U||!job||!snap||!run_state_alloc())return 0U;
    run_t *r=&s_runs[ch-1U];
    memset(r,0,sizeof(*r));
    run_start(r,ch,job,now,snap);
    capture_t c;memset(&c,0,sizeof(c));
    for(uint32_t i=0;i<n;i++){
        c.msg.has_sequence=true;c.msg.sequence=i+1U;c.received_ms=now+i*10U;
        c.channels[ch-1U]=*snap;c.channels[ch-1U].active_job_id=job->id;
        (void)append_sample(r,ch,&c,true,(int32_t)(i*10U),true);
    }
    (void)upload_samples(r,ch);
    uint32_t seq=r->batch_seq;
    run_finish(r,ch,job);
    return seq;
}

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t last_status_ms = 0U;
    for(;;){
        uint32_t now=ticks_ms();
        if (now - last_status_ms >= (mqtt_up() ? MQTT_STATUS_MS : 2000U)) {
            upload_device_status(now);
            last_status_ms = now;
        }
        if(!s_run_track){   /* run history allocation failed: status/diag only */
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_MS));
            continue;
        }
        for(uint8_t ch=1;ch<=2;ch++){
            dual_channel_snapshot_t snap;
            if(!dual_dispense_controller_snapshot(ch,now,&snap))continue;
            if(snap.active_job_id){dispense_job_t job;if(job_queue_get(snap.active_job_id,&job)){
                run_start(&s_runs[ch-1],ch,&job,now,&snap);}}
        }
        capture_t c;
        for(unsigned drained=0;drained<CAPTURE_DEPTH&&xQueueReceive(s_capture_q,&c,0)==pdTRUE;drained++){
            for(uint8_t ch=1;ch<=2;ch++){
                run_t *r=&s_runs[ch-1]; if(!r->allocated||!r->started)continue;
                /* Single physical scale is routed by controller ownership,
                 * not by the UART connector's channel number. */
                bool has=c.channels[ch-1].have_weight;
                int32_t g=c.channels[ch-1].current_weight_g;
                bool stable=c.channels[ch-1].stable;
                (void)append_sample(r,ch,&c,has,g,stable);
            }
        }
        for(uint8_t ch=1;ch<=2;ch++){
            run_t *r=&s_runs[ch-1];if(!r->allocated)continue;
            (void)upload_samples(r,ch);
            dispense_job_t job;
            if(job_queue_get(r->job_id,&job)&&job.state!=JOB_RUNNING&&job.state!=JOB_QUEUED)run_finish(r,ch,&job);
        }
        static uint32_t last_diag;
        if(now-last_diag>=10000U){
            last_diag=now;
            ESP_LOGI(TAG,"history diag drops=%lu capture_pending=%u peak=%lu stack=%u heap=%lu largest_block=%lu",
                (unsigned long)s_dropped,(unsigned)uxQueueMessagesWaiting(s_capture_q),
                (unsigned long)s_capture_peak,
                (unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned long)esp_get_free_heap_size(),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        vTaskDelay(pdMS_TO_TICKS(UPLOAD_MS));
    }
}

/* Independent tasks prevent history retry/DB latency from delaying commands
 * or live UI. Each task owns its JSON buffers. No HTTP runs in a control task. */
static void command_task(void *arg)
{
    (void)arg;
    for(;;){
        poll_commands(ticks_ms());
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

/* mqtt_cmd: drain the link's RX queue into staging, then run STOP-class first.
 * Draining before executing means a STOP that arrived behind JOBs is classified
 * before any of them runs, and the RX queue never backs up behind slow work. */
size_t telemetry_client_mqtt_drain(uint32_t first_timeout_ms)
{
    static telemetry_mqtt_rx_t rx;
    size_t taken=0;
    if(!s_ops.rx_pop)return 0;
    bool got=s_ops.rx_pop(&rx,first_timeout_ms);
    for(unsigned n=0;got&&n<16U;n++){
        (void)telemetry_client_mqtt_intake(rx.data,rx.len,false,rx.recv_ms,rx.recv_us);
        taken++;
        got=s_ops.rx_pop(&rx,0U);
    }
    return taken;
}

static void mqtt_cmd_task(void *arg)
{
    (void)arg;
    uint32_t last_diag=0;
    for(;;){
        (void)telemetry_client_mqtt_drain(telemetry_client_mqtt_staged()?0U:100U);
        uint32_t now=ticks_ms();
        (void)telemetry_client_mqtt_step(now);
        if(now-last_diag>=10000U){
            last_diag=now;
            telemetry_cmd_stats_t st;telemetry_client_cmd_stats(&st);
            ESP_LOGI(TAG,"mqtt_cmd diag urgent=%lu normal=%lu dup=%lu expired=%lu drop_full=%lu/%lu lat_us urgent=%lu/%lu normal=%lu/%lu stack=%u",
                (unsigned long)st.urgent_executed,(unsigned long)st.normal_executed,(unsigned long)st.dup_reacked,
                (unsigned long)st.expired,(unsigned long)st.drop_full_urgent,(unsigned long)st.drop_full_normal,
                (unsigned long)st.lat_urgent_last_us,(unsigned long)st.lat_urgent_max_us,
                (unsigned long)st.lat_normal_last_us,(unsigned long)st.lat_normal_max_us,
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

/* Weight-only MQTT live body. Invalid weight or unknown (UINT32_MAX) age -> null, so a
 * stale last-known value is never presented as a reading. Returns length, -1 on truncation. */
int telemetry_client_live_body(char *buf,size_t cap,uint32_t now,const dual_channel_snapshot_t snap[2])
{
    if(!buf||!snap||cap==0)return -1;
    size_t mu=0;
    int n=snprintf(buf,cap,"{\"uptime_ms\":%lu,\"channels\":[",(unsigned long)now);
    if(n<0||(size_t)n>=cap)return -1;
    mu=(size_t)n;
    for(uint8_t ch=1;ch<=2;ch++){
        const dual_channel_snapshot_t *s=&snap[ch-1];
        bool wv=s->have_weight&&s->weight_age_ms<=WEIGHT_MESSAGE_TIMEOUT_MS;
        char wg[16],age[12];
        if(wv)snprintf(wg,sizeof(wg),"%ld",(long)s->current_weight_g);
        else snprintf(wg,sizeof(wg),"null");
        if(s->weight_age_ms==UINT32_MAX)snprintf(age,sizeof(age),"null");
        else snprintf(age,sizeof(age),"%lu",(unsigned long)s->weight_age_ms);
        n=snprintf(buf+mu,cap-mu,
            "%s{\"channel_id\":\"CH%u\",\"weight_g\":%s,\"weight_valid\":%s,\"weight_age_ms\":%s,\"stable\":%s}",
            ch==1?"":",",ch,wg,wv?"true":"false",age,(wv&&s->stable)?"true":"false");
        if(n<0||(size_t)n>=cap-mu)return -1;
        mu+=(size_t)n;
    }
    if(mu+3>cap)return -1;
    memcpy(buf+mu,"]}",3);
    return (int)(mu+2);
}

static void live_status_task(void *arg)
{
    (void)arg;
    static char body[1800];
    uint32_t sent=0,failed=0,last_diag=0;
    TickType_t wake=xTaskGetTickCount();
    for(;;){
        uint32_t now=ticks_ms();
        /* The MQTT telemetry/status stream already feeds the same live state, so the
         * 5 Hz HTTP POST is skipped while the link is up and resumes when it drops. */
        bool via_mqtt=mqtt_up();
        if(via_mqtt){
            /* Compact weight-only message (no state/relay_on: those stay on the 1 Hz
             * status). Non-blocking enqueue; QoS 0 drop is fine. UINT32_MAX age = unknown -> null. */
            static char mbody[300];
            dual_channel_snapshot_t ls[2];
            bool mok=dual_dispense_controller_snapshot(1,now,&ls[0])&&dual_dispense_controller_snapshot(2,now,&ls[1]);
            if(mok&&telemetry_client_live_body(mbody,sizeof(mbody),now,ls)>=0){
                if(mqtt_pub(MQTT_SUFFIX_LIVE,mbody,0))sent++;else failed++;
            }
        }
        size_t used=via_mqtt?0U:(size_t)snprintf(body,sizeof(body),
            "{\"device_id\":\"%s\",\"status\":{\"role\":\"relay_controller\",\"uptime_ms\":%lu,\"channels\":[",
            s_device_id,(unsigned long)now);
        bool valid=!via_mqtt&&used<sizeof(body);
        for(uint8_t ch=1;ch<=2&&valid;ch++){
            dual_channel_snapshot_t s;
            if(!dual_dispense_controller_snapshot(ch,now,&s)){valid=false;break;}
            int n=snprintf(body+used,sizeof(body)-used,
                "%s{\"channel_id\":\"CH%u\",\"material_id\":\"M%u\",\"pump_id\":\"Pump %u\","
                "\"active_job_id\":%lu,\"weight_g\":%ld,\"target_g\":%ld,\"error_g\":%ld,"
                "\"weight_age_ms\":%lu,\"weight_valid\":%s,\"stable\":%s,\"relay_on\":%s,\"manual\":%s,"
                "\"state\":\"%s\",\"stage\":\"%s\",\"scale_owner\":\"%s\",\"output\":%.6f,\"fault\":\"%s\"}",
                ch==1?"":",",ch,ch,ch,(unsigned long)s.active_job_id,(long)s.current_weight_g,
                (long)s.target_g,(long)s.error_g,(unsigned long)s.weight_age_ms,
                s.have_weight&&s.weight_age_ms<=WEIGHT_MESSAGE_TIMEOUT_MS?"true":"false",
                s.stable?"true":"false",s.relay_on?"true":"false",s.manual?"true":"false",
                dual_dispense_state_name(s.state),dispense_stage_name(s.stage),scale_owner_name(s.scale_owner),
                (double)s.output,s.fault?s.fault:"");
            if(n<0||(size_t)n>=sizeof(body)-used)valid=false;else used+=(size_t)n;
        }
        if(valid&&used+4<sizeof(body)){
            snprintf(body+used,sizeof(body)-used,"]}}");
            if(request("POST","/api/v1/live/status",body,NULL,0)==ESP_OK)sent++;else failed++;
        }
        if(now-last_diag>=10000U){
            last_diag=now;
            ESP_LOGI(TAG,"live diag sent=%lu fail=%lu stack=%u",
                (unsigned long)sent,(unsigned long)failed,(unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        /* After a slow request, resume from now instead of sending a catch-up
         * burst of obsolete snapshots. Healthy path: at most 5 Hz. */
        if(xTaskGetTickCount()-wake>=pdMS_TO_TICKS(200U))wake=xTaskGetTickCount();
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(200U));
    }
}

/* Pumps the run ring (resend after reconnect, resend of unacked items). Zero-timeout
 * publishes only; it never touches the control loop. */
static void run_mqtt_task(void *arg)
{
    (void)arg;
    uint32_t last_diag=0;
    for(;;){
        uint32_t now=ticks_ms();
        (void)telemetry_client_run_ack_drain();
        (void)runlog_pump(now);
        if(now-last_diag>=10000U){
            last_diag=now;
            runlog_stats_t st;runlog_stats(&st);
            if(st.pending||st.dropped_samples)
                ESP_LOGI(TAG,"run ring pending=%lu sample_bytes=%lu pub=%lu resent=%lu dropped_samples=%lu stack=%u",
                    (unsigned long)st.pending,(unsigned long)st.sample_bytes,(unsigned long)st.published,
                    (unsigned long)st.resent,(unsigned long)st.dropped_samples,(unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

bool telemetry_client_run_tracking_enabled(void) { return s_run_track; }
uint32_t telemetry_client_run_alloc_failures(void) { return s_run_alloc_failed; }

/* Run history is optional: when its memory is missing, run tracking is switched off
 * (counted, logged) and commands, STOP and live status carry on without it. */
static bool run_tracking_init(bool force_fail)
{
    bool ok=!force_fail&&run_state_alloc();
    if(ok&&!s_capture_q)s_capture_q=xQueueCreate(CAPTURE_DEPTH,sizeof(capture_t));
    ok=ok&&s_capture_q!=NULL;
    s_run_track=ok;
    if(!ok){
        s_run_alloc_failed++;
        s_run_mqtt=false;
        ESP_LOGE(TAG,"run history allocation failed; run tracking disabled, commands unaffected");
    }
    return ok;
}
bool telemetry_client_test_run_tracking_init(bool force_fail) { return run_tracking_init(force_fail); }

/* No ring means run items would be lost silently: fall back to the HTTP run path. */
static bool run_mqtt_ring_init(bool force_fail)
{
    if(!s_run_mqtt)return true;
    const runlog_io_t io={.connected=mqtt_up,.publish=run_publish};
    bool ok=!force_fail&&runlog_init(&io);
    if(!ok){
        (void)runlog_init(NULL);
        s_run_alloc_failed++;
        s_run_mqtt=false;
        ESP_LOGE(TAG,"run MQTT ring allocation failed; run lifecycle stays HTTP");
    }
    return ok;
}
bool telemetry_client_test_run_mqtt_ring_init(bool force_fail) { return run_mqtt_ring_init(force_fail); }

esp_err_t telemetry_client_start(void)
{
    uint32_t flag=0;
    s_run_mqtt=nvs_config_get_u32(NVS_KEY_RUN_MQTT,&flag)==ESP_OK&&flag==1U;
    flag=0;
    s_require_pin=nvs_config_get_u32(NVS_KEY_REQ_PIN_PROFILE,&flag)==ESP_OK&&flag==1U;
    flag=0;
    s_hash_check=nvs_config_get_u32(NVS_KEY_HASH_CHECK,&flag)==ESP_OK&&flag==1U;
    if(s_run_mqtt&&!s_ops.publish){
        ESP_LOGW(TAG,"run_mqtt set but no MQTT transport registered; run lifecycle stays HTTP");
        s_run_mqtt=false;
    }
    if(s_run_mqtt||s_require_pin||s_hash_check)
        ESP_LOGW(TAG,"flags: run_mqtt=%d require_pinned_profile=%d hash_check=%d",
            (int)s_run_mqtt,(int)s_require_pin,(int)s_hash_check);
    if(!enabled()){ESP_LOGI(TAG,"upload disabled; configure telemetry server address");return ESP_OK;}
    (void)nvs_config_get_str(NVS_KEY_DEVICE_ID,s_device_id,sizeof(s_device_id));
    if(CONFIG_WEIGHT_DEMO_TELEMETRY_DEVICE_ID[0])snprintf(s_device_id,sizeof(s_device_id),"%s",CONFIG_WEIGHT_DEMO_TELEMETRY_DEVICE_ID);
    if(!s_device_id[0])snprintf(s_device_id,sizeof(s_device_id),"relay-controller");
    (void)run_tracking_init(false);   /* failure only disables run tracking: never the command tasks */
    ESP_LOGI(TAG,"dual-channel uploader host=%s port=%d",CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_IP,CONFIG_WEIGHT_DEMO_TELEMETRY_SERVER_PORT);
    (void)run_mqtt_ring_init(false);   /* before any task can add an item */
    /* Command tasks first: a later failure must not cost the remote STOP path. */
    esp_err_t err=ESP_OK;
    if(xTaskCreate(command_task,"commands",6144,NULL,3,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    /* mqtt_cmd: priority 4 = above the HTTP tasks (3) so a STOP is never queued behind
     * telemetry/HTTP CPU, equal to safety_watchdog/valve_tx and below supervisor (5). It
     * blocks on a queue and does only short controller calls; no network I/O. */
    if(s_ops.rx_pop&&xTaskCreate(mqtt_cmd_task,"mqtt_cmd",6144,NULL,4,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    if(xTaskCreate(telemetry_task,"telemetry",7168,NULL,3,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    if(s_run_mqtt&&xTaskCreate(run_mqtt_task,"run_mqtt",4096,NULL,2,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    if(xTaskCreate(live_status_task,"live_status",4096,NULL,2,NULL)!=pdPASS)err=ESP_ERR_NO_MEM;
    return err;
}

void telemetry_client_on_weight(const weight_msg_t *msg,uint32_t received_ms)
{
    if(!s_capture_q||!msg||msg->simulated)return;
    capture_t c={.msg=*msg,.received_ms=received_ms};
    for(uint8_t ch=1;ch<=2;ch++)
        (void)dual_dispense_controller_snapshot(ch,ticks_ms(),&c.channels[ch-1]);
    if(xQueueSend(s_capture_q,&c,0)!=pdTRUE){capture_t old;(void)xQueueReceive(s_capture_q,&old,0);++s_dropped;(void)xQueueSend(s_capture_q,&c,0);}
    uint32_t pending=(uint32_t)uxQueueMessagesWaiting(s_capture_q);
    if(pending>s_capture_peak)s_capture_peak=pending; /* one capture producer */
}
