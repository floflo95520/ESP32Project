#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GPIO_BASE       0x3FF44000

#define GPIO_OUT_W1TS   (*(volatile uint32_t *)(GPIO_BASE + 0x0008))
#define GPIO_ENABLE_W1TS (*(volatile uint32_t *)(GPIO_BASE + 0x0024))
#define GPIO_OUT_W1TC      (*(volatile uint32_t *)(GPIO_BASE + 0x000C))

#define LED_GPIO 2

void app_main(void)
{
    // GPIO2 comme sortie
    GPIO_ENABLE_W1TS = (1 << LED_GPIO);

    
    while (1)
    {
        // GPIO2 = HIGH → LED allumée
        GPIO_OUT_W1TS = (1 << LED_GPIO);

        vTaskDelay(pdMS_TO_TICKS(500));

        // GPIO2 = LOW → LED éteinte
        GPIO_OUT_W1TC = (1 << LED_GPIO);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}