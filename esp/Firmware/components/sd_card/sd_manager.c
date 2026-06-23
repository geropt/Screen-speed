#include "sd_manager.h"
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <inttypes.h>

const static char *TAG = "SD_MANAGER";

#define EXAMPLE_MAX_CHAR_SIZE    64

static sdmmc_card_t *card = NULL;

#ifdef CONFIG_DEBUG_PIN_CONNECTIONS
const char* names[] = {"CLK", "CMD", "D0"};
const int pins[] = {SD_SCK_GPIO,
                    SD_MOSI_GPIO,
                    SD_MISO_GPIO
                    };

const int pin_count = sizeof(pins)/sizeof(pins[0]);

pin_configuration_t config = {
    .names = names,
    .pins = pins,
};
#endif //CONFIG_DEBUG_PIN_CONNECTIONS

static esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, "%s", data);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

static esp_err_t s_example_read_file(const char *path)
{
    ESP_LOGI(TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE];
    fgets(line, sizeof(line), f);
    fclose(f);

    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);

    return ESP_OK;
}

esp_err_t sd_card_init()
{
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 40MHz for SDMMC)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_52M;

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_SCK_GPIO;
    slot_config.cmd = SD_MOSI_GPIO;
    slot_config.d0 = SD_MISO_GPIO;
    // slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
            check_sd_card_pins(&config, pin_count);
        }
        ESP_LOGW(TAG, "Failed to mount!!! ret: %d 0x%X", ret, ret);
        return ret;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    // Card has been initialized, adjust its speed
    adjust_card_speed(card, &host);

//     // Use POSIX and C standard library functions to work with files:

//     // First create a file.
//     const char *file_hello = MOUNT_POINT"/hello.txt";
//     char data[EXAMPLE_MAX_CHAR_SIZE];
//     snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Hello", card->cid.name);
//     ret = s_example_write_file(file_hello, data);
//     if (ret != ESP_OK) {
//         return ret;
//     }

//     const char *file_foo = MOUNT_POINT"/foo.txt";
//     // Check if destination file exists before renaming
//     struct stat st;
//     if (stat(file_foo, &st) == 0) {
//         // Delete it if it exists
//         unlink(file_foo);
//     }

//     // Rename original file
//     ESP_LOGI(TAG, "Renaming file %s to %s", file_hello, file_foo);
//     if (rename(file_hello, file_foo) != 0) {
//         ESP_LOGE(TAG, "Rename failed");
//         return ret;
//     }

//     ret = s_example_read_file(file_foo);
//     if (ret != ESP_OK) {
//         return ret;
//     }

//     // Format FATFS
// #ifdef CONFIG_FORMAT_SD_CARD
//     ret = esp_vfs_fat_sdcard_format(mount_point, card);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to format FATFS (%s)", esp_err_to_name(ret));
//         return ret;
//     }

//     if (stat(file_foo, &st) == 0) {
//         ESP_LOGI(TAG, "file still exists");
//         return ret;
//     } else {
//         ESP_LOGI(TAG, "file doesn't exist, formatting done");
//     }
// #endif // CONFIG_FORMAT_SD_CARD

//     const char *file_nihao = MOUNT_POINT"/nihao.txt";
//     memset(data, 0, EXAMPLE_MAX_CHAR_SIZE);
//     snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Nihao", card->cid.name);
//     ret = s_example_write_file(file_nihao, data);
//     if (ret != ESP_OK) {
//         return ret;
//     }

//     //Open file for reading
//     ret = s_example_read_file(file_nihao);
//     if (ret != ESP_OK) {
//         return ret;
//     }

    return ESP_OK;
}

esp_err_t sd_card_deinit()
{
    const char mount_point[] = MOUNT_POINT;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mount_point, card);

    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error unmounting card, err=%X", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Card unmounted");
    return ESP_OK;
}

void adjust_card_speed(sdmmc_card_t *ctx, sdmmc_host_t *host)
{
    sdmmc_card_print_info(stdout, ctx);

    uint32_t current = ctx->real_freq_khz;
    uint32_t maximum = ctx->max_freq_khz;

    // Nothing to do if already at peak
    if (current >= maximum) {
        // ESP_LOGI(TAG, "SD card is already running at its maximum speed");
        return;
    }

    // Clamp to host capability
    uint32_t target = maximum;
    if (target > SDMMC_FREQ_HIGHSPEED) {
        target = SDMMC_FREQ_HIGHSPEED;
    }

    if(target == current)
        return;

    ESP_LOGI(TAG, "Attempting to increase SDMMC clock to %" PRIu32" kHz", target);

    esp_err_t err = sdmmc_host_set_card_clk(host->slot, target);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch SD clock speed (%s)", esp_err_to_name(err));
        return;
    }

    // Update card info
    ctx->real_freq_khz = target;
    sdmmc_card_print_info(stdout, ctx);

    ESP_LOGI(TAG, "SD card speed updated successfully to %"PRIu32" kHz", target);
}

