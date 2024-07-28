#include "cJSON.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gecl-logger-manager.h"
#include "gecl-mqtt-manager.h"
#include "gecl-ota-manager.h"
#include "gecl-rgb-led-manager.h"
#include "gecl-time-sync-manager.h"
#include "gecl-wifi-manager.h"
#include "mbedtls/debug.h"  // Add this to include mbedtls debug functions
#include "mp3.h"            // Include the mp3 header
#include "nvs_flash.h"

#ifndef VERSION_TAG
#define VERSION_TAG "undefined"
#endif

static const char *TAG = "COOP_SNOOPER";
SemaphoreHandle_t audioSemaphore;   // Add semaphore handle for audio playback
SemaphoreHandle_t timer_semaphore;  // Add semaphore handle timer for audio playback

QueueHandle_t log_queue = NULL;
QueueHandle_t led_state_queue = NULL;

#ifdef TENNIS_HOUSE
extern const uint8_t coop_snooper_tennis_home_certificate_pem[];
extern const uint8_t coop_snooper_tennis_home_private_pem_key[];
#elif defined(FARM_HOUSE)
extern const uint8_t coop_snooper_farmhouse_certificate_pem[];
extern const uint8_t coop_snooper_farmhouse_private_pem_key[];
#endif

void custom_handle_mqtt_event_connected(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    ESP_LOGI(TAG, "Custom handler: MQTT_EVENT_CONNECTED");
    int msg_id;

    msg_id = esp_mqtt_client_subscribe(client, CONFIG_MQTT_SUBSCRIBE_STATUS_TOPIC, 0);
    ESP_LOGI(TAG, "Subscribed to topic %s, msg_id=%d", CONFIG_MQTT_SUBSCRIBE_STATUS_TOPIC, msg_id);

    msg_id = esp_mqtt_client_subscribe(client, CONFIG_MQTT_SUBSCRIBE_OTA_UPDATE_SNOOPER_TOPIC, 0);
    ESP_LOGI(TAG, "Subscribed to topic %s, msg_id=%d", CONFIG_MQTT_SUBSCRIBE_OTA_UPDATE_SNOOPER_TOPIC, msg_id);

    msg_id =
        esp_mqtt_client_publish(client, CONFIG_MQTT_PUBLISH_STATUS_TOPIC, "{\"message\":\"status_request\"}", 0, 0, 0);
    ESP_LOGI(TAG, "Published initial status request, msg_id=%d", msg_id);
}

void custom_handle_mqtt_event_disconnected(esp_mqtt_event_handle_t event) {
    ESP_LOGI(TAG, "Custom handler: MQTT_EVENT_DISCONNECTED");
    if (ota_task_handle != NULL) {
        vTaskDelete(ota_task_handle);
        ota_task_handle = NULL;
    }
    // Reconnect logic
    int retry_count = 0;
    const int max_retries = 5;
    const int retry_delay_ms = 5000;
    esp_err_t err;
    esp_mqtt_client_handle_t client = event->client;

    // Check if the network is connected before attempting reconnection
    if (wifi_active()) {
        do {
            ESP_LOGI(TAG, "Attempting to reconnect, retry %d/%d", retry_count + 1, max_retries);
            err = esp_mqtt_client_reconnect(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to reconnect MQTT client, retrying in %d seconds...", retry_delay_ms / 1000);
                vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));  // Delay for 5 seconds
                retry_count++;
            }
        } while (err != ESP_OK && retry_count < max_retries);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconnect MQTT client after %d retries", retry_count);
        }
    } else {
        ESP_LOGE(TAG, "Network not connected, skipping MQTT reconnection");
    }
}

void custom_handle_mqtt_event_data(esp_mqtt_event_handle_t event) {
    ESP_LOGI(TAG, "Custom handler: MQTT_EVENT_DATA");
    if (strncmp(event->topic, CONFIG_MQTT_SUBSCRIBE_STATUS_TOPIC, event->topic_len) == 0) {
        ESP_LOGW(TAG, "Received topic %s", CONFIG_MQTT_SUBSCRIBE_STATUS_TOPIC);
        // Handle the status response
        cJSON *json = cJSON_Parse(event->data);
        if (json == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
        } else {
            cJSON *state = cJSON_GetObjectItem(json, "LED");
            if (cJSON_IsString(state)) {
                ESP_LOGI(TAG, "Parsed state: %s", state->valuestring);
                led_state_t led_state = convert_led_string_to_enum(state->valuestring);
                if (led_state == LED_FLASHING_RED) {
                    ESP_LOGI(TAG, "Squeal! LED state to flashing red");
                    set_audio_playback(true);
                    set_volume(1.0f);
                    set_gain(true);
                    enable_amplifier(true);
                }
                set_led(led_state);
            } else {
                ESP_LOGE(TAG, "JSON state item is not a string");
            }
            cJSON_Delete(json);
        }
    } else if (strncmp(event->topic, CONFIG_MQTT_SUBSCRIBE_OTA_UPDATE_SNOOPER_TOPIC, event->topic_len) == 0) {
        ESP_LOGI(TAG, "Received topic %s", CONFIG_MQTT_SUBSCRIBE_OTA_UPDATE_SNOOPER_TOPIC);
        if (ota_task_handle != NULL) {
            eTaskState task_state = eTaskGetState(ota_task_handle);
            if (task_state != eDeleted) {
                ESP_LOGW(TAG, "OTA task is already running or not yet cleaned up, skipping OTA update");
                return;
            }
            // Clean up task handle if it has been deleted
            ota_task_handle = NULL;
        }
        set_led(LED_FLASHING_GREEN);
        xTaskCreate(&ota_task, "ota_task", 8192, event, 5, &ota_task_handle);
    }
}

void custom_handle_mqtt_event_error(esp_mqtt_event_handle_t event) {
    ESP_LOGI(TAG, "Custom handler: MQTT_EVENT_ERROR");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_ESP_TLS) {
        ESP_LOGI(TAG, "Last ESP error code: 0x%x", event->error_handle->esp_tls_last_esp_err);
        ESP_LOGI(TAG, "Last TLS stack error code: 0x%x", event->error_handle->esp_tls_stack_err);
        ESP_LOGI(TAG, "Last TLS library error code: 0x%x", event->error_handle->esp_tls_cert_verify_flags);
    } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
    } else {
        ESP_LOGI(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
    }
    esp_restart();
}

static void tls_debug_callback(void *ctx, int level, const char *file, int line, const char *str) {
    // Uncomment to enable verbose debugging
    // const char *MBEDTLS_DEBUG_LEVEL[] = {"Error", "Warning", "Info", "Debug", "Verbose"};
    // ESP_LOGI("mbedTLS", "%s: %s:%04d: %s", MBEDTLS_DEBUG_LEVEL[level], file, line, str);
}

void app_main(void) {
#ifdef TENNIS_HOUSE
    printf("Configuration: TENNIS_HOUSE\n");
    static const char *LOCATION = "Tennis House";
    const uint8_t *cert = coop_snooper_tennis_home_certificate_pem;
    const uint8_t *key = coop_snooper_tennis_home_private_pem_key;
#elif defined(FARM_HOUSE)
    printf("Configuration: FARM_HOUSE\n");
    static const char *LOCATION = "Farm House";
    const uint8_t *cert = coop_snooper_farmhouse_certificate_pem;
    const uint8_t *key = coop_snooper_farmhouse_private_pem_key;
#else
    printf("Configuration: UNKNOWN\n");
#endif

    ESP_LOGI(TAG, "\n\nFirmware Version: %s\n\n", VERSION_TAG);

    ESP_LOGI(TAG, "Initializing LED PWM");
    init_led_pwm();

    led_state_queue = xQueueCreate(10, sizeof(led_state_t));
    if (led_state_queue == NULL) {
        ESP_LOGE(TAG, "Could not initialize LED PWM");
        esp_restart();
    }

    ESP_LOGI(TAG, "Creating LED task");
    xTaskCreate(&led_task, "led_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Setting LED state to flashing white");

    set_led(LED_FLASHING_WHITE);

    // Initialize the mbedtls SSL configuration
    // mbedtls_ssl_config conf;
    // mbedtls_ssl_config_init(&conf);
    // mbedtls_ssl_conf_dbg(&conf, tls_debug_callback, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initialize WiFi");
    wifi_init_sta();

    synchronize_time();

    log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_message_t));

    if (log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create logger queue");
        esp_restart();
    }

    xTaskCreate(&logger_task, "logger_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initializing MQTT");
    // esp_mqtt_client_handle_t mqtt_client_handle = mqtt_app_start();
    // Define the configuration
    mqtt_config_t config = {.certificate = cert, .private_key = key, .broker_uri = CONFIG_AWS_IOT_ENDPOINT};

    // Set the custom event handlers
    mqtt_set_event_connected_handler(custom_handle_mqtt_event_connected);
    mqtt_set_event_disconnected_handler(custom_handle_mqtt_event_disconnected);
    mqtt_set_event_data_handler(custom_handle_mqtt_event_data);
    mqtt_set_event_error_handler(custom_handle_mqtt_event_error);

    // Start the MQTT client
    esp_mqtt_client_handle_t client = mqtt_app_start(&config);

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

    xTaskCreate(audio_player_task, "audio_player_task", 8192, NULL, 5, NULL);

    if (was_booted_after_ota_update()) {
        char buffer[128];
        ESP_LOGW(TAG, "Device booted after an OTA update.");
        cJSON *root = cJSON_CreateObject();
        sprintf(buffer, "Successful reboot after OTA update");
        cJSON_AddStringToObject(root, WIFI_HOSTNAME, buffer);
        const char *json_string = cJSON_Print(root);
        esp_mqtt_client_publish(client, CONFIG_MQTT_PUBLISH_OTA_PROGRESS_TOPIC, json_string, 0, 1, 0);
        free(root);
        free(json_string);
    } else {
        ESP_LOGW(TAG, "Device did not boot after an OTA update.");
    }

    // Infinite loop to prevent exiting app_main
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // Delay to allow other tasks to run
    }
}
