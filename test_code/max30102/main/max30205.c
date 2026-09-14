#include "max30205.h"
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

#define MAX30205_ADDR       0x48

#define REG_TEMPERATURE     0x00
#define REG_CONFIGURATION   0x01

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

static esp_err_t max30205_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(
        I2C_MASTER_PORT,
        MAX30205_ADDR,
        data,
        sizeof(data),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );
}

static esp_err_t max30205_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_MASTER_PORT,
        MAX30205_ADDR,
        &reg,
        1,
        data,
        len,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS)
    );
}

esp_err_t max30205_init(void)
{
    ESP_ERROR_CHECK(i2c_master_init_once());

    // continuous conversion mode
    ESP_ERROR_CHECK(max30205_write_reg(REG_CONFIGURATION, 0x00));
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

esp_err_t max30205_read_temperature_c(float *temp_c)
{
    if (temp_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[2] = {0};
    esp_err_t err = max30205_read_regs(REG_TEMPERATURE, raw, 2);
    if (err != ESP_OK) {
        return err;
    }

    int16_t temp_raw = (int16_t)((raw[0] << 8) | raw[1]);
    *temp_c = temp_raw / 256.0f;

    return ESP_OK;
}