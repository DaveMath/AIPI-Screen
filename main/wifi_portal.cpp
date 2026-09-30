#include "wifi_portal.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <vector>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "status_led.h"

namespace {

constexpr char kTag[] = "wifi_portal";
constexpr char kNamespace[] = "aipi_screen";
constexpr char kPortalIp[] = "1.2.3.4";
constexpr size_t kMaxNetworks = 24;
constexpr uint8_t kDefaultRed = 201;
constexpr uint8_t kDefaultGreen = 168;
constexpr uint8_t kDefaultBlue = 76;
constexpr uint8_t kDefaultBrightness = 38;

enum class HostMode : uint8_t {
    kSetupOnly = 0,
    kAlwaysOn = 1,
};

struct DeviceSettings {
    char ssid[33] = {};
    char password[65] = {};
    uint8_t red = kDefaultRed;
    uint8_t green = kDefaultGreen;
    uint8_t blue = kDefaultBlue;
    uint8_t brightness = kDefaultBrightness;
    HostMode host_mode = HostMode::kSetupOnly;
};

constexpr char kPortalHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AIPI-Screen Setup</title><style>
:root{color-scheme:dark;--ink:#0f0f1a;--panel:#1d1e29;--line:#3a3d4a;--gold:#c9a84c;--paprika:#d66231;--text:#f7f4ed;--muted:#b6b5bb}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--ink);color:var(--text);font:16px/1.45 system-ui,-apple-system,sans-serif}
header{padding:18px 20px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}header b{font-size:19px}.credit{padding:5px 9px;border:1px solid var(--gold);border-radius:4px;color:var(--gold);font-size:12px;font-weight:800;text-transform:uppercase}
main{width:min(580px,calc(100% - 28px));margin:28px auto 50px}h1{font-size:34px;line-height:1.08;margin:7px 0 10px;letter-spacing:0}.eyebrow{color:var(--paprika);font-size:12px;font-weight:800;text-transform:uppercase}p{color:var(--muted)}
form{margin-top:22px;padding:22px;background:var(--panel);border:1px solid var(--line);border-radius:8px}fieldset{margin:0;padding:0 0 21px;border:0;border-bottom:1px solid var(--line)}fieldset+fieldset{padding-top:21px}fieldset:last-of-type{padding-bottom:0;border-bottom:0}legend{padding:0;font-size:18px;font-weight:800}label{display:block;margin:15px 0 7px;font-weight:700}.hint{margin:5px 0 0;color:#92949e;font-size:12px}.row{display:grid;grid-template-columns:1fr auto;gap:8px}.password{position:relative}.password button{position:absolute;right:6px;top:6px;width:auto;margin:0;padding:8px 11px;background:#30323e;color:var(--text)}
select,input{width:100%;min-width:0;padding:12px;border:1px solid #505361;border-radius:5px;background:#11121a;color:var(--text);font:inherit}select:focus,input:focus{outline:2px solid var(--gold);border-color:transparent}button{border:0;border-radius:5px;padding:12px 15px;font:inherit;font-weight:800;cursor:pointer}.secondary{background:#30323e;color:var(--text)}.save{width:100%;margin-top:22px;background:linear-gradient(110deg,var(--gold),var(--paprika));color:#111}
.color-controls{display:grid;grid-template-columns:80px 1fr;gap:16px;align-items:center}.color-controls input[type=color]{height:64px;padding:5px;cursor:pointer}.range-row{display:grid;grid-template-columns:1fr 48px;gap:10px;align-items:center}.range-row output{text-align:right;font-variant-numeric:tabular-nums}.swatches{display:flex;gap:9px;margin:11px 0}.swatch{width:34px;height:34px;padding:0;border:2px solid #60636f;border-radius:50%}.preview{width:100%;margin-top:10px}.status{min-height:22px;margin:12px 0 0;font-size:13px;color:var(--muted)}.fine{font-size:12px;color:#858790;margin-top:20px}.ad{display:block;margin-top:26px;padding:18px 2px;border-top:1px solid var(--line);border-bottom:1px solid var(--line);color:var(--text);font-size:18px;font-weight:800;text-decoration:none}.ad span{display:block;margin-bottom:3px;color:var(--gold);font-size:11px;text-transform:uppercase}.ad:hover,.ad:focus{color:var(--gold)}@media(max-width:440px){h1{font-size:29px}.row{grid-template-columns:1fr}.secondary{width:100%}}
</style></head><body><header><b>AIPI-Screen</b><span class="credit">Free by @GGDM</span></header><main>
<div class="eyebrow">Device setup</div><h1>Wi-Fi and status light.</h1><p>Configure the AIPI from a phone. Settings stay in local device storage.</p>
<form method="post" action="/save">
<fieldset><legend>Wi-Fi</legend><label for="networks">Nearby networks</label><div class="row"><select id="networks"><option value="">Scanning...</option></select><button class="secondary" type="button" id="scan">Scan</button></div>
<label for="ssid">Network name</label><input id="ssid" name="ssid" maxlength="32" autocomplete="off" required><p class="hint">You can type a hidden network name.</p>
<label for="password">Password</label><div class="password"><input id="password" name="password" type="password" maxlength="64" autocomplete="new-password"><button type="button" id="show" aria-label="Show password">Show</button></div>
<label for="host_mode">Setup hotspot</label><select id="host_mode" name="host_mode"><option value="setup">Setup only (recommended)</option><option value="always">Always on (development)</option></select><p class="hint">Setup only turns off the hotspot after Wi-Fi connects and restores it if connection repeatedly fails.</p></fieldset>
<fieldset><legend>Status LED</legend><label for="led_color">Color</label><div class="color-controls"><input id="led_color" name="led_color" type="color" value="#c9a84c"><div><div class="swatches"><button class="swatch" type="button" data-color="#e53935" style="background:#e53935" aria-label="Red"></button><button class="swatch" type="button" data-color="#43a047" style="background:#43a047" aria-label="Green"></button><button class="swatch" type="button" data-color="#1e88e5" style="background:#1e88e5" aria-label="Blue"></button><button class="swatch" type="button" data-color="#c9a84c" style="background:#c9a84c" aria-label="Gold"></button><button class="swatch" type="button" data-color="#ffffff" style="background:#fff" aria-label="White"></button></div><p class="hint">Live preview uses the onboard RGB LED.</p></div></div>
<label for="brightness">Brightness</label><div class="range-row"><input id="brightness" name="brightness" type="range" min="0" max="40" value="15"><output id="brightness_value">15%</output></div><button class="secondary preview" type="button" id="preview">Preview color</button></fieldset>
<button class="save" type="submit">Save settings and connect</button><div class="status" id="status" role="status"></div></form>
<p class="fine">Setup network: AIPI-Screen-XXXX &middot; Password: GGDM-XXXX &middot; Portal: http://1.2.3.4</p><p class="fine">Free firmware by @GGDM. No subscription, bridge, or cloud account required.</p><a class="ad" href="https://krystalize.ai"><span>Local-first AI</span>Get your own Free Local Harness and LLM from Krystalize.AI</a></main>
<script>
const list=document.querySelector('#networks'),ssid=document.querySelector('#ssid'),status=document.querySelector('#status'),scan=document.querySelector('#scan'),pass=document.querySelector('#password'),show=document.querySelector('#show'),color=document.querySelector('#led_color'),brightness=document.querySelector('#brightness'),level=document.querySelector('#brightness_value'),hostMode=document.querySelector('#host_mode');
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function networks(){scan.disabled=true;status.textContent='Scanning nearby networks...';try{const r=await fetch('/api/networks',{cache:'no-store'});if(!r.ok)throw Error();const data=await r.json();list.innerHTML='<option value="">Choose a network or type one below</option>'+data.map(n=>'<option value="'+esc(n.ssid)+'">'+esc(n.ssid)+' ('+n.rssi+' dBm'+(n.secure?', secured':' open')+')</option>').join('');status.textContent=data.length?data.length+' networks found.':'No networks found. Type the network name below.'}catch(e){status.textContent='Scan failed. Type the network name or retry.'}finally{scan.disabled=false}}
async function loadSettings(){try{const r=await fetch('/api/settings',{cache:'no-store'}),s=await r.json();ssid.value=s.ssid||'';color.value=s.led_color;brightness.value=s.brightness;level.value=s.brightness+'%';hostMode.value=s.host_mode}catch(e){status.textContent='Could not load saved settings.'}}
async function preview(){status.textContent='Updating LED...';const body=new URLSearchParams({led_color:color.value,brightness:brightness.value});try{const r=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw Error();status.textContent='LED preview updated.'}catch(e){status.textContent='LED preview failed.'}}
list.addEventListener('change',()=>{if(list.value)ssid.value=list.value});scan.addEventListener('click',networks);show.addEventListener('click',()=>{const visible=pass.type==='text';pass.type=visible?'password':'text';show.textContent=visible?'Show':'Hide';show.setAttribute('aria-label',visible?'Show password':'Hide password')});brightness.addEventListener('input',()=>level.value=brightness.value+'%');document.querySelector('#preview').addEventListener('click',preview);document.querySelectorAll('.swatch').forEach(b=>b.addEventListener('click',()=>{color.value=b.dataset.color;preview()}));loadSettings();networks();
</script></body></html>)HTML";

DeviceSettings active_settings;
esp_netif_t* ap_netif = nullptr;
int reconnect_attempts = 0;

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(const char* input, char* output, size_t capacity) {
    size_t written = 0;
    while (*input != '\0' && written + 1 < capacity) {
        const int high = *input == '%' && input[1] != '\0' ? hex_value(input[1]) : -1;
        const int low = high >= 0 && input[2] != '\0' ? hex_value(input[2]) : -1;
        if (high >= 0 && low >= 0) {
            output[written++] = static_cast<char>((high << 4) | low);
            input += 3;
        } else {
            output[written++] = *input == '+' ? ' ' : *input;
            ++input;
        }
    }
    output[written] = '\0';
}

bool form_value(const char* body, const char* name, char* output, size_t capacity) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s=", name);
    const char* start = body;
    while ((start = strstr(start, prefix)) != nullptr) {
        if (start == body || start[-1] == '&') break;
        ++start;
    }
    if (start == nullptr) return false;
    start += strlen(prefix);
    const char* end = strchr(start, '&');
    size_t length = end == nullptr ? strlen(start) : static_cast<size_t>(end - start);
    char encoded[256];
    length = std::min(length, sizeof(encoded) - 1);
    memcpy(encoded, start, length);
    encoded[length] = '\0';
    url_decode(encoded, output, capacity);
    return true;
}

bool read_body(httpd_req_t* req, char* body, size_t capacity) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) >= capacity) return false;
    int total = 0;
    while (total < req->content_len) {
        const int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) return false;
        total += received;
    }
    body[total] = '\0';
    return true;
}

bool parse_led(const char* color, const char* brightness, DeviceSettings* settings) {
    if (strlen(color) != 7 || color[0] != '#') return false;
    int digits[6];
    for (int i = 0; i < 6; ++i) {
        digits[i] = hex_value(color[i + 1]);
        if (digits[i] < 0) return false;
    }
    char* end = nullptr;
    const long percent = strtol(brightness, &end, 10);
    if (end == brightness || *end != '\0' || percent < 0 || percent > 40) return false;
    settings->red = static_cast<uint8_t>((digits[0] << 4) | digits[1]);
    settings->green = static_cast<uint8_t>((digits[2] << 4) | digits[3]);
    settings->blue = static_cast<uint8_t>((digits[4] << 4) | digits[5]);
    settings->brightness = static_cast<uint8_t>((percent * 255 + 50) / 100);
    return true;
}

esp_err_t save_settings(const DeviceSettings& settings) {
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "open settings");
    esp_err_t err = nvs_set_str(handle, "wifi_ssid", settings.ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pass", settings.password);
    if (err == ESP_OK) err = nvs_set_u8(handle, "led_r", settings.red);
    if (err == ESP_OK) err = nvs_set_u8(handle, "led_g", settings.green);
    if (err == ESP_OK) err = nvs_set_u8(handle, "led_b", settings.blue);
    if (err == ESP_OK) err = nvs_set_u8(handle, "led_level", settings.brightness);
    if (err == ESP_OK) err = nvs_set_u8(handle, "host_mode", static_cast<uint8_t>(settings.host_mode));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

DeviceSettings load_settings() {
    DeviceSettings settings;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    size_t ssid_size = sizeof(settings.ssid);
    size_t password_size = sizeof(settings.password);
    nvs_get_str(handle, "wifi_ssid", settings.ssid, &ssid_size);
    nvs_get_str(handle, "wifi_pass", settings.password, &password_size);
    nvs_get_u8(handle, "led_r", &settings.red);
    nvs_get_u8(handle, "led_g", &settings.green);
    nvs_get_u8(handle, "led_b", &settings.blue);
    nvs_get_u8(handle, "led_level", &settings.brightness);
    uint8_t host_mode = static_cast<uint8_t>(settings.host_mode);
    nvs_get_u8(handle, "host_mode", &host_mode);
    settings.host_mode = host_mode == static_cast<uint8_t>(HostMode::kAlwaysOn)
        ? HostMode::kAlwaysOn : HostMode::kSetupOnly;
    nvs_close(handle);
    return settings;
}

esp_err_t apply_station(const DeviceSettings& settings) {
    if (settings.ssid[0] == '\0') return ESP_OK;
    wifi_config_t config = {};
    strlcpy(reinterpret_cast<char*>(config.sta.ssid), settings.ssid, sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(config.sta.password), settings.password, sizeof(config.sta.password));
    config.sta.threshold.authmode = settings.password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), kTag, "set station config");
    return esp_wifi_connect();
}

esp_err_t portal_get(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPortalHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t networks_get(httpd_req_t* req) {
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), kTag, "get scan count");
    count = std::min<uint16_t>(count, kMaxNetworks);
    std::vector<wifi_ap_record_t> records(count);
    if (count != 0) ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records.data()), kTag, "get scan records");
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) { return left.rssi > right.rssi; });

    cJSON* root = cJSON_CreateArray();
    std::vector<std::string> seen;
    for (const auto& record : records) {
        const char* ssid = reinterpret_cast<const char*>(record.ssid);
        if (ssid[0] == '\0' || std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
        seen.emplace_back(ssid);
        cJSON* network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", ssid);
        cJSON_AddNumberToObject(network, "rssi", record.rssi);
        cJSON_AddBoolToObject(network, "secure", record.authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(root, network);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t response = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return response;
}

esp_err_t settings_get(httpd_req_t* req) {
    char color[8];
    snprintf(color, sizeof(color), "#%02x%02x%02x", active_settings.red, active_settings.green, active_settings.blue);
    const int brightness = (active_settings.brightness * 100 + 127) / 255;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", active_settings.ssid);
    cJSON_AddStringToObject(root, "led_color", color);
    cJSON_AddNumberToObject(root, "brightness", brightness);
    cJSON_AddStringToObject(root, "host_mode", active_settings.host_mode == HostMode::kAlwaysOn ? "always" : "setup");
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t response = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return response;
}

esp_err_t led_post(httpd_req_t* req) {
    char body[256] = {};
    char color[16] = {};
    char brightness[8] = {};
    DeviceSettings preview = active_settings;
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "led_color", color, sizeof(color)) ||
        !form_value(body, "brightness", brightness, sizeof(brightness)) || !parse_led(color, brightness, &preview)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid LED settings");
    }
    ESP_RETURN_ON_ERROR(status_led_set_rgb(preview.red, preview.green, preview.blue, preview.brightness), kTag, "preview LED");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t save_post(httpd_req_t* req) {
    char body[768] = {};
    char color[16] = {};
    char brightness[8] = {};
    char host_mode[16] = {};
    DeviceSettings settings;
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "ssid", settings.ssid, sizeof(settings.ssid)) ||
        settings.ssid[0] == '\0' || !form_value(body, "password", settings.password, sizeof(settings.password)) ||
        !form_value(body, "led_color", color, sizeof(color)) ||
        !form_value(body, "brightness", brightness, sizeof(brightness)) ||
        !form_value(body, "host_mode", host_mode, sizeof(host_mode)) ||
        !parse_led(color, brightness, &settings)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
    }
    settings.host_mode = strcmp(host_mode, "always") == 0 ? HostMode::kAlwaysOn : HostMode::kSetupOnly;
    ESP_RETURN_ON_ERROR(save_settings(settings), kTag, "save settings");
    active_settings = settings;
    ESP_RETURN_ON_ERROR(status_led_set_rgb(settings.red, settings.green, settings.blue, settings.brightness), kTag, "set LED");
    reconnect_attempts = 0;
    esp_wifi_disconnect();
    const esp_err_t connect_result = apply_station(settings);
    memset(body, 0, sizeof(body));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char* result = connect_result == ESP_OK
        ? "<html><meta name=viewport content='width=device-width'><body style='background:#0f0f1a;color:#f7f4ed;font:18px system-ui;padding:40px'><h1>Settings saved.</h1><p>The AIPI is connecting. In Setup only mode, this hotspot will close after the connection succeeds.</p><p><a style='color:#dfc06c' href='/'>Back to setup</a></p></body></html>"
        : "<html><meta name=viewport content='width=device-width'><body style='background:#0f0f1a;color:#f7f4ed;font:18px system-ui;padding:40px'><h1>Settings saved.</h1><p>The connection could not start. Return to setup and verify the password.</p><p><a style='color:#dfc06c' href='/'>Back to setup</a></p></body></html>";
    return httpd_resp_sendstr(req, result);
}

void disable_setup_ap_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1800));
    if (active_settings.host_mode == HostMode::kSetupOnly) {
        ESP_LOGI(kTag, "station connected; disabling setup hotspot");
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    vTaskDelete(nullptr);
}

void wifi_event(void*, esp_event_base_t base, int32_t event_id, void*) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && active_settings.ssid[0] != '\0') {
        if (reconnect_attempts++ < 5) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(kTag, "station connection failed; restoring setup hotspot");
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            reconnect_attempts = 0;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        reconnect_attempts = 0;
        if (active_settings.host_mode == HostMode::kSetupOnly) {
            xTaskCreate(disable_setup_ap_task, "disable_setup_ap", 2048, nullptr, 4, nullptr);
        }
    }
}

void dns_task(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ESP_LOGE(kTag, "could not start captive DNS");
        vTaskDelete(nullptr);
        return;
    }
    uint8_t query[512];
    while (true) {
        sockaddr_in client = {};
        socklen_t client_length = sizeof(client);
        const int length = recvfrom(sock, query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&client), &client_length);
        if (length < 12 || length + 16 > static_cast<int>(sizeof(query))) continue;
        query[2] = 0x81;
        query[3] = 0x80;
        query[6] = 0;
        query[7] = 1;
        int output = length;
        query[output++] = 0xc0;
        query[output++] = 0x0c;
        query[output++] = 0;
        query[output++] = 1;
        query[output++] = 0;
        query[output++] = 1;
        query[output++] = 0;
        query[output++] = 0;
        query[output++] = 0;
        query[output++] = 30;
        query[output++] = 0;
        query[output++] = 4;
        query[output++] = 1;
        query[output++] = 2;
        query[output++] = 3;
        query[output++] = 4;
        sendto(sock, query, output, 0, reinterpret_cast<sockaddr*>(&client), client_length);
    }
}

esp_err_t start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;
    httpd_handle_t server = nullptr;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), kTag, "start HTTP server");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = portal_get, .user_ctx = nullptr},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_get, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get, .user_ctx = nullptr},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_post, .user_ctx = nullptr},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = nullptr},
        {.uri = "/*", .method = HTTP_GET, .handler = portal_get, .user_ctx = nullptr},
    };
    for (const auto& route : routes) ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &route), kTag, "register route");
    return ESP_OK;
}

}  // namespace

esp_err_t wifi_portal_start() {
    active_settings = load_settings();
    ESP_RETURN_ON_ERROR(status_led_set_rgb(active_settings.red, active_settings.green, active_settings.blue,
                                           active_settings.brightness), kTag, "restore LED settings");
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "init network interfaces");
    esp_err_t event_loop_result = esp_event_loop_create_default();
    if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) return event_loop_result;
    ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr), kTag, "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr), kTag, "register IP events");

    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag, "read MAC");
    char ap_ssid[33];
    char ap_password[65];
    snprintf(ap_ssid, sizeof(ap_ssid), "AIPI-Screen-%02X%02X", mac[4], mac[5]);
    snprintf(ap_password, sizeof(ap_password), "GGDM-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {};
    strlcpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy(reinterpret_cast<char*>(ap_config.ap.password), ap_password, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "set AP+station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "configure setup AP");

    esp_netif_ip_info_t ip_info = {};
    inet_pton(AF_INET, kPortalIp, &ip_info.ip);
    inet_pton(AF_INET, kPortalIp, &ip_info.gw);
    inet_pton(AF_INET, "255.255.255.0", &ip_info.netmask);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), kTag, "stop DHCP server");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), kTag, "set portal address");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), kTag, "start DHCP server");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "disable Wi-Fi power saving");
    ESP_RETURN_ON_ERROR(start_http_server(), kTag, "start portal");
    xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 4, nullptr);

    ESP_LOGI(kTag, "setup hotspot %s password %s; portal http://%s", ap_ssid, ap_password, kPortalIp);
    return apply_station(active_settings);
}
