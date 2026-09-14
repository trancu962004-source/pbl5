#ifndef MAX30102_H
#define MAX30102_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t red;
    uint32_t ir;
} max30102_sample_t;

esp_err_t max30102_init(void);
esp_err_t max30102_read_sample(max30102_sample_t *sample);

#endif