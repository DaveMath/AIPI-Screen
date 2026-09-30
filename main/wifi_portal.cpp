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

namespace {

constexpr char kTag[] = "wifi_portal";
constexpr char kNamespace[] = "aipi_screen";
constexpr char kSsidKey[] = "wifi_ssid";
constexpr char kPasswordKey[] = "wifi_pass";
constexpr char kPortalIp[] = "1.2.3.4";
constexpr size_t kMaxNetworks = 24;

constexpr char kPortalHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AIPI-Screen Wi-Fi Setup</title><style>
:root{color-scheme:dark;--ink:#0f0f1a;--panel:#1d1e29;--line:#3a3d4a;--gold:#c9a84c;--paprika:#d66231;--text:#f7f4ed;--muted:#b6b5bb}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--ink);color:var(--text);font:16px/1.45 system-ui,-apple-system,sans-serif}
header{padding:20px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}header b{font-size:19px}.credit{color:var(--gold);font-size:12px;font-weight:800;text-transform:uppercase}
main{width:min(560px,calc(100% - 28px));margin:30px auto 50px}h1{font-size:36px;line-height:1.05;margin:8px 0 12px;letter-spacing:0}.eyebrow{color:var(--paprika);font-size:12px;font-weight:800;text-transform:uppercase}p{color:var(--muted)}
form{margin-top:24px;padding:22px;background:var(--panel);border:1px solid var(--line);border-radius:8px}label{display:block;margin:17px 0 7px;font-weight:700}.row{display:grid;grid-template-columns:1fr auto;gap:8px}.password{position:relative}.password button{position:absolute;right:6px;top:6px;width:auto;margin:0;padding:8px 11px;background:#30323e;color:var(--text)}
select,input{width:100%;min-width:0;padding:13px;border:1px solid #505361;border-radius:5px;background:#11121a;color:var(--text);font:inherit}select:focus,input:focus{outline:2px solid var(--gold);border-color:transparent}button{border:0;border-radius:5px;padding:12px 15px;font:inherit;font-weight:800;cursor:pointer}.scan{background:#30323e;color:var(--text)}.save{width:100%;margin-top:23px;background:linear-gradient(110deg,var(--gold),var(--paprika));color:#111}
.status{min-height:22px;margin:12px 0 0;font-size:13px;color:var(--muted)}.fine{font-size:12px;color:#858790;margin-top:22px}@media(max-width:420px){h1{font-size:31px}.row{grid-template-columns:1fr}.scan{width:100%}}
</style></head><body><header><b>AIPI-Screen</b><span class="credit">by @GGDM</span></header><main>
<div class="eyebrow">Phone setup</div><h1>Connect your AIPI to Wi-Fi.</h1><p>Select a nearby network, enter its password, and save. Credentials stay in the device's local NVS storage.</p>
<form method="post" action="/save"><label for="ssid">Wi-Fi network</label><div class="row"><select id="ssid" name="ssid" required><option value="">Scanning...</option></select><button class="scan" type="button" id="scan">Scan</button></div>
<label for="password">Wi-Fi password</label><div class="password"><input id="password" name="password" type="password" maxlength="64" autocomplete="new-password"><button type="button" id="show" aria-label="Show password">Show</button></div>
<button class="save" type="submit">Save and connect</button><div class="status" id="status" role="status"></div></form>
<p class="fine">Setup network: AIPI-Screen-XXXX &middot; Portal: http://1.2.3.4 &middot; Firmware by @GGDM</p></main>
<script>
const list=document.querySelector('#ssid'),status=document.querySelector('#status'),scan=document.querySelector('#scan'),pass=document.querySelector('#password'),show=document.querySelector('#show');
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function networks(){scan.disabled=true;status.textContent='Scanning nearby networks...';try{const r=await fetch('/api/networks',{cache:'no-store'});if(!r.ok)throw Error();const data=await r.json();list.innerHTML='<option value="">Choose a network</option>'+data.map(n=>'<option value="'+esc(n.ssid)+'">'+esc(n.ssid)+' ('+n.rssi+' dBm'+(n.secure?', secured':' open')+')</option>').join('');status.textContent=data.length?data.length+' networks found.':'No networks found. Try again.'}catch(e){status.textContent='Scan failed. Tap Scan to retry.'}finally{scan.disabled=false}}
scan.addEventListener('click',networks);show.addEventListener('click',()=>{const visible=pass.type==='text';pass.type=visible?'password':'text';show.textContent=visible?'Show':'Hide';show.setAttribute('aria-label',visible?'Show password':'Hide password')});networks();
</script></body></html>)HTML";

struct StoredWifi {
    char ssid[33];
    char password[65];
};

esp_netif_t* ap_netif = nullptr;

char hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void url_decode(const char* input, char* output, size_t capacity) {
    size_t written = 0;
    while (*input != '\0' && written + 1 < capacity) {
        if (*input == '%' && isxdigit(static_cast<unsigned char>(input[1])) &&
            isxdigit(static_cast<unsigned char>(input[2]))) {
            output[written++] = static_cast<char>((hex_value(input[1]) << 4) | hex_value(input[2]));
            input += 3;
        } else {
            output[written++] = *input == '+' ? ' ' : *input;
            ++input;
        }
    }
    output[written] = '\0';
}

bool form_value(const char* body, const char* name, char* output, size_t capacity) {
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s=", name);
    const char* start = strstr(body, prefix);
    if (start == nullptr || (start != body && start[-1] != '&')) return false;
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

esp_err_t save_wifi(const StoredWifi& settings) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, kSsidKey, settings.ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, kPasswordKey, settings.password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

StoredWifi load_wifi() {
    StoredWifi settings = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    size_t ssid_size = sizeof(settings.ssid);
    size_t password_size = sizeof(settings.password);
    nvs_get_str(handle, kSsidKey, settings.ssid, &ssid_size);
    nvs_get_str(handle, kPasswordKey, settings.password, &password_size);
    nvs_close(handle);
    return settings;
}

esp_err_t apply_station(const StoredWifi& settings) {
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
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");

    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), kTag, "get scan count");
    count = std::min<uint16_t>(count, kMaxNetworks);
    std::vector<wifi_ap_record_t> records(count);
    if (count != 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records.data()), kTag, "get scan records");
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.rssi > right.rssi;
    });

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

esp_err_t save_post(httpd_req_t* req) {
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char body[513] = {};
    int total = 0;
    while (total < req->content_len) {
        const int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) return ESP_FAIL;
        total += received;
    }

    StoredWifi settings = {};
    if (!form_value(body, "ssid", settings.ssid, sizeof(settings.ssid)) || settings.ssid[0] == '\0' ||
        !form_value(body, "password", settings.password, sizeof(settings.password))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "network name required");
    }
    ESP_RETURN_ON_ERROR(save_wifi(settings), kTag, "save Wi-Fi settings");
    esp_wifi_disconnect();
    const esp_err_t connect_result = apply_station(settings);
    memset(body, 0, sizeof(body));
    memset(&settings, 0, sizeof(settings));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char* result = connect_result == ESP_OK
        ? "<html><meta name=viewport content='width=device-width'><body style='background:#0f0f1a;color:#f7f4ed;font:18px system-ui;padding:40px'><h1>Saved.</h1><p>The AIPI is connecting. You can close this page.</p><p><a style='color:#dfc06c' href='/'>Back to setup</a></p></body></html>"
        : "<html><meta name=viewport content='width=device-width'><body style='background:#0f0f1a;color:#f7f4ed;font:18px system-ui;padding:40px'><h1>Saved.</h1><p>The connection could not start. Return to setup and verify the password.</p><p><a style='color:#dfc06c' href='/'>Back to setup</a></p></body></html>";
    return httpd_resp_sendstr(req, result);
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
    uint8_t packet[512];
    while (true) {
        sockaddr_in source = {};
        socklen_t source_length = sizeof(source);
        const int length = recvfrom(sock, packet, sizeof(packet), 0,
                                    reinterpret_cast<sockaddr*>(&source), &source_length);
        if (length < 12) continue;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        int question_end = 12;
        while (question_end < length && packet[question_end] != 0) {
            question_end += packet[question_end] + 1;
        }
        question_end += 5;
        if (question_end + 16 > static_cast<int>(sizeof(packet))) continue;
        const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 1, 2, 3, 4};
        memcpy(packet + question_end, answer, sizeof(answer));
        sendto(sock, packet, question_end + sizeof(answer), 0,
               reinterpret_cast<sockaddr*>(&source), source_length);
    }
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(kTag, "station disconnected; setup remains available at %s", kPortalIp);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "station connected: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t register_uri(httpd_handle_t server, const char* uri, httpd_method_t method,
                       esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    return httpd_register_uri_handler(server, &route);
}

}  // namespace

esp_err_t wifi_portal_start() {
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "init netif");
    esp_err_t event_result = esp_event_loop_create_default();
    if (event_result != ESP_OK && event_result != ESP_ERR_INVALID_STATE) return event_result;
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info = {};
    inet_pton(AF_INET, kPortalIp, &ip_info.ip);
    inet_pton(AF_INET, kPortalIp, &ip_info.gw);
    inet_pton(AF_INET, "255.255.255.0", &ip_info.netmask);
    esp_netif_dhcps_stop(ap_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), kTag, "set portal IP");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), kTag, "start DHCP");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    wifi_event_handler, nullptr), kTag, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    wifi_event_handler, nullptr), kTag, "IP events");

    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag, "read AP MAC");
    char portal_ssid[33];
    snprintf(portal_ssid, sizeof(portal_ssid), "AIPI-Screen-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap_config = {};
    strlcpy(reinterpret_cast<char*>(ap_config.ap.ssid), portal_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(portal_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "set AP+station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "configure AP");

    const StoredWifi saved = load_wifi();
    if (saved.ssid[0] != '\0') {
        wifi_config_t sta_config = {};
        strlcpy(reinterpret_cast<char*>(sta_config.sta.ssid), saved.ssid, sizeof(sta_config.sta.ssid));
        strlcpy(reinterpret_cast<char*>(sta_config.sta.password), saved.password, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = saved.password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        sta_config.sta.pmf_cfg.capable = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), kTag, "restore station config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    if (saved.ssid[0] != '\0') esp_wifi_connect();

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    http_config.max_uri_handlers = 8;
    httpd_handle_t server = nullptr;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &http_config), kTag, "start setup web server");
    ESP_RETURN_ON_ERROR(register_uri(server, "/", HTTP_GET, portal_get), kTag, "root route");
    ESP_RETURN_ON_ERROR(register_uri(server, "/api/networks", HTTP_GET, networks_get), kTag, "scan route");
    ESP_RETURN_ON_ERROR(register_uri(server, "/save", HTTP_POST, save_post), kTag, "save route");
    ESP_RETURN_ON_ERROR(register_uri(server, "/*", HTTP_GET, portal_get), kTag, "captive route");
    xTaskCreate(dns_task, "captive_dns", 4096, nullptr, 4, nullptr);

    ESP_LOGI(kTag, "connect a phone to %s and open http://%s", portal_ssid, kPortalIp);
    return ESP_OK;
}
