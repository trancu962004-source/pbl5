#ifndef SENSOR_NODE_H
#define SENSOR_NODE_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t hr;
    uint16_t spo2;
    uint16_t temp_x100;
    uint8_t  status;
    uint8_t  battery;
} vital_data_t;

esp_err_t sensor_node_init(void);
esp_err_t sensor_node_read(vital_data_t *out);

#endif