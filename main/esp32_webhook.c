#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "nvs_flash.h"
#include "freertos/idf_additions.h"
#include "hal/gpio_types.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#define WEBHOOK_SERVER CONFIG_WEBHOOK_SERVER
#define WEBHOOK_PORT CONFIG_WEBHOOK_PORT
#define WEBHOOK_ID CONFIG_WEBHOOK_ID
#define WEBHOOK_PATH "/webhook/" WEBHOOK_ID
// HACK: Needed to convert the CONFIG_WEBHOOK_BUTTON_ID to a string literal.
// It is not a string in the menuconfig because it needs to be range validated (1-9).
#define _STR_HELPER(x) #x
#define _STR(x) _STR_HELPER(x)
#define WEBHOOK_BUTTON_ID _STR(CONFIG_WEBHOOK_BUTTON_ID)

static const char *WEBHOOK_REQUEST = "POST "WEBHOOK_PATH" HTTP/1.0\r\n"
    "Host: "WEBHOOK_SERVER":"WEBHOOK_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32_webhook\r\n"
    "Content-Length: 15\r\n" // NOTE: This content length is hard-coded
    "\r\n"
    "{\"button_id\":"WEBHOOK_BUTTON_ID"}";

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD
#define GPIO_INPUT_PIN CONFIG_GPIO_INPUT_PIN
#define GPIO_INPUT_PIN_SEL (1ULL<<GPIO_INPUT_PIN)

#define ESP_INTR_FLAG_DEFAULT 0

// log identifiers
#define TAG "webhook"
#define WIFI_TAG "wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static void setup_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void setup_wifi() {
    // Wifi setup uses nvs
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(WIFI_TAG, "Starting setup_wifi");
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(WIFI_TAG, "Running esp_netif_init");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(WIFI_TAG, "Running esp_event_loop_create_default");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(WIFI_TAG, "Running esp_netif_create_default_wifi_sta");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(WIFI_TAG, "Running esp_wifi_init(WIFI_INIT_CONFIG_DEFAULT)");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_LOGI(WIFI_TAG, "Registering 'any_id' event handler");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &setup_wifi_event_handler,
        NULL,
        &instance_any_id
    ));
    ESP_LOGI(WIFI_TAG, "Registering 'got_ip' event handler");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &setup_wifi_event_handler,
        NULL,
        &instance_got_ip
    ));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // TODO: Make configurable
            // .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH, // NOTE: Default
            .sae_h2e_identifier = "", // NOTE: Default
// #ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
//             .disable_wpa3_compatible_mode = 0,
// #endif
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "setup_wifi finished. Waiting for event..."); // FIXME: It didn't really finish...

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "Connected to AP: SSID:%s, PASS:%s", WIFI_SSID, WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "Failed to connect to AP: SSID:%s, PASS:%s", WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// https://github.com/espressif/esp-idf/tree/master/examples/protocols/http_request
static void invoke_webhook() {
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    int err = getaddrinfo(WEBHOOK_SERVER, WEBHOOK_PORT, &hints, &res);

    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        return;
    }

    // NOTE: inet_ntoa is non-reentrant. I'm gonna risk it (it probably just uses an internal static buffer to store the result)
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
    if(s < 0) {
        ESP_LOGE(TAG, "... Failed to allocate socket.");
        freeaddrinfo(res);
    }
    ESP_LOGI(TAG, "... allocated socket");

    if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        return;
    }
    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);

    if (write(s, WEBHOOK_REQUEST, strlen(WEBHOOK_REQUEST)) < 0) {
        ESP_LOGE(TAG, "... socket send failed");
        close(s);
        return;
    }
    ESP_LOGI(TAG, "... socket send success");

    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
            sizeof(receiving_timeout)) < 0) {
        ESP_LOGE(TAG, "... failed to set socket receiving timeout");
        close(s);
        return;
    }
    ESP_LOGI(TAG, "... set socket receiving timeout success");

    /* Read HTTP response */
    do {
        bzero(recv_buf, sizeof(recv_buf));
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        for(int i = 0; i < r; i++) {
            putchar(recv_buf[i]);
        }
    } while(r > 0);

    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
    close(s);
}

void app_main(void)
{
    // Setup Wifi
    setup_wifi();

    // Configure GPIO input
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_INPUT_PIN, gpio_isr_handler, (void*)GPIO_INPUT_PIN);
    ESP_LOGI(TAG, "Waiting for button press on GPIO pin %d", GPIO_INPUT_PIN);

    // The level that the gpio pin will change to when the button is pressed
    int PRESS_LEVEL = 0;

    uint32_t io_num;
    int prev_level = 1;
    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            int curr_level = gpio_get_level(io_num);
            if (curr_level != prev_level) {
                prev_level = curr_level;
                if (curr_level == PRESS_LEVEL) invoke_webhook();
            }
        }
    }
}
