#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#define DHT_GPIO        GPIO_NUM_4
#define SLEEP_TIME_S    30          // durée du sommeil profond entre deux mesures

static const char *TAG = "DHT11";
static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

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

    // Signal de start : on tire la ligne à 0 pendant 20 ms
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(DHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Partie critique : timings en µs, on bloque les interruptions
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

    // 40 bits : ~50 µs à 0, puis 26-28 µs (bit 0) ou ~70 µs (bit 1) à 1
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

    // Vérification du checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return ESP_ERR_INVALID_CRC;
    }

    *humidity    = data[0];   // data[1] = partie décimale (toujours 0 sur DHT11)
    *temperature = data[2];   // data[3] = partie décimale (toujours 0 sur DHT11)
    return ESP_OK;
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Réveil (cause = %d)", cause);

    // Le DHT11 a besoin d'environ 1 s après la mise sous tension / entre deux lectures
    vTaskDelay(pdMS_TO_TICKS(1500));

    int temp = 0, hum = 0;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= 3; attempt++) {
        err = dht11_read(&temp, &hum);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Lecture échouée (%s), essai %d/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (err == ESP_OK) {
        printf("Température : %d °C | Humidité : %d %%\n", temp, hum);
    } else {
        ESP_LOGE(TAG, "Impossible de lire le DHT11");
    }

    // Laisser le temps au message de partir sur l'UART
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Retour en sommeil profond
    ESP_LOGI(TAG, "Deep sleep pour %d s", SLEEP_TIME_S);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIME_S * 1000000ULL);
    esp_deep_sleep_start();
}