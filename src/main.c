#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"

#include "esp_http_client.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/adc.h"

#include "secrets.h"

static const char *TAG = "HassRemote";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;


// GPIO related
#define GPIO_INPUT_ON     5
#define GPIO_INPUT_PIN_SEL (1ULL<<GPIO_INPUT_ON)
#define ESP_INTR_FLAG_DEFAULT 0
static xQueueHandle gpio_evt_queue = NULL;

#define GPIO_WAKE_UP 4


// Declarations
esp_err_t _http_event_handle(esp_http_client_event_t *evt);


static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

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
    ESP_LOGI(TAG, "Status = %d, content_length = %d",
            esp_http_client_get_status_code(client),
            esp_http_client_get_content_length(client));
    }
    esp_http_client_cleanup(client);
}

static void gpio_task_example(void* arg) {
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));

            if (io_num == GPIO_INPUT_ON) {
                // TODO - Better debouncing
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                if (gpio_get_level(io_num)) {
                    triggerAutomation("hyppoclock_on");
                }
            }
        }
    }
}


static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
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

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}


esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
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
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                printf("%.*s", evt->data_len, (char*)evt->data);
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


void gpio_init() {
    gpio_config_t io_conf;
    // enable pull-down mode
    io_conf.pull_down_en = 1;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO5
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 4096, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_ON, gpio_isr_handler, (void*) GPIO_INPUT_ON);
}


void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch(wakeup_reason) {
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
    print_wakeup_reason();

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
}


void app_main()
{
    init_logs();
    nvs_flash_init();
    wifi_init();
    gpio_init();
    ESP_LOGI(TAG, "========\nESP initialised\n========");
    

    // Deep sleep setup
    if (rtc_gpio_pulldown_en(GPIO_WAKE_UP) != ESP_OK) {
        ESP_LOGE(TAG, "Can not set GPIO_WAKE_UP");
    };
    esp_sleep_enable_ext0_wakeup(GPIO_WAKE_UP, 1);
    //const uint8_t ext1_wakeup_pin = 4;
    //const uint64_t ext1_wakeup_mask = 1ULL << ext1_wakeup_pin;
    // esp_sleep_enable_ext1_wakeup(ext1_wakeup_mask, ESP_EXT1_WAKEUP_ANY_HIGH);


    // Disable peripherals
    if (esp_wifi_stop() != ESP_OK) {
        ESP_LOGE(TAG, "Can stop Wi-Fi");
    };
    adc_power_off();    // Link to ESP-IDF bug if Wi-Fi has been activated

    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (wakeup_reason != ESP_SLEEP_WAKEUP_EXT0) {
        // Skip delay if wake up from ext0
        for (uint8_t i = 5; i>0; i--) {
            ESP_LOGI(TAG, "%d", i);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
    ESP_LOGI(TAG, "Going into deep sleep");
    esp_deep_sleep_start();


}
