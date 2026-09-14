#ifndef __UTILS_H
#define __UTILS_H

#include "stm32f1xx_hal.h"

#define MCAR_NULL   0
#define NULL_MCAR   1
#define MCAR_OCAR   2
#define OCAR_MCAR   3
#define OCAR_NULL   4
#define NULL_OCAR   5
#define OCAR_OCAR   6
#define NULL_NULL   7

extern uint8_t Tmycar_Bempty[8];
extern uint8_t Tempty_Bmycar[8];
extern uint8_t Tmycar_Bcar[8];
extern uint8_t Tcar_Bmycar[8];
extern uint8_t Tcar_Bempty[8];
extern uint8_t Tempty_Bcar[8];
extern uint8_t Tempty_Bempty[8];
extern uint8_t Tcar_Bcar[8];

#endif
