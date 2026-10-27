#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
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

/* Wi-Fi Configuration */
#define CONFIG_WIFI_SSID       "iPhoneSayparn"
#define CONFIG_WIFI_PASSWORD   "0641474746"
#define MAXIMUM_RETRY          5

/* GPIO and ADC Configuration */
#define LED_GPIO_PIN           GPIO_NUM_2
#define POT_ADC_CHANNEL        ADC_CHANNEL_6  // GPIO34, ADC1 Channel 6

/* Wi-Fi Event Bits */
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static httpd_handle_t s_http_server = NULL;


/* =========================================================
   Wi-Fi Event Handler
   ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi started. Connecting to access point...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG,
                 "Wi-Fi disconnected. Reason code: %d",
                 event->reason);

        if (s_retry_num < MAXIMUM_RETRY) {

            s_retry_num++;

            ESP_LOGI(TAG,
                     "Retrying Wi-Fi connection (%d/%d)...",
                     s_retry_num,
                     MAXIMUM_RETRY);

            esp_wifi_connect();

        } else {

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAIL_BIT
            );

            ESP_LOGE(TAG, "Failed to connect to Wi-Fi.");
        }

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG,
                 "Wi-Fi connected! IP Address: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        s_retry_num = 0;

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}


/* =========================================================
   Initialize mDNS
   ========================================================= */

static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());

    ESP_ERROR_CHECK(
        mdns_hostname_set("esp32-node")
    );

    ESP_ERROR_CHECK(
        mdns_instance_name_set("ESP32 RESTful Controller")
    );

    mdns_txt_item_t serviceTxtData[] = {
        {"board", "esp32"},
        {"role", "actuator"}
    };

    ESP_ERROR_CHECK(
        mdns_service_add(
            "ESP32-WebControl",
            "_http",
            "_tcp",
            80,
            serviceTxtData,
            2
        )
    );

    ESP_LOGI(TAG,
             "mDNS initialized: http://esp32-node.local");
}


/* =========================================================
   Initialize Wi-Fi Station
   ========================================================= */

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group.");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() != NULL
                    ? ESP_OK
                    : ESP_FAIL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &instance_any_id
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &instance_got_ip
        )
    );

    wifi_config_t wifi_config = {0};

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /*
     * Copy SSID and password into Wi-Fi configuration.
     * The actual password must be set above.
     */
    strlcpy(
        (char *)wifi_config.sta.ssid,
        CONFIG_WIFI_SSID,
        sizeof(wifi_config.sta.ssid)
    );

    strlcpy(
        (char *)wifi_config.sta.password,
        CONFIG_WIFI_PASSWORD,
        sizeof(wifi_config.sta.password)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config)
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG,
             "Connecting to SSID: %s",
             CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {

        ESP_LOGI(TAG, "Connected to access point successfully!");

        return true;

    } else if (bits & WIFI_FAIL_BIT) {

        ESP_LOGE(TAG, "Failed to connect to access point.");

        return false;
    }

    ESP_LOGE(TAG, "Unexpected Wi-Fi connection state.");

    return false;
}


/* =========================================================
   GET /api/status
   Read potentiometer, free heap and LED state
   ========================================================= */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    int pot_val = 0;

    if (s_adc1_handle != NULL) {

        esp_err_t adc_result = adc_oneshot_read(
            s_adc1_handle,
            POT_ADC_CHANNEL,
            &pot_val
        );

        if (adc_result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read potentiometer.");
        }

    } else {
        ESP_LOGW(TAG, "ADC is not initialized.");
    }

    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "pot_raw", pot_val);

    cJSON_AddNumberToObject(
        root,
        "free_heap",
        esp_get_free_heap_size()
    );

    cJSON_AddBoolToObject(
        root,
        "led",
        gpio_get_level(LED_GPIO_PIN) != 0
    );

    char *resp = cJSON_PrintUnformatted(root);

    if (resp == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t result = httpd_resp_send(
        req,
        resp,
        HTTPD_RESP_USE_STRLEN
    );

    cJSON_free(resp);
    cJSON_Delete(root);

    return result;
}


/* =========================================================
   POST /api/led
   Control LED using JSON {"state": true/false}
   ========================================================= */

static esp_err_t led_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 128) {

        httpd_resp_set_status(req, "400 Bad Request");

        httpd_resp_set_type(req, "application/json");

        httpd_resp_sendstr(
            req,
            "{\"result\":\"error\",\"message\":\"Invalid body size\"}"
        );

        return ESP_OK;
    }

    char buf[128];

    int total_received = 0;

    while (total_received < req->content_len) {

        int ret = httpd_req_recv(
            req,
            buf + total_received,
            req->content_len - total_received
        );

        if (ret <= 0) {

            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }

            httpd_resp_send_500(req);

            return ESP_FAIL;
        }

        total_received += ret;
    }

    buf[total_received] = '\0';

    cJSON *root = cJSON_Parse(buf);

    if (root == NULL) {

        httpd_resp_set_status(req, "400 Bad Request");

        httpd_resp_set_type(req, "application/json");

        httpd_resp_sendstr(
            req,
            "{\"result\":\"error\",\"message\":\"Invalid JSON\"}"
        );

        return ESP_OK;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(
        root,
        "state"
    );

    if (!cJSON_IsBool(state)) {

        cJSON_Delete(root);

        httpd_resp_set_status(req, "400 Bad Request");

        httpd_resp_set_type(req, "application/json");

        httpd_resp_sendstr(
            req,
            "{\"result\":\"error\",\"message\":\"state must be boolean\"}"
        );

        return ESP_OK;
    }

    bool led_on = cJSON_IsTrue(state);

    esp_err_t gpio_result = gpio_set_level(
        LED_GPIO_PIN,
        led_on ? 1 : 0
    );

    cJSON_Delete(root);

    if (gpio_result != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "LED (GPIO %d) switched to: %s",
        (int)LED_GPIO_PIN,
        led_on ? "ON" : "OFF"
    );

    httpd_resp_set_type(req, "application/json");

    httpd_resp_sendstr(
        req,
        "{\"result\":\"success\"}"
    );

    return ESP_OK;
}


/* =========================================================
   Start HTTP Server
   ========================================================= */

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

        ESP_ERROR_CHECK(
            httpd_register_uri_handler(
                server,
                &uri_get_status
            )
        );

        ESP_ERROR_CHECK(
            httpd_register_uri_handler(
                server,
                &uri_post_led
            )
        );

        ESP_LOGI(
            TAG,
            "HTTP Server started on port %d",
            config.server_port
        );

        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server.");

    return NULL;
}


/* =========================================================
   Application Main
   ========================================================= */

void app_main(void)
{
    /* 1. Initialize NVS */

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);


    /* 2. Initialize TCP/IP and Event Loop */

    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    /* 3. Initialize mDNS */

    initialise_mdns();


    /* 4. Configure LED GPIO */

    ESP_ERROR_CHECK(
        gpio_reset_pin(LED_GPIO_PIN)
    );

    ESP_ERROR_CHECK(
        gpio_set_direction(
            LED_GPIO_PIN,
            GPIO_MODE_OUTPUT
        )
    );

    ESP_ERROR_CHECK(
        gpio_set_level(LED_GPIO_PIN, 0)
    );


    /* 5. Initialize ADC1 for potentiometer on GPIO34 */

    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };

    ret = adc_oneshot_new_unit(
        &init_config1,
        &s_adc1_handle
    );

    if (ret == ESP_OK) {

        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };

        ret = adc_oneshot_config_channel(
            s_adc1_handle,
            POT_ADC_CHANNEL,
            &chan_config
        );

        if (ret == ESP_OK) {
            ESP_LOGI(
                TAG,
                "ADC initialized on GPIO34 (ADC1 Channel 6)."
            );
        } else {
            ESP_LOGE(TAG, "Failed to configure ADC channel.");
        }

    } else {

        ESP_LOGE(TAG, "Failed to initialize ADC.");
    }


    /* 6. Connect to Wi-Fi */

    if (wifi_init_sta()) {

        /* 7. Start HTTP Server */

        s_http_server = start_webserver();

        if (s_http_server != NULL) {

            ESP_LOGI(
                TAG,
                "Ready! Test GET /api/status and POST /api/led."
            );

            ESP_LOGI(
                TAG,
                "Hostname: http://esp32-node.local"
            );

        } else {

            ESP_LOGE(
                TAG,
                "Wi-Fi connected, but HTTP server failed to start."
            );
        }

    } else {

        ESP_LOGE(
            TAG,
            "Cannot start server due to Wi-Fi connection failure."
        );
    }
}
