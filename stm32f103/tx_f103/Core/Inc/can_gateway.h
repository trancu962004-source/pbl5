#ifndef CAN_GATEWAY_H
#define CAN_GATEWAY_H

#include <stdint.h>
#include "app_types.h"

void CAN_Print_Error(const char *prefix);
uint8_t CAN_Filter_Config_AllPass(void);
uint32_t CAN_MakeExtId(uint8_t msg_type,
                       uint8_t wing,
                       uint8_t room,
                       uint8_t bed,
                       uint8_t patient);
void CAN_PackVital(const VitalData_t *p, uint8_t data[8]);
void CAN_PackLoadcell(const LoadcellData_t *p, uint8_t data[8]);
uint8_t CAN_SendRawWait(uint8_t msg_type,
                        uint8_t wing,
                        uint8_t room,
                        uint8_t bed,
                        uint8_t patient,
                        const uint8_t data[8],
                        uint32_t *out_id);
uint8_t CAN_SendVital(const VitalData_t *p, uint32_t *out_id);
uint8_t CAN_SendLoadcell(const LoadcellData_t *p, uint32_t *out_id);

#endif /* CAN_GATEWAY_H */
