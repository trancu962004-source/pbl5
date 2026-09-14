#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "driver/i2c.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#include "max30100.h"

static const char *TAG = "WATCH_TX_REAL";

/* =========================
 * I2C + MAX30100
 * ========================= */
#define I2C_PORT                I2C_NUM_0
#define I2C_SDA_GPIO            4
#define I2C_SCL_GPIO            3
#define I2C_FREQ_HZ             50000

#define REPORT_INTERVAL_MS      1000
#define SPO2_INTERVAL_MS        5000
#define NO_FINGER_RESET_MS      1000

#define HR_MIN_BPM              45.0f
#define HR_MAX_BPM              120.0f

#define SATURATION_LIMIT        65000U
#define MIN_VALID_IR            1000U
#define MIN_VALID_RED           1000U

/* =========================
 * BLE packet format
 * ========================= */
#define WING_ID                 'B'
#define ROOM_ID                 1
#define BED_ID                  2
#define PATIENT_ID              3
#define NODE_TYPE_VITAL         1

#define BLE_COMPANY_ID_LOW      0xFF
#define BLE_COMPANY_ID_HIGH     0xFF
#define PACKET_MAGIC            0xA5
#define DEVICE_NAME             "WATCH_B_R01_B02"
#define ADV_UPDATE_INTERVAL_MS  1000
#define SEQ_START_VALUE         100

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

typedef struct {
    uint16_t hr;
    uint16_t spo2;
    uint16_t temp_x100;
    uint8_t status;
    uint8_t battery;
} vital_data_t;

/* =========================
 * Pulse oximeter state
 * ========================= */
typedef struct {
    float ir_dc;
    float red_dc;

    float ir_prev;
    float ir_curr;

    bool peak_armed;
    int64_t last_peak_ms;
    float hr_bpm;

    uint32_t last_ir;
    uint32_t last_red;

    uint32_t good_samples;
    uint32_t bad_samples;
    uint32_t sat_samples;

    float spo2_ir_ac_sq_sum;
    float spo2_red_ac_sq_sum;
    float spo2_ir_dc_sum;
    float spo2_red_dc_sum;
    uint32_t spo2_sample_count;

    float spo2_value;
    bool spo2_valid;
} pulseox_state_t;

/* =========================
 * Globals
 * ========================= */
static max30100_t g_sensor;
static pulseox_state_t g_pox;

static vital_data_t g_latest_vital = {0};
static portMUX_TYPE g_data_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_own_addr_type = 0;
static bool s_ble_synced = false;
static uint8_t s_seq = SEQ_START_VALUE;

static int64_t g_last_valid_sample_ms = 0;

static vital_adv_pkt_t s_adv_pkt = {
    .magic = PACKET_MAGIC,
    .wing_id = (uint8_t)WING_ID,
    .room_id = ROOM_ID,
    .bed_id = BED_ID,
    .patient_id = PATIENT_ID,
    .node_type = NODE_TYPE_VITAL,
};

/* =========================
 * Time helpers
 * ========================= */
static int64_t millis_now(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* =========================
 * I2C init
 * ========================= */
static void i2c_master_init_simple(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));

    esp_err_t err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

/* =========================
 * MAX30100 signal processing
 * ========================= */
static void pulseox_init(pulseox_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->peak_armed = true;
    st->last_peak_ms = 0;
    st->hr_bpm = 0.0f;
    st->spo2_value = 0.0f;
    st->spo2_valid = false;
}

static void pulseox_reset_report_window(pulseox_state_t *st)
{
    st->good_samples = 0;
    st->bad_samples = 0;
    st->sat_samples = 0;
}

static void pulseox_reset_spo2_window(pulseox_state_t *st)
{
    st->spo2_ir_ac_sq_sum = 0.0f;
    st->spo2_red_ac_sq_sum = 0.0f;
    st->spo2_ir_dc_sum = 0.0f;
    st->spo2_red_dc_sum = 0.0f;
    st->spo2_sample_count = 0;
}

static void pulseox_clear_measurement(pulseox_state_t *st)
{
    st->ir_prev = 0.0f;
    st->ir_curr = 0.0f;
    st->peak_armed = true;
    st->last_peak_ms = 0;
    st->hr_bpm = 0.0f;

    st->spo2_value = 0.0f;
    st->spo2_valid = false;

    pulseox_reset_spo2_window(st);
}

static bool sample_is_saturated(uint32_t ir, uint32_t red)
{
    return (ir >= SATURATION_LIMIT || red >= SATURATION_LIMIT);
}

static bool sample_is_weird_pattern(uint32_t ir, uint32_t red)
{
    if ((ir == 8192U  && red == 16384U) ||
        (ir == 16384U && red == 8192U)  ||
        (ir == 16384U && red == 32768U) ||
        (ir == 32768U && red == 16384U)) {
        return true;
    }
    return false;
}

static bool sample_is_valid(uint32_t ir, uint32_t red)
{
    if (ir == 0 || red == 0) {
        return false;
    }

    if (sample_is_saturated(ir, red)) {
        return false;
    }

    if (ir < MIN_VALID_IR || red < MIN_VALID_RED) {
        return false;
    }

    if (sample_is_weird_pattern(ir, red)) {
        return false;
    }

    return true;
}

static void pulseox_process_sample(pulseox_state_t *st, uint32_t ir, uint32_t red)
{
    st->last_ir = ir;
    st->last_red = red;

    const float alpha = 0.95f;

    if (st->ir_dc == 0.0f) {
        st->ir_dc = (float)ir;
    }
    if (st->red_dc == 0.0f) {
        st->red_dc = (float)red;
    }

    st->ir_dc  = alpha * st->ir_dc  + (1.0f - alpha) * (float)ir;
    st->red_dc = alpha * st->red_dc + (1.0f - alpha) * (float)red;

    float ir_ac  = (float)ir  - st->ir_dc;
    float red_ac = (float)red - st->red_dc;

    st->good_samples++;

    st->spo2_ir_ac_sq_sum  += ir_ac * ir_ac;
    st->spo2_red_ac_sq_sum += red_ac * red_ac;
    st->spo2_ir_dc_sum     += st->ir_dc;
    st->spo2_red_dc_sum    += st->red_dc;
    st->spo2_sample_count++;

    {
        float prev = st->ir_prev;
        float curr = st->ir_curr;
        float next = ir_ac;

        float threshold = 300.0f;

        if (curr > threshold && curr > prev && curr > next) {
            int64_t now = millis_now();

            if (st->peak_armed) {
                if (st->last_peak_ms != 0) {
                    float dt_ms = (float)(now - st->last_peak_ms);

                    if (dt_ms >= 333.0f && dt_ms <= 1333.0f) {
                        float bpm = 60000.0f / dt_ms;

                        if (bpm >= HR_MIN_BPM && bpm <= HR_MAX_BPM) {
                            if (st->hr_bpm == 0.0f) {
                                st->hr_bpm = bpm;
                            } else {
                                st->hr_bpm = 0.92f * st->hr_bpm + 0.08f * bpm;
                            }
                        }
                    }
                }

                st->last_peak_ms = now;
                st->peak_armed = false;
            }
        }

        if (curr < 30.0f) {
            st->peak_armed = true;
        }

        st->ir_prev = st->ir_curr;
        st->ir_curr = ir_ac;
    }
}

static bool pulseox_compute_spo2(pulseox_state_t *st, float *spo2_out)
{
    if (st->spo2_sample_count < 300) {
        return false;
    }

    float ir_dc_avg  = st->spo2_ir_dc_sum  / (float)st->spo2_sample_count;
    float red_dc_avg = st->spo2_red_dc_sum / (float)st->spo2_sample_count;

    if (ir_dc_avg < 100.0f || red_dc_avg < 100.0f) {
        return false;
    }

    float ir_ac_rms  = sqrtf(st->spo2_ir_ac_sq_sum  / (float)st->spo2_sample_count);
    float red_ac_rms = sqrtf(st->spo2_red_ac_sq_sum / (float)st->spo2_sample_count);

    if (ir_ac_rms < 20.0f || red_ac_rms < 20.0f) {
        return false;
    }

    float ir_norm  = ir_ac_rms / ir_dc_avg;
    float red_norm = red_ac_rms / red_dc_avg;

    if (ir_norm <= 0.0f || red_norm <= 0.0f) {
        return false;
    }

    float r = red_norm / ir_norm;

    if (r < 0.4f || r > 1.3f) {
        return false;
    }

float spo2 = 110.0f - 15.0f * r;
    if (spo2 > 100.0f) spo2 = 100.0f;
    if (spo2 < 85.0f)  spo2 = 85.0f;

    *spo2_out = spo2;
    return true;
}

/* =========================
 * Sensor init
 * ========================= */
static esp_err_t sensor_node_init(void)
{
    i2c_master_init_simple();

    ESP_ERROR_CHECK(max30100_init(&g_sensor, I2C_PORT, MAX30100_I2C_ADDR));
    ESP_ERROR_CHECK(max30100_start_default(&g_sensor));
    ESP_ERROR_CHECK(max30100_clear_fifo(&g_sensor));

    ESP_ERROR_CHECK(
        max30100_set_spo2_config(&g_sensor,
                                 MAX30100_SR_50HZ,
                                 MAX30100_LED_PW_1600US,
                                 true)
    );

    ESP_ERROR_CHECK(
        max30100_set_led_current(&g_sensor,
                                 MAX30100_LED_CURR_27_1MA,
                                 MAX30100_LED_CURR_27_1MA)
    );

    ESP_ERROR_CHECK(max30100_set_mode(&g_sensor, MAX30100_MODE_SPO2));
    ESP_ERROR_CHECK(max30100_clear_fifo(&g_sensor));

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_ERROR_CHECK(
        max30100_set_spo2_config(&g_sensor,
                                 MAX30100_SR_100HZ,
                                 MAX30100_LED_PW_1600US,
                                 true)
    );

    ESP_ERROR_CHECK(max30100_set_mode(&g_sensor, MAX30100_MODE_SPO2));
    ESP_ERROR_CHECK(max30100_clear_fifo(&g_sensor));

    pulseox_init(&g_pox);
    pulseox_reset_report_window(&g_pox);
    pulseox_reset_spo2_window(&g_pox);
    g_last_valid_sample_ms = millis_now();

    ESP_LOGI(TAG, "MAX30100 init done");
    return ESP_OK;
}

/* =========================
 * Convert sensor state -> BLE payload data
 * ========================= */
static uint8_t calc_status(uint16_t hr, uint16_t spo2, uint16_t temp_x100)
{
    uint8_t status = 0;

    if (hr > 0 && (hr < 50 || hr > 120)) {
        status |= (1 << 0);
    }
    if (spo2 > 0 && spo2 < 94) {
        status |= (1 << 1);
    }
    if (temp_x100 > 0 && temp_x100 > 3800) {
        status |= (1 << 2);
    }

    return status;
}

/* =========================
 * Sensor task: poll MAX30100 liên tục
 * ========================= */
static void sensor_task(void *arg)
{
    (void)arg;

    int64_t last_spo2_ms = millis_now();
    int64_t last_report_ms = millis_now();

    while (1) {
        max30100_sample_t samples[16];
        size_t n = 0;

        esp_err_t err = max30100_poll_samples(&g_sensor, samples, 16, &n);
        if (err == ESP_OK) {
            for (size_t i = 0; i < n; i++) {
                uint32_t ir = samples[i].ir;
                uint32_t red = samples[i].red;

                g_pox.last_ir = ir;
                g_pox.last_red = red;

                if (sample_is_saturated(ir, red)) {
                    g_pox.sat_samples++;
                    continue;
                }

                if (!sample_is_valid(ir, red)) {
                    g_pox.bad_samples++;
                    continue;
                }

                g_last_valid_sample_ms = millis_now();
                pulseox_process_sample(&g_pox, ir, red);
            }
        } else {
            ESP_LOGW(TAG, "max30100_poll_samples failed: %s", esp_err_to_name(err));
        }

        int64_t now = millis_now();

        if ((now - g_last_valid_sample_ms) >= NO_FINGER_RESET_MS) {
            pulseox_clear_measurement(&g_pox);
        }

        if ((now - last_spo2_ms) >= SPO2_INTERVAL_MS) {
            float spo2 = 0.0f;
            bool ok = pulseox_compute_spo2(&g_pox, &spo2);

            if (ok) {
                if (!g_pox.spo2_valid) {
                    g_pox.spo2_value = spo2;
                    g_pox.spo2_valid = true;
                } else {
                    g_pox.spo2_value = 0.7f * g_pox.spo2_value + 0.3f * spo2;
                }
            }

            pulseox_reset_spo2_window(&g_pox);
            last_spo2_ms = now;
        }

        vital_data_t tmp = {0};
        tmp.hr = (g_pox.hr_bpm > 0.0f) ? (uint16_t)lroundf(g_pox.hr_bpm) : 0;
        tmp.spo2 = (g_pox.spo2_valid) ? (uint16_t)lroundf(g_pox.spo2_value) : 0;
        tmp.temp_x100 = 3650;
        tmp.battery = 100;
        tmp.status = calc_status(tmp.hr, tmp.spo2, tmp.temp_x100);

        portENTER_CRITICAL(&g_data_lock);
        g_latest_vital = tmp;
        portEXIT_CRITICAL(&g_data_lock);

        if ((now - last_report_ms) >= REPORT_INTERVAL_MS) {
            ESP_LOGI(TAG,
                     "REAL DATA | HR=%u bpm | SpO2=%u %% | Temp=%.2f | good=%lu bad=%lu sat=%lu | IR=%lu RED=%lu",
                     tmp.hr,
                     tmp.spo2,
                     tmp.temp_x100 / 100.0f,
                     (unsigned long)g_pox.good_samples,
                     (unsigned long)g_pox.bad_samples,
                     (unsigned long)g_pox.sat_samples,
                     (unsigned long)g_pox.last_ir,
                     (unsigned long)g_pox.last_red);

            pulseox_reset_report_window(&g_pox);
            last_report_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* =========================
 * BLE helpers
 * ========================= */
static uint8_t calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum ^= data[i];
    }
    return sum;
}

static uint8_t next_seq(void)
{
    uint8_t current = s_seq;
    if (s_seq >= 255) {
        s_seq = SEQ_START_VALUE;
    } else {
        s_seq++;
    }
    return current;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        ESP_LOGI(TAG, "Advertising complete, restarting");
    }
    return 0;
}

static void ble_start_advertising(void)
{
    if (!s_ble_synced) {
        return;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
    }

    uint8_t mfg_data[2 + sizeof(vital_adv_pkt_t)] = {0};
    mfg_data[0] = BLE_COMPANY_ID_LOW;
    mfg_data[1] = BLE_COMPANY_ID_HIGH;
    memcpy(&mfg_data[2], &s_adv_pkt, sizeof(s_adv_pkt));

    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.mfg_data = mfg_data;
    adv_fields.mfg_data_len = sizeof(mfg_data);

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    const char *name = ble_svc_gap_device_name();
    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG,
             "ADV REAL | W=%c R=%u B=%u P=%u | HR=%u SpO2=%u Temp=%.2f Bat=%u Status=0x%02X Seq=%u",
             s_adv_pkt.wing_id,
             s_adv_pkt.room_id,
             s_adv_pkt.bed_id,
             s_adv_pkt.patient_id,
             s_adv_pkt.hr,
             s_adv_pkt.spo2,
             s_adv_pkt.temp_x100 / 100.0f,
             s_adv_pkt.battery,
             s_adv_pkt.status,
             s_adv_pkt.seq);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    s_ble_synced = true;
    ESP_LOGI(TAG, "BLE synced");
    ble_start_advertising();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_init(void)
{
    esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release rc=%s", esp_err_to_name(rel));
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static void packet_fill_from_sensor(const vital_data_t *vd)
{
    s_adv_pkt.magic = PACKET_MAGIC;
    s_adv_pkt.wing_id = (uint8_t)WING_ID;
    s_adv_pkt.room_id = ROOM_ID;
    s_adv_pkt.bed_id = BED_ID;
    s_adv_pkt.patient_id = PATIENT_ID;
    s_adv_pkt.node_type = NODE_TYPE_VITAL;
    s_adv_pkt.hr = vd->hr;
    s_adv_pkt.spo2 = vd->spo2;
    s_adv_pkt.temp_x100 = vd->temp_x100;
    s_adv_pkt.status = vd->status;
    s_adv_pkt.battery = vd->battery;
    s_adv_pkt.seq = next_seq();
    s_adv_pkt.checksum = calc_checksum((const uint8_t *)&s_adv_pkt, sizeof(s_adv_pkt) - 1);
}

/* =========================
 * app_main
 * ========================= */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(sensor_node_init());
    ESP_ERROR_CHECK(ble_init());

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    while (1) {
        vital_data_t vd;

        portENTER_CRITICAL(&g_data_lock);
        vd = g_latest_vital;
        portEXIT_CRITICAL(&g_data_lock);

        packet_fill_from_sensor(&vd);

        if (s_ble_synced) {
            ble_start_advertising();
        }

        vTaskDelay(pdMS_TO_TICKS(ADV_UPDATE_INTERVAL_MS));
    }
}