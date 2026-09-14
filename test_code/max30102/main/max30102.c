#include "max30102.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

#define I2C_MASTER_PORT     I2C_NUM_0
#define I2C_MASTER_SDA_IO   4
#define I2C_MASTER_SCL_IO   3
#define I2C_MASTER_FREQ_HZ  400000
#define I2C_TIMEOUT_MS      1000

#define MAX30102_ADDR       0x57

#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06

static esp_err_t i2c_master_init_once(void)
{
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = 0
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_PORT, conf.mode, 0, 0, 0));

    initialized = true;
    return ESP_OK;
}

static esp_err_t max30102_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(
        I2C_MASTER_PORT,
        MAX30102_ADDR,
        data,
        sizeof(data),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );
}

static esp_err_t max30102_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_MASTER_PORT,
        MAX30102_ADDR,
        &reg,
        1,
        data,
        len,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );
}

esp_err_t max30102_init(void)
{
    ESP_ERROR_CHECK(i2c_master_init_once());

    // reset FIFO
    ESP_ERROR_CHECK(max30102_write_reg(REG_FIFO_WR_PTR, 0x00));
    ESP_ERROR_CHECK(max30102_write_reg(REG_OVF_COUNTER, 0x00));
    ESP_ERROR_CHECK(max30102_write_reg(REG_FIFO_RD_PTR, 0x00));

    // FIFO config
    ESP_ERROR_CHECK(max30102_write_reg(REG_FIFO_CONFIG, 0x4F));

    // SpO2 mode
    ESP_ERROR_CHECK(max30102_write_reg(REG_MODE_CONFIG, 0x03));

    // sample rate + pulse width
    ESP_ERROR_CHECK(max30102_write_reg(REG_SPO2_CONFIG, 0x27));

    // LED current
    ESP_ERROR_CHECK(max30102_write_reg(REG_LED1_PA, 0x24));
    ESP_ERROR_CHECK(max30102_write_reg(REG_LED2_PA, 0x24));

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t max30102_read_sample(max30102_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[6] = {0};
    esp_err_t err = max30102_read_regs(REG_FIFO_DATA, raw, 6);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t red = ((uint32_t)raw[0] << 16) |
                   ((uint32_t)raw[1] << 8)  |
                   ((uint32_t)raw[2]);

    uint32_t ir  = ((uint32_t)raw[3] << 16) |
                   ((uint32_t)raw[4] << 8)  |
                   ((uint32_t)raw[5]);

    sample->red = red & 0x03FFFF;
    sample->ir  = ir  & 0x03FFFF;

    return ESP_OK;
}