#include "uart_parser.h"
#include "app_config.h"

#include <stdlib.h>
#include <string.h>

const char *Find_Field(const char *line, const char *key)
{
    const char *p;
    size_t key_len;

    if (line == NULL || key == NULL)
    {
        return NULL;
    }

    key_len = strlen(key);
    p = line;

    while ((p = strstr(p, key)) != NULL)
    {
        if (p == line || *(p - 1) == ',')
        {
            return p + key_len;
        }

        p += key_len;
    }

    return NULL;
}

uint8_t Extract_Int_Field(const char *line, const char *key, int *value)
{
    const char *p;
    char *endptr;
    long v;

    if (value == NULL)
    {
        return 0;
    }

    p = Find_Field(line, key);
    if (p == NULL)
    {
        return 0;
    }

    v = strtol(p, &endptr, 10);
    if (endptr == p)
    {
        return 0;
    }

    *value = (int)v;
    return 1;
}

uint8_t Extract_Float_Field(const char *line, const char *key, float *value)
{
    const char *p;

    if (value == NULL)
    {
        return 0;
    }

    p = Find_Field(line, key);
    if (p == NULL)
    {
        return 0;
    }

    *value = (float)atof(p);
    return 1;
}

uint8_t Extract_Wing_Field(const char *line, uint8_t *wing)
{
    const char *p;

    if (wing == NULL)
    {
        return 0;
    }

    p = Find_Field(line, "W=");
    if (p == NULL)
    {
        return 0;
    }

    if (*p == 'B' || *p == 'b')
    {
        *wing = WING_B;
    }
    else
    {
        *wing = WING_A;
    }

    return 1;
}

uint8_t Parse_Vital_Line(const char *line, VitalData_t *p)
{
    int room, bed, patient, hr, spo2, battery, seq;
    float temp_c;
    uint8_t wing;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    if (strstr(line, "TYPE=VITAL") == NULL)
    {
        return 0;
    }

    if (!Extract_Wing_Field(line, &wing)) return 0;
    if (!Extract_Int_Field(line, "R=", &room)) return 0;
    if (!Extract_Int_Field(line, "B=", &bed)) return 0;
    if (!Extract_Int_Field(line, "P=", &patient)) return 0;
    if (!Extract_Int_Field(line, "HR=", &hr)) return 0;
    if (!Extract_Int_Field(line, "SPO2=", &spo2)) return 0;
    if (!Extract_Float_Field(line, "T=", &temp_c)) return 0;
    if (!Extract_Int_Field(line, "BAT=", &battery)) battery = 0;
    if (!Extract_Int_Field(line, "SEQ=", &seq)) seq = 0;

    if (room < 0 || room > 255) return 0;
    if (bed < 0 || bed > 255) return 0;
    if (patient < 0 || patient > 255) return 0;
    if (hr < 0 || hr > 300) return 0;
    if (spo2 < 0 || spo2 > 100) return 0;
    if (temp_c < -20.0f || temp_c > 80.0f) return 0;
    if (battery < 0 || battery > 100) battery = 0;
    if (seq < 0 || seq > 255) seq = 0;

    memset(p, 0, sizeof(*p));

    p->wing = wing;
    p->room = (uint8_t)room;
    p->bed = (uint8_t)bed;
    p->patient = (uint8_t)patient;
    p->hr = (uint16_t)hr;
    p->spo2 = (uint8_t)spo2;
    p->temp_x10 = (int16_t)(temp_c * 10.0f + ((temp_c >= 0.0f) ? 0.5f : -0.5f));
    p->battery = (uint8_t)battery;
    p->seq = (uint8_t)seq;
    p->status = 0;
    p->valid = 1;

    return 1;
}

uint8_t Parse_Loadcell_Line(const char *line, LoadcellData_t *p)
{
    int room, bed, patient, ml, dpm, seq;
    uint8_t wing;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    if (strstr(line, "TYPE=LOADCELL") == NULL)
    {
        return 0;
    }

    if (!Extract_Wing_Field(line, &wing)) return 0;
    if (!Extract_Int_Field(line, "R=", &room)) return 0;
    if (!Extract_Int_Field(line, "B=", &bed)) return 0;
    if (!Extract_Int_Field(line, "P=", &patient)) return 0;
    if (!Extract_Int_Field(line, "ML=", &ml)) return 0;
    if (!Extract_Int_Field(line, "DPM=", &dpm)) return 0;
    if (!Extract_Int_Field(line, "SEQ=", &seq)) seq = 0;

    if (room < 0 || room > 255) return 0;
    if (bed < 0 || bed > 255) return 0;
    if (patient < 0 || patient > 255) return 0;
    if (ml < 0 || ml > 65535) return 0;
    if (dpm < 0 || dpm > 65535) return 0;
    if (seq < 0 || seq > 255) seq = 0;

    memset(p, 0, sizeof(*p));

    p->wing = wing;
    p->room = (uint8_t)room;
    p->bed = (uint8_t)bed;
    p->patient = (uint8_t)patient;
    p->volume_ml = (uint16_t)ml;
    p->drops_per_min = (uint16_t)dpm;
    p->seq = (uint8_t)seq;
    p->status = 0;
    p->valid = 1;

    return 1;
}
