#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "freertos/idf_additions.h"
#include "hal/gpio_types.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD
#define GPIO_INPUT_PIN CONFIG_GPIO_INPUT_PIN
#define GPIO_INPUT_PIN_SEL (1ULL<<GPIO_INPUT_PIN)

#define ESP_INTR_FLAG_DEFAULT 0

static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void invoke_webhook() {
    printf("Invoking webhook /s\n");
}

void app_main(void)
{
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
