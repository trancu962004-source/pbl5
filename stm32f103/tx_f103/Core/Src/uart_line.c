#include "uart_line.h"
#include "app_config.h"
#include "app_handles.h"
#include "debug_uart.h"

#include <string.h>

static uint8_t uart2_rx_byte;
static char uart2_rx_line[UART_LINE_MAX_LEN];
char uart2_line_copy[UART_LINE_MAX_LEN];
static volatile uint16_t uart2_rx_index = 0;
volatile uint8_t uart2_line_ready = 0;

static uint8_t uart3_rx_byte;
static char uart3_rx_line[UART_LINE_MAX_LEN];
char uart3_line_copy[UART_LINE_MAX_LEN];
static volatile uint16_t uart3_rx_index = 0;
volatile uint8_t uart3_line_ready = 0;

void UART2_StartReceiveIT(void)
{
    if (HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1) != HAL_OK)
    {
        UART1_Print("UART2 RX IT start failed\r\n");
    }
}

void UART3_StartReceiveIT(void)
{
    if (HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1) != HAL_OK)
    {
        UART1_Print("UART3 RX IT start failed\r\n");
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        UART1_EchoByte(2, uart2_rx_byte);

        if (uart2_rx_byte == '\n')
        {
            uart2_rx_line[uart2_rx_index] = '\0';

            strncpy(uart2_line_copy, uart2_rx_line, sizeof(uart2_line_copy) - 1U);
            uart2_line_copy[sizeof(uart2_line_copy) - 1U] = '\0';

            uart2_rx_index = 0;
            uart2_line_ready = 1;
        }
        else if (uart2_rx_byte != '\r')
        {
            if (uart2_rx_index < (sizeof(uart2_rx_line) - 1U))
            {
                uart2_rx_line[uart2_rx_index++] = (char)uart2_rx_byte;
            }
            else
            {
                uart2_rx_index = 0;
                UART1_Print("\r\nUART2 line overflow -> reset\r\n");
            }
        }

        UART2_StartReceiveIT();
    }
    else if (huart->Instance == USART3)
    {
        UART1_EchoByte(3, uart3_rx_byte);

        if (uart3_rx_byte == '\n')
        {
            uart3_rx_line[uart3_rx_index] = '\0';

            strncpy(uart3_line_copy, uart3_rx_line, sizeof(uart3_line_copy) - 1U);
            uart3_line_copy[sizeof(uart3_line_copy) - 1U] = '\0';

            uart3_rx_index = 0;
            uart3_line_ready = 1;
        }
        else if (uart3_rx_byte != '\r')
        {
            if (uart3_rx_index < (sizeof(uart3_rx_line) - 1U))
            {
                uart3_rx_line[uart3_rx_index++] = (char)uart3_rx_byte;
            }
            else
            {
                uart3_rx_index = 0;
                UART1_Print("\r\nUART3 line overflow -> reset\r\n");
            }
        }

        UART3_StartReceiveIT();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        UART1_Print("UART2 ERROR -> restart RX\r\n");
        UART2_StartReceiveIT();
    }
    else if (huart->Instance == USART3)
    {
        UART1_Print("UART3 ERROR -> restart RX\r\n");
        UART3_StartReceiveIT();
    }
}
