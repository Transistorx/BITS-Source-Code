#include "provisioning.h"
#include "nvs_config.h"
#include "app_error.h"
#include "broker_cfg.h"
#include "mqtt_link_core.h"
#include "nvs.h"
#include "led_status.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PROV";

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define PROV_DONE_BIT    ((EventBits_t)(1U << 0U))
#define AP_DEFAULT_IP    "192.168.4.1"
#define DNS_PORT         53
#define DNS_BUF_SIZE     512U
#define BODY_BUF_SIZE    1024U
#define FIELD_RAW_SIZE   (3U * BROKER_CFG_URI_MAX + 4U)
#define MAX_SCAN_APS     20U
#define HTML_BUF_SIZE    14336U

/* --------------------------------------------------------------------------
 * Scan result record — SSID deduplicated, strongest RSSI kept
 * -------------------------------------------------------------------------- */
typedef struct {
    char   ssid[33U];
    int8_t rssi;
    bool   open;
} scan_ap_t;

/* --------------------------------------------------------------------------
 * Module state — one provisioning session at a time
 * -------------------------------------------------------------------------- */
static EventGroupHandle_t s_event    = NULL;
static httpd_handle_t     s_http     = NULL;
static TaskHandle_t       s_dns_task = NULL;
static int                s_dns_sock = -1;

static scan_ap_t         s_scan_results[MAX_SCAN_APS];
static wifi_ap_record_t  s_raw_scan[MAX_SCAN_APS];
static uint16_t          s_scan_count = 0U;
static char              s_html_buf[HTML_BUF_SIZE];

typedef struct {
    char body[BODY_BUF_SIZE];
    char raw[FIELD_RAW_SIZE];
    char slot_str[4U];
    char ssid[33U];
    char pass[65U];
    char mqtt_uri[BROKER_CFG_URI_MAX + 1U];
    char mqtt_user[BROKER_CFG_USER_MAX + 1U];
    char mqtt_pass[BROKER_CFG_PASS_MAX + 1U];
    char mqtt_clear[4U];
    broker_cfg_rec_t rec;
} prov_form_t;

static prov_form_t s_form;
static broker_cfg_rec_t s_page_rec;

/* --------------------------------------------------------------------------
 * HTML — assembled per request in build_html() from four parts:
 *   p1   : head + CSS + card header (static)
 *   dev  : device id + MAC info box (snprintf — per device)
 *   p2   : form skeleton (network list, password row, Connect) up to <script>
 *   data : DEF_SSID/DEF_PASS prefill vars + scan results array (snprintf)
 *   p3   : JS that builds the network list + handlers, then closing tags (static)
 *
 * Design: an inline scrollable network list (iOS-style), always saves to the
 * primary slot.  It is in normal flow (not a pop-up) so .card{overflow:hidden}
 * cannot clip it and the list scrolls internally.  A native <select> can't
 * render graphical icons, so each row draws phone-style signal bars as inline
 * SVG (1-4 bars, coloured by RSSI) plus a lock glyph for secured networks; rows
 * are sorted strongest-first in JS.  The stored SSID/password are pre-selected
 * and pre-filled when that SSID is in range, and an eye button reveals the
 * password.
 *
 * Encoding note: SSID/password values in the data block are escaped by the C
 * code (json_escape_str) using < > \/ so a crafted value cannot close
 * the <script> block.
 * -------------------------------------------------------------------------- */
static const char s_html_p1[] =
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Wi-Fi Setup</title><style>"
    "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#f0f2f5;min-height:100vh;display:flex;align-items:flex-start;"
    "justify-content:center;padding:24px 16px}"
    ".card{background:#fff;border-radius:16px;"
    "box-shadow:0 2px 20px rgba(0,0,0,.10);width:100%;max-width:420px;overflow:hidden}"
    ".hdr{padding:20px 20px 16px;border-bottom:1px solid #f0f0f0}"
    ".hdr h1{font-size:20px;font-weight:600;color:#111}"
    ".hdr p{font-size:13px;color:#888;margin-top:2px}"
    ".dev{margin:16px 20px 0;padding:12px 14px;background:#f8f9fa;border-radius:10px;"
    "font-size:13px;color:#666;line-height:1.7}"
    ".dev b{color:#111;font-weight:600;"
    "font-family:ui-monospace,Menlo,Consolas,monospace}"
    ".fa{padding:16px 20px 22px}"
    ".fl{font-size:13px;font-weight:600;color:#444;margin-bottom:6px}"
    /* Network picker — an inline scrollable list (iOS-style), NOT a pop-up:
     * lives in normal flow so .card{overflow:hidden} can't clip it, and the
     * list scrolls internally with momentum on touch. A native <select> can't
     * render the per-row signal bars, hence the custom rows. */
    ".lblrow{display:flex;align-items:center;justify-content:space-between;"
    "margin-bottom:6px}"
    ".rscan{display:flex;align-items:center;gap:5px;background:none;border:none;"
    "padding:4px 2px;cursor:pointer;color:#1a73e8;font-size:13px;font-weight:600}"
    ".rscan:active{opacity:.6}"
    ".rscan.sp svg{animation:spin .8s linear infinite}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".nl{border:1.5px solid #e3e3e3;border-radius:10px;overflow:hidden;"
    "margin-bottom:16px;background:#fff}"
    ".nls{max-height:266px;overflow-y:auto;-webkit-overflow-scrolling:touch;"
    "overscroll-behavior:contain}"
    ".nls::-webkit-scrollbar{width:7px}"
    ".nls::-webkit-scrollbar-thumb{background:#c7c7cc;border-radius:4px}"
    ".nls::-webkit-scrollbar-track{background:transparent}"
    ".opt{display:flex;align-items:center;gap:11px;padding:12px 14px;cursor:pointer;"
    "font-size:15px;color:#111;border-bottom:1px solid #f1f1f1;"
    "transition:background .12s}"
    ".opt:last-child{border-bottom:none}"
    ".opt:active{background:#eef4ff}"
    ".opt.sel{background:#eaf2ff}"
    ".ck{flex:none;width:18px;color:#1a73e8;font-weight:700;font-size:15px;"
    "text-align:center;opacity:0}"
    ".opt.sel .ck{opacity:1}"
    ".opt .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;"
    "white-space:nowrap}"
    ".lk{flex:none;font-size:12px;line-height:1;opacity:.55}"
    ".opt svg{flex:none;display:block}"
    ".empty{padding:22px 14px;text-align:center;color:#999;font-size:14px}"
    ".pw-w{max-height:0;overflow:hidden;transition:max-height .28s ease}"
    ".pw-w.open{max-height:120px}"
    ".pr{display:flex;border:1.5px solid #d0d0d0;border-radius:8px;"
    "overflow:hidden;margin-bottom:16px;transition:border-color .15s}"
    ".pr:focus-within{border-color:#1a73e8}"
    ".pr input{flex:1;border:none;outline:none;padding:11px 13px;"
    "font-size:15px;background:transparent;color:#111;min-width:0}"
    ".eye{background:none;border:none;border-left:1.5px solid #e8e8e8;"
    "padding:0 14px;cursor:pointer;color:#888;font-size:13px;"
    "white-space:nowrap;transition:color .15s}"
    ".eye:hover{color:#1a73e8}"
    ".cb{width:100%;padding:14px;background:#1a73e8;color:#fff;border:none;"
    "border-radius:10px;font-size:16px;font-weight:500;cursor:pointer;"
    "transition:background .15s,opacity .15s}"
    ".cb:disabled{background:#b0c4f1;cursor:default}"
    "@media(hover:hover){.cb:not(:disabled):hover{background:#1459c4}}"
    "select.sel-p{display:block;width:100%;padding:11px 13px;border:1.5px solid #d0d0d0;border-radius:8px;font-size:15px;color:#111;margin-bottom:16px;outline:none;background:transparent}"
    "select.sel-p:focus{border-color:#1a73e8}"
    "</style></head><body><div class=\"card\">"
    "<div class=\"hdr\"><h1>Setup Device</h1><p>Wi-Fi & Port Config</p></div>";

/* p2: form skeleton — scrollable network list + Rescan, password row, Connect.
 * No slot picker: credentials always save to the primary slot. */
static const char s_html_p2[] =
    "<form class=\"fa\" id=\"frm\" method=\"POST\" action=\"/save\">"
    "<div class=\"lblrow\"><span class=\"fl\" style=\"margin:0\">Wi-Fi Network</span>"
    "<button type=\"button\" class=\"rscan\" id=\"rsc\" "
    "onclick=\"this.classList.add('sp');location.href='/scan'\">"
    "<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "stroke=\"currentColor\" stroke-width=\"2.4\" stroke-linecap=\"round\" "
    "stroke-linejoin=\"round\"><path d=\"M21 12a9 9 0 1 1-2.64-6.36\"/>"
    "<path d=\"M21 3v6h-6\"/></svg>Rescan</button></div>"
    "<div class=\"nl\"><div class=\"nls\" id=\"nls\"></div></div>"
    "<input type=\"hidden\" name=\"ssid\" id=\"ssid\">"
    "<div class=\"pw-w\" id=\"pws\"><div class=\"fl\">Password</div>"
    "<div class=\"pr\"><input type=\"password\" name=\"password\" id=\"pw\""
    " autocomplete=\"new-password\" placeholder=\"Wi-Fi password\" maxlength=\"64\">"
    "<button type=\"button\" class=\"eye\" id=\"eye\" onclick=\"tpw()\">"
    "Show</button></div></div>";

static const char s_html_p2b[] =
    "<button type=\"submit\" class=\"cb\" id=\"cb\" style=\"margin-top:16px;\">Save & Restart</button>"
    "</form></div>"
    "<script>\n";

static const char s_html_mqtt[] =
    "<div class=\"fl\">MQTT broker</div>"
    "<div class=\"dev\">Stored config: <b>%s</b>%s%s</div>"
    "<div class=\"fl\">Broker URI (blank = no change)</div><div class=\"pr\">"
    "<input name=\"mqtt_uri\" maxlength=\"95\" autocomplete=\"off\" "
    "placeholder=\"mqtt://host:1883\" value=\"%s\"></div>"
    "<div class=\"fl\">Broker username (ACL expects dev_&lt;device id&gt;)</div><div class=\"pr\">"
    "<input name=\"mqtt_user\" maxlength=\"31\" autocomplete=\"off\" "
    "placeholder=\"dev_%s\" value=\"%s\"></div>"
    "<div class=\"fl\">Broker password (blank = keep stored)</div><div class=\"pr\">"
    "<input type=\"password\" name=\"mqtt_pass\" maxlength=\"63\" autocomplete=\"new-password\" "
    "placeholder=\"not shown\"></div>"
    "<label class=\"fl\"><input type=\"checkbox\" name=\"mqtt_clear\" value=\"1\"> "
    "Clear stored broker config (use built-in)</label>";

/* p3: JS — builds the scrollable network list from d[] (sorted strongest
 * first), pre-selects DEF_SSID and pre-fills DEF_PASS when present, toggles
 * the password row for secured networks, and wires the show/hide eye button. */
static const char s_html_p3[] =
    "var nls=document.getElementById('nls'),hid=document.getElementById('ssid'),"
    "pws=document.getElementById('pws'),pw=document.getElementById('pw'),"
    "cb=document.getElementById('cb');"
    "var cidx=-1;\n"
    /* SVG WiFi bars — fill 1..4 bars by RSSI, colour by strength */
    "function icon(r){"
    "var l=r>=-60?4:r>=-68?3:r>=-75?2:1;"
    "var c=l>=3?'#34a853':l===2?'#f9ab00':'#ea4335';"
    "var s='<svg viewBox=\"0 0 20 16\" width=\"20\" height=\"16\">';"
    "for(var i=0;i<4;i++){var h=4+i*3;"
    "s+='<rect x=\"'+(0.5+i*4.5)+'\" y=\"'+(15-h)+'\" width=\"3\" height=\"'+h+"
    "'\" rx=\"1\" fill=\"'+(i<l?c:'#dcdce0')+'\"/>';}"
    "return s+'</svg>';}\n"
    "function lock(o){return o?'':'<span class=\"lk\">\\uD83D\\uDD12</span>';}\n"
    "d.sort(function(a,b){return b.r-a.r;});\n"
    "function pick(i){cidx=i;var n=d[i];hid.value=n.s;"
    "pws.classList.toggle('open',!n.o);"
    "pw.value=((!n.o)&&(n.s===DEF_SSID))?DEF_PASS:'';"
    "var os=nls.children;"
    "for(var k=0;k<os.length;k++){"
    "os[k].className='opt'+(k===i?' sel':'');}}\n"
    "function build(){nls.innerHTML='';"
    "if(!d.length){nls.innerHTML='<div class=\"empty\">No networks found"
    " \\u2014 tap Rescan</div>';cb.disabled=true;return;}"
    "d.forEach(function(n,i){var o=document.createElement('div');o.className='opt';"
    "o.innerHTML='<span class=\"ck\">\\u2713</span><span class=\"nm\"></span>'"
    "+lock(n.o)+icon(n.r);"
    "o.children[1].textContent=n.s;"
    "o.onclick=function(){pick(i);};nls.appendChild(o);});}\n"
    "build();"
    "if(d.length){var pre=0;"
    "for(var i=0;i<d.length;i++){if(d[i].s===DEF_SSID){pre=i;break;}}pick(pre);"
    "if(pre>0){nls.children[pre].scrollIntoView({block:'nearest'});}}\n"
    "function tpw(){var b=document.getElementById('eye');"
    "var sh=pw.type==='password';pw.type=sh?'text':'password';"
    "b.textContent=sh?'Hide':'Show';}"
    "</script></body></html>";

static const char s_html_done[] =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Saved</title>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,"
    "sans-serif;background:#f0f2f5;min-height:100vh;display:flex;"
    "align-items:center;justify-content:center}"
    ".card{background:#fff;border-radius:16px;box-shadow:0 2px 20px rgba(0,0,0,.10);"
    "padding:40px 32px;max-width:360px;width:100%;text-align:center}"
    ".ic{font-size:48px;margin-bottom:12px}"
    "h2{font-size:20px;font-weight:600;color:#111;margin-bottom:8px}"
    "p{font-size:14px;color:#888;line-height:1.5}"
    "</style></head><body><div class=\"card\">"
    "<div class=\"ic\">&#9989;</div>"
    "<h2>Credentials Saved</h2>"
    "<p>The device is restarting.<br>Reconnect to your Wi-Fi network.</p>"
    "</div></body></html>";

/* --------------------------------------------------------------------------
 * json_escape_str — write src into dst as a JSON string body (no outer quotes).
 *
 * < / > are written as < / > so a crafted SSID cannot
 * close the <script> block.  Control chars are dropped.
 * -------------------------------------------------------------------------- */
static void json_escape_str(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0U;
    size_t i   = 0U;

    while ((src[i] != '\0') && (out + 7U < dst_size)) {
        unsigned char c = (unsigned char)src[i];
        if ((c == '"') || (c == '\\')) {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c == '/') {
            dst[out++] = '\\';
            dst[out++] = '/';
        } else if (c == '<') {
            (void)memcpy(dst + out, "\\u003c", 6U);
            out += 6U;
        } else if (c == '>') {
            (void)memcpy(dst + out, "\\u003e", 6U);
            out += 6U;
        } else if (c < 0x20U) {
            /* drop control characters */
        } else {
            dst[out++] = (char)c;
        }
        i++;
    }
    dst[out] = '\0';
}

/* --------------------------------------------------------------------------
 * do_scan — blocking WiFi scan, populate s_scan_results[].
 *
 * Requires WIFI_MODE_APSTA.  IDF returns records sorted by RSSI descending,
 * so the first occurrence of each SSID is the strongest — dedup by keeping
 * first-seen only.  Hidden networks (empty SSID) are skipped.
 * -------------------------------------------------------------------------- */
static void do_scan(void)
{
    s_scan_count = 0U;

    wifi_scan_config_t cfg = {0};
    cfg.show_hidden        = false;

    esp_err_t ret = esp_wifi_scan_start(&cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t n = MAX_SCAN_APS;
    ret = esp_wifi_scan_get_ap_records(&n, s_raw_scan);
    if ((ret != ESP_OK) || (n == 0U)) {
        ESP_LOGW(TAG, "No scan records: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t i = 0U;
    uint16_t j = 0U;

    for (i = 0U; (i < n) && (s_scan_count < MAX_SCAN_APS); i++) {
        if (s_raw_scan[i].ssid[0] == '\0') { continue; }

        bool dup = false;
        for (j = 0U; j < s_scan_count; j++) {
            if (strncmp(s_scan_results[j].ssid,
                        (char *)s_raw_scan[i].ssid, 32U) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) { continue; }

        (void)strncpy(s_scan_results[s_scan_count].ssid,
                      (char *)s_raw_scan[i].ssid, 32U);
        s_scan_results[s_scan_count].ssid[32U] = '\0';
        s_scan_results[s_scan_count].rssi       = s_raw_scan[i].rssi;
        s_scan_results[s_scan_count].open       =
            (s_raw_scan[i].authmode == WIFI_AUTH_OPEN);
        s_scan_count++;
    }

    ESP_LOGI(TAG, "Scan complete: %u unique SSIDs", (unsigned)s_scan_count);
}

/* --------------------------------------------------------------------------
 * build_html — generate the portal page into s_html_buf.
 * Called on every GET / and after every /scan redirect.
 * -------------------------------------------------------------------------- */
static void html_attr_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0U;
    if ((dst == NULL) || (dst_size == 0U)) return;
    for (size_t i = 0U; (src != NULL) && (src[i] != '\0'); i++) {
        const char *rep = NULL;
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if ((c < 0x20U) || (c > 0x7EU)) continue;
        if (rep != NULL) {
            size_t n = strlen(rep);
            if (out + n >= dst_size) break;
            memcpy(dst + out, rep, n);
            out += n;
        } else {
            if (out + 1U >= dst_size) break;
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static broker_cfg_src_t mqtt_rec_load(broker_cfg_rec_t *rec)
{
    static uint8_t blob[sizeof(broker_cfg_rec_t)];
    size_t len = sizeof(blob);
    broker_cfg_src_t src;
    esp_err_t err = nvs_config_get_blob(NVS_KEY_MQTT_CFG, blob, &len);

    broker_cfg_wipe(rec);
    if (err == ESP_OK) {
        src = broker_cfg_decode(blob, len, rec);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        src = BROKER_CFG_SRC_FALLBACK_ABSENT;
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        src = BROKER_CFG_SRC_FALLBACK_VERSION;
    } else {
        src = BROKER_CFG_SRC_FALLBACK_INVALID;
    }
    broker_cfg_wipe_bytes(blob, sizeof(blob));
    return src;
}

static size_t append_mqtt_block(size_t pos, size_t rem, const char *device_id)
{
    char uri_esc[BROKER_CFG_URI_MAX * 6U + 1U] = {0};
    char user_esc[BROKER_CFG_USER_MAX * 6U + 1U] = {0};
    char shown[BROKER_CFG_URI_MAX + 1U] = {0};
    broker_cfg_src_t src = mqtt_rec_load(&s_page_rec);
    const char *note = "";

    if (src == BROKER_CFG_SRC_NVS) {
        if (s_page_rec.uri[0] == '\0') {
            note = " (empty URI: built-in broker)";
        } else {
            mqtt_link_uri_redact(shown, sizeof(shown), s_page_rec.uri);
            html_attr_escape(shown, uri_esc, sizeof(uri_esc));
            note = " ";
        }
        html_attr_escape(s_page_rec.user, user_esc, sizeof(user_esc));
    } else {
        note = " (built-in broker)";
    }
    int n = snprintf(s_html_buf + pos, rem, s_html_mqtt, broker_cfg_src_name(src), note, uri_esc,
                     uri_esc, device_id, user_esc);
    broker_cfg_wipe(&s_page_rec);
    broker_cfg_wipe_bytes(shown, sizeof(shown));
    if ((n > 0) && ((size_t)n < rem)) return (size_t)n;
    return 0U;
}

static void build_html(void)
{
    char     esc[200U]   = {0};
    char     device_id[40U] = {0};
    char     def_ssid[33U]  = {0};
    char     def_pass[65U]  = {0};
    uint8_t  mac[6]      = {0};
    size_t   pos         = 0U;
    size_t   rem         = HTML_BUF_SIZE;
    int      n           = 0;
    uint16_t i           = 0U;

    /* Per-device values shown in the header and used to pre-fill the form. */
    (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id));
    (void)nvs_config_get_str(NVS_KEY_WIFI_SSID_1, def_ssid, sizeof(def_ssid));
    (void)nvs_config_get_str(NVS_KEY_WIFI_PASS_1, def_pass, sizeof(def_pass));
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* p1: head + CSS + header */
    n = snprintf(s_html_buf + pos, rem, "%s", s_html_p1);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    /* device info box — device_id and MAC are hex/':' only, no escaping needed */
    n = snprintf(s_html_buf + pos, rem,
                 "<div class=\"dev\">Device&nbsp;ID:&nbsp;<b>%s</b><br>"
                 "MAC:&nbsp;<b>%02X:%02X:%02X:%02X:%02X:%02X</b></div>",
                 (device_id[0] != '\0') ? device_id : "(unset)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    /* p2: form skeleton up to <script> */
    n = snprintf(s_html_buf + pos, rem, "%s", s_html_p2);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    {
        size_t added = append_mqtt_block(pos, rem, (device_id[0] != '\0') ? device_id : "");
        pos += added;
        rem -= added;
    }

    n = snprintf(s_html_buf + pos, rem, "%s", s_html_p2b);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    /* Prefill vars + scan-results array (escaped for the <script> context) */
    json_escape_str(def_ssid, esc, sizeof(esc));
    n = snprintf(s_html_buf + pos, rem, "var DEF_SSID=\"%s\";", esc);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    json_escape_str(def_pass, esc, sizeof(esc));
    n = snprintf(s_html_buf + pos, rem, "var DEF_PASS=\"%s\";var d=[", esc);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    for (i = 0U; (i < s_scan_count) && (rem > 4U); i++) {
        json_escape_str(s_scan_results[i].ssid, esc, sizeof(esc));
        n = snprintf(s_html_buf + pos, rem,
                     "%s{\"s\":\"%s\",\"o\":%d,\"r\":%d}",
                     (i > 0U) ? "," : "",
                     esc,
                     s_scan_results[i].open ? 1 : 0,
                     (int)s_scan_results[i].rssi);
        if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }
    }

    n = snprintf(s_html_buf + pos, rem, "];\n");
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; rem -= (size_t)n; }

    /* p3: JS that fills the dropdown + handlers, then closing tags */
    n = snprintf(s_html_buf + pos, rem, "%s", s_html_p3);
    if ((n > 0) && ((size_t)n < rem)) { pos += (size_t)n; }
}

/* --------------------------------------------------------------------------
 * url_decode — decode a URL-encoded field value in place.
 * -------------------------------------------------------------------------- */
static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0U;
    size_t j = 0U;

    while ((src[i] != '\0') && (j < dst_len - 1U)) {
        if ((src[i] == '%')
            && (isxdigit((unsigned char)src[i + 1U]) != 0)
            && (isxdigit((unsigned char)src[i + 2U]) != 0)) {
            char hex[3] = {src[i + 1U], src[i + 2U], '\0'};
            dst[j] = (char)(uint8_t)strtol(hex, NULL, 16);
            i += 3U;
        } else if (src[i] == '+') {
            dst[j] = ' ';
            i++;
        } else {
            dst[j] = src[i];
            i++;
        }
        j++;
    }
    dst[j] = '\0';
}

/* --------------------------------------------------------------------------
 * HTTP handler: GET /
 * -------------------------------------------------------------------------- */
static esp_err_t http_get_root(httpd_req_t *req)
{
    build_html();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_html_buf, HTTPD_RESP_USE_STRLEN);
}

/* --------------------------------------------------------------------------
 * HTTP handler: GET /scan
 * Re-run WiFi scan (blocking ~3 s), redirect to / with fresh results.
 * -------------------------------------------------------------------------- */
static esp_err_t http_get_scan(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Rescan requested");
    do_scan();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_DEFAULT_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* --------------------------------------------------------------------------
 * HTTP handler: GET wildcard catch-all
 * Redirect every unknown GET to the portal (captive portal trigger).
 * -------------------------------------------------------------------------- */
static esp_err_t http_get_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_DEFAULT_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* --------------------------------------------------------------------------
 * HTTP handler: POST /save
 * -------------------------------------------------------------------------- */
typedef enum {
    FIELD_USER = 0,
    FIELD_PASS
} field_kind_t;

static bool field_chars_ok(const char *s, field_kind_t kind)
{
    for (size_t i = 0U; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (kind == FIELD_USER) {
            if ((isalnum(c) == 0) && (c != '.') && (c != '_') && (c != '-')) return false;
        } else if ((c < 0x20U) || (c > 0x7EU)) {
            return false;
        }
    }
    return true;
}

static size_t url_decoded_len(const char *src)
{
    size_t n = 0U;
    for (size_t i = 0U; src[i] != '\0'; n++) {
        if ((src[i] == '%') && (isxdigit((unsigned char)src[i + 1U]) != 0) &&
            (isxdigit((unsigned char)src[i + 2U]) != 0)) {
            i += 3U;
        } else {
            i++;
        }
    }
    return n;
}

static esp_err_t form_field(const char *key, char *dst, size_t dst_len, bool strict)
{
    memset(s_form.raw, 0, sizeof(s_form.raw));
    dst[0] = '\0';
    esp_err_t err = httpd_query_key_value(s_form.body, key, s_form.raw, sizeof(s_form.raw));
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return strict ? ESP_ERR_INVALID_SIZE : ESP_OK;
    if (strict && (url_decoded_len(s_form.raw) >= dst_len)) return ESP_ERR_INVALID_SIZE;
    url_decode(dst, dst_len, s_form.raw);
    return ESP_OK;
}

static esp_err_t mqtt_apply(const char **why)
{
    esp_err_t ret = ESP_OK;
    char shown[BROKER_CFG_URI_MAX + 1U] = {0};
    broker_cfg_src_t stored;

    *why = NULL;
    if (s_form.mqtt_clear[0] == '1') {
        ret = nvs_config_erase_key(NVS_KEY_MQTT_CFG);
        if (ret != ESP_OK) *why = "NVS erase failed";
        else ESP_LOGI(TAG, "mqtt_cfg cleared; built-in broker config applies after restart");
        goto cleanup;
    }
    if (s_form.mqtt_uri[0] == '\0') {
        if ((s_form.mqtt_user[0] != '\0') || (s_form.mqtt_pass[0] != '\0')) {
            ret = ESP_ERR_INVALID_ARG;
            *why = "Broker URI required when a username or password is given";
        }
        goto cleanup;
    }
    if (!broker_cfg_uri_valid(s_form.mqtt_uri)) {
        ret = ESP_ERR_INVALID_ARG;
        *why = "Invalid broker URI (mqtt://host[:port] or mqtts://host[:port], no credentials)";
        goto cleanup;
    }
    if (!field_chars_ok(s_form.mqtt_user, FIELD_USER)) {
        ret = ESP_ERR_INVALID_ARG;
        *why = "Username may contain only letters, digits, '.', '_' and '-'";
        goto cleanup;
    }
    if (!field_chars_ok(s_form.mqtt_pass, FIELD_PASS)) {
        ret = ESP_ERR_INVALID_ARG;
        *why = "Password must be printable ASCII";
        goto cleanup;
    }
    stored = mqtt_rec_load(&s_form.rec);
    if ((s_form.mqtt_pass[0] == '\0') && (stored == BROKER_CFG_SRC_NVS)) {
        memcpy(s_form.mqtt_pass, s_form.rec.pass, sizeof(s_form.mqtt_pass));
        s_form.mqtt_pass[sizeof(s_form.mqtt_pass) - 1U] = '\0';
    }
    if (!broker_cfg_encode(&s_form.rec, s_form.mqtt_uri, s_form.mqtt_user, s_form.mqtt_pass)) {
        ret = ESP_ERR_INVALID_ARG;
        *why = "Broker config could not be encoded";
        goto cleanup;
    }
    ret = nvs_config_set_blob(NVS_KEY_MQTT_CFG, &s_form.rec, sizeof(s_form.rec));
    if (ret != ESP_OK) {
        *why = "NVS write failed (broker config)";
        goto cleanup;
    }
    mqtt_link_uri_redact(shown, sizeof(shown), s_form.mqtt_uri);
    ESP_LOGI(TAG, "Saved mqtt_cfg uri=%s auth=%s", shown, (s_form.mqtt_user[0] != '\0') ? "yes" : "no");

cleanup:
    broker_cfg_wipe(&s_form.rec);
    broker_cfg_wipe_bytes(shown, sizeof(shown));
    return ret;
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    esp_err_t ret = ESP_FAIL;
    const char *why = NULL;

    broker_cfg_wipe_bytes(&s_form, sizeof(s_form));
    int received = httpd_req_recv(req, s_form.body, sizeof(s_form.body) - 1U);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        goto cleanup;
    }
    s_form.body[received] = '\0';

    (void)form_field("slot", s_form.slot_str, sizeof(s_form.slot_str), false);
    (void)form_field("ssid", s_form.ssid, sizeof(s_form.ssid), false);
    (void)form_field("password", s_form.pass, sizeof(s_form.pass), false);
    (void)form_field("mqtt_clear", s_form.mqtt_clear, sizeof(s_form.mqtt_clear), false);
    if ((form_field("mqtt_uri", s_form.mqtt_uri, sizeof(s_form.mqtt_uri), true) != ESP_OK) ||
        (form_field("mqtt_user", s_form.mqtt_user, sizeof(s_form.mqtt_user), true) != ESP_OK) ||
        (form_field("mqtt_pass", s_form.mqtt_pass, sizeof(s_form.mqtt_pass), true) != ESP_OK)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Broker field too long");
        goto cleanup;
    }

    if (s_form.ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        goto cleanup;
    }

    uint8_t slot = (uint8_t)atoi(s_form.slot_str);
    if ((slot < 1U) || (slot > 2U)) {
        slot = 1U;
    }

    ret = mqtt_apply(&why);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "broker config not saved: %s", (why != NULL) ? why : esp_err_to_name(ret));
        httpd_resp_send_err(req, (ret == ESP_ERR_INVALID_ARG) ? HTTPD_400_BAD_REQUEST
                                                               : HTTPD_500_INTERNAL_SERVER_ERROR,
                            (why != NULL) ? why : "Broker config not saved");
        goto cleanup;
    }

    const char *ssid_key = (slot == 1U) ? NVS_KEY_WIFI_SSID_1 : NVS_KEY_WIFI_SSID_2;
    const char *pass_key = (slot == 1U) ? NVS_KEY_WIFI_PASS_1 : NVS_KEY_WIFI_PASS_2;

    ret = nvs_config_set_str(ssid_key, s_form.ssid);
    if (ret == ESP_OK) {
        ret = nvs_config_set_str(pass_key, s_form.pass);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed (Wi-Fi)");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Saved slot %u, SSID: '%s'", (unsigned)slot, s_form.ssid);

    httpd_resp_set_type(req, "text/html");
    (void)httpd_resp_send(req, s_html_done, -1);

    xEventGroupSetBits(s_event, PROV_DONE_BIT);
    ret = ESP_OK;

cleanup:
    broker_cfg_wipe_bytes(&s_form, sizeof(s_form));
    return ret;
}

/* --------------------------------------------------------------------------
 * URI handler descriptors — file-scope so their lifetime exceeds the
 * httpd_register_uri_handler() calls.
 * Specific routes registered before the wildcard catch-all.
 * -------------------------------------------------------------------------- */
static const httpd_uri_t s_uri_root = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = http_get_root,
};
static const httpd_uri_t s_uri_scan = {
    .uri     = "/scan",
    .method  = HTTP_GET,
    .handler = http_get_scan,
};
static const httpd_uri_t s_uri_save = {
    .uri     = "/save",
    .method  = HTTP_POST,
    .handler = http_post_save,
};
static const httpd_uri_t s_uri_catch = {
    .uri     = "/*",
    .method  = HTTP_GET,
    .handler = http_get_redirect,
};

/* --------------------------------------------------------------------------
 * dns_task — minimal UDP DNS responder.
 *
 * Replies to every A-record query with 192.168.4.1.  This causes iOS,
 * Android, and Windows to auto-pop the captive portal in the browser.
 * -------------------------------------------------------------------------- */
static void dns_task(void *arg)
{
    static uint8_t s_rx[DNS_BUF_SIZE];
    static uint8_t s_tx[DNS_BUF_SIZE];

    (void)arg;

    struct sockaddr_in srv = {0};
    srv.sin_family         = AF_INET;
    srv.sin_addr.s_addr    = htonl(INADDR_ANY);
    srv.sin_port           = htons((uint16_t)DNS_PORT);

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(s_dns_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind on port %d failed", DNS_PORT);
        close(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS responder ready on port %d", DNS_PORT);

    for (;;) {
        struct sockaddr_in client = {0};
        socklen_t clen            = sizeof(client);

        int rxlen = recvfrom(s_dns_sock, s_rx, sizeof(s_rx), 0,
                             (struct sockaddr *)&client, &clen);

        if (rxlen < 12) { continue; }

        size_t copy = ((size_t)rxlen < DNS_BUF_SIZE - 16U)
                      ? (size_t)rxlen : (DNS_BUF_SIZE - 16U);
        (void)memcpy(s_tx, s_rx, copy);

        s_tx[2] = (uint8_t)((s_rx[2] & 0x01U) | 0x84U);
        s_tx[3] = 0x80U;
        s_tx[6] = 0x00U;
        s_tx[7] = 0x01U;

        uint8_t *a = s_tx + copy;
        a[0]  = 0xC0U;  a[1]  = 0x0CU;
        a[2]  = 0x00U;  a[3]  = 0x01U;
        a[4]  = 0x00U;  a[5]  = 0x01U;
        a[6]  = 0x00U;  a[7]  = 0x00U;
        a[8]  = 0x00U;  a[9]  = 0x3CU;
        a[10] = 0x00U;  a[11] = 0x04U;
        a[12] = 192U;   a[13] = 168U;
        a[14] = 4U;     a[15] = 1U;

        (void)sendto(s_dns_sock, s_tx, copy + 16U, 0,
                     (struct sockaddr *)&client, clen);
    }
}

/* --------------------------------------------------------------------------
 * provisioning_build_ap_name — pure, unit-tested
 *
 * "<prefix>-XXXX": the last four hex characters of the device id, upper-cased.
 * Scanning from the end keeps the suffix stable if the id's textual prefix
 * changes; left-padding keeps a short id usable instead of producing a
 * nameless AP (two such devices would be indistinguishable to the user).
 * -------------------------------------------------------------------------- */
void provisioning_build_ap_name(const char *prefix, const char *device_id,
                                char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return;
    }

    char suffix[5] = {'0', '0', '0', '0', '\0'};

    if (device_id != NULL) {
        size_t len = strlen(device_id);

        /* Walk backwards collecting hex digits, noting where the run starts. */
        size_t run_start = len;
        size_t collected = 0U;
        while (run_start > 0U && collected < 4U) {
            unsigned char c = (unsigned char)device_id[run_start - 1U];
            if (!isxdigit(c)) {
                break;
            }
            run_start--;
            collected++;
        }

        /* Right-align: the digits found are the LAST ones, so they fill the
         * low end of the suffix and any shortfall stays '0' on the left. */
        size_t pos = 4U - collected;
        for (size_t i = 0U; i < collected; i++) {
            suffix[pos + i] = (char)toupper((unsigned char)device_id[run_start + i]);
        }
    }

    snprintf(out, cap, "%s-%s", (prefix != NULL) ? prefix : "BITS", suffix);
}

/* --------------------------------------------------------------------------
 * provisioning_run — public entry point
 * -------------------------------------------------------------------------- */
esp_err_t provisioning_run(const char *ap_ssid, uint32_t timeout_ms)
{
    if (ap_ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Fast-blink the status LED for the whole portal session. */
    led_status_set(LED_PROVISIONING);

    ESP_LOGW(TAG, "==============================");
    ESP_LOGW(TAG, "  PROVISIONING MODE");
    ESP_LOGW(TAG, "  Connect to WiFi: '%s'", ap_ssid);
    ESP_LOGW(TAG, "  Then open:  http://%s", AP_DEFAULT_IP);
    ESP_LOGW(TAG, "  Timeout: %lu s", (unsigned long)(timeout_ms / 1000U));
    ESP_LOGW(TAG, "==============================");

    s_event = xEventGroupCreate();
    if (s_event == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }

    /* AP netif — DHCP server + gateway at 192.168.4.1 */
    (void)esp_netif_create_default_wifi_ap();

    /* STA netif — required for esp_wifi_scan_start() in APSTA mode.
     * The STA never connects; it provides scan infrastructure only. */
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    CHECK_ERR(TAG, esp_wifi_init(&wifi_init_cfg));

    wifi_config_t ap_cfg     = {0};
    ap_cfg.ap.channel        = 1U;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4U;
    (void)strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1U);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);

    /* APSTA mode: AP broadcasts SSID while STA radio performs scan */
    CHECK_ERR(TAG, esp_wifi_set_mode(WIFI_MODE_APSTA));
    CHECK_ERR(TAG, esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    CHECK_ERR(TAG, esp_wifi_start());

    /* Scan once before the HTTP server starts — no clients connected yet,
     * so the ~3 s channel-hop scan is invisible to users. */
    do_scan();

    BaseType_t task_ok = xTaskCreate(dns_task, "prov_dns",
                                     4096U, NULL, 5U, &s_dns_task);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "DNS task failed — navigate manually to http://%s",
                 AP_DEFAULT_IP);
    }

    httpd_config_t http_cfg   = HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn     = httpd_uri_match_wildcard;
    http_cfg.max_uri_handlers = 8U;

    CHECK_ERR(TAG, httpd_start(&s_http, &http_cfg));

    /* Specific routes must be registered before the wildcard catch-all */
    CHECK_ERR(TAG, httpd_register_uri_handler(s_http, &s_uri_root));
    CHECK_ERR(TAG, httpd_register_uri_handler(s_http, &s_uri_scan));
    CHECK_ERR(TAG, httpd_register_uri_handler(s_http, &s_uri_save));
    CHECK_ERR(TAG, httpd_register_uri_handler(s_http, &s_uri_catch));

    EventBits_t bits = xEventGroupWaitBits(s_event, PROV_DONE_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    if ((bits & PROV_DONE_BIT) != 0U) {
        ESP_LOGI(TAG, "Credentials saved — restarting to connect");
    } else {
        ESP_LOGW(TAG, "Provisioning timed out — restarting");
    }

    esp_restart();
    return ESP_OK;
}
