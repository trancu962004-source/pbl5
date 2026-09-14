#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"

// ======================= CẤU HÌNH PHẦN CỨNG =======================

#define I2C_SDA_GPIO        GPIO_NUM_4
#define I2C_SCL_GPIO        GPIO_NUM_3
#define I2C_PORT_NUM        I2C_NUM_0
#define I2C_FREQ_HZ         100000
#define I2C_TIMEOUT_MS      1000

#define MPU6050_ADDR_1      0x68
#define MPU6050_ADDR_2      0x69

// Chân cảnh báo: LED / buzzer module / transistor điều khiển motor rung
#define ALERT_GPIO          GPIO_NUM_2

// Nút SOS nối từ GPIO xuống GND
#define SOS_BUTTON_GPIO     GPIO_NUM_1

// ======================= THANH GHI MPU6050 =======================

#define MPU6050_REG_SMPLRT_DIV    0x19
#define MPU6050_REG_CONFIG        0x1A
#define MPU6050_REG_GYRO_CONFIG   0x1B
#define MPU6050_REG_ACCEL_CONFIG  0x1C
#define MPU6050_REG_ACCEL_XOUT_H  0x3B
#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_WHO_AM_I      0x75

// Accelerometer ±8g  => 4096 LSB/g
// Gyroscope ±500 dps => 65.5 LSB/(deg/s)
#define ACCEL_SCALE_LSB_PER_G     4096.0f
#define GYRO_SCALE_LSB_PER_DPS    65.5f

// ======================= NGƯỠNG PHÁT HIỆN TÉ NGÃ =======================

// Gia tốc nhỏ, nghi ngờ rơi tự do
#define FREE_FALL_THRESHOLD_G      0.55f

// Va chạm sau khi rơi
#define IMPACT_THRESHOLD_G         2.7f

// Trường hợp không thấy rơi tự do rõ nhưng có va chạm + xoay mạnh
#define DIRECT_IMPACT_G            3.2f
#define DIRECT_IMPACT_GYRO_DPS     250.0f

// Sau va chạm, nếu gần như bất động thì xác nhận té
#define STILL_ACCEL_DIFF_G         0.30f
#define STILL_GYRO_DPS             25.0f

#define FREEFALL_TIMEOUT_MS        1000
#define POST_IMPACT_WINDOW_MS      8000
#define STILL_TIME_MS              2500
#define FALL_ALERT_DURATION_MS     5000
#define FALL_COOLDOWN_MS           10000

// ======================= NÚT SOS =======================

#define SOS_DEBOUNCE_MS            50
#define SOS_LONG_PRESS_MS          2000
#define HELP_ALERT_DURATION_MS     10000

static const char *TAG = "WATCH";

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t mpu6050_handle = NULL;
static uint8_t mpu6050_addr = MPU6050_ADDR_1;

// ======================= KIỂU DỮ LIỆU =======================

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;

    float gx_dps;
    float gy_dps;
    float gz_dps;

    float accel_total_g;
    float gyro_total_dps;
} imu_data_t;

typedef enum {
    FALL_STATE_NORMAL = 0,
    FALL_STATE_FREEFALL,
    FALL_STATE_IMPACT,
    FALL_STATE_ALERT,
    FALL_STATE_COOLDOWN
} fall_state_t;

static fall_state_t fall_state = FALL_STATE_NORMAL;

static int64_t fall_state_start_ms = 0;
static int64_t still_start_ms = 0;

// ======================= BIẾN NÚT SOS =======================

static bool help_alert_active = false;
static int64_t help_alert_start_ms = 0;

static bool sos_last_raw_pressed = false;
static bool sos_stable_pressed = false;
static int64_t sos_last_change_ms = 0;
static int64_t sos_press_start_ms = 0;
static bool sos_long_press_fired = false;

// ======================= HÀM THỜI GIAN =======================

static int64_t millis(void)
{
    return esp_timer_get_time() / 1000;
}

// ======================= GPIO =======================

static esp_err_t gpio_init_all(void)
{
    gpio_config_t alert_conf = {
        .pin_bit_mask = 1ULL << ALERT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&alert_conf));
    gpio_set_level(ALERT_GPIO, 0);

    gpio_config_t button_conf = {
        .pin_bit_mask = 1ULL << SOS_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&button_conf));

    return ESP_OK;
}

// ======================= I2C LOW LEVEL =======================

static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(mpu6050_handle, data, sizeof(data), I2C_TIMEOUT_MS);
}

static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(
        mpu6050_handle,
        &reg,
        1,
        data,
        len,
        I2C_TIMEOUT_MS
    );
}

static int16_t make_int16(uint8_t high, uint8_t low)
{
    return (int16_t)((high << 8) | low);
}

// ======================= I2C INIT + SCAN =======================

static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT_NUM,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    return i2c_new_master_bus(&bus_config, &i2c_bus_handle);
}

static void i2c_scan_once(void)
{
    ESP_LOGI(TAG, "Dang quet I2C...");

    int found = 0;

    for (uint8_t addr = 1; addr < 127; addr++) {
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, 100);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Tim thay thiet bi I2C tai dia chi 0x%02X", addr);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "Khong tim thay thiet bi I2C nao");
    }
}

static esp_err_t mpu6050_add_device_auto_address(void)
{
    esp_err_t ret;

    ret = i2c_master_probe(i2c_bus_handle, MPU6050_ADDR_1, 300);
    if (ret == ESP_OK) {
        mpu6050_addr = MPU6050_ADDR_1;
    } else {
        ret = i2c_master_probe(i2c_bus_handle, MPU6050_ADDR_2, 300);
        if (ret == ESP_OK) {
            mpu6050_addr = MPU6050_ADDR_2;
        } else {
            ESP_LOGE(TAG, "Khong tim thay MPU6050 tai 0x68 hoac 0x69");
            ESP_LOGE(TAG, "Hay kiem tra VCC, GND, SDA, SCL, AD0");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "MPU6050 dang o dia chi 0x%02X", mpu6050_addr);

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = mpu6050_addr,
        .scl_speed_hz = I2C_FREQ_HZ
    };

    return i2c_master_bus_add_device(
        i2c_bus_handle,
        &dev_config,
        &mpu6050_handle
    );
}

// ======================= MPU6050 INIT =======================

static esp_err_t mpu6050_init(void)
{
    uint8_t who_am_i = 0;

    esp_err_t ret = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who_am_i, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khong doc duoc WHO_AM_I: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MPU6050 WHO_AM_I = 0x%02X", who_am_i);

    if (who_am_i != 0x68) {
        ESP_LOGW(TAG, "WHO_AM_I khac 0x68, co the module clone hoac ket noi chua on dinh");
    }

    // Thoát sleep mode
    ret = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(100));

    // DLPF khoảng 44 Hz
    ret = mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03);
    if (ret != ESP_OK) return ret;

    // Sample rate: 1 kHz / (1 + 19) = 50 Hz
    ret = mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 19);
    if (ret != ESP_OK) return ret;

    // Gyro ±500 dps
    ret = mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x08);
    if (ret != ESP_OK) return ret;

    // Accel ±8g
    ret = mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x10);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6050 init thanh cong");

    return ESP_OK;
}

// ======================= ĐỌC MPU6050 =======================

static esp_err_t mpu6050_read_data(imu_data_t *imu)
{
    uint8_t raw[14];

    esp_err_t ret = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw_ax = make_int16(raw[0], raw[1]);
    int16_t raw_ay = make_int16(raw[2], raw[3]);
    int16_t raw_az = make_int16(raw[4], raw[5]);

    int16_t raw_gx = make_int16(raw[8], raw[9]);
    int16_t raw_gy = make_int16(raw[10], raw[11]);
    int16_t raw_gz = make_int16(raw[12], raw[13]);

    imu->ax_g = raw_ax / ACCEL_SCALE_LSB_PER_G;
    imu->ay_g = raw_ay / ACCEL_SCALE_LSB_PER_G;
    imu->az_g = raw_az / ACCEL_SCALE_LSB_PER_G;

    imu->gx_dps = raw_gx / GYRO_SCALE_LSB_PER_DPS;
    imu->gy_dps = raw_gy / GYRO_SCALE_LSB_PER_DPS;
    imu->gz_dps = raw_gz / GYRO_SCALE_LSB_PER_DPS;

    imu->accel_total_g = sqrtf(
        imu->ax_g * imu->ax_g +
        imu->ay_g * imu->ay_g +
        imu->az_g * imu->az_g
    );

    imu->gyro_total_dps = sqrtf(
        imu->gx_dps * imu->gx_dps +
        imu->gy_dps * imu->gy_dps +
        imu->gz_dps * imu->gz_dps
    );

    return ESP_OK;
}

// ======================= GỌI HỖ TRỢ =======================

static void send_help_request(const char *reason)
{
    ESP_LOGE(TAG, "====================================");
    ESP_LOGE(TAG, "YEU CAU GOI HO TRO!");
    ESP_LOGE(TAG, "LY DO: %s", reason);
    ESP_LOGE(TAG, "====================================");

    /*
        Sau này bạn thêm BLE/WiFi/SIM ở đây.

        Ví dụ:
        - Gửi BLE sang app điện thoại
        - Gửi Firebase
        - Gửi Telegram
        - Gửi SMS bằng SIM800L/SIM7600
    */
}

static void start_help_alert(const char *reason)
{
    help_alert_active = true;
    help_alert_start_ms = millis();

    send_help_request(reason);
}

static void help_alert_update(void)
{
    if (!help_alert_active) {
        return;
    }

    int64_t now = millis();

    if ((now - help_alert_start_ms) >= HELP_ALERT_DURATION_MS) {
        help_alert_active = false;
        ESP_LOGI(TAG, "Ket thuc canh bao goi ho tro");
    }
}

// ======================= NÚT SOS =======================

static void sos_button_update(void)
{
    int64_t now = millis();

    // Nút nối xuống GND nên nhấn = mức 0
    bool raw_pressed = (gpio_get_level(SOS_BUTTON_GPIO) == 0);

    if (raw_pressed != sos_last_raw_pressed) {
        sos_last_raw_pressed = raw_pressed;
        sos_last_change_ms = now;
    }

    // Chống dội nút
    if ((now - sos_last_change_ms) >= SOS_DEBOUNCE_MS) {
        if (raw_pressed != sos_stable_pressed) {
            sos_stable_pressed = raw_pressed;

            if (sos_stable_pressed) {
                sos_press_start_ms = now;
                sos_long_press_fired = false;
                ESP_LOGI(TAG, "Dang nhan nut SOS...");
            } else {
                ESP_LOGI(TAG, "Da tha nut SOS");
            }
        }
    }

    // Nhấn giữ đủ lâu thì gọi hỗ trợ
    if (sos_stable_pressed &&
        !sos_long_press_fired &&
        (now - sos_press_start_ms >= SOS_LONG_PRESS_MS)) {

        sos_long_press_fired = true;

        ESP_LOGE(TAG, "Nhan giu nut SOS du %d ms", SOS_LONG_PRESS_MS);

        start_help_alert("SOS_BUTTON_LONG_PRESS");
    }
}

// ======================= PHÁT HIỆN TÉ NGÃ =======================

static void set_fall_state(fall_state_t new_state)
{
    fall_state = new_state;
    fall_state_start_ms = millis();

    if (new_state != FALL_STATE_IMPACT) {
        still_start_ms = 0;
    }
}

static void fall_detection_update(const imu_data_t *imu)
{
    int64_t now = millis();

    bool free_fall =
        imu->accel_total_g < FREE_FALL_THRESHOLD_G;

    bool impact =
        imu->accel_total_g > IMPACT_THRESHOLD_G;

    bool direct_impact =
        imu->accel_total_g > DIRECT_IMPACT_G &&
        imu->gyro_total_dps > DIRECT_IMPACT_GYRO_DPS;

    bool still =
        fabsf(imu->accel_total_g - 1.0f) < STILL_ACCEL_DIFF_G &&
        imu->gyro_total_dps < STILL_GYRO_DPS;

    switch (fall_state) {
        case FALL_STATE_NORMAL:
            if (free_fall) {
                ESP_LOGW(TAG, "Nghi ngo roi tu do: A=%.2fg", imu->accel_total_g);
                set_fall_state(FALL_STATE_FREEFALL);
            } else if (direct_impact) {
                ESP_LOGW(TAG,
                         "Va cham manh truc tiep: A=%.2fg, G=%.1fdps",
                         imu->accel_total_g,
                         imu->gyro_total_dps);

                set_fall_state(FALL_STATE_IMPACT);
            }
            break;

        case FALL_STATE_FREEFALL:
            if (impact) {
                ESP_LOGW(TAG, "Phat hien va cham sau roi: A=%.2fg", imu->accel_total_g);
                set_fall_state(FALL_STATE_IMPACT);
            } else if ((now - fall_state_start_ms) > FREEFALL_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Huy trang thai roi tu do");
                set_fall_state(FALL_STATE_NORMAL);
            }
            break;

        case FALL_STATE_IMPACT:
            if (still) {
                if (still_start_ms == 0) {
                    still_start_ms = now;
                    ESP_LOGW(TAG, "Sau va cham, bat dau kiem tra bat dong...");
                }

                if ((now - still_start_ms) >= STILL_TIME_MS) {
                    ESP_LOGE(TAG, "CANH BAO: PHAT HIEN TE NGA!");

                    start_help_alert("FALL_DETECTED");

                    set_fall_state(FALL_STATE_ALERT);
                }
            } else {
                still_start_ms = 0;
            }

            if ((now - fall_state_start_ms) > POST_IMPACT_WINDOW_MS) {
                ESP_LOGI(TAG, "Qua thoi gian xac nhan, khong tinh la te nga");
                set_fall_state(FALL_STATE_NORMAL);
            }
            break;

        case FALL_STATE_ALERT:
            if ((now - fall_state_start_ms) > FALL_ALERT_DURATION_MS) {
                ESP_LOGI(TAG, "Ket thuc canh bao te nga, vao cooldown");
                set_fall_state(FALL_STATE_COOLDOWN);
            }
            break;

        case FALL_STATE_COOLDOWN:
            if ((now - fall_state_start_ms) > FALL_COOLDOWN_MS) {
                ESP_LOGI(TAG, "San sang phat hien te nga lai");
                set_fall_state(FALL_STATE_NORMAL);
            }
            break;

        default:
            set_fall_state(FALL_STATE_NORMAL);
            break;
    }
}

// ======================= CẬP NHẬT NGÕ RA CẢNH BÁO =======================

static void alert_output_update(void)
{
    bool alert_on = false;

    if (fall_state == FALL_STATE_ALERT) {
        alert_on = true;
    }

    if (help_alert_active) {
        alert_on = true;
    }

    gpio_set_level(ALERT_GPIO, alert_on ? 1 : 0);
}

// ======================= APP MAIN =======================

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_init_all());

    esp_err_t ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loi khoi tao I2C: %s", esp_err_to_name(ret));
        return;
    }

    i2c_scan_once();

    ret = mpu6050_add_device_auto_address();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khong add duoc MPU6050 vao I2C bus");
        return;
    }

    ret = mpu6050_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "BAT DAU DONG HO PHAT HIEN TE NGA");
    ESP_LOGI(TAG, "SDA GPIO%d, SCL GPIO%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
    ESP_LOGI(TAG, "ALERT GPIO%d", ALERT_GPIO);
    ESP_LOGI(TAG, "SOS BUTTON GPIO%d", SOS_BUTTON_GPIO);
    ESP_LOGI(TAG, "====================================");

    imu_data_t imu;
    int64_t last_print_ms = 0;

    while (1) {
        ret = mpu6050_read_data(&imu);

        if (ret == ESP_OK) {
            fall_detection_update(&imu);

            int64_t now = millis();

            if ((now - last_print_ms) >= 500) {
                last_print_ms = now;

                ESP_LOGI(TAG,
                         "A=%.2fg | G=%.1fdps | ax=%.2f ay=%.2f az=%.2f | gx=%.1f gy=%.1f gz=%.1f",
                         imu.accel_total_g,
                         imu.gyro_total_dps,
                         imu.ax_g,
                         imu.ay_g,
                         imu.az_g,
                         imu.gx_dps,
                         imu.gy_dps,
                         imu.gz_dps);
            }
        } else {
            ESP_LOGE(TAG, "Loi doc MPU6050: %s", esp_err_to_name(ret));
        }

        sos_button_update();
        help_alert_update();
        alert_output_update();

        // 50 Hz
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}