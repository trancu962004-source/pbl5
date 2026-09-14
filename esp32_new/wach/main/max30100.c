#include "max30100.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "MAX30100";

/* Registers */
#define MAX30100_REG_INT_STATUS      0x00
#define MAX30100_REG_INT_ENABLE      0x01
#define MAX30100_REG_FIFO_WR_PTR     0x02
#define MAX30100_REG_OVF_COUNTER     0x03
#define MAX30100_REG_FIFO_RD_PTR     0x04
#define MAX30100_REG_FIFO_DATA       0x05
#define MAX30100_REG_MODE_CONFIG     0x06
#define MAX30100_REG_SPO2_CONFIG     0x07
#define MAX30100_REG_LED_CONFIG      0x09
#define MAX30100_REG_TEMP_INT        0x16
#define MAX30100_REG_TEMP_FRAC       0x17
#define MAX30100_REG_REV_ID          0xFE
#define MAX30100_REG_PART_ID         0xFF

#define MAX30100_PART_ID_EXPECTED    0x11

#define MAX30100_TIMEOUT_MS          100

static esp_err_t max30100_write_reg(max30100_t *dev, uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev null");

    uint8_t data[2] = { reg, value };
    return i2c_master_write_to_device(
        dev->i2c_port,
        dev->i2c_addr,
        data,
        sizeof(data),
        pdMS_TO_TICKS(MAX30100_TIMEOUT_MS));
}

static esp_err_t max30100_read_reg(max30100_t *dev, uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev null");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data null");

    return i2c_master_write_read_device(
        dev->i2c_port,
        dev->i2c_addr,
        &reg,
        1,
        data,
        len,
        pdMS_TO_TICKS(MAX30100_TIMEOUT_MS));
}

esp_err_t max30100_init(max30100_t *dev, i2c_port_t port, uint8_t addr)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev null");

    memset(dev, 0, sizeof(*dev));
    dev->i2c_port = port;
    dev->i2c_addr = addr;
    dev->initialized = true;

    return ESP_OK;
}

esp_err_t max30100_reset(max30100_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    /* MODE_CONFIG bit 6 = RESET */
    ESP_RETURN_ON_ERROR(max30100_write_reg(dev, MAX30100_REG_MODE_CONFIG, 0x40), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t max30100_read_part_id(max30100_t *dev, uint8_t *part_id, uint8_t *rev_id)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(part_id != NULL && rev_id != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");

    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_PART_ID, part_id, 1), TAG, "read PART_ID failed");
    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_REV_ID, rev_id, 1), TAG, "read REV_ID failed");

    return ESP_OK;
}

esp_err_t max30100_clear_fifo(max30100_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    ESP_RETURN_ON_ERROR(max30100_write_reg(dev, MAX30100_REG_FIFO_WR_PTR, 0x00), TAG, "clear FIFO_WR_PTR failed");
    ESP_RETURN_ON_ERROR(max30100_write_reg(dev, MAX30100_REG_OVF_COUNTER, 0x00), TAG, "clear OVF_COUNTER failed");
    ESP_RETURN_ON_ERROR(max30100_write_reg(dev, MAX30100_REG_FIFO_RD_PTR, 0x00), TAG, "clear FIFO_RD_PTR failed");

    return ESP_OK;
}

esp_err_t max30100_set_mode(max30100_t *dev, max30100_mode_t mode)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    return max30100_write_reg(dev, MAX30100_REG_MODE_CONFIG, (uint8_t)mode);
}

esp_err_t max30100_set_spo2_config(max30100_t *dev,
                                   max30100_sample_rate_t sample_rate,
                                   max30100_led_pw_t led_pw,
                                   bool high_res_en)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    uint8_t reg = 0;
    if (high_res_en) {
        reg |= (1 << 6);
    }
    reg |= ((uint8_t)sample_rate & 0x07) << 2;
    reg |= ((uint8_t)led_pw & 0x03);

    return max30100_write_reg(dev, MAX30100_REG_SPO2_CONFIG, reg);
}

esp_err_t max30100_set_led_current(max30100_t *dev,
                                   max30100_led_current_t red_current,
                                   max30100_led_current_t ir_current)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    uint8_t reg = (((uint8_t)red_current & 0x0F) << 4) | ((uint8_t)ir_current & 0x0F);
    return max30100_write_reg(dev, MAX30100_REG_LED_CONFIG, reg);
}

esp_err_t max30100_start_default(max30100_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    uint8_t part_id = 0;
    uint8_t rev_id = 0;

    ESP_RETURN_ON_ERROR(max30100_read_part_id(dev, &part_id, &rev_id), TAG, "read id failed");
    ESP_LOGI(TAG, "PART_ID=0x%02X REV_ID=0x%02X", part_id, rev_id);

    if (part_id != MAX30100_PART_ID_EXPECTED) {
    ESP_LOGE(TAG, "Invalid MAX30100 PART_ID=0x%02X", part_id);
    return ESP_ERR_NOT_SUPPORTED;
}

    ESP_RETURN_ON_ERROR(max30100_reset(dev), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(max30100_clear_fifo(dev), TAG, "clear fifo failed");

    /* Disable interrupts for polling mode */
    ESP_RETURN_ON_ERROR(max30100_write_reg(dev, MAX30100_REG_INT_ENABLE, 0x00), TAG, "disable int failed");

    /* SpO2 config:
       - high resolution enable = 1
       - sample rate = 100Hz
       - LED pulse width = 1600us
    */
    ESP_RETURN_ON_ERROR(
        max30100_set_spo2_config(dev, MAX30100_SR_100HZ, MAX30100_LED_PW_1600US, true),
        TAG,
        "spo2 config failed");

    /* Medium LED current to start */
    ESP_RETURN_ON_ERROR(
        max30100_set_led_current(dev, MAX30100_LED_CURR_24_0MA, MAX30100_LED_CURR_24_0MA),
        TAG,
        "led config failed");

    /* SpO2 mode = both RED + IR active */
    ESP_RETURN_ON_ERROR(max30100_set_mode(dev, MAX30100_MODE_SPO2), TAG, "set mode failed");

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t max30100_get_fifo_pointers(max30100_t *dev, uint8_t *wr_ptr, uint8_t *rd_ptr, uint8_t *ovf)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(wr_ptr && rd_ptr && ovf, ESP_ERR_INVALID_ARG, TAG, "null arg");

    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_FIFO_WR_PTR, wr_ptr, 1), TAG, "read wr_ptr failed");
    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_FIFO_RD_PTR, rd_ptr, 1), TAG, "read rd_ptr failed");
    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_OVF_COUNTER, ovf, 1), TAG, "read ovf failed");

    *wr_ptr &= 0x0F;
    *rd_ptr &= 0x0F;
    *ovf &= 0x0F;

    return ESP_OK;
}

esp_err_t max30100_get_num_available_samples(max30100_t *dev, uint8_t *samples)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "samples null");

    uint8_t wr = 0, rd = 0, ovf = 0;
    ESP_RETURN_ON_ERROR(max30100_get_fifo_pointers(dev, &wr, &rd, &ovf), TAG, "get fifo ptr failed");

    if (wr >= rd) {
        *samples = wr - rd;
    } else {
        *samples = (uint8_t)(16 - rd + wr);
    }

    return ESP_OK;
}

esp_err_t max30100_read_sample(max30100_t *dev, max30100_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG, "sample null");

    uint8_t raw[4] = {0};

    /* In SpO2 mode each sample = 4 bytes from FIFO_DATA:
       IR_MSB, IR_LSB, RED_MSB, RED_LSB
     */
    ESP_RETURN_ON_ERROR(max30100_read_reg(dev, MAX30100_REG_FIFO_DATA, raw, sizeof(raw)), TAG, "read fifo sample failed");

    sample->ir  = ((uint16_t)raw[0] << 8) | raw[1];
    sample->red = ((uint16_t)raw[2] << 8) | raw[3];

    return ESP_OK;
}

esp_err_t max30100_poll_samples(max30100_t *dev,
                                max30100_sample_t *out,
                                size_t max_samples,
                                size_t *num_read)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out null");
    ESP_RETURN_ON_FALSE(num_read != NULL, ESP_ERR_INVALID_ARG, TAG, "num_read null");

    *num_read = 0;

    uint8_t available = 0;
    ESP_RETURN_ON_ERROR(max30100_get_num_available_samples(dev, &available), TAG, "available failed");

    if (available == 0) {
        return ESP_OK;
    }

    size_t to_read = available;
    if (to_read > max_samples) {
        to_read = max_samples;
    }

    for (size_t i = 0; i < to_read; i++) {
        ESP_RETURN_ON_ERROR(max30100_read_sample(dev, &out[i]), TAG, "read sample failed");
        (*num_read)++;
    }

    return ESP_OK;
}
esp_err_t max30100_read_reg_u8(max30100_t *dev, uint8_t reg, uint8_t *val)
{
    if (!dev || !val) return ESP_ERR_INVALID_ARG;

    return i2c_master_write_read_device(
        dev->i2c_port,
        dev->i2c_addr,
        &reg,
        1,
        val,
        1,
        pdMS_TO_TICKS(100));
}