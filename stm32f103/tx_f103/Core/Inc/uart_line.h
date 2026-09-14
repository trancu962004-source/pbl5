#ifndef UART_LINE_H
#define UART_LINE_H

#include <stdint.h>

extern char uart2_line_copy[];
extern char uart3_line_copy[];
extern volatile uint8_t uart2_line_ready;
extern volatile uint8_t uart3_line_ready;

void UART2_StartReceiveIT(void);
void UART3_StartReceiveIT(void);

#endif /* UART_LINE_H */
