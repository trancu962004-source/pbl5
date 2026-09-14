#include "gateway_process.h"
#include "app_config.h"
#include "app_types.h"
#include "can_gateway.h"
#include "debug_uart.h"
#include "uart_parser.h"
#include "main.h"

#include <stdio.h>

void Process_UART_Line(const char *line, const char *port_name)
{
    VitalData_t vital;
    LoadcellData_t loadcell;
    char logbuf[320];
    uint32_t id = 0;

    if (line == NULL)
    {
        return;
    }

    snprintf(logbuf, sizeof(logbuf), "%s RAW | %s\r\n", port_name, line);
    UART1_Print(logbuf);

    if (Parse_Vital_Line(line, &vital))
    {
        if (CAN_SendVital(&vital, &id))
        {
            snprintf(logbuf, sizeof(logbuf),
                     "%s -> CAN VITAL OK | ID=0x%08lX W=%c R=%u B=%u P=%u HR=%u SPO2=%u T_x10=%d BAT=%u SEQ=%u\r\n",
                     port_name,
                     (unsigned long)id,
                     vital.wing == WING_B ? 'B' : 'A',
                     vital.room,
                     vital.bed,
                     vital.patient,
                     vital.hr,
                     vital.spo2,
                     vital.temp_x10,
                     vital.battery,
                     vital.seq);
        }
        else
        {
            snprintf(logbuf, sizeof(logbuf),
                     "%s -> CAN VITAL FAIL | ID=0x%08lX\r\n",
                     port_name,
                     (unsigned long)id);
        }

        UART1_Print(logbuf);
        return;
    }

    if (Parse_Loadcell_Line(line, &loadcell))
    {
        if (CAN_SendLoadcell(&loadcell, &id))
        {
            snprintf(logbuf, sizeof(logbuf),
                     "%s -> CAN LOADCELL OK | ID=0x%08lX W=%c R=%u B=%u P=%u ML=%u DPM=%u SEQ=%u\r\n",
                     port_name,
                     (unsigned long)id,
                     loadcell.wing == WING_B ? 'B' : 'A',
                     loadcell.room,
                     loadcell.bed,
                     loadcell.patient,
                     loadcell.volume_ml,
                     loadcell.drops_per_min,
                     loadcell.seq);
        }
        else
        {
            snprintf(logbuf, sizeof(logbuf),
                     "%s -> CAN LOADCELL FAIL | ID=0x%08lX\r\n",
                     port_name,
                     (unsigned long)id);
        }

        UART1_Print(logbuf);
        return;
    }

    UART1_Print("PARSE SKIP: wrong format or missing field\r\n");
    UART1_Print("Expected VITAL: TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25\r\n");
    UART1_Print("Expected LOAD : TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25\r\n");
}

void Heartbeat_Task(void)
{
#if HEARTBEAT_ENABLE
    static uint32_t last = 0;

    if ((HAL_GetTick() - last) >= 3000U)
    {
        last = HAL_GetTick();
        UART1_Print("Heartbeat: waiting UART2/UART3...\r\n");
    }
#endif
}

void Fake_UART_Test_Once(void)
{
#if ENABLE_FAKE_UART_TEST
    static uint8_t done = 0;

    if (done)
    {
        return;
    }

    done = 1;

    UART1_Print("FAKE UART TEST START\r\n");

    Process_UART_Line(
        "TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25",
        "FAKE"
    );

    HAL_Delay(20);

    Process_UART_Line(
        "TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25",
        "FAKE"
    );

    UART1_Print("FAKE UART TEST END\r\n");
#endif
}

void CAN_Ext_Test_Task(void)
{
#if ENABLE_CAN_EXT_TEST
    static uint32_t last = 0;
    uint8_t data[8];
    uint32_t id = 0;

    if ((HAL_GetTick() - last) < 1000U)
    {
        return;
    }

    last = HAL_GetTick();

    data[0] = 88;
    data[1] = 0;
    data[2] = 97;
    data[3] = 0x70;
    data[4] = 0x01;
    data[5] = 90;
    data[6] = 10;
    data[7] = 0;

    if (CAN_SendRawWait(MSG_TYPE_VITAL, WING_B, 1, 2, 3, data, &id))
    {
        UART1_Print("CAN EXT TEST OK | ID=0x05010203\r\n");
    }
    else
    {
        UART1_Print("CAN EXT TEST FAIL | ID=0x05010203\r\n");
    }
#endif
}
