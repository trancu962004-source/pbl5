#ifndef MAX30205_H
#define MAX30205_H

#include "esp_err.h"

esp_err_t max30205_init(void);
esp_err_t max30205_read_temperature_c(float *temp_c);

#endif