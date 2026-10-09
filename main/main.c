#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// CHANGEMENT ICI : On passe du GPIO 2 au GPIO 23
#define LED_PIN GPIO_NUM_23

void app_main(void)
{
    // Réinitialiser le GPIO 23
    gpio_reset_pin(LED_PIN);
    
    // Configurer le GPIO 23 en mode SORTIE
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    printf("Clignotement de la LED externe sur le GPIO 23 !\n");

    while (1) {
        // Allumer la LED externe (3.3V envoyé sur le GPIO 23)
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        // Éteindre la LED externe (0V sur le GPIO 23)
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
