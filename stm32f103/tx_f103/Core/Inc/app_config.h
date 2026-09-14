#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#define UART_LINE_MAX_LEN          180U

#define MSG_TYPE_VITAL             1U
#define MSG_TYPE_LOADCELL          2U

#define WING_A                     0U
#define WING_B                     1U

#define ENABLE_UART_BYTE_ECHO      0
#define ENABLE_FAKE_UART_TEST      0
#define ENABLE_CAN_EXT_TEST        0
#define HEARTBEAT_ENABLE           1
#define CAN_TX_TIMEOUT_MS          100U

#endif /* APP_CONFIG_H */
