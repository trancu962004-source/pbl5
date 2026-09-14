#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "GW_LCD_KEYPAD";

/* =========================
 * LCD 20x4 I2C
 * ========================= */
#define I2C_PORT                 I2C_NUM_0
#define I2C_SDA_GPIO             GPIO_NUM_3
#define I2C_SCL_GPIO             GPIO_NUM_2
#define I2C_FREQ_HZ              100000
#define LCD_I2C_ADDR             0x27

#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE    0x04
#define LCD_RS        0x01

/* =========================
 * Keypad 4x4
 * ========================= */
#define KP_R1 GPIO_NUM_5
#define KP_R2 GPIO_NUM_6
#define KP_R3 GPIO_NUM_7
#define KP_R4 GPIO_NUM_8

#define KP_C1 GPIO_NUM_9
#define KP_C2 GPIO_NUM_10
#define KP_C3 GPIO_NUM_20
#define KP_C4 GPIO_NUM_21

static const char keypad_map[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

/* =========================
 * BLE packet config
 * ========================= */
#define COMPANY_ID_LOW           0xFF
#define COMPANY_ID_HIGH          0xFF
#define PACKET_MAGIC             0xA5
#define NODE_TYPE_VITAL          1
#define NODE_TYPE_LOADCELL       2

#define DATA_TIMEOUT_MS          5000
#define LCD_REFRESH_MS           250

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  wing_id;
    uint8_t  room_id;
    uint8_t  bed_id;
    uint8_t  patient_id;
    uint8_t  node_type;
    uint16_t hr;
    uint16_t spo2;
    uint16_t temp_x100;
    uint8_t  status;
    uint8_t  battery;
    uint8_t  seq;
    uint8_t  checksum;
} vital_adv_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  wing_id;
    uint8_t  room_id;
    uint8_t  bed_id;
    uint8_t  patient_id;
    uint8_t  node_type;
    uint16_t volume_ml;
    uint16_t drops_per_min;
    uint8_t  seq;
    uint8_t  checksum;
} loadcell_adv_pkt_t;

typedef struct {
    bool     has_watch;
    bool     has_loadcell;

    uint16_t hr;
    uint16_t spo2;
    uint16_t temp_x100;

    uint16_t ml;
    uint16_t dpm;

    uint8_t  watch_seq;
    uint8_t  load_seq;

    uint32_t watch_seen_ms;
    uint32_t load_seen_ms;
} selected_data_t;

typedef enum {
    INPUT_NONE = 0,
    INPUT_W,
    INPUT_R,
    INPUT_B,
    INPUT_P
} input_mode_t;

static uint8_t s_own_addr_type = 0;

static volatile char s_sel_wing = 0;
static volatile uint8_t s_sel_room = 0;
static volatile uint8_t s_sel_bed = 0;
static volatile uint8_t s_sel_patient = 0;

static volatile input_mode_t s_input_mode = INPUT_NONE;
static char s_input_buf[8] = {0};
static uint8_t s_input_len = 0;

static selected_data_t s_sel = {0};

/* =========================
 * Utils
 * ========================= */
static uint8_t calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum ^= data[i];
    }
    return sum;
}

static uint32_t now_ms(void)
{
    return (uint32_t)esp_log_timestamp();
}

/* =========================
 * LCD driver
 * ========================= */
static esp_err_t lcd_write_raw(uint8_t data)
{
    return i2c_master_write_to_device(I2C_PORT, LCD_I2C_ADDR, &data, 1, pdMS_TO_TICKS(50));
}

static void lcd_pulse(uint8_t data)
{
    lcd_write_raw(data | LCD_ENABLE | LCD_BACKLIGHT);
    esp_rom_delay_us(1);
    lcd_write_raw((data & ~LCD_ENABLE) | LCD_BACKLIGHT);
    esp_rom_delay_us(50);
}

static void lcd_write4(uint8_t nibble, uint8_t mode)
{
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    lcd_write_raw(data);
    lcd_pulse(data);
}

static void lcd_send(uint8_t value, uint8_t mode)
{
    lcd_write4(value & 0xF0, mode);
    lcd_write4((value << 4) & 0xF0, mode);
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_send(cmd, 0);
    if (cmd == 0x01 || cmd == 0x02) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void lcd_data(uint8_t data)
{
    lcd_send(data, LCD_RS);
}

static void lcd_set_cursor(uint8_t col, uint8_t row)
{
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row > 3) row = 3;
    lcd_cmd(0x80 | (col + row_offsets[row]));
}

static void lcd_print_line(uint8_t row, const char *text)
{
    char buf[21];
    size_t n = strlen(text);
    if (n > 20) n = 20;

    memset(buf, ' ', 20);
    memcpy(buf, text, n);
    buf[20] = '\0';

    lcd_set_cursor(0, row);
    for (int i = 0; i < 20; i++) {
        lcd_data((uint8_t)buf[i]);
    }
}

static esp_err_t lcd_init_hw(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));

    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_write4(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_write4(0x30, 0);
    esp_rom_delay_us(150);
    lcd_write4(0x30, 0);
    lcd_write4(0x20, 0);

    lcd_cmd(0x28);
    lcd_cmd(0x0C);
    lcd_cmd(0x06);
    lcd_cmd(0x01);
    return ESP_OK;
}

/* =========================
 * Keypad
 * ========================= */
static const gpio_num_t row_pins[4] = {KP_R1, KP_R2, KP_R3, KP_R4};
static const gpio_num_t col_pins[4] = {KP_C1, KP_C2, KP_C3, KP_C4};

static void keypad_init(void)
{
    gpio_config_t row_conf = {
        .pin_bit_mask = (1ULL << KP_R1) | (1ULL << KP_R2) | (1ULL << KP_R3) | (1ULL << KP_R4),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&row_conf));

    gpio_config_t col_conf = {
        .pin_bit_mask = (1ULL << KP_C1) | (1ULL << KP_C2) | (1ULL << KP_C3) | (1ULL << KP_C4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&col_conf));

    for (int i = 0; i < 4; i++) {
        gpio_set_level(row_pins[i], 1);
    }
}

static char keypad_get_key(void)
{
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 4; i++) {
            gpio_set_level(row_pins[i], 1);
        }

        gpio_set_level(row_pins[r], 0);
        esp_rom_delay_us(5);

        for (int c = 0; c < 4; c++) {
            if (gpio_get_level(col_pins[c]) == 0) {
                while (gpio_get_level(col_pins[c]) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                return keypad_map[r][c];
            }
        }
    }
    return 0;
}

static void apply_input_value(void)
{
    if (s_input_len == 0) return;

    int val = atoi(s_input_buf);

    switch (s_input_mode) {
        case INPUT_W:
            if (val >= 1 && val <= 26) {
                s_sel_wing = 'A' + (val - 1);
            }
            break;

        case INPUT_R:
            if (val >= 1 && val <= 99) {
                s_sel_room = (uint8_t)val;
            }
            break;

        case INPUT_B:
            if (val >= 1 && val <= 99) {
                s_sel_bed = (uint8_t)val;
            }
            break;

        case INPUT_P:
            if (val >= 1 && val <= 99) {
                s_sel_patient = (uint8_t)val;
            }
            break;

        default:
            break;
    }

    memset(s_input_buf, 0, sizeof(s_input_buf));
    s_input_len = 0;
    s_input_mode = INPUT_NONE;
}

static void keypad_task(void *arg)
{
    (void)arg;

    while (1) {
        char key = keypad_get_key();
        if (key != 0) {
            if (key == 'A') {
                s_input_mode = INPUT_W;
                s_input_len = 0;
                memset(s_input_buf, 0, sizeof(s_input_buf));
            } else if (key == 'B') {
                s_input_mode = INPUT_R;
                s_input_len = 0;
                memset(s_input_buf, 0, sizeof(s_input_buf));
            } else if (key == 'C') {
                s_input_mode = INPUT_B;
                s_input_len = 0;
                memset(s_input_buf, 0, sizeof(s_input_buf));
            } else if (key == 'D') {
                s_input_mode = INPUT_P;
                s_input_len = 0;
                memset(s_input_buf, 0, sizeof(s_input_buf));
            } else if (key == '*') {
                if (s_input_len > 0) {
                    s_input_len--;
                    s_input_buf[s_input_len] = '\0';
                }
            } else if (key == '#') {
                apply_input_value();
            } else if (isdigit((unsigned char)key)) {
                if (s_input_mode != INPUT_NONE && s_input_len < sizeof(s_input_buf) - 1) {
                    s_input_buf[s_input_len++] = key;
                    s_input_buf[s_input_len] = '\0';
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* =========================
 * BLE parse + scan
 * ========================= */
static bool parse_vital_packet(const uint8_t *mfg, uint8_t len, vital_adv_pkt_t *pkt)
{
    if (!mfg || !pkt) return false;
    if (len < 2 + sizeof(vital_adv_pkt_t)) return false;
    if (mfg[0] != COMPANY_ID_LOW || mfg[1] != COMPANY_ID_HIGH) return false;

    memcpy(pkt, &mfg[2], sizeof(vital_adv_pkt_t));
    if (pkt->magic != PACKET_MAGIC) return false;
    if (pkt->node_type != NODE_TYPE_VITAL) return false;

    uint8_t cs = calc_checksum((const uint8_t *)pkt, sizeof(vital_adv_pkt_t) - 1);
    return cs == pkt->checksum;
}

static bool parse_loadcell_packet(const uint8_t *mfg, uint8_t len, loadcell_adv_pkt_t *pkt)
{
    if (!mfg || !pkt) return false;
    if (len < 2 + sizeof(loadcell_adv_pkt_t)) return false;
    if (mfg[0] != COMPANY_ID_LOW || mfg[1] != COMPANY_ID_HIGH) return false;

    memcpy(pkt, &mfg[2], sizeof(loadcell_adv_pkt_t));
    if (pkt->magic != PACKET_MAGIC) return false;
    if (pkt->node_type != NODE_TYPE_LOADCELL) return false;

    uint8_t cs = calc_checksum((const uint8_t *)pkt, sizeof(loadcell_adv_pkt_t) - 1);
    return cs == pkt->checksum;
}

static void handle_vital(const vital_adv_pkt_t *pkt)
{
    if (pkt->wing_id != (uint8_t)s_sel_wing ||
        pkt->room_id != s_sel_room ||
        pkt->bed_id != s_sel_bed ||
        pkt->patient_id != s_sel_patient) {
        return;
    }

    if (s_sel.has_watch && pkt->seq == s_sel.watch_seq) {
        return;
    }

    s_sel.has_watch = true;
    s_sel.hr = pkt->hr;
    s_sel.spo2 = pkt->spo2;
    s_sel.temp_x100 = pkt->temp_x100;
    s_sel.watch_seq = pkt->seq;
    s_sel.watch_seen_ms = now_ms();
}

static void handle_loadcell(const loadcell_adv_pkt_t *pkt)
{
    if (pkt->wing_id != (uint8_t)s_sel_wing ||
        pkt->room_id != s_sel_room ||
        pkt->bed_id != s_sel_bed ||
        pkt->patient_id != s_sel_patient) {
        return;
    }

    if (s_sel.has_loadcell && pkt->seq == s_sel.load_seq) {
        return;
    }

    s_sel.has_loadcell = true;
    s_sel.ml = pkt->volume_ml;
    s_sel.dpm = pkt->drops_per_min;
    s_sel.load_seq = pkt->seq;
    s_sel.load_seen_ms = now_ms();
}

static void start_scan(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            memset(&fields, 0, sizeof(fields));

            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            if (rc != 0 || fields.mfg_data == NULL) return 0;

            const uint8_t *mfg = fields.mfg_data;
            uint8_t mfg_len = fields.mfg_data_len;
            if (mfg_len < 2 + 7) return 0;

            uint8_t node_type = mfg[2 + 5];

            if (node_type == NODE_TYPE_VITAL) {
                vital_adv_pkt_t pkt;
                if (parse_vital_packet(mfg, mfg_len, &pkt)) {
                    handle_vital(&pkt);
                }
            } else if (node_type == NODE_TYPE_LOADCELL) {
                loadcell_adv_pkt_t pkt;
                if (parse_loadcell_packet(mfg, mfg_len, &pkt)) {
                    handle_loadcell(&pkt);
                }
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            start_scan();
            return 0;

        default:
            return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));

    disc_params.passive = 0;
    disc_params.itvl = 0x0010;
    disc_params.window = 0x0010;
    disc_params.filter_duplicates = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE synced");
    start_scan();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* =========================
 * LCD task
 * ========================= */
static void lcd_task(void *arg)
{
    (void)arg;
    char line1[21];
    char line2[21];
    char line3[21];
    char line4[21];

    while (1) {
        uint32_t now = now_ms();
        bool watch_ok = s_sel.has_watch && ((now - s_sel.watch_seen_ms) < DATA_TIMEOUT_MS);
        bool load_ok  = s_sel.has_loadcell && ((now - s_sel.load_seen_ms) < DATA_TIMEOUT_MS);

       if (s_input_mode == INPUT_NONE) {
    char wing_buf[4];
    char room_buf[4];
    char bed_buf[4];
    char patient_buf[4];

    if (s_sel_wing >= 'A' && s_sel_wing <= 'Z') {
        snprintf(wing_buf, sizeof(wing_buf), "%c", s_sel_wing);
    } else {
        snprintf(wing_buf, sizeof(wing_buf), "-");
    }

    if (s_sel_room > 0) {
        snprintf(room_buf, sizeof(room_buf), "%02u", (unsigned)s_sel_room);
    } else {
        snprintf(room_buf, sizeof(room_buf), "--");
    }

    if (s_sel_bed > 0) {
        snprintf(bed_buf, sizeof(bed_buf), "%02u", (unsigned)s_sel_bed);
    } else {
        snprintf(bed_buf, sizeof(bed_buf), "--");
    }

    if (s_sel_patient > 0) {
        snprintf(patient_buf, sizeof(patient_buf), "%02u", (unsigned)s_sel_patient);
    } else {
        snprintf(patient_buf, sizeof(patient_buf), "--");
    }

    snprintf(line1, sizeof(line1),
             "W%s R%s B%s P%s",
             wing_buf, room_buf, bed_buf, patient_buf);
}
        else {
            char mode_char = '?';
            if (s_input_mode == INPUT_W) mode_char = 'W';
            if (s_input_mode == INPUT_R) mode_char = 'R';
            if (s_input_mode == INPUT_B) mode_char = 'B';
            if (s_input_mode == INPUT_P) mode_char = 'P';

            snprintf(line1, sizeof(line1), "SET %c:%s", mode_char, s_input_buf);
        }

        if (load_ok) {
            snprintf(line2, sizeof(line2),
                     "ML:%u DPM:%u",
                     (unsigned)s_sel.ml,
                     (unsigned)s_sel.dpm);
        } else {
            snprintf(line2, sizeof(line2), "ML:--- DPM:---");
        }

        if (watch_ok) {
            snprintf(line3, sizeof(line3),
                     "HR:%u SpO2:%u",
                     (unsigned)s_sel.hr,
                     (unsigned)s_sel.spo2);

            snprintf(line4, sizeof(line4),
                     "Temp:%u.%02uC",
                     (unsigned)(s_sel.temp_x100 / 100),
                     (unsigned)(s_sel.temp_x100 % 100));
        } else {
            snprintf(line3, sizeof(line3), "HR:--- SpO2:---");
            snprintf(line4, sizeof(line4), "Temp:---.--C");
        }

        lcd_print_line(0, line1);
        lcd_print_line(1, line2);
        lcd_print_line(2, line3);
        lcd_print_line(3, line4);

        vTaskDelay(pdMS_TO_TICKS(LCD_REFRESH_MS));
    }
}

/* =========================
 * Main
 * ========================= */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    keypad_init();
    ESP_ERROR_CHECK(lcd_init_hw());

    esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release rc=%s", esp_err_to_name(rel));
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(ble_host_task);

    xTaskCreate(keypad_task, "keypad_task", 3072, NULL, 5, NULL);
    xTaskCreate(lcd_task, "lcd_task", 4096, NULL, 5, NULL);
}