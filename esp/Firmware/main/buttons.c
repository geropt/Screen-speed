#include "buttons.h"
#include "dynamic.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// BOOT button on all ESP32-S3 boards is the GPIO0 strapping pin. It reads HIGH
// when released (external/internal pull-up) and LOW while pressed.
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BUTTON_POLL_MS          20
// Number of consecutive stable polls required to accept a level change.
#define BUTTON_DEBOUNCE_SAMPLES 3

static const char *TAG = "BUTTONS";

static void button_task(void *pvParameter)
{
    int stable_level = 1;   // released
    int last_raw = 1;
    int stable_count = 0;

    while (1)
    {
        int raw = gpio_get_level(BOOT_BUTTON_GPIO);

        if (raw == last_raw)
        {
            if (stable_count < BUTTON_DEBOUNCE_SAMPLES)
                stable_count++;
        }
        else
        {
            stable_count = 0;
        }
        last_raw = raw;

        // Accept the new level only once it has been stable long enough.
        if (stable_count >= BUTTON_DEBOUNCE_SAMPLES && raw != stable_level)
        {
            stable_level = raw;
            if (stable_level == 0)   // falling edge -> pressed
            {
                ESP_LOGI(TAG, "BOOT button pressed");
                show_temp_message("Boton OK!", 2000);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(button_task, "button_task", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "BOOT button (GPIO%d) ready", BOOT_BUTTON_GPIO);
}
