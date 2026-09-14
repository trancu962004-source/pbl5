#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

void UART1_Print(const char *msg);
void UART1_EchoByte(uint8_t port, uint8_t ch);

#endif /* DEBUG_UART_H */
