#include "can_gateway.h"
#include "app_config.h"
#include "app_handles.h"
#include "debug_uart.h"

#include <stdio.h>
#include <string.h>

void CAN_Print_Error(const char *prefix)
{
    char buf[220];

    snprintf(buf, sizeof(buf),
             "%s | HAL_ERR=0x%08lX | ESR=0x%08lX | MSR=0x%08lX | TSR=0x%08lX\r\n",
             prefix,
             (unsigned long)HAL_CAN_GetError(&hcan),
             (unsigned long)CAN1->ESR,
             (unsigned long)CAN1->MSR,
             (unsigned long)CAN1->TSR);

    UART1_Print(buf);
}

uint8_t CAN_Filter_Config_AllPass(void)
{
    CAN_FilterTypeDef filter;

    memset(&filter, 0, sizeof(filter));

    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0x0000;
    filter.FilterIdLow = 0x0000;
    filter.FilterMaskIdHigh = 0x0000;
    filter.FilterMaskIdLow = 0x0000;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK)
    {
        return 0;
    }

    return 1;
}

uint32_t CAN_MakeExtId(uint8_t msg_type,
                       uint8_t wing,
                       uint8_t room,
                       uint8_t bed,
                       uint8_t patient)
{
    return (((uint32_t)(msg_type & 0x07U)) << 26) |
           (((uint32_t)(wing & 0x03U)) << 24) |
           (((uint32_t)room) << 16) |
           (((uint32_t)bed) << 8) |
           ((uint32_t)patient);
}

void CAN_PackVital(const VitalData_t *p, uint8_t data[8])
{
    data[0] = (uint8_t)(p->hr & 0xFFU);
    data[1] = (uint8_t)((p->hr >> 8) & 0xFFU);
    data[2] = p->spo2;
    data[3] = (uint8_t)(p->temp_x10 & 0xFF);
    data[4] = (uint8_t)((p->temp_x10 >> 8) & 0xFF);
    data[5] = p->battery;
    data[6] = p->seq;
    data[7] = p->status;
}

void CAN_PackLoadcell(const LoadcellData_t *p, uint8_t data[8])
{
    data[0] = (uint8_t)(p->volume_ml & 0xFFU);
    data[1] = (uint8_t)((p->volume_ml >> 8) & 0xFFU);
    data[2] = (uint8_t)(p->drops_per_min & 0xFFU);
    data[3] = (uint8_t)((p->drops_per_min >> 8) & 0xFFU);
    data[4] = p->seq;
    data[5] = p->status;
    data[6] = 0;
    data[7] = 0;
}

uint8_t CAN_SendRawWait(uint8_t msg_type,
                        uint8_t wing,
                        uint8_t room,
                        uint8_t bed,
                        uint8_t patient,
                        const uint8_t data[8],
                        uint32_t *out_id)
{
    CAN_TxHeaderTypeDef txHeader;
    uint32_t txMailbox;
    uint32_t id;
    uint32_t start_tick;

    if (data == NULL)
    {
        return 0;
    }

    id = CAN_MakeExtId(msg_type, wing, room, bed, patient);

    if (out_id != NULL)
    {
        *out_id = id;
    }

    memset(&txHeader, 0, sizeof(txHeader));

    txHeader.ExtId = id;
    txHeader.StdId = 0;
    txHeader.IDE = CAN_ID_EXT;
    txHeader.RTR = CAN_RTR_DATA;
    txHeader.DLC = 8;
    txHeader.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U)
    {
        UART1_Print("CAN TX mailbox full -> abort all pending TX\r\n");

        HAL_CAN_AbortTxRequest(&hcan,
                               CAN_TX_MAILBOX0 |
                               CAN_TX_MAILBOX1 |
                               CAN_TX_MAILBOX2);

        CAN_Print_Error("CAN TX mailbox full detail");
        return 0;
    }

    if (HAL_CAN_AddTxMessage(&hcan, &txHeader, (uint8_t *)data, &txMailbox) != HAL_OK)
    {
        CAN_Print_Error("CAN AddTxMessage failed");
        return 0;
    }

    start_tick = HAL_GetTick();

    while (HAL_CAN_IsTxMessagePending(&hcan, txMailbox))
    {
        if ((HAL_GetTick() - start_tick) > CAN_TX_TIMEOUT_MS)
        {
            UART1_Print("CAN TX timeout -> abort TX\r\n");
            HAL_CAN_AbortTxRequest(&hcan, txMailbox);
            CAN_Print_Error("CAN TX timeout detail");
            return 0;
        }
    }

    return 1;
}

uint8_t CAN_SendVital(const VitalData_t *p, uint32_t *out_id)
{
    uint8_t data[8];

    if (p == NULL || p->valid == 0U)
    {
        return 0;
    }

    CAN_PackVital(p, data);

    return CAN_SendRawWait(MSG_TYPE_VITAL,
                           p->wing,
                           p->room,
                           p->bed,
                           p->patient,
                           data,
                           out_id);
}

uint8_t CAN_SendLoadcell(const LoadcellData_t *p, uint32_t *out_id)
{
    uint8_t data[8];

    if (p == NULL || p->valid == 0U)
    {
        return 0;
    }

    CAN_PackLoadcell(p, data);

    return CAN_SendRawWait(MSG_TYPE_LOADCELL,
                           p->wing,
                           p->room,
                           p->bed,
                           p->patient,
                           data,
                           out_id);
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan_ptr)
{
    (void)hcan_ptr;
    CAN_Print_Error("CAN ERROR CALLBACK");
}
