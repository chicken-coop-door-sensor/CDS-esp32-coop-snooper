#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "mqtt.h"
#include "led.h"
#include "state_handler.h"
#include "mp3.h"  // Include the mp3 header
#include "esp_task_wdt.h"
#include "mbedtls/debug.h"  // Add this to include mbedtls debug functions
#include "spiffs.h"
#include "time_sync.h"

static const char *TAG = "COOP_SNOOPER";
SemaphoreHandle_t audioSemaphore;  // Add semaphore handle for audio playback
SemaphoreHandle_t timer_semaphore;  // Add semaphore handle timer for audio playback


static void tls_debug_callback(void *ctx, int level, const char *file, int line, const char *str)
{
    // Uncomment to enable verbose debugging
    // const char *MBEDTLS_DEBUG_LEVEL[] = {"Error", "Warning", "Info", "Debug", "Verbose"};
    // ESP_LOGI("mbedTLS", "%s: %s:%04d: %s", MBEDTLS_DEBUG_LEVEL[level], file, line, str);
}

void app_main(void)
{
#ifdef TENNIS_HOUSE
    printf("Configuration: TENNIS_HOUSE\n");
#elif defined(FARM_HOUSE)
    printf("Configuration: FARM_HOUSE\n");
#else
    printf("Configuration: UNKNOWN\n");
#endif

    init_spiffs();

    ESP_LOGI(TAG, "Initializing LED PWM");
    init_led_pwm();

    ESP_LOGI(TAG, "Setting LED state to PULSATING_WHITE");
    current_led_state = LED_FLASHING_WHITE;

    ESP_LOGI(TAG, "Creating LED task");
    xTaskCreate(&led_task, "led_task", 4096, NULL, 5, NULL);

    // Set mbedtls debug threshold to 0 to disable verbose debugging
    // mbedtls_debug_set_threshold(0);

    // Initialize the mbedtls SSL configuration
    mbedtls_ssl_config conf;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_conf_dbg(&conf, tls_debug_callback, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    wifi_init_sta();

    // Initialize MQTT
    ESP_LOGI(TAG, "Initializing MQTT");
    esp_mqtt_client_handle_t mqtt_client_handle = mqtt_app_start();


    // Initialize audio semaphore
    audioSemaphore = xSemaphoreCreateBinary();
    if (audioSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create audio semaphore");
        return;
    }

    timer_semaphore = xSemaphoreCreateBinary();
    if (timer_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create timer semaphore");
        return;
    }

    synchronize_time();

    xTaskCreate(audio_player_task, "audio_player_task", 8192, NULL, 5, NULL);

    if (was_booted_after_ota_update()) {
        char buffer[128];
        ESP_LOGW(TAG, "Device booted after an OTA update.");
        cJSON *root = cJSON_CreateObject();
        sprintf(buffer, "Successful reboot after OTA update");
        cJSON_AddStringToObject(root, WIFI_HOSTNAME, buffer);
        const char *json_string = cJSON_Print(root);
        esp_mqtt_client_publish(mqtt_client_handle, CONFIG_MQTT_PUBLISH_OTA_PROGRESS_TOPIC, json_string, 0, 1, 0);
        free(root);
        free(json_string);
    } else {
        ESP_LOGW(TAG, "Device did not boot after an OTA update.");
    }

    // Infinite loop to prevent exiting app_main
    while (true) {
        // ESP_ERROR_CHECK(esp_task_wdt_reset()); // Reset the watchdog timer
        vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow other tasks to run
    }
}
