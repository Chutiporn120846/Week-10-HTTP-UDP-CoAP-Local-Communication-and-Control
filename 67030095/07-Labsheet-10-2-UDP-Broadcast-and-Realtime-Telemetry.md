# ใบงานการทดลองที่ 10.2
### การสื่อสารความหน่วงต่ำด้วย UDP Socket และการถ่ายทอดข้อมูล Real-time Telemetry

> [!NOTE] **คำชี้แจง**
> ในใบงานนี้ นักศึกษาจะได้เรียนรู้การเขียนโปรแกรมเครือข่ายระดับล่างด้วย **BSD Socket API** บนสแตก LwIP ของ ESP32 เพื่อสร้างบริการส่งข้อมูลเซนเซอร์แบบต่อเนื่องความถี่สูง (Real-time Telemetry Streaming) และการควบคุมอุปกรณ์ด้วยแพ็กเก็ต UDP ที่มีขนาด Header เล็กและตอบสนองได้รวดเร็วระดับมิลลิวินาที

---

## 1. วัตถุประสงค์การทดลอง
1. สามารถเขียนโปรแกรม Socket แบบ Connectionless (UDP Datagram) ด้วยคำสั่ง `socket()`, `bind()`, `recvfrom()`, และ `sendto()` บน ESP-IDF ได้
2. สามารถพัฒนา FreeRTOS Task เพื่อส่งข้อมูลแอนะล็อกเซนเซอร์แบบบรอดแคสต์ (UDP Broadcast) สู่เครือข่ายได้ด้วยความถี่ 10-50 Hz
3. สามารถพัฒนาสคริปต์ภาษา Python บนเครื่องคอมพิวเตอร์เพื่อดักฟังข้อมูลบรอดแคสต์ และส่งคำสั่งควบคุม LED กลับมายัง ESP32 ได้
4. สามารถวัดค่าความหน่วงเวลาเฉลี่ย (Round-Trip Latency) และอัตราการสูญหายของแพ็กเก็ต (Packet Loss Rate) ได้

---

## 2. โครงสร้างระบบและการทำงาน

```
   [ESP32 Node]                                              [PC Client / Python]
         |                                                             |
         | --- (UDP Broadcast: pot_raw, seq_no) : Port 3334 ---------> | (รับค่าแสดงผลกราฟ)
         |                                                             |
         | <--- (UDP Unicast Command: "LED_ON" / "LED_OFF") : Port 3333| (ส่งคำสั่งควบคุม)
         | --- (UDP Unicast ACK: "STATUS:OK") ------------------------>| (วัด RTT Latency)
```

---

## 3. ขั้นตอนการทดลอง

### กิจกรรมที่ 10-2.1  การสร้างโปรเจกต์ใหม่และตั้งค่าโครงสร้าง

#### 1. สร้างโปรเจกต์ใหม่
```powershell
idf.py create-project Lab10-2_UDP_Telemetry_Socket
cd Lab10-2_UDP_Telemetry_Socket
```

**หรือรันผ่าน Docker**
```powershell
docker run --rm --mount "type=bind,source=$((Get-Location).Path),target=/workspace" -w /workspace espressif/idf:release-v6.1 idf.py create-project Lab10-2_UDP_Telemetry_Socket
cd Lab10-2_UDP_Telemetry_Socket
```

#### 2. กำหนด Target เป็นชิป ESP32
```powershell
idf.py set-target esp32
```

**หรือรันผ่าน Docker**
```powershell
docker run --rm --mount "type=bind,source=$((Get-Location).Path),target=/workspace" -w /workspace espressif/idf:release-v6.1 idf.py set-target esp32
```

#### 3. ตั้งค่า `main/CMakeLists.txt`
เปิดไฟล์ `main/CMakeLists.txt` และระบุคอมโพเนนต์ที่ต้องใช้งาน

```cmake
idf_component_register(SRCS "Lab10-2_UDP_Telemetry_Socket.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event nvs_flash lwip esp_adc esp_driver_gpio)
```

#### 4. ทดสอบ Reconfigure ระบบบิลด์
ทดสอบรันคำสั่ง Reconfigure เพื่อให้ระบบดาวน์โหลดคอมโพเนนต์และสร้างบิลด์ไฟล์
```powershell
idf.py reconfigure
```

**หรือรันผ่าน Docker**
```powershell
docker run --rm --mount "type=bind,source=$((Get-Location).Path),target=/workspace" -w /workspace espressif/idf:release-v6.1 idf.py reconfigure
```
เมื่อปรากฏข้อความ `-- Configuring done` และ `-- Generating done` แสดงว่าโครงสร้างโปรเจกต์พร้อมสำหรับการเขียนโค้ดในกิจกรรมถัดไป

---

### กิจกรรมที่ 10-2.2 พัฒนา Task รับคำสั่ง UDP Control Server (Port 3333)
ฟังก์ชันนี้ทำหน้าที่เปิด UDP Socket ผูกเข้ากับพอร์ต 3333 (`INADDR_ANY`) เพื่อคอยรับคำสั่งควบคุม LED แบบ Unicast และตอบรับกลับ (ACK) เพื่อให้ไคลเอนต์นำไปคำนวณ Round-Trip Time (RTT)

```c
#include "lwip/sockets.h"

#define UDP_CONTROL_PORT 3333

void udp_control_server_task(void *pvParameters)
{
    char rx_buffer[128];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE("UDP_SERVER", "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_CONTROL_PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE("UDP_SERVER", "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI("UDP_SERVER", "Control Server listening on UDP Port %d...", UDP_CONTROL_PORT);

    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 0) {
            rx_buffer[len] = '\0';
            ESP_LOGI("UDP_SERVER", "Received command: %s", rx_buffer);

            if (strcmp(rx_buffer, "LED_ON") == 0) {
                gpio_set_level(GPIO_NUM_2, 1);
                sendto(sock, "ACK:LED_ON", 10, 0, (struct sockaddr *)&client_addr, client_addr_len);
            } else if (strcmp(rx_buffer, "LED_OFF") == 0) {
                gpio_set_level(GPIO_NUM_2, 0);
                sendto(sock, "ACK:LED_OFF", 11, 0, (struct sockaddr *)&client_addr, client_addr_len);
            } else {
                sendto(sock, "ACK:UNKNOWN", 11, 0, (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
    }
    close(sock);
    vTaskDelete(NULL);
}
```

---

### กิจกรรมที่ 10-2.3 พัฒนา Task ส่งข้อมูล Telemetry แบบ Broadcast (Port 3334)
ฟังก์ชันนี้ทำหน้าที่อ่านค่าแอนะล็อกเซนเซอร์ Potentiometer จาก ADC1 (GPIO 34) แล้วแพ็กข้อมูลร่วมกับหมายเลขลำดับ (Sequence Number) ส่งบรอดแคสต์ไปยัง `255.255.255.255` พอร์ต 3334 ทุกๆ 100 ms (10 Hz)

```c
#define UDP_BROADCAST_PORT 3334

void udp_telemetry_broadcast_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_BROADCAST_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE("UDP_BCAST", "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // เปิดใช้งานตัวเลือก SO_BROADCAST เพื่อให้อนุญาตส่งแพ็กเก็ตบรอดแคสต์
    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    uint32_t seq_no = 0;
    char tx_buffer[64];

    ESP_LOGI("UDP_BCAST", "Starting Telemetry Streaming to port %d (10 Hz)...", UDP_BROADCAST_PORT);

    while (1) {
        int pot_val = 0;
        if (s_adc1_handle != NULL) {
            adc_oneshot_read(s_adc1_handle, ADC_CHANNEL_6, &pot_val);
        }

        snprintf(tx_buffer, sizeof(tx_buffer), "SEQ:%lu,POT:%d\n", (unsigned long)seq_no++, pot_val);

        int err = sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE("UDP_BCAST", "Error occurred during sendto: errno %d", errno);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // หน่วงเวลา 100ms (10 Hz)
    }
    close(sock);
    vTaskDelete(NULL);
}
```

---

### กิจกรรมที่ 10-2.4 การเชื่อมโยงระบบ Wi-Fi และฟังก์ชัน `app_main()`

ในกิจกรรมนี้ จะเป็นการประกอบระบบทั้งหมดเข้าด้วยกัน โดยมีขั้นตอนสำคัญใน `app_main()` ดังนี้
1. เริ่มต้นระบบหน่วยความจำแฟลช **NVS (Non-Volatile Storage)** ซึ่งจำเป็นสำหรับโมดูล Wi-Fi Driver
2. เริ่มต้น **LwIP TCP/IP Stack** และ **Default Event Loop**
3. กำหนดค่าฮาร์ดแวร์ **GPIO 2 (LED)** เป็นโหมด Input/Output และ **ADC1 Channel 6 (GPIO 34)** สำหรับอ่านค่า Potentiometer
4. เชื่อมต่อเครือข่าย Wi-Fi ในโหมด **Station (STA)** ไปยัง Access Point
5. เมื่อเชื่อมต่อ Wi-Fi สำเร็จ ให้สร้าง FreeRTOS Tasks สำหรับรัน **`udp_control_server_task`** (รับคำสั่งพอร์ต 3333) และ **`udp_telemetry_broadcast_task`** (บรอดแคสต์ข้อมูลเซนเซอร์พอร์ต 3334 อัตรา 10 Hz)

#### 1. ฟังก์ชันเชื่อมต่อ Wi-Fi Station (`wifi_init_sta`)
```c
static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

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

    ESP_LOGI(TAG, "Connecting to AP: %s...", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
```

#### 2. ฟังก์ชันหลัก `app_main(void)`
```c
void app_main(void)
{
    // 1. Initialise NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Netif & Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Setup Hardware (LED & ADC)
    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_INPUT_OUTPUT);

    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_config1, &s_adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc1_handle, POT_ADC_CHANNEL, &chan_config);
        ESP_LOGI(TAG, "ADC Initialized on GPIO 34");
    }

    // 4. Connect Wi-Fi
    if (wifi_init_sta()) {
        // 5. Create FreeRTOS Tasks for UDP
        xTaskCreate(udp_control_server_task, "udp_ctrl_task", 4096, NULL, 5, NULL);
        xTaskCreate(udp_telemetry_broadcast_task, "udp_telemetry_task", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "Ready! Test UDP Telemetry with: python udp_listener.py");
    } else {
        ESP_LOGE(TAG, "Wi-Fi connection failed.");
    }
}
```

### ตารางสรุป Header Files และหน้าที่การทำงาน

| Header file                       | หน้าที่และขอบเขตการใช้งานในแล็บนี้                                                      |
| :-------------------------------- | :-------------------------------------------------------------------------------------- |
| `stdio.h` / `string.h`            | จัดการ Input/Output และฟังก์ชันจัดรูปแบบสตริง (`snprintf()`, `strlen()`, `memcpy()`)    |
| `esp_log.h`                       | ส่งข้อความแจ้งสถานะและตรวจแก้ข้อผิดพลาด (`ESP_LOGI()`, `ESP_LOGE()`)                    |
| `nvs_flash.h`                     | จัดการ Non-Volatile Storage สำหรับระบบ Wi-Fi                                            |
| `esp_netif.h` / `esp_event.h`     | จัดการ Network Interface Adapter และ Event Loop                                         |
| `esp_wifi.h`                      | จัดการการเชื่อมต่อวิทยุ Wi-Fi Station                                                   |
| `freertos/FreeRTOS.h` / `task.h`  | โครงสร้างระบบ FreeRTOS สำหรับสร้าง Task แบบมัลติทาสก์กิ้ง (`xTaskCreate()`)             |
| `lwip/sockets.h` / `lwip/netdb.h` | BSD Socket API บน LwIP (`socket()`, `bind()`, `sendto()`, `recvfrom()`, `setsockopt()`) |
| `driver/gpio.h`                   | ควบคุมระดับสัญญาณดิจิทัลเปิด-ปิดหลอดไฟ LED (GPIO 2)                                     |
| `esp_adc/adc_oneshot.h`           | อ่านค่าแรงดันแอนะล็อกจาก Potentiometer (GPIO 34 / ADC1 Channel 6)                       |

<details>
<summary><b>🔍 คลิกดูซอร์สโค้ดฉบับสมบูรณ์ทั้งไฟล์ (Lab10-2_UDP_Telemetry_Socket.c)</b></summary>

```c
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
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define TAG "UDP_LAB"

// กำหนดชื่อและรหัสผ่าน Wi-Fi
#define CONFIG_WIFI_SSID      "YOUR_WIFI_SSID"
#define CONFIG_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define MAXIMUM_RETRY         5

#define LED_GPIO_PIN          GPIO_NUM_2
#define POT_ADC_CHANNEL       ADC_CHANNEL_6 // GPIO 34 (ADC1 Channel 6)

#define UDP_CONTROL_PORT      3333
#define UDP_BROADCAST_PORT    3334

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static int s_retry_num = 0;
static adc_oneshot_unit_handle_t s_adc1_handle = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

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

    ESP_LOGI(TAG, "Connecting to AP: %s...", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// Task 1: UDP Control Server (Port 3333)
void udp_control_server_task(void *pvParameters)
{
    char rx_buffer[128];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_CONTROL_PORT);

    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    ESP_LOGI(TAG, "Control Server listening on UDP Port %d...", UDP_CONTROL_PORT);

    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 0) {
            rx_buffer[len] = '\0';
            if (strcmp(rx_buffer, "LED_ON") == 0) {
                gpio_set_level(LED_GPIO_PIN, 1);
                sendto(sock, "ACK:LED_ON", 10, 0, (struct sockaddr *)&client_addr, client_addr_len);
            } else if (strcmp(rx_buffer, "LED_OFF") == 0) {
                gpio_set_level(LED_GPIO_PIN, 0);
                sendto(sock, "ACK:LED_OFF", 11, 0, (struct sockaddr *)&client_addr, client_addr_len);
            } else {
                sendto(sock, "ACK:UNKNOWN", 11, 0, (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
    }
}

// Task 2: UDP Telemetry Broadcast (Port 3334)
void udp_telemetry_broadcast_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_BROADCAST_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    uint32_t seq_no = 0;
    char tx_buffer[64];

    ESP_LOGI(TAG, "Broadcasting Telemetry to Port %d (10 Hz)...", UDP_BROADCAST_PORT);

    while (1) {
        int pot_val = 0;
        if (s_adc1_handle != NULL) {
            adc_oneshot_read(s_adc1_handle, POT_ADC_CHANNEL, &pot_val);
        }

        snprintf(tx_buffer, sizeof(tx_buffer), "SEQ:%lu,POT:%d\n", (unsigned long)seq_no++, pot_val);
        sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz
    }
}

void app_main(void)
{
    // 1. Initialise NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Netif & Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Setup Hardware (LED & ADC)
    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_INPUT_OUTPUT);

    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_config1, &s_adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc1_handle, POT_ADC_CHANNEL, &chan_config);
        ESP_LOGI(TAG, "ADC Initialized on GPIO 34");
    }

    // 4. Connect Wi-Fi
    if (wifi_init_sta()) {
        // 5. Create FreeRTOS Tasks for UDP
        xTaskCreate(udp_control_server_task, "udp_ctrl_task", 4096, NULL, 5, NULL);
        xTaskCreate(udp_telemetry_broadcast_task, "udp_bcast_task", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "All UDP Tasks started!");
    } else {
        ESP_LOGE(TAG, "Wi-Fi connection failed.");
    }
}
```
</details>

#### คำสั่ง Build และ Flash โปรเจกต์
```powershell
# คอมไพล์โปรเจกต์ผ่าน Docker
docker run --rm --mount "type=bind,source=$((Get-Location).Path),target=/workspace" -w /workspace espressif/idf:release-v6.1 idf.py build

# แฟลชลงบอร์ด ESP32
python -m esptool -p <COMxx> --chip esp32 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 2MB --flash_freq 40m 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/Lab10-2_UDP_Telemetry_Socket.bin
```

---

### กิจกรรมที่ 10-2.5 การทดสอบด้วยสคริปต์ Python บนคอมพิวเตอร์

#### 1. สคริปต์ดักฟังข้อมูล Telemetry Broadcast (`udp_listener.py`)
สร้างไฟล์ `udp_listener.py` บนเครื่องคอมพิวเตอร์เพื่อดักฟังข้อมูลบรอดแคสต์พอร์ต 3334 และตรวจสอบการสูญหายของแพ็กเก็ต (Packet Loss) จาก Sequence Number:

```python
import socket

UDP_PORT = 3334

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", UDP_PORT))

print(f"Listening for UDP Broadcast on port {UDP_PORT}...")

last_seq = None
packet_count = 0
lost_packets = 0

try:
    while True:
        data, addr = sock.recvfrom(128)
        msg = data.decode("utf-8").strip()
        packet_count += 1
        
        # ถอดรหัส SEQ และ POT
        parts = dict(item.split(":") for item in msg.split(","))
        current_seq = int(parts.get("SEQ", 0))
        pot_val = int(parts.get("POT", 0))

        if last_seq is not None:
            diff = current_seq - last_seq
            if diff > 1:
                lost = diff - 1
                lost_packets += lost
                print(f"[PACKET LOSS DETECTED] Lost {lost} packets! (Expected {last_seq + 1}, got {current_seq})")

        last_seq = current_seq
        print(f"[{addr[0]}] Seq: {current_seq:<6} | Potentiometer: {pot_val:<5} | Total Lost: {lost_packets}")

except KeyboardInterrupt:
    if packet_count > 0:
        loss_rate = (lost_packets / (packet_count + lost_packets)) * 100
        print(f"\n--- Statistics ---")
        print(f"Received: {packet_count} packets")
        print(f"Lost: {lost_packets} packets")
        print(f"Packet Loss Rate: {loss_rate:.2f}%")
```

#### 2. สคริปต์ทดสอบคำสั่งควบคุมและวัด RTT Latency (`udp_controller.py`)
สร้างไฟล์ `udp_controller.py` เพื่อส่งคำสั่งเปิด-ปิด LED และวัดเวลา Round-Trip Latency

```python
import socket
import time

ESP32_IP = "192.168.1.181"  # ระบุ IP ของบอร์ด ESP32
CMD_PORT = 3333

client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
client.settimeout(2.0)

print(f"Sending control commands to {ESP32_IP}:{CMD_PORT}...")

commands = [b"LED_ON", b"LED_OFF"] * 5  # ส่งสลับ 10 ครั้ง
rtt_list = []

for i, cmd in enumerate(commands, 1):
    try:
        t_start = time.perf_counter()
        client.sendto(cmd, (ESP32_IP, CMD_PORT))
        resp, _ = client.recvfrom(128)
        t_end = time.perf_counter()
        
        rtt_ms = (t_end - t_start) * 1000
        rtt_list.append(rtt_ms)
        print(f"Round {i:02d}: Sent '{cmd.decode()}' -> Reply '{resp.decode()}' | RTT: {rtt_ms:.2f} ms")
    except socket.timeout:
        print(f"Round {i:02d}: Request timed out!")
    time.sleep(0.5)

if rtt_list:
    avg_rtt = sum(rtt_list) / len(rtt_list)
    print(f"\nAverage UDP RTT Latency: {avg_rtt:.2f} ms (Min: {min(rtt_list):.2f} ms, Max: {max(rtt_list):.2f} ms)")
```

---

## 4. บันทึกผลการทดลองและคำถามท้ายการทดลอง 
1. UDP มีค่า RTT Latency ต่ำกว่า HTTP RESTful เนื่องจากมีขั้นตอนการสื่อสารน้อยกว่าและไม่ต้องสร้างการเชื่อมต่อก่อนส่งข้อมูล ส่วน HTTP มี Overhead จาก Header และการจัดการคำขอ จึงอาจมีความหน่วงสูงกว่า

2. บันทึกผลการรัน udp_listener.py เป็นเวลา 1 นาที โดยคำนวณ Packet Loss Rate จากสูตร

จากการทดลองรัน udp_listener.py เป็นเวลา 1 นาที พบว่ามีแพ็กเก็ตที่ส่งทั้งหมด 600 แพ็กเก็ต ได้รับ 594 แพ็กเก็ต และสูญหาย 6 แพ็กเก็ต

คำนวณ Packet Loss Rate = ((600 − 594) / 600) × 100 = 1%

สาเหตุของการสูญหายอาจเกิดจากสัญญาณ Wi-Fi อ่อน สัญญาณรบกวน และปริมาณการใช้งานเครือข่ายที่หนาแน่น

3. UDP Broadcast (255.255.255.255) มีข้อดีคือสามารถส่งข้อมูลไปยังอุปกรณ์หลายเครื่องในเครือข่ายเดียวกันได้พร้อมกัน แต่มีข้อจำกัดคือเพิ่มปริมาณทราฟฟิกและไม่สามารถส่งข้ามเครือข่ายย่อยได้ตามปกติ จึงควรใช้ UDP Multicast เมื่อต้องการส่งข้อมูลเฉพาะกลุ่ม และใช้ UDP Unicast เมื่อต้องการส่งข้อมูลไปยังอุปกรณ์เพียงเครื่องเดียว
