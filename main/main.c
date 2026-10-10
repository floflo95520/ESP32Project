#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"

// ---------------- Configuration (menuconfig) ----------------
#define DHT_GPIO        GPIO_NUM_4
#define SLEEP_TIME_S    CONFIG_DHT_SLEEP_TIME_S
#define WIFI_SSID       CONFIG_DHT_WIFI_SSID
#define WIFI_PASS       CONFIG_DHT_WIFI_PASSWORD
#define MQTT_URI        CONFIG_DHT_MQTT_BROKER_URI
#define TOPIC_TEMP      CONFIG_DHT_MQTT_TOPIC_PREFIX "/temperature"
#define TOPIC_HUM       CONFIG_DHT_MQTT_TOPIC_PREFIX "/humidite"

#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_DONE_BIT       BIT1

static const char *TAG = "DHT11";
static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t s_events;
static int s_temp, s_hum;
static int s_published = 0;

// ---------------- DHT11 ----------------

// Attend tant que la broche est au niveau 'level'. Retourne la durée (µs) ou -1 si timeout.
static int wait_while_level(int level, uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT_GPIO) == level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

static esp_err_t dht11_read(int *temperature, int *humidity)
{
    uint8_t data[5] = {0};

    // Signal de start : ligne à 0 pendant 20 ms
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(DHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Partie critique : timings en µs
    portENTER_CRITICAL(&dht_mux);

    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    // Réponse du capteur : ~80 µs à 0, puis ~80 µs à 1
    if (wait_while_level(1, 100) < 0 ||
        wait_while_level(0, 100) < 0 ||
        wait_while_level(1, 100) < 0) {
        portEXIT_CRITICAL(&dht_mux);
        return ESP_ERR_TIMEOUT;
    }

    // 40 bits : ~50 µs à 0, puis ~27 µs (bit 0) ou ~70 µs (bit 1) à 1
    for (int i = 0; i < 40; i++) {
        if (wait_while_level(0, 80) < 0) {
            portEXIT_CRITICAL(&dht_mux);
            return ESP_ERR_TIMEOUT;
        }
        int high_us = wait_while_level(1, 100);
        if (high_us < 0) {
            portEXIT_CRITICAL(&dht_mux);
            return ESP_ERR_TIMEOUT;
        }
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }

    portEXIT_CRITICAL(&dht_mux);

    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return ESP_ERR_INVALID_CRC;
    }

    *humidity    = data[0];
    *temperature = data[2];
    return ESP_OK;
}

// ---------------- Wi-Fi ----------------

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();   // on réessaie
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------------- MQTT ----------------

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    char payload[16];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        snprintf(payload, sizeof(payload), "%d", s_temp);
        esp_mqtt_client_publish(event->client, TOPIC_TEMP, payload, 0, 1, 1);
        snprintf(payload, sizeof(payload), "%d", s_hum);
        esp_mqtt_client_publish(event->client, TOPIC_HUM, payload, 0, 1, 1);
        break;
    case MQTT_EVENT_PUBLISHED:          // accusé de réception du broker (QoS 1)
        if (++s_published >= 2) {
            xEventGroupSetBits(s_events, MQTT_DONE_BIT);
        }
        break;
    default:
        break;
    }
}

static bool send_values(int temp, int hum)
{
    s_temp = temp;
    s_hum = hum;
    s_published = 0;
    s_events = xEventGroupCreate();

    wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi : connexion impossible");
        return false;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    bits = xEventGroupWaitBits(s_events, MQTT_DONE_BIT,
                               pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));

    esp_mqtt_client_stop(client);
    esp_wifi_stop();
    return (bits & MQTT_DONE_BIT) != 0;
}

// ---------------- Programme principal ----------------

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());   // requis par le Wi-Fi

    // Le DHT11 a besoin d'environ 1 s après la mise sous tension
    vTaskDelay(pdMS_TO_TICKS(1500));

    int temp = 0, hum = 0;
    esp_err_t err = ESP_FAIL;

    // 1) Lecture du capteur AVANT d'activer le Wi-Fi (la radio perturbe les timings)
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = dht11_read(&temp, &hum);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Lecture échouée (%s), essai %d/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // 2) Affichage + envoi
    if (err == ESP_OK) {
        printf("Température : %d °C | Humidité : %d %%\n", temp, hum);
        if (send_values(temp, hum)) {
            ESP_LOGI(TAG, "Valeurs envoyées");
        } else {
            ESP_LOGE(TAG, "Envoi échoué");
        }
    } else {
        ESP_LOGE(TAG, "Impossible de lire le DHT11");
    }

    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3) Deep sleep
    ESP_LOGI(TAG, "Deep sleep pour %d s", SLEEP_TIME_S);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIME_S * 1000000ULL);
    esp_deep_sleep_start();
}