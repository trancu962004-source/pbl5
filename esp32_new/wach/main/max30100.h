#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX30100_I2C_ADDR            0x57

typedef enum {
    MAX30100_MODE_HR   = 0x02,
    MAX30100_MODE_SPO2 = 0x03,
} max30100_mode_t;

typedef enum {
    MAX30100_SR_50HZ   = 0x00,
    MAX30100_SR_100HZ  = 0x01,
    MAX30100_SR_167HZ  = 0x02,
    MAX30100_SR_200HZ  = 0x03,
    MAX30100_SR_400HZ  = 0x04,
    MAX30100_SR_600HZ  = 0x05,
    MAX30100_SR_800HZ  = 0x06,
    MAX30100_SR_1000HZ = 0x07,
} max30100_sample_rate_t;

typedef enum {
    MAX30100_LED_PW_200US  = 0x00,
    MAX30100_LED_PW_400US  = 0x01,
    MAX30100_LED_PW_800US  = 0x02,
    MAX30100_LED_PW_1600US = 0x03,
} max30100_led_pw_t;

/* Giá trị 0x0..0xF, dòng LED tăng dần theo datasheet */
typedef enum {
    MAX30100_LED_CURR_0_0MA  = 0x0,
    MAX30100_LED_CURR_4_4MA  = 0x1,
    MAX30100_LED_CURR_7_6MA  = 0x2,
    MAX30100_LED_CURR_11_0MA = 0x3,
    MAX30100_LED_CURR_14_2MA = 0x4,
    MAX30100_LED_CURR_17_4MA = 0x5,
    MAX30100_LED_CURR_20_8MA = 0x6,
    MAX30100_LED_CURR_24_0MA = 0x7,
    MAX30100_LED_CURR_27_1MA = 0x8,
    MAX30100_LED_CURR_30_6MA = 0x9,
    MAX30100_LED_CURR_33_8MA = 0xA,
    MAX30100_LED_CURR_37_0MA = 0xB,
    MAX30100_LED_CURR_40_2MA = 0xC,
    MAX30100_LED_CURR_43_6MA = 0xD,
    MAX30100_LED_CURR_46_8MA = 0xE,
    MAX30100_LED_CURR_50_0MA = 0xF,
} max30100_led_current_t;

typedef struct {
    i2c_port_t i2c_port;
    uint8_t i2c_addr;
    bool initialized;
} max30100_t;

typedef struct {
    uint16_t ir;
    uint16_t red;
} max30100_sample_t;
esp_err_t max30100_read_reg_u8(max30100_t *dev, uint8_t reg, uint8_t *val);
esp_err_t max30100_init(max30100_t *dev, i2c_port_t port, uint8_t addr);
esp_err_t max30100_reset(max30100_t *dev);
esp_err_t max30100_read_part_id(max30100_t *dev, uint8_t *part_id, uint8_t *rev_id);
esp_err_t max30100_clear_fifo(max30100_t *dev);
esp_err_t max30100_set_mode(max30100_t *dev, max30100_mode_t mode);
esp_err_t max30100_set_spo2_config(max30100_t *dev,
                                   max30100_sample_rate_t sample_rate,
                                   max30100_led_pw_t led_pw,
                                   bool high_res_en);
esp_err_t max30100_set_led_current(max30100_t *dev,
                                   max30100_led_current_t red_current,
                                   max30100_led_current_t ir_current);
esp_err_t max30100_start_default(max30100_t *dev);

esp_err_t max30100_get_fifo_pointers(max30100_t *dev, uint8_t *wr_ptr, uint8_t *rd_ptr, uint8_t *ovf);
esp_err_t max30100_get_num_available_samples(max30100_t *dev, uint8_t *samples);
esp_err_t max30100_read_sample(max30100_t *dev, max30100_sample_t *sample);
esp_err_t max30100_poll_samples(max30100_t *dev,
                                max30100_sample_t *out,
                                size_t max_samples,
                                size_t *num_read);

#ifdef __cplusplus
}
#endif