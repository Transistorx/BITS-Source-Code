#include "web_api.h"

#include "dual_dispense_controller.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "job_queue.h"
#include "safety_manager.h"
#include "websocket_client.h"
#include "weight_receiver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_WEIGHT_DEMO_WEB_API_PORT
#define CONFIG_WEIGHT_DEMO_WEB_API_PORT 80
#endif
#define WEB_API_PORT CONFIG_WEIGHT_DEMO_WEB_API_PORT
#define BODY_MAX 256U
#define JSON_MAX 4096U

static const char *TAG = "RELAY_WEB";
static httpd_handle_t s_server;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static size_t read_body(httpd_req_t *req, char *body, size_t cap)
{
    body[0] = '\0';
    if (!req->content_len || req->content_len >= cap) return 0;
    int n = httpd_req_recv(req, body, req->content_len);
    if (n <= 0) return 0;
    body[n] = '\0';
    return (size_t)n;
}

static bool param(httpd_req_t *req, const char *body, const char *key,
                  char *out, size_t cap)
{
    out[0] = '\0';
    size_t qn = httpd_req_get_url_query_len(req) + 1;
    if (qn > 1) {
        char *query = malloc(qn);
        if (query) {
            bool found = httpd_req_get_url_query_str(req, query, qn) == ESP_OK &&
                httpd_query_key_value(query, key, out, cap) == ESP_OK;
            free(query);
            if (found) return true;
        }
    }
    return body && body[0] && httpd_query_key_value(body, key, out, cap) == ESP_OK;
}

static bool integer(httpd_req_t *req, const char *body, const char *key,
                    long *value)
{
    char tmp[24];
    if (!param(req, body, key, tmp, sizeof(tmp))) return false;
    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (end == tmp || !end || *end) return false;
    *value = v;
    return true;
}

static bool channel_param(httpd_req_t *req, const char *body, uint8_t *channel)
{
    long n;
    if (integer(req, body, "channel_id", &n) && n >= 1 && n <= 2) {
        *channel = (uint8_t)n;
        return true;
    }
    char id[8];
    if (!param(req, body, "channel_id", id, sizeof(id))) return false;
    if (!strcmp(id, "CH1")) *channel = 1;
    else if (!strcmp(id, "CH2")) *channel = 2;
    else return false;
    return true;
}

static bool material_param(httpd_req_t *req, const char *body, uint8_t *material)
{
    long n;
    if (integer(req, body, "material_id", &n) && n >= 1 && n <= 2) {
        *material = (uint8_t)n;
        return true;
    }
    char id[8];
    if (!param(req, body, "material_id", id, sizeof(id))) return false;
    if (!strcmp(id, "M1")) *material = 1U;
    else if (!strcmp(id, "M2")) *material = 2U;
    else return false;
    return true;
}

static esp_err_t json(httpd_req_t *req, const char *data, int len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, data, len);
}

static const char *nullable(const char *s) { return s ? s : ""; }

static size_t status_json(char *out, size_t cap)
{
    uint32_t now = now_ms();
    dispense_job_t jobs[JOB_QUEUE_MAX];
    uint32_t count = job_queue_snapshot(jobs, JOB_QUEUE_MAX);
    int n = snprintf(out, cap, "{\"sender_link\":%s,\"channels\":[",
                     websocket_client_is_connected() ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t used = (size_t)n;
    for (uint8_t ch = 1; ch <= 2; ++ch) {
        dual_channel_snapshot_t s;
        if (!dual_dispense_controller_snapshot(ch, now, &s)) return 0;
        n = snprintf(out + used, cap - used,
            "%s{\"material_id\":\"M%u\",\"pump_id\":\"Pump %u\",\"channel_id\":%u,\"relay_name\":\"Relay %u\",\"scale_name\":\"Scale %u\",\"weight_g\":%ld,"
            "\"weight_age_ms\":%lu,\"weight_valid\":%s,\"stable\":%s,"
            "\"target_g\":%ld,\"start_weight_g\":%ld,\"error_g\":%ld,"
            "\"active_job_id\":%lu,\"queue_depth\":%lu,\"state\":\"%s\","
            "\"relay_on\":%s,\"kp\":%.6g,\"ki\":%.6g,\"kd\":%.6g,"
            "\"p_term\":%.6g,\"i_term\":%.6g,\"d_term\":%.6g,\"output\":%.6g,"
            "\"profile_id\":\"%s\",\"profile_version\":%lu,\"fault\":\"%s\"}",
            ch == 1 ? "" : ",", ch, ch, ch, ch, ch, (long)s.current_weight_g,
            (unsigned long)s.weight_age_ms, s.have_weight ? "true" : "false",
            s.stable ? "true" : "false", (long)s.target_g,
            (long)s.start_weight_g, (long)s.error_g,
            (unsigned long)s.active_job_id, (unsigned long)s.queue_depth,
            dual_dispense_state_name(s.state), s.relay_on ? "true" : "false",
            (double)s.kp, (double)s.ki, (double)s.kd, (double)s.p_term,
            (double)s.i_term, (double)s.d_term, (double)s.output,
            s.profile_id, (unsigned long)s.profile_version, nullable(s.fault));
        if (n < 0 || (size_t)n >= cap - used) return 0;
        used += (size_t)n;
    }
    n = snprintf(out + used, cap - used, "],\"queue\":[");
    if (n < 0 || (size_t)n >= cap - used) return 0;
    used += (size_t)n;
    for (uint32_t i = 0; i < count; ++i) {
        n = snprintf(out + used, cap - used,
            "%s{\"id\":%lu,\"material_id\":\"M%u\",\"channel_id\":%u,\"target_g\":%ld,"
            "\"priority\":%u,\"enqueue_sequence\":%lu,\"state\":\"%s\",\"queued_ms\":%lu,\"start_g\":%ld,\"final_g\":%ld}",
            i ? "," : "", (unsigned long)jobs[i].id, (unsigned)jobs[i].material_id, (unsigned)jobs[i].channel_id,
            (long)jobs[i].target_g, (unsigned)jobs[i].priority,
            (unsigned long)jobs[i].seq, job_state_name(jobs[i].state), (unsigned long)jobs[i].requested_ms, (long)jobs[i].start_g,
            (long)jobs[i].final_g);
        if (n < 0 || (size_t)n >= cap - used) return 0;
        used += (size_t)n;
    }
    n = snprintf(out + used, cap - used,
        "],\"safety\":{\"fault\":\"%s\",\"active\":%s},\"estop\":%s}",
        safety_fault_name(safety_manager_fault()),
        safety_manager_fault_active() ? "true" : "false",
        safety_manager_fault() == SAFETY_EMERGENCY_STOP ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - used) return 0;
    return used + (size_t)n;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char data[JSON_MAX];
    size_t n = status_json(data, sizeof(data));
    return n ? json(req, data, (int)n) : httpd_resp_send_err(req, 500, "status serialization failed");
}

static esp_err_t job_handler(httpd_req_t *req)
{
    char body[BODY_MAX]; (void)read_body(req, body, sizeof(body));
    uint8_t priority = JOB_PRIORITY_NORMAL;
    long target = 0, p = 0;
    uint8_t material = 0;
    if (!integer(req, body, "target_g", &target))
        return httpd_resp_send_err(req, 400, "target_g required");
    if (!material_param(req, body, &material))
        return httpd_resp_send_err(req, 400, "material_id required (M1 or M2)");
    char override[24];
    if (param(req, body, "channel_id", override, sizeof(override)) ||
        param(req, body, "pump_id", override, sizeof(override)) ||
        param(req, body, "relay_id", override, sizeof(override)) ||
        param(req, body, "scale_id", override, sizeof(override)))
        return httpd_resp_send_err(req, 400, "physical assignments are fixed by material_id");
    if (integer(req, body, "priority", &p)) {
        if (p < 0 || p > JOB_PRIORITY_MAX) return httpd_resp_send_err(req, 400, "priority must be 0 or 1");
        priority = (uint8_t)p;
    }
    uint32_t id = 0;
    esp_err_t err = job_queue_add_global_material(material, (int32_t)target,
        priority, now_ms(), &id);
    char response[128];
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        int n = snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}",
                         err == ESP_ERR_NO_MEM ? "queue full" : "invalid job");
        return json(req, response, n);
    }
    int n = snprintf(response, sizeof(response),
        "{\"ok\":true,\"id\":%lu,\"material_id\":\"M%u\",\"channel_id\":null,\"target_g\":%ld,\"priority\":%u}",
        (unsigned long)id, (unsigned)material, target, (unsigned)priority);
    return json(req, response, n);
}

static esp_err_t channel_action(httpd_req_t *req, const char *action)
{
    char body[BODY_MAX]; (void)read_body(req, body, sizeof(body));
    uint8_t ch;
    if (!channel_param(req, body, &ch)) return httpd_resp_send_err(req, 400, "channel_id required (1 or 2)");
    bool ok = false;
    if (!strcmp(action, "pause")) ok = dual_dispense_controller_pause(ch, now_ms());
    else if (!strcmp(action, "resume")) ok = dual_dispense_controller_resume(ch, now_ms());
    else ok = dual_dispense_controller_cancel(ch, now_ms());
    char response[80];
    int n = snprintf(response, sizeof(response), "{\"ok\":%s,\"channel_id\":\"CH%u\"}", ok ? "true" : "false", ch);
    return json(req, response, n);
}
static esp_err_t pause_handler(httpd_req_t *r) { return channel_action(r, "pause"); }
static esp_err_t resume_handler(httpd_req_t *r) { return channel_action(r, "resume"); }
static esp_err_t cancel_handler(httpd_req_t *req)
{
    char body[BODY_MAX]; (void)read_body(req, body, sizeof(body));
    long id = 0;
    if (integer(req, body, "id", &id) && id > 0) {
        /* Strictly one job. A queued sibling with the same target_g survives. */
        bool ok = dual_dispense_controller_cancel_job((uint32_t)id, now_ms());
        return json(req, ok ? "{\"ok\":true,\"scope\":\"one_job\"}" :
                             "{\"ok\":false,\"error\":\"job is no longer cancellable\"}", HTTPD_RESP_USE_STRLEN);
    }
    uint8_t ch;
    if (!channel_param(req, body, &ch)) return httpd_resp_send_err(req, 400, "job id or active channel required");
    /* Channel-keyed: the ACTIVE job on that channel only, never the backlog. */
    bool ok = dual_dispense_controller_cancel(ch, now_ms());
    return json(req, ok ? "{\"ok\":true,\"scope\":\"active_job\"}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t estop_handler(httpd_req_t *req)
{
    safety_manager_raise(SAFETY_EMERGENCY_STOP, now_ms());
    dual_dispense_controller_emergency_stop(now_ms());
    return json(req, "{\"ok\":true,\"estop\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    char body[BODY_MAX]; (void)read_body(req, body, sizeof(body));
    uint8_t ch;
    bool ok = false;
    if (channel_param(req, body, &ch)) ok = dual_dispense_controller_clear_fault(ch, now_ms());
    else if (safety_manager_fault() == SAFETY_EMERGENCY_STOP) {
        ok = safety_manager_release_estop();
        if (ok) dual_dispense_controller_clear_emergency_stop();
    } else ok = safety_manager_clear();
    return json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
"<title>BITS Relay Controller</title><style>body{font:16px system-ui;background:#101820;color:#eef;padding:20px;max-width:1100px;margin:auto}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:16px}.card{background:#1b2c38;padding:18px;border-radius:12px}"
"input,select,button{font:inherit;padding:9px;border-radius:7px;margin:4px}button{cursor:pointer;background:#55c2a3;border:0;color:#071713}"
".stop{background:#d54444;color:white}pre{white-space:pre-wrap}.muted{color:#aab8c0}</style></head><body>"
"<h1>Global dispensing queue</h1><p class='muted'>The ESP32 scheduler assigns waiting jobs to either available channel. CH1: Scale 1 / Relay 1 / P6. CH2: Scale 2 / Relay 2 / P7.</p>"
"<section class='card'><label>Target kg <input id='target' type='number' min='.001' max='20' step='.001' value='5.000'></label>"
"<label>Priority <select id='priority'><option value='0'>0 Normal FIFO</option><option value='1'>1 Priority</option></select></label><button onclick='add()'>Add to global queue</button><span id='msg'></span></section>"
"<div class='grid'><section class='card'><h2>Channel 1 · Scale 1 · PID 1 · Relay 1</h2><div id='c1'></div><button onclick='act(\"pause\",1)'>Pause</button><button onclick='act(\"resume\",1)'>Resume</button><button onclick='act(\"cancel\",1)'>Cancel active job</button></section>"
"<section class='card'><h2>Channel 2 · Scale 2 · PID 2 · Relay 2</h2><div id='c2'></div><button onclick='act(\"pause\",2)'>Pause</button><button onclick='act(\"resume\",2)'>Resume</button><button onclick='act(\"cancel\",2)'>Cancel active job</button></section></div>"
"<p><button class='stop' onclick='post(\"estop\")'>GLOBAL EMERGENCY STOP — BOTH RELAYS OFF</button> <button onclick='post(\"clear\")'>Clear global safety latch</button></p>"
"<section class='card'><h2>Queue</h2><pre id='q'>Loading…</pre><p id='safety'></p></section>"
"<script>function post(path,data){return fetch('/api/'+path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data||{})}).then(r=>r.json())}"
"function add(){let kg=Number(document.getElementById('target').value);if(!(kg>0&&kg<=20)){document.getElementById('msg').textContent='Enter a target from 0.001 to 20 kg';return}post('job',{target_g:Math.round(kg*1000),priority:document.getElementById('priority').value}).then(r=>document.getElementById('msg').textContent=r.ok?'Queued job '+r.id:r.error)}"
"function act(a,ch){post(a,{channel_id:ch})}function fmt(g){return (g/1000).toFixed(3)+' kg'}function tick(){fetch('/api/status').then(r=>r.json()).then(d=>{d.channels.forEach(c=>{let box=document.getElementById('c'+c.channel_id);box.innerHTML='<p>State: '+c.state+' · Job '+(c.active_job_id||'—')+' · '+(c.weight_valid?'Scale valid':'Scale stale/unknown')+'</p><p>Weight: '+(c.weight_valid?fmt(c.weight_g):'—')+' · Target: '+fmt(c.target_g)+' · Remaining/error: '+fmt(c.error_g)+'</p><p>'+c.relay_name+': '+(c.relay_on?'ON':'OFF')+' · PID output '+(c.output*100).toFixed(1)+'% · profile '+c.profile_id+' v'+c.profile_version+'</p><p>Fault: '+(c.fault||'none')+'</p>'});document.getElementById('q').textContent=JSON.stringify(d.queue,null,2);document.getElementById('safety').textContent='Sender link: '+(d.sender_link?'connected':'disconnected')+' · Global safety: '+d.safety.fault});}tick();setInterval(tick,1000)</script></body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{ httpd_resp_set_type(req, "text/html; charset=utf-8"); return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN); }

typedef struct { const char *uri; esp_err_t (*handler)(httpd_req_t *); } route_t;
esp_err_t web_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_API_PORT; cfg.max_uri_handlers = 16; cfg.stack_size = 6144; cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err)); s_server = NULL; return err; }
    const route_t routes[] = {{"/",page_handler},{"/api/status",status_handler},{"/api/job",job_handler},
        {"/api/cancel",cancel_handler},{"/api/pause",pause_handler},{"/api/resume",resume_handler},
        {"/api/estop",estop_handler},{"/api/clear",clear_handler}};
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        httpd_uri_t uri = {.uri=routes[i].uri,.method=HTTP_GET,.handler=routes[i].handler};
        if ((err=httpd_register_uri_handler(s_server,&uri)) != ESP_OK) return err;
        uri.method=HTTP_POST;
        if ((err=httpd_register_uri_handler(s_server,&uri)) != ESP_OK) return err;
    }
    ESP_LOGI(TAG,"independent channel web API listening on %d",WEB_API_PORT);
    return ESP_OK;
}
bool web_api_is_started(void) { return s_server != NULL; }
