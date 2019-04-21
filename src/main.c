#include "esp_event_loop.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_log.h"

#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "esp_http_client.h"

#include "secrets.h"

static const char *TAG = "HassRemote";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

// GPIO related
#define GPIO_WAKE_UP 4

// Declarations
esp_err_t _http_event_handle(esp_http_client_event_t *evt);

void triggerAutomation(char *automation) {
  char buffer[255];
  memcpy(buffer, "{\"entity_id\": \"automation.", 26);
  // TODO - Check max buffer size
  memcpy(buffer + 26, automation, strlen(automation));
  memcpy(buffer + 26 + strlen(automation), "\"}", 2);

  esp_http_client_config_t config = {
      .url = HTTP_URL,
      .event_handler = _http_event_handle,
      .method = HTTP_METHOD_POST,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Authorization", HTTP_TOKEN);
  esp_http_client_set_post_field(client, buffer, 28 + strlen(automation));
  esp_err_t err = esp_http_client_perform(client);

  if (err == ESP_OK) {
    ESP_LOGI("HASS", "Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE("HASS", "Unable to trigger automation. Error: %d\n", err);
  }
  esp_http_client_cleanup(client);
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event) {
  switch (event->event_id) {
  case SYSTEM_EVENT_STA_START:
    esp_wifi_connect();
    break;
  case SYSTEM_EVENT_STA_GOT_IP:
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    esp_wifi_connect();
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    break;
  default:
    break;
  }
  return ESP_OK;
}

static void wifi_init(void) {
  tcpip_adapter_init();
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = CONFIG_WIFI_SSID,
              .password = CONFIG_WIFI_PASSWORD,
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_LOGI(TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID,
           "******");
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Waiting for wifi");
  xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true,
                      portMAX_DELAY);
}

esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
    printf("%.*s", evt->data_len, (char *)evt->data);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    if (!esp_http_client_is_chunked_response(evt->client)) {
      printf("%.*s", evt->data_len, (char *)evt->data);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  }
  return ESP_OK;
}

void print_wakeup_reason(esp_sleep_wakeup_cause_t wakeup_reason) {
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
  case ESP_SLEEP_WAKEUP_EXT0:
    ESP_LOGI("WAKEUP", "Ext0 - RTC_IO");
    break;
  case ESP_SLEEP_WAKEUP_EXT1:
    ESP_LOGI("WAKEUP", "Ext1 - RTC_CNTL");
    break;
  case ESP_SLEEP_WAKEUP_TIMER:
    ESP_LOGI("WAKEUP", "Timer");
    break;
  case ESP_SLEEP_WAKEUP_TOUCHPAD:
    ESP_LOGI("WAKEUP", "Touchpad");
    break;
  case ESP_SLEEP_WAKEUP_ULP:
    ESP_LOGI("WAKEUP", "ULP");
    break;
  /*
  case ESP_SLEEP_WAKEUP_GPIO:
      ESP_LOGI("WAKEUP", "Lightleep - GPIO");
      break;
  case ESP_SLEEP_WAKEUP_UART:
      ESP_LOGI("WAKEUP", "Lightleep - GPIO");
      break;
  case ESP_SLEEP_WAKEUP_ALL:
      ESP_LOGI("WAKEUP", "Lightleep - GPIO");
      break;
  */
  case ESP_SLEEP_WAKEUP_UNDEFINED:
    ESP_LOGI("WAKEUP", "Reset was not caused by exit from deep sleep");
    break;
  default:
    ESP_LOGI("WAKEUP", "Unknown, check latest doc");
    break;
  }
}

void init_logs() {
  ESP_LOGI("APP", "Startup..");
  ESP_LOGI("APP", "Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI("APP", "IDF version: %s", esp_get_idf_version());

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
}

void deep_sleep() {
  // Setup ext GPIO wakeup
  if (rtc_gpio_pulldown_en(GPIO_WAKE_UP) != ESP_OK) {
    ESP_LOGE(TAG, "Can not set GPIO_WAKE_UP");
  };
  esp_sleep_enable_ext0_wakeup(GPIO_WAKE_UP, 1);
  // const uint8_t ext1_wakeup_pin = 4;
  // const uint64_t ext1_wakeup_mask = 1ULL << ext1_wakeup_pin;
  // esp_sleep_enable_ext1_wakeup(ext1_wakeup_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Disable peripherals
  if (esp_wifi_stop() != ESP_OK) {
    ESP_LOGE(TAG, "Can stop Wi-Fi");
  };
  adc_power_off(); // Link to ESP-IDF bug if Wi-Fi has been activated before

  ESP_LOGI(TAG, "Going into deep sleep");
  esp_deep_sleep_start();
}

void app_main() {
  init_logs();
  nvs_flash_init();
  wifi_init();
  ESP_LOGI(TAG, "========\nESP initialised\n========");

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  print_wakeup_reason(wakeup_reason);

  if (wakeup_reason != ESP_SLEEP_WAKEUP_EXT0) {
    // Skip delay if wake up from ext0
    for (uint8_t i = 5; i > 0; i--) {
      ESP_LOGI(TAG, "%d", i);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}
