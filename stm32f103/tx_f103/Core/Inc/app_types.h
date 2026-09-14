#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>

typedef struct
{
    uint8_t wing;
    uint8_t room;
    uint8_t bed;
    uint8_t patient;

    uint16_t hr;
    uint8_t spo2;
    int16_t temp_x10;
    uint8_t battery;
    uint8_t seq;
    uint8_t status;
    uint8_t valid;
} VitalData_t;

typedef struct
{
    uint8_t wing;
    uint8_t room;
    uint8_t bed;
    uint8_t patient;

    uint16_t volume_ml;
    uint16_t drops_per_min;
    uint8_t seq;
    uint8_t status;
    uint8_t valid;
} LoadcellData_t;

#endif /* APP_TYPES_H */
