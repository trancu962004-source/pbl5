#include "sensor_node.h"
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "SENSOR_NODE";

static uint8_t calc_status(uint16_t hr, uint16_t spo2, uint16_t temp_x100)
{
    uint8_t status = 0;

    if (hr < 50 || hr > 120) {
        status |= (1 << 0);
    }
    if (spo2 < 94) {
        status |= (1 << 1);
    }
    if (temp_x100 > 3800) {
        status |= (1 << 2);
    }

    return status;
}

esp_err_t sensor_node_init(void)
{
    ESP_LOGI(TAG, "Sensor simulation mode");
    return ESP_OK;
}

esp_err_t sensor_node_read(vital_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out->hr = 70 + (esp_random() % 31);         // 70..100
    out->spo2 = 95 + (esp_random() % 5);        // 95..99
    out->temp_x100 = 3600 + (esp_random() % 151); // 36.00..37.50
    out->battery = 80 + (esp_random() % 21);    // 80..100
    out->status = calc_status(out->hr, out->spo2, out->temp_x100);

    return ESP_OK;
}