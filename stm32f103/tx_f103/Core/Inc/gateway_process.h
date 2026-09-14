#ifndef GATEWAY_PROCESS_H
#define GATEWAY_PROCESS_H

void Process_UART_Line(const char *line, const char *port_name);
void Heartbeat_Task(void);
void Fake_UART_Test_Once(void);
void CAN_Ext_Test_Task(void);

#endif /* GATEWAY_PROCESS_H */
