#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "esp_adc/adc_oneshot.h"

#define TAG "HTTP_REST_LAB"

// กำหนดชื่อและรหัสผ่าน Wi-Fi (แก้ไขให้ตรงกับ Access Point ของตนเอง)
#define CONFIG_WIFI_SSID      "IS 4G Hi-Speed Home WiFi_769475"
#define CONFIG_WIFI_PASSWORD  "50769475"
#define MAXIMUM_RETRY         5

#define LED_GPIO_PIN          GPIO_NUM_2
#define POT_ADC_CHANNEL       ADC_CHANNEL_6 // GPIO 34 (ADC1 Channel 6)

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static int s_retry_num = 0;
static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static httpd_handle_t s_http_server = NULL;

// Wi-Fi Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// กำหนดค่าเริ่มต้นให้กับ mDNS
static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp32-node"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 RESTful Controller"));

    mdns_txt_item_t serviceTxtData[] = {
        {"board", "esp32"},
        {"role", "actuator"}
    };
    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebControl", "_http", "_tcp", 80, serviceTxtData, 2));
    ESP_LOGI(TAG, "mDNS initialized! Hostname: http://esp32-node.local");
}

// เริ่มต้นระบบเชื่อมต่อ Wi-Fi Station
static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    size_t ssid_len = strlen(CONFIG_WIFI_SSID);
    if (ssid_len > sizeof(wifi_config.sta.ssid)) ssid_len = sizeof(wifi_config.sta.ssid);
    memcpy(wifi_config.sta.ssid, CONFIG_WIFI_SSID, ssid_len);

    size_t pass_len = strlen(CONFIG_WIFI_PASSWORD);
    if (pass_len > sizeof(wifi_config.sta.password)) pass_len = sizeof(wifi_config.sta.password);
    memcpy(wifi_config.sta.password, CONFIG_WIFI_PASSWORD, pass_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s ...", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP successfully!");
        if (sta_netif) {
            mdns_netif_action(sta_netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
        }
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to connect to AP");
        return false;
    }
}

// 1. GET /api/status - อ่านค่าเซนเซอร์และสถานะระบบ
static esp_err_t status_get_handler(httpd_req_t *req)
{
    int pot_val = 0;
    if (s_adc1_handle != NULL) {
        adc_oneshot_read(s_adc1_handle, POT_ADC_CHANNEL, &pot_val);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "pot_raw", pot_val);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(root, "led", gpio_get_level(LED_GPIO_PIN));

    const char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    cJSON_free((void *)resp);
    cJSON_Delete(root);
    return ESP_OK;
}

// 2. POST /api/led - ควบคุมหลอดไฟ LED
static esp_err_t led_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root != NULL) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsBool(state)) {
            bool led_on = cJSON_IsTrue(state);
            gpio_set_level(LED_GPIO_PIN, led_on ? 1 : 0);
            ESP_LOGI(TAG, "LED (GPIO %d) switched to: %s", LED_GPIO_PIN, led_on ? "ON" : "OFF");
        }
        cJSON_Delete(root);
    }

    const char *resp = "{\"result\":\"success\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// เริ่มต้น HTTP Web Server และลงทะเบียน URI Handlers
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_uri_t uri_get_status = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = status_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t uri_post_led = {
        .uri      = "/api/led",
        .method   = HTTP_POST,
        .handler  = led_post_handler,
        .user_ctx = NULL
    };

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_get_status);
        httpd_register_uri_handler(server, &uri_post_led);
        ESP_LOGI(TAG, "HTTP Server started on port %d", config.server_port);
        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server!");
    return NULL;
}

void app_main(void)
{
    // 1. กำหนดค่าเริ่มต้น NVS Flash (จำเป็นสำหรับ Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. กำหนดค่า TCP/IP Network Interface และ Default Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. เริ่มต้น mDNS Service Discovery (ทำก่อน Wi-Fi เชื่อมต่อเพื่อดักจับ IP Event)
    initialise_mdns();

    // 4. กำหนดค่าขา GPIO 2 เป็น Output สำหรับควบคุม LED
    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_INPUT_OUTPUT);

    // 5. กำหนดค่า ADC1 สำหรับ Potentiometer (GPIO 34)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&init_config1, &s_adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc1_handle, POT_ADC_CHANNEL, &chan_config);
        ESP_LOGI(TAG, "ADC Initialized on GPIO 34 (Channel 6)");
    }

    // 6. เชื่อมต่อระบบ Wi-Fi
    if (wifi_init_sta()) {
        // 7. เริ่มต้น HTTP RESTful Web Server
        s_http_server = start_webserver();
        ESP_LOGI(TAG, "Ready! Test with: curl.exe http://esp32-node.local/api/status");
    } else {
        ESP_LOGE(TAG, "Cannot start server due to Wi-Fi connection failure.");
    }
}
