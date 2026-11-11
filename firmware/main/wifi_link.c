// WiFi mode: join the stored network, find the receiver by UDP broadcast, then
// exchange input and feedback packets with it. With no stored network the pad
// starts an access point and serves a setup page instead.
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ks_reports.h"
#include "ks_wifi_proto.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define DISCOVER_PERIOD_US 500000
#define LINK_TIMEOUT_US 2000000
#define DEFAULT_RATE_HZ 250

static char sta_ssid[33], sta_pass[65];
static volatile bool got_ip;
static uint32_t ip_addr, ip_broadcast; // network byte order
static link_status_t status = {.link = KS_LINK_SEARCHING};
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_status(uint8_t link, const char *host, const char *detail)
{
    link_status_t st = {.link = link};
    snprintf(st.host, sizeof st.host, "%s", host);
    snprintf(st.detail, sizeof st.detail, "%s", detail);
    taskENTER_CRITICAL(&status_lock);
    status = st;
    taskEXIT_CRITICAL(&status_lock);
}

void wifi_link_status(link_status_t *st)
{
    taskENTER_CRITICAL(&status_lock);
    *st = status;
    taskEXIT_CRITICAL(&status_lock);
}

static bool creds_load(void)
{
    nvs_handle_t h;
    size_t ssid_len = sizeof sta_ssid, pass_len = sizeof sta_pass;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "ssid", sta_ssid, &ssid_len) == ESP_OK && sta_ssid[0] != '\0';
    if (nvs_get_str(h, "pass", sta_pass, &pass_len) != ESP_OK) {
        sta_pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

static bool creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void wifi_link_forget(void) { storage_erase_namespace("wifi"); }

// Setup page.

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Copies the URL-decoded value of key from a form body. Returns false when
// the key is missing or the value does not fit.
static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t key_len = strlen(key), n = 0;
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            for (p += key_len + 1; *p && *p != '&'; p++) {
                char c = *p;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
                    c = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
                    p += 2;
                }
                if (n + 1 >= cap) {
                    return false;
                }
                out[n++] = c;
            }
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the reply reach the browser
    esp_restart();
}

static esp_err_t root_get(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html lang=en><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Kestrel WiFi setup</title><style>"
        "body{font-family:system-ui,sans-serif;max-width:22rem;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin-top:1rem}"
        "input,button{font-size:1rem;width:100%;padding:.6rem;margin-top:.3rem;box-sizing:border-box}"
        "button{margin-top:1.5rem}</style></head><body><h1>Kestrel WiFi setup</h1>"
        "<form method=post action=/save>"
        "<label for=s>Network name</label><input id=s name=ssid maxlength=32 required>"
        "<label for=p>Password</label><input id=p name=pass type=password maxlength=64>"
        "<button>Save and restart</button></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[400], ssid[33], pass[65] = "";
    int total = (int)req->content_len, got = 0;
    if (total <= 0 || total >= (int)sizeof body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The form is too large.");
    }
    while (got < total) {
        int n = httpd_req_recv(req, body + got, (size_t)(total - got));
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';
    if (!form_value(body, "ssid", ssid, sizeof ssid) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter the network name.");
    }
    bool has_pass = form_value(body, "pass", pass, sizeof pass);
    if (strstr(body, "pass=") && !has_pass) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The password is too long.");
    }
    if (pass[0] != '\0' && strlen(pass) < 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "A WiFi password has at least 8 characters.");
    }
    if (!creds_save(ssid, pass)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "The controller could not save the network.");
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved. The controller restarts and joins the network.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static void start_setup(void)
{
    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    static const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    char ssid[16], pass[12], detail[40];
    uint8_t mac[6] = {0};
    wifi_config_t ap = {0};
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    // The setup network gets a fresh password on every start, shown on the
    // pad's screen, so the home network's password does not cross an open link.
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, sizeof ssid, "Kestrel-%02X%02X", mac[4], mac[5]);
    snprintf(pass, sizeof pass, "%08lu", (unsigned long)(esp_random() % 100000000UL));
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", ssid);
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", pass);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (httpd_start(&server, &http) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
    } else {
        ESP_LOGE(TAG, "setup page did not start");
    }
    snprintf(detail, sizeof detail, "%.12s pw %.8s 192.168.4.1", ssid, pass);
    set_status(KS_LINK_SETUP, "", detail);
    ESP_LOGI(TAG, "setup network %s", ssid);
}

// Station mode and the UDP link.

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        got_ip = false;
        esp_wifi_connect(); // keep trying; the menu can forget the network
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ip_addr = e->ip_info.ip.addr;
        ip_broadcast = (e->ip_info.ip.addr & e->ip_info.netmask.addr) | ~e->ip_info.netmask.addr;
        got_ip = true;
    }
}

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), yes = 1;
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0) {
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    if (bind(sock, (struct sockaddr *)&local, sizeof local) != 0 ||
        fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void send_packet(int sock, const ks_wp_packet_t *pkt, uint32_t addr, uint16_t port)
{
    uint8_t buf[KS_WP_MAX_LEN];
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = port, .sin_addr.s_addr = addr};
    size_t len = ks_wp_encode(pkt, buf, sizeof buf);
    if (len) {
        sendto(sock, buf, len, 0, (struct sockaddr *)&to, sizeof to);
    }
}

static void link_task(void *arg)
{
    int sock = -1, period_ms = 1000 / DEFAULT_RATE_HZ;
    bool linked = false;
    struct sockaddr_in peer = {0};
    uint16_t seq = 0, session = 0;
    int64_t last_rx = 0, last_discover = 0;
    uint8_t buf[64], mac[6] = {0};
    char text[40];
    ks_wp_packet_t pkt;

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (;;) {
        if (!got_ip) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            if (linked) {
                linked = false;
                app_clear_host_state();
            }
            snprintf(text, sizeof text, "Joining %.31s", sta_ssid);
            set_status(KS_LINK_SEARCHING, "", text);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (sock < 0 && (sock = open_socket()) < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        int64_t now = esp_timer_get_time();

        for (;;) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof from;
            int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);
            if (n <= 0) {
                break;
            }
            if (!ks_wp_decode(buf, (size_t)n, &pkt)) {
                continue;
            }
            if (!linked && pkt.type == KS_WP_OFFER) {
                int rate = pkt.u.offer.rate_hz;
                rate = rate < 50 ? 50 : rate > 1000 ? 1000 : rate;
                period_ms = 1000 / rate;
                peer = from;
                session = pkt.session;
                linked = true;
                last_rx = now;
                snprintf(text, sizeof text, "%s   %d Hz", inet_ntoa(from.sin_addr), rate);
                set_status(KS_LINK_CONNECTED, pkt.u.offer.name, text);
            } else if (linked && from.sin_addr.s_addr == peer.sin_addr.s_addr &&
                       from.sin_port == peer.sin_port && pkt.session == session) {
                last_rx = now;
                if (pkt.type == KS_WP_FEEDBACK) {
                    const ks_wp_feedback_t *f = &pkt.u.feedback;
                    app_set_rumble(f->rumble);
                    if (f->fx_valid & KS_FX_TARGET_LEFT) {
                        app_set_trigger_fx(KS_FX_TARGET_LEFT, &f->fx[0]);
                    }
                    if (f->fx_valid & KS_FX_TARGET_RIGHT) {
                        app_set_trigger_fx(KS_FX_TARGET_RIGHT, &f->fx[1]);
                    }
                } else if (pkt.type == KS_WP_BYE) {
                    last_rx = now - LINK_TIMEOUT_US - 1; // drop the link below
                }
            }
        }

        if (linked && now - last_rx > LINK_TIMEOUT_US) {
            linked = false;
            app_clear_host_state();
        }
        memset(&pkt, 0, sizeof pkt);
        pkt.seq = seq++;
        if (!linked) {
            if (now - last_discover >= DISCOVER_PERIOD_US) {
                ks_gamepad_t pad;
                last_discover = now;
                pkt.type = KS_WP_DISCOVER;
                memcpy(pkt.u.discover.device_id, mac, 6);
                pkt.u.discover.fw_version = KS_FW_VERSION;
                app_get_report(&pad, &pkt.u.discover.battery_pct, &pkt.u.discover.flags);
                snprintf(pkt.u.discover.name, sizeof pkt.u.discover.name, "Kestrel");
                send_packet(sock, &pkt, ip_broadcast, htons(KS_WP_PORT));
                send_packet(sock, &pkt, htonl(INADDR_BROADCAST), htons(KS_WP_PORT));
                snprintf(text, sizeof text, "Looking for the receiver");
                set_status(KS_LINK_SEARCHING, "", text);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        pkt.type = KS_WP_INPUT;
        pkt.session = session;
        app_get_report(&pkt.u.input.pad, &pkt.u.input.battery_pct, &pkt.u.input.flags);
        pkt.u.input.time_ms = (uint32_t)(now / 1000);
        send_packet(sock, &pkt, peer.sin_addr.s_addr, peer.sin_port);
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

void wifi_link_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta = {0};

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // credentials live in our own NVS keys
    if (!creds_load()) {
        start_setup();
        return;
    }
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    // These fields need no terminator when full: a network name can use all
    // 32 bytes.
    memcpy(sta.sta.ssid, sta_ssid, strnlen(sta_ssid, sizeof sta.sta.ssid));
    memcpy(sta.sta.password, sta_pass, strnlen(sta_pass, sizeof sta.sta.password));
    sta.sta.threshold.authmode = sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); // modem sleep would add tens of milliseconds
    xTaskCreatePinnedToCore(link_task, "wifi_link", 4096, NULL, 9, NULL, 1);
    ESP_LOGI(TAG, "joining %s", sta_ssid);
}
