/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : STM32F103 UART2/UART3 -> CAN Extended Gateway
  *
  * Dùng khi ESP gửi format cũ hoặc format gói ngắn:
  *
  *   Format cũ:
  *   TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,FALL=0,SOS=1,SEQ=25\r\n
  *   TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25\r\n
  *
  *   Format ngắn:
  *   1,W,R,B,P,HR,SPO2,TEMP, BAT,FALL,SOS,SEQ\r\n
  *   2,W,R,B,P,ML,DPM,SEQ\r\n
  *
  *   Trong đó TYPE 1 = VITAL, TYPE 2 = LOADCELL.
  *   FALL/SOS dùng 1 = có cảnh báo, 0 = không cảnh báo.
  *
  * Luồng:
  *   ESP A TX -> STM32 USART2_RX PA3
  *   ESP B TX -> STM32 USART3_RX PB11
  *   STM32 parse UART line -> đóng gói CAN Extended ID -> gửi CAN bus
  *
  * CAN Extended ID 29-bit:
  *   bit 28..26 : msg_type   1=VITAL, 2=LOADCELL
  *   bit 25..24 : wing       0=A, 1=B
  *   bit 23..16 : room       0..255
  *   bit 15..8  : bed        0..255
  *   bit 7..0   : patient    0..255
  *
  * CAN data VITAL:
  *   byte 0: HR low
  *   byte 1: HR high
  *   byte 2: SpO2
  *   byte 3: Temp x10 low
  *   byte 4: Temp x10 high
  *   byte 5: Battery
  *   byte 6: Seq
  *   byte 7: Status
  *           bit3 = FALL
  *           bit4 = SOS
  *
  * CAN data LOADCELL:
  *   byte 0: Volume ml low
  *   byte 1: Volume ml high
  *   byte 2: Drops/min low
  *   byte 3: Drops/min high
  *   byte 4: Seq
  *   byte 5: Status
  *   byte 6: 0
  *   byte 7: 0
  *
  * CAN bitrate:
  *   125 kbps @ APB1 = 36 MHz
  *
  * UART:
  *   USART1 debug: 115200, TX=PA9
  *   USART2 ESP A: 115200, RX=PA3
  *   USART3 ESP B: 115200, RX=PB11
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;

/* ===================== CONFIG ===================== */
#define UART_LINE_MAX_LEN          180U

#define MSG_TYPE_VITAL             1U
#define MSG_TYPE_LOADCELL          2U

#define STATUS_BIT_HR_ABNORMAL     (1U << 0)
#define STATUS_BIT_SPO2_LOW        (1U << 1)
#define STATUS_BIT_TEMP_HIGH       (1U << 2)
#define STATUS_BIT_FALL            (1U << 3)
#define STATUS_BIT_SOS             (1U << 4)

#define WING_A                     0U
#define WING_B                     1U

/*
  1: in từng byte UART2/UART3 ra UART1 để debug.
  0: chỉ in khi nhận đủ dòng và parse/gửi CAN.
*/
#define ENABLE_UART_BYTE_ECHO      0

/*
  1: tự test parser + CAN không cần ESP.
  0: chạy thật, chờ ESP gửi UART.
*/
#define ENABLE_FAKE_UART_TEST      0

/*
  1: tự gửi CAN EXT test 0x05010203 mỗi giây.
  0: chạy thật.
*/
#define ENABLE_CAN_EXT_TEST        0

#define HEARTBEAT_ENABLE           1
#define CAN_TX_TIMEOUT_MS          100U

/* ===================== UART2 RX ===================== */
static uint8_t uart2_rx_byte;
static char uart2_rx_line[UART_LINE_MAX_LEN];
static char uart2_line_copy[UART_LINE_MAX_LEN];
static volatile uint16_t uart2_rx_index = 0;
static volatile uint8_t uart2_line_ready = 0;

/* ===================== UART3 RX ===================== */
static uint8_t uart3_rx_byte;
static char uart3_rx_line[UART_LINE_MAX_LEN];
static char uart3_line_copy[UART_LINE_MAX_LEN];
static volatile uint16_t uart3_rx_index = 0;
static volatile uint8_t uart3_line_ready = 0;

/* ===================== DATA STRUCT ===================== */
typedef struct
{
    uint8_t wing;
    uint8_t room;
    uint8_t bed;
    uint8_t patient;

    uint16_t hr;
    uint8_t spo2;
    int16_t temp_x10;
    uint8_t battery;
    uint8_t seq;
    uint8_t status;
    uint8_t valid;
} VitalData_t;

typedef struct
{
    uint8_t wing;
    uint8_t room;
    uint8_t bed;
    uint8_t patient;

    uint16_t volume_ml;
    uint16_t drops_per_min;
    uint8_t seq;
    uint8_t status;
    uint8_t valid;
} LoadcellData_t;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

static void MX_GPIO_Init(void);
static void MX_CAN_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);

static void UART1_Print(const char *msg);
static void UART1_EchoByte(uint8_t port, uint8_t ch);

static void UART2_StartReceiveIT(void);
static void UART3_StartReceiveIT(void);

static void CAN_Print_Error(const char *prefix);
static uint8_t CAN_Filter_Config_AllPass(void);
static uint32_t CAN_MakeExtId(uint8_t msg_type,
                              uint8_t wing,
                              uint8_t room,
                              uint8_t bed,
                              uint8_t patient);

static const char *Find_Field(const char *line, const char *key);
static uint8_t Extract_Int_Field(const char *line, const char *key, int *value);
static uint8_t Extract_Float_Field(const char *line, const char *key, float *value);
static uint8_t Extract_Wing_Field(const char *line, uint8_t *wing);

static char *Trim_Token(char *s);
static uint8_t Split_CSV_Line(const char *line, char *buf, size_t buf_len, char *argv[], uint8_t max_arg);
static uint8_t Parse_Wing_Token(const char *token, uint8_t *wing);
static uint8_t Parse_Int_Token(const char *token, int min_v, int max_v, int *out);
static uint8_t TempRaw_To_TempX10(int temp_raw, int16_t *temp_x10);

static uint8_t Parse_Vital_Compact_Line(const char *line, VitalData_t *p);
static uint8_t Parse_Loadcell_Compact_Line(const char *line, LoadcellData_t *p);
static uint8_t Parse_Vital_Line(const char *line, VitalData_t *p);
static uint8_t Parse_Loadcell_Line(const char *line, LoadcellData_t *p);

static void CAN_PackVital(const VitalData_t *p, uint8_t data[8]);
static void CAN_PackLoadcell(const LoadcellData_t *p, uint8_t data[8]);

static uint8_t CAN_SendRawWait(uint8_t msg_type,
                               uint8_t wing,
                               uint8_t room,
                               uint8_t bed,
                               uint8_t patient,
                               const uint8_t data[8],
                               uint32_t *out_id);

static uint8_t CAN_SendVital(const VitalData_t *p, uint32_t *out_id);
static uint8_t CAN_SendLoadcell(const LoadcellData_t *p, uint32_t *out_id);

static void Process_UART_Line(const char *line, const char *port_name);

static void Heartbeat_Task(void);
static void Fake_UART_Test_Once(void);
static void CAN_Ext_Test_Task(void);

/* USER CODE BEGIN 0 */

static void UART1_Print(const char *msg)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

static void UART1_EchoByte(uint8_t port, uint8_t ch)
{
#if ENABLE_UART_BYTE_ECHO
    static uint8_t u2_new_line = 1;
    static uint8_t u3_new_line = 1;

    if (port == 2U)
    {
        if (u2_new_line)
        {
            UART1_Print("\r\n[UART2 RX] ");
            u2_new_line = 0;
        }

        HAL_UART_Transmit(&huart1, &ch, 1, 10);

        if (ch == '\n')
        {
            u2_new_line = 1;
        }
    }
    else if (port == 3U)
    {
        if (u3_new_line)
        {
            UART1_Print("\r\n[UART3 RX] ");
            u3_new_line = 0;
        }

        HAL_UART_Transmit(&huart1, &ch, 1, 10);

        if (ch == '\n')
        {
            u3_new_line = 1;
        }
    }
#else
    (void)port;
    (void)ch;
#endif
}

static void CAN_Print_Error(const char *prefix)
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

static void UART2_StartReceiveIT(void)
{
    if (HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1) != HAL_OK)
    {
        UART1_Print("UART2 RX IT start failed\r\n");
    }
}

static void UART3_StartReceiveIT(void)
{
    if (HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1) != HAL_OK)
    {
        UART1_Print("UART3 RX IT start failed\r\n");
    }
}

static uint8_t CAN_Filter_Config_AllPass(void)
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

static uint32_t CAN_MakeExtId(uint8_t msg_type,
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

/*
  Tìm field chính xác theo key.
  key chỉ match khi ở đầu dòng hoặc đứng sau dấu phẩy.
  Ví dụ "B=" sẽ không ăn nhầm trong "BAT=".
*/
static const char *Find_Field(const char *line, const char *key)
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

static uint8_t Extract_Int_Field(const char *line, const char *key, int *value)
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

static uint8_t Extract_Float_Field(const char *line, const char *key, float *value)
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

static uint8_t Extract_Wing_Field(const char *line, uint8_t *wing)
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


static char *Trim_Token(char *s)
{
    char *end;

    if (s == NULL)
    {
        return NULL;
    }

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    {
        s++;
    }

    end = s + strlen(s);
    while (end > s && (*(end - 1) == ' ' || *(end - 1) == '\t' ||
                       *(end - 1) == '\r' || *(end - 1) == '\n'))
    {
        end--;
    }

    *end = '\0';
    return s;
}

static uint8_t Split_CSV_Line(const char *line, char *buf, size_t buf_len, char *argv[], uint8_t max_arg)
{
    uint8_t argc = 0;
    char *token;

    if (line == NULL || buf == NULL || argv == NULL || buf_len == 0U || max_arg == 0U)
    {
        return 0;
    }

    strncpy(buf, line, buf_len - 1U);
    buf[buf_len - 1U] = '\0';

    token = strtok(buf, ",");
    while (token != NULL && argc < max_arg)
    {
        argv[argc++] = Trim_Token(token);
        token = strtok(NULL, ",");
    }

    return argc;
}

static uint8_t Parse_Wing_Token(const char *token, uint8_t *wing)
{
    if (token == NULL || wing == NULL)
    {
        return 0;
    }

    if (token[0] == 'B' || token[0] == 'b' || token[0] == '1')
    {
        *wing = WING_B;
        return 1;
    }

    if (token[0] == 'A' || token[0] == 'a' || token[0] == '0')
    {
        *wing = WING_A;
        return 1;
    }

    return 0;
}

static uint8_t Parse_Int_Token(const char *token, int min_v, int max_v, int *out)
{
    char *endptr;
    long v;

    if (token == NULL || out == NULL)
    {
        return 0;
    }

    v = strtol(token, &endptr, 10);
    if (endptr == token)
    {
        return 0;
    }

    if (v < min_v || v > max_v)
    {
        return 0;
    }

    *out = (int)v;
    return 1;
}

static uint8_t TempRaw_To_TempX10(int temp_raw, int16_t *temp_x10)
{
    int val_x10;

    if (temp_x10 == NULL)
    {
        return 0;
    }

    /*
      ESP có thể gửi nhiệt độ dạng x100, ví dụ 3650 = 36.50 C,
      hoặc dạng x10, ví dụ 365 = 36.5 C.
      CAN cũ vẫn giữ byte nhiệt độ dạng x10.
    */
    if (temp_raw > 1000 || temp_raw < -1000)
    {
        if (temp_raw >= 0)
        {
            val_x10 = (temp_raw + 5) / 10;
        }
        else
        {
            val_x10 = (temp_raw - 5) / 10;
        }
    }
    else
    {
        val_x10 = temp_raw;
    }

    if (val_x10 < -200 || val_x10 > 800)
    {
        return 0;
    }

    *temp_x10 = (int16_t)val_x10;
    return 1;
}

static uint8_t Parse_Vital_Compact_Line(const char *line, VitalData_t *p)
{
    char buf[UART_LINE_MAX_LEN];
    char *argv[14];
    uint8_t argc;
    uint8_t wing;
    int type, room, bed, patient, hr, spo2, temp_raw, battery, fall, sos, seq;
    int16_t temp_x10;
    uint8_t status = 0;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    argc = Split_CSV_Line(line, buf, sizeof(buf), argv, 14U);
    if (argc < 12U)
    {
        return 0;
    }

    if (!Parse_Int_Token(argv[0], 1, 1, &type)) return 0;
    (void)type;
    if (!Parse_Wing_Token(argv[1], &wing)) return 0;
    if (!Parse_Int_Token(argv[2], 0, 255, &room)) return 0;
    if (!Parse_Int_Token(argv[3], 0, 255, &bed)) return 0;
    if (!Parse_Int_Token(argv[4], 0, 255, &patient)) return 0;
    if (!Parse_Int_Token(argv[5], 0, 300, &hr)) return 0;
    if (!Parse_Int_Token(argv[6], 0, 100, &spo2)) return 0;
    if (!Parse_Int_Token(argv[7], -20000, 80000, &temp_raw)) return 0;
    if (!TempRaw_To_TempX10(temp_raw, &temp_x10)) return 0;
    if (!Parse_Int_Token(argv[8], 0, 100, &battery)) battery = 0;
    if (!Parse_Int_Token(argv[9], 0, 1, &fall)) fall = 0;
    if (!Parse_Int_Token(argv[10], 0, 1, &sos)) sos = 0;
    if (!Parse_Int_Token(argv[11], 0, 255, &seq)) seq = 0;

    if (fall) status |= STATUS_BIT_FALL;
    if (sos)  status |= STATUS_BIT_SOS;

    memset(p, 0, sizeof(*p));
    p->wing = wing;
    p->room = (uint8_t)room;
    p->bed = (uint8_t)bed;
    p->patient = (uint8_t)patient;
    p->hr = (uint16_t)hr;
    p->spo2 = (uint8_t)spo2;
    p->temp_x10 = temp_x10;
    p->battery = (uint8_t)battery;
    p->seq = (uint8_t)seq;
    p->status = status;
    p->valid = 1U;

    return 1;
}

static uint8_t Parse_Loadcell_Compact_Line(const char *line, LoadcellData_t *p)
{
    char buf[UART_LINE_MAX_LEN];
    char *argv[10];
    uint8_t argc;
    uint8_t wing;
    int type, room, bed, patient, ml, dpm, seq;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    argc = Split_CSV_Line(line, buf, sizeof(buf), argv, 10U);
    if (argc < 8U)
    {
        return 0;
    }

    if (!Parse_Int_Token(argv[0], 2, 2, &type)) return 0;
    (void)type;
    if (!Parse_Wing_Token(argv[1], &wing)) return 0;
    if (!Parse_Int_Token(argv[2], 0, 255, &room)) return 0;
    if (!Parse_Int_Token(argv[3], 0, 255, &bed)) return 0;
    if (!Parse_Int_Token(argv[4], 0, 255, &patient)) return 0;
    if (!Parse_Int_Token(argv[5], 0, 65535, &ml)) return 0;
    if (!Parse_Int_Token(argv[6], 0, 65535, &dpm)) return 0;
    if (!Parse_Int_Token(argv[7], 0, 255, &seq)) seq = 0;

    memset(p, 0, sizeof(*p));
    p->wing = wing;
    p->room = (uint8_t)room;
    p->bed = (uint8_t)bed;
    p->patient = (uint8_t)patient;
    p->volume_ml = (uint16_t)ml;
    p->drops_per_min = (uint16_t)dpm;
    p->seq = (uint8_t)seq;
    p->status = 0U;
    p->valid = 1U;

    return 1;
}

static uint8_t Parse_Vital_Line(const char *line, VitalData_t *p)
{
    int room, bed, patient, hr, spo2, battery, seq;
    int fall = 0;
    int sos = 0;
    int status = 0;
    float temp_c;
    uint8_t wing;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    /* Format ngắn: 1,W,R,B,P,HR,SPO2,TEMP,BAT,FALL,SOS,SEQ */
    if (Parse_Vital_Compact_Line(line, p))
    {
        return 1;
    }

    /* Format cũ: TYPE=VITAL,... */
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
    if (!Extract_Int_Field(line, "STATUS=", &status))
    {
        if (!Extract_Int_Field(line, "ST=", &status)) status = 0;
    }
    if (!Extract_Int_Field(line, "FALL=", &fall)) fall = 0;
    if (!Extract_Int_Field(line, "SOS=", &sos)) sos = 0;

    if (room < 0 || room > 255) return 0;
    if (bed < 0 || bed > 255) return 0;
    if (patient < 0 || patient > 255) return 0;
    if (hr < 0 || hr > 300) return 0;
    if (spo2 < 0 || spo2 > 100) return 0;
    if (temp_c < -20.0f || temp_c > 80.0f) return 0;
    if (battery < 0 || battery > 100) battery = 0;
    if (seq < 0 || seq > 255) seq = 0;
    if (status < 0 || status > 255) status = 0;

    if (fall > 0) status |= STATUS_BIT_FALL;
    if (sos > 0)  status |= STATUS_BIT_SOS;

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
    p->status = (uint8_t)status;
    p->valid = 1;

    return 1;
}

static uint8_t Parse_Loadcell_Line(const char *line, LoadcellData_t *p)
{
    int room, bed, patient, ml, dpm, seq;
    int status = 0;
    uint8_t wing;

    if (line == NULL || p == NULL)
    {
        return 0;
    }

    /* Format ngắn: 2,W,R,B,P,ML,DPM,SEQ */
    if (Parse_Loadcell_Compact_Line(line, p))
    {
        return 1;
    }

    /* Format cũ: TYPE=LOADCELL,... */
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
    if (!Extract_Int_Field(line, "STATUS=", &status))
    {
        if (!Extract_Int_Field(line, "ST=", &status)) status = 0;
    }

    if (room < 0 || room > 255) return 0;
    if (bed < 0 || bed > 255) return 0;
    if (patient < 0 || patient > 255) return 0;
    if (ml < 0 || ml > 65535) return 0;
    if (dpm < 0 || dpm > 65535) return 0;
    if (seq < 0 || seq > 255) seq = 0;
    if (status < 0 || status > 255) status = 0;

    memset(p, 0, sizeof(*p));

    p->wing = wing;
    p->room = (uint8_t)room;
    p->bed = (uint8_t)bed;
    p->patient = (uint8_t)patient;
    p->volume_ml = (uint16_t)ml;
    p->drops_per_min = (uint16_t)dpm;
    p->seq = (uint8_t)seq;
    p->status = (uint8_t)status;
    p->valid = 1;

    return 1;
}

static void CAN_PackVital(const VitalData_t *p, uint8_t data[8])
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

static void CAN_PackLoadcell(const LoadcellData_t *p, uint8_t data[8])
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

static uint8_t CAN_SendRawWait(uint8_t msg_type,
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

static uint8_t CAN_SendVital(const VitalData_t *p, uint32_t *out_id)
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

static uint8_t CAN_SendLoadcell(const LoadcellData_t *p, uint32_t *out_id)
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

static void Process_UART_Line(const char *line, const char *port_name)
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
                     "%s -> CAN VITAL OK | ID=0x%08lX W=%c R=%u B=%u P=%u HR=%u SPO2=%u T_x10=%d BAT=%u FALL=%u SOS=%u STATUS=0x%02X SEQ=%u\r\n",
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
                     (vital.status & STATUS_BIT_FALL) ? 1U : 0U,
                     (vital.status & STATUS_BIT_SOS) ? 1U : 0U,
                     vital.status,
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
                     "%s -> CAN LOADCELL OK | ID=0x%08lX W=%c R=%u B=%u P=%u ML=%u DPM=%u STATUS=0x%02X SEQ=%u\r\n",
                     port_name,
                     (unsigned long)id,
                     loadcell.wing == WING_B ? 'B' : 'A',
                     loadcell.room,
                     loadcell.bed,
                     loadcell.patient,
                     loadcell.volume_ml,
                     loadcell.drops_per_min,
                     loadcell.status,
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
    UART1_Print("Expected VITAL old: TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,FALL=0,SOS=1,SEQ=25\r\n");
    UART1_Print("Expected VITAL new: 1,A,1,2,4,80,97,3680,100,0,1,25\r\n");
    UART1_Print("Expected LOAD  old: TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25\r\n");
    UART1_Print("Expected LOAD  new: 2,B,1,2,4,450,25,25\r\n");
}

static void Heartbeat_Task(void)
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

static void Fake_UART_Test_Once(void)
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
        "TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,FALL=1,SOS=0,SEQ=25",
        "FAKE"
    );

    HAL_Delay(20);

    Process_UART_Line(
        "1,B,1,2,4,82,98,3650,99,0,1,26",
        "FAKE"
    );

    HAL_Delay(20);

    Process_UART_Line(
        "TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25",
        "FAKE"
    );

    HAL_Delay(20);

    Process_UART_Line(
        "2,A,1,2,4,460,26,27",
        "FAKE"
    );

    UART1_Print("FAKE UART TEST END\r\n");
#endif
}

static void CAN_Ext_Test_Task(void)
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

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan_ptr)
{
    (void)hcan_ptr;
    CAN_Print_Error("CAN ERROR CALLBACK");
}

/* USER CODE END 0 */

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_CAN_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();

    UART1_Print("\r\n========================================\r\n");
    UART1_Print("STM32 UART2/UART3 -> CAN EXT GATEWAY FINAL\r\n");
    UART1_Print("CAN bitrate: 125 kbps\r\n");
    UART1_Print("USART1 debug: 115200 TX=PA9\r\n");
    UART1_Print("USART2 ESP A/B input: 115200 RX=PA3\r\n");
    UART1_Print("USART3 ESP A/B input: 115200 RX=PB11\r\n");
#if ENABLE_UART_BYTE_ECHO
    UART1_Print("UART byte echo: ON\r\n");
#else
    UART1_Print("UART byte echo: OFF\r\n");
#endif
    UART1_Print("========================================\r\n");

    if (!CAN_Filter_Config_AllPass())
    {
        UART1_Print("CAN filter config failed\r\n");
        Error_Handler();
    }

    if (HAL_CAN_Start(&hcan) != HAL_OK)
    {
        CAN_Print_Error("CAN start failed");
        Error_Handler();
    }

    UART1_Print("CAN started\r\n");

    if (HAL_CAN_ActivateNotification(&hcan,
                                     CAN_IT_ERROR |
                                     CAN_IT_BUSOFF |
                                     CAN_IT_LAST_ERROR_CODE) != HAL_OK)
    {
        CAN_Print_Error("CAN notification failed");
        Error_Handler();
    }

    UART2_StartReceiveIT();
    UART3_StartReceiveIT();

    UART1_Print("UART2/UART3 RX interrupt started\r\n");
    UART1_Print("Ready. ESP lines must end with CRLF or LF.\r\n");
    UART1_Print("Support packets: old TYPE=... and short 1/2 CSV format.\r\n");

    while (1)
    {
        Fake_UART_Test_Once();
        CAN_Ext_Test_Task();

        if (uart2_line_ready)
        {
            uart2_line_ready = 0;
            Process_UART_Line(uart2_line_copy, "UART2");
        }

        if (uart3_line_ready)
        {
            uart3_line_ready = 0;
            Process_UART_Line(uart3_line_copy, "UART3");
        }

        Heartbeat_Task();
    }
}

/**
  * @brief System Clock Configuration
  *
  * STM32F103C8T6 Blue Pill với HSE 8 MHz:
  *   SYSCLK = 72 MHz
  *   APB1   = 36 MHz
  *
  * Nếu board của bạn dùng clock khác, có thể giữ SystemClock_Config
  * do CubeMX sinh ra trong project đã test CAN thành công.
  */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 |
                                  RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief CAN Initialization Function
  *
  * 125 kbps @ APB1 = 36 MHz:
  *   36 MHz / 72 / (1 + 2 + 1) = 125 kbps
  */
static void MX_CAN_Init(void)
{
    hcan.Instance = CAN1;

    hcan.Init.Prescaler = 72;
    hcan.Init.Mode = CAN_MODE_NORMAL;
    hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan.Init.TimeSeg1 = CAN_BS1_2TQ;
    hcan.Init.TimeSeg2 = CAN_BS2_1TQ;

    hcan.Init.TimeTriggeredMode = DISABLE;
    hcan.Init.AutoBusOff = ENABLE;
    hcan.Init.AutoWakeUp = DISABLE;

    /*
      Khi đã có USB-CAN ACK ổn định, ENABLE giúp CAN tự retransmit khi lỗi nhẹ.
      Nếu debug no-ACK hoặc mailbox full, có thể đổi tạm thành DISABLE.
    */
    hcan.Init.AutoRetransmission = ENABLE;

    hcan.Init.ReceiveFifoLocked = DISABLE;
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief USART1 Initialization Function
  * Debug log: PA9 TX, PA10 RX, 115200
  */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief USART2 Initialization Function
  * ESP input: PA3 RX, 115200
  */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief USART3 Initialization Function
  * ESP input: PB11 RX, 115200
  */
static void MX_USART3_UART_Init(void)
{
    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart3) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief GPIO Initialization Function
  */
static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
}

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
