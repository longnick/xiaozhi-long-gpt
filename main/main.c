#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "gc9a01.h"

#define TAG "LONGGPT"

#ifndef CONFIG_XIAOZHI_WIFI_SSID
#define CONFIG_XIAOZHI_WIFI_SSID "YourWiFi"
#endif
#ifndef CONFIG_XIAOZHI_WIFI_PASS
#define CONFIG_XIAOZHI_WIFI_PASS "YourPassword"
#endif
#ifndef CONFIG_XIAOZHI_OPENAI_KEY
#define CONFIG_XIAOZHI_OPENAI_KEY "sk-xxxxx"
#endif
#ifndef CONFIG_XIAOZHI_OPENAI_MODEL
#define CONFIG_XIAOZHI_OPENAI_MODEL "gpt-4o-mini"
#endif
#ifndef CONFIG_XIAOZHI_OWM_KEY
#define CONFIG_XIAOZHI_OWM_KEY ""
#endif
#ifndef CONFIG_XIAOZHI_CITY
#define CONFIG_XIAOZHI_CITY "Bao Loc"
#endif
#ifndef CONFIG_XIAOZHI_COUNTRY
#define CONFIG_XIAOZHI_COUNTRY "VN"
#endif
#ifndef CONFIG_XIAOZHI_LCD_SCK
#define CONFIG_XIAOZHI_LCD_SCK 12
#endif
#ifndef CONFIG_XIAOZHI_LCD_MOSI
#define CONFIG_XIAOZHI_LCD_MOSI 11
#endif
#ifndef CONFIG_XIAOZHI_LCD_DC
#define CONFIG_XIAOZHI_LCD_DC 9
#endif
#ifndef CONFIG_XIAOZHI_LCD_CS
#define CONFIG_XIAOZHI_LCD_CS 10
#endif
#ifndef CONFIG_XIAOZHI_LCD_RST
#define CONFIG_XIAOZHI_LCD_RST 8
#endif
#ifndef CONFIG_XIAOZHI_LCD_BL
#define CONFIG_XIAOZHI_LCD_BL 14
#endif

#define WIFI_SSID        CONFIG_XIAOZHI_WIFI_SSID
#define WIFI_PASS        CONFIG_XIAOZHI_WIFI_PASS
#define OPENAI_API_KEY   CONFIG_XIAOZHI_OPENAI_KEY
#define OPENAI_MODEL     CONFIG_XIAOZHI_OPENAI_MODEL
#define OWM_API_KEY      CONFIG_XIAOZHI_OWM_KEY
#define CITY             CONFIG_XIAOZHI_CITY
#define COUNTRY          CONFIG_XIAOZHI_COUNTRY

#define PIN_SCK          CONFIG_XIAOZHI_LCD_SCK
#define PIN_MOSI         CONFIG_XIAOZHI_LCD_MOSI
#define PIN_DC           CONFIG_XIAOZHI_LCD_DC
#define PIN_CS           CONFIG_XIAOZHI_LCD_CS
#define PIN_RST          CONFIG_XIAOZHI_LCD_RST
#define PIN_BL           CONFIG_XIAOZHI_LCD_BL

#define LCD_W 240
#define LCD_H 240


typedef struct { char role[12]; char content[384]; } turn_t;
#define MAX_TURNS 10
static turn_t history[MAX_TURNS*2];
static int turn_count = 0;
static SemaphoreHandle_t hist_mux;

static void push_turn(const char *role, const char *content){
    xSemaphoreTake(hist_mux, portMAX_DELAY);
    if (turn_count >= MAX_TURNS*2){ memmove(&history[0], &history[2], sizeof(turn_t)*(MAX_TURNS*2-2)); turn_count -= 2; }
    snprintf(history[turn_count].role, sizeof(history[turn_count].role), "%s", role);
    snprintf(history[turn_count].content, sizeof(history[turn_count].content), "%s", content);
    turn_count++;
    xSemaphoreGive(hist_mux);
}


static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    snprintf((char*)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char*)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi connecting to %s...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
}


static void time_sync_notification_cb(struct timeval *tv){ ESP_LOGI(TAG, "Time synced"); }
static void sntp_start(void)
{
    setenv("TZ", "ICT-7", 1); tzset(); // GMT+7

esp_sntp_config_t sntp_cfg = ESP_SNTP_CONFIG_DEFAULT();
sntp_cfg.server_from_dhcp = false;
sntp_cfg.start = true;
sntp_cfg.smooth_sync = false;
sntp_cfg.sync_cb = time_sync_notification_cb;

// đặt server thủ công (vd: pool.ntp.org)
esp_sntp_setservername(0, "pool.ntp.org");

esp_sntp_init(&sntp_cfg);
}


static esp_err_t http_get_to_buf(const char *url, char *buf, size_t buflen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(cli); return err; }
    int total = 0, r;
    while ((r = esp_http_client_read(cli, buf+total, buflen-1-total)) > 0) total += r;
    buf[total] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ESP_OK;
}

static esp_err_t http_post_json_to_buf(const char *url, const char *auth_bearer, const char *json, char *buf, size_t buflen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (auth_bearer) esp_http_client_set_header(cli, "Authorization", auth_bearer);
    esp_http_client_set_post_field(cli, json, strlen(json));

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) { esp_http_client_cleanup(cli); return err; }

    int status = esp_http_client_get_status_code(cli);
    if (status != 200){ ESP_LOGE(TAG, "HTTP %d", status); }

    int total = 0, r; esp_http_client_set_method(cli, HTTP_METHOD_GET);
    esp_http_client_open(cli, 0);
    while ((r = esp_http_client_read(cli, buf+total, buflen-1-total)) > 0) total += r;
    buf[total] = 0;

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return (status==200)?ESP_OK:ESP_FAIL;
}

// Weather (OpenWeatherMap current weather)
static bool fetch_weather(char *out_temp, size_t out_len, char *out_city, size_t city_len)
{
    if (strlen(OWM_API_KEY)==0){ snprintf(out_temp,out_len,"--°C"); snprintf(out_city,city_len,"%s", CITY); return true; }
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/weather?q=%s,%s&units=metric&lang=vi&appid=%s",
        CITY, COUNTRY, OWM_API_KEY);
    static char buf[4096];
    if (http_get_to_buf(url, buf, sizeof(buf)) != ESP_OK) return false;
    // Very small parse: look for "temp": and "name":""
    char *t = strstr(buf, "\"temp\":");
    char *n = strstr(buf, "\"name\":\"");
    if (t){ float temp = atof(t+8); snprintf(out_temp,out_len, "%d°C", (int)(temp+0.5f)); }
    else { snprintf(out_temp,out_len, "--°C"); }
    if (n){ n+=8; char *e=strchr(n,'"'); size_t L=(e&&e-n<city_len-1)?(size_t)(e-n):city_len-1; strncpy(out_city,n,L); out_city[L]='\0'; }
    else { snprintf(out_city,city_len, "%s", CITY); }
    return true;
}

// ChatGPT (chat/completions)
static bool openai_chat(const char *user_text, char *out_reply, size_t out_len)
{
    static char body[12288];
    static char resp[16384];

    // Build minimal JSON with last 10 turns
    // NOTE: To keep code small, we assemble JSON manually.
    strcpy(body, "{\"model\":\""); strcat(body, OPENAI_MODEL);
    strcat(body, "\",\"messages\":[");
    // system
    strcat(body, "{\"role\":\"system\",\"content\":\"Bạn là trợ lý tiếng Việt tên LONG GPT. Trả lời ngắn gọn, rõ ràng.\"}");
    xSemaphoreTake(hist_mux, portMAX_DELAY);
    int start = (turn_count>MAX_TURNS*2)? (turn_count - MAX_TURNS*2) : 0;
    for (int i=start;i<turn_count;i++){
        strcat(body, ",{");
        strcat(body, "\"role\":\""); strcat(body, history[i].role);
        strcat(body, "\",\"content\":\"");
        // escape quotes/newlines minimal
        for (const char *p=history[i].content; *p; ++p){ if (*p=='"'||*p=='\\') { size_t l=strlen(body); body[l]='\\'; body[l+1]=*p; body[l+2]='\0'; } else if (*p=='\n'){ strcat(body, " "); } else { size_t l=strlen(body); body[l]=*p; body[l+1]='\0'; } }
        strcat(body, "\"}");
    }
    xSemaphoreGive(hist_mux);
    // current user
    strcat(body, ",{\"role\":\"user\",\"content\":\"");
    for (const char *p=user_text; *p; ++p){ if (*p=='"'||*p=='\\') { size_t l=strlen(body); body[l]='\\'; body[l+1]=*p; body[l+2]='\0'; } else if (*p=='\n'){ strcat(body, " "); } else { size_t l=strlen(body); body[l]=*p; body[l+1]='\0'; } }
    strcat(body, "\"}]");
    strcat(body, ",\"temperature\":0.7}");

    char auth[256]; snprintf(auth, sizeof(auth), "Bearer %s", OPENAI_API_KEY);
    if (http_post_json_to_buf("https://api.openai.com/v1/chat/completions", auth, body, resp, sizeof(resp)) != ESP_OK)
        return false;
    // Find reply: "choices":[{"message":{"content":"..."}}
    char *p = strstr(resp, "\"message\":");
    if (!p) p = strstr(resp, "\"content\":\"");
    if (!p){ snprintf(out_reply,out_len,"(không có phản hồi)"); return true; }
    p = strstr(p, "\"content\":\""); if (!p){ snprintf(out_reply,out_len,"(không có phản hồi)"); return true; }
    p += 11; char *e = strchr(p, '"'); if (!e){ snprintf(out_reply,out_len,"(không có phản hồi)"); return true; }
    size_t L = (size_t)(e-p); if (L>out_len-1) L=out_len-1; strncpy(out_reply, p, L); out_reply[L]='\0';

    push_turn("user", user_text);
    push_turn("assistant", out_reply);
    return true;
}

// UI helpers
static void draw_idle(const char *temp, const char *city)
{
    gc9a01_fill(rgb565(0,0,0));
    time_t now=time(NULL); struct tm ti; localtime_r(&now,&ti);
    char line[64];
    // Clock HH:MM
    snprintf(line,sizeof(line),"%02d:%02d", ti.tm_hour, ti.tm_min);
    draw_text6x8(80, 80, line, rgb565(255,255,255));
    // Date
    snprintf(line,sizeof(line),"%02d-%02d-%04d", ti.tm_mday, ti.tm_mon+1, ti.tm_year+1900);
    draw_text6x8(70, 100, line, rgb565(200,200,200));
    // Weather badge
    char w[64]; snprintf(w,sizeof(w),"%s  %s", city, temp);
    draw_text6x8(40, 130, w, rgb565(180,220,255));
    // Hint
    draw_text6x8(20, 210, "Nói: Hey LONG GPT ...", rgb565(180,180,180));
}

static void draw_chat(const char *user, const char *bot)
{
    gc9a01_fill(rgb565(0,0,0));
    draw_text6x8(10, 10, "LONG GPT", rgb565(255,255,255));
    draw_text6x8(10, 30, "Bạn:", rgb565(160,220,255));
    draw_text6x8(10, 42, user, rgb565(255,255,255));
    draw_text6x8(10, 70, "Bot:", rgb565(160,255,160));
    draw_text6x8(10, 82, bot,  rgb565(255,255,255));
}

// Serial input line buffer
static void serial_task(void *arg)
{
    char line[512]; int len=0;
    while (1){
        int ch = fgetc(stdin);
        if (ch==EOF){ vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (ch=='\r') continue;
        if (ch=='\n'){
            line[len]='\0'; len=0;
            if (strlen(line)>0){
                // wake word strip
                const char *wake = "hey long gpt";
                char low[512]; for (int i=0; line[i] && i<sizeof(low)-1; ++i){ low[i]= (char)tolower((unsigned char)line[i]); low[i+1]='\0'; }
                if (strncmp(low, wake, strlen(wake))==0){
                    const char *q = line + strlen(line) - strlen(low) + strlen(wake);
                    while (*q==' '||*q=='\t') q++;
                    snprintf(line, sizeof(line), "%s", (*q)?q:"(wake)");
                }
                char reply[2048];
                draw_chat(line, "Đang hỏi ChatGPT...");
                if (openai_chat(line, reply, sizeof(reply))) draw_chat(line, reply);
                else draw_chat(line, "Lỗi kết nối OpenAI");
            }
        } else if (len < (int)sizeof(line)-1) line[len++]=(char)ch;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_start();
    sntp_start();

    // LCD
    ESP_ERROR_CHECK(gc9a01_init(PIN_SCK, PIN_MOSI, PIN_DC, PIN_CS, PIN_RST, PIN_BL));

    // Create mutex and serial task
    hist_mux = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(serial_task, "serial_task", 4096, NULL, 5, NULL, 0);

    char temp[16] = "--°C"; char city[32] = CITY;
    int counter=0;
    while (1){
        if ((counter++ % 10) == 0) fetch_weather(temp,sizeof(temp), city,sizeof(city));
        draw_idle(temp, city);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

