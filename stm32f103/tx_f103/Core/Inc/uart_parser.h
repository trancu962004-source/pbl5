#ifndef UART_PARSER_H
#define UART_PARSER_H

#include <stdint.h>
#include "app_types.h"

const char *Find_Field(const char *line, const char *key);
uint8_t Extract_Int_Field(const char *line, const char *key, int *value);
uint8_t Extract_Float_Field(const char *line, const char *key, float *value);
uint8_t Extract_Wing_Field(const char *line, uint8_t *wing);
uint8_t Parse_Vital_Line(const char *line, VitalData_t *p);
uint8_t Parse_Loadcell_Line(const char *line, LoadcellData_t *p);

#endif /* UART_PARSER_H */
