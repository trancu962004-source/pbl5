#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "LOADCELL_TX";

#define WING_ID                 'B'
#define ROOM_ID                 1
#define BED_ID                  2
#define PATIENT_ID              3
#define NODE_TYPE_LOADCELL      2

#define HX711_DOUT              GPIO_NUM_1
#define HX711_SCK               GPIO_NUM_0
#define IR_DROP_GPIO            GPIO_NUM_10

#define HX711_SAMPLES           10
#define HX711_SAMPLE_DELAY_MS   20
#define HX711_READY_TIMEOUT_MS  1000
#define HX711_STARTUP_DELAY_MS  1000
#define ADV_UPDATE_INTERVAL_MS  1000

// Sửa theo cảm biến hồng ngoại digital vừa test ổn
#define MIN_DROP_GAP_US         180000
#define DROP_REPORT_INTERVAL_MS 10000

#define LOADCELL_SCALE_FACTOR   255.0f
#define NEGATIVE_CLAMP_ML       -2.0f

#define BLE_COMPANY_ID_LOW      0xFF
#define BLE_COMPANY_ID_HIGH     0xFF
#define PACKET_MAGIC            0xA5
#define DEVICE_NAME             "LOADCELL_B_R01_B03"
#define SEQ_START_VALUE         100

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
    uint16_t volume_ml;
    uint16_t drops_per_min;
} loadcell_data_t;

static uint8_t s_own_addr_type = 0;
static bool s_ble_synced = false;
static int32_t s_hx711_offset = 0;
static uint8_t s_seq = SEQ_START_VALUE;

static volatile uint32_t s_drop_total = 0;
static volatile uint32_t s_drop_count_window = 0;
static volatile int64_t s_last_drop_us = 0;
static uint32_t s_last_drops_per_min = 0;
static TickType_t s_last_rate_tick = 0;

static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;

static loadcell_adv_pkt_t s_adv_pkt = {
    .magic = PACKET_MAGIC,
    .wing_id = (uint8_t)WING_ID,
    .room_id = ROOM_ID,
    .bed_id = BED_ID,
    .patient_id = PATIENT_ID,
    .node_type = NODE_TYPE_LOADCELL,
    .volume_ml = 0,
    .drops_per_min = 0,
    .seq = SEQ_START_VALUE,
    .checksum = 0,
};

static uint8_t calc_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
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

static void IRAM_ATTR drop_isr_handler(void *arg)
{
    (void)arg;

    int64_t now_us = esp_timer_get_time();

    if ((now_us - s_last_drop_us) < MIN_DROP_GAP_US) {
        return;
    }

    s_last_drop_us = now_us;

    portENTER_CRITICAL_ISR(&s_drop_mux);
    s_drop_total++;
    s_drop_count_window++;
    portEXIT_CRITICAL_ISR(&s_drop_mux);
}

static esp_err_t hx711_gpio_init(void)
{
    gpio_config_t in_conf = {
        .pin_bit_mask = 1ULL << HX711_DOUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_conf), TAG, "hx711 dout init failed");

    gpio_config_t out_conf = {
        .pin_bit_mask = 1ULL << HX711_SCK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "hx711 sck init failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(HX711_SCK, 0), TAG, "set HX711_SCK failed");
    return ESP_OK;
}

static esp_err_t drop_sensor_init(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << IR_DROP_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&conf), TAG, "drop sensor init failed");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "gpio_install_isr_service failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(IR_DROP_GPIO, drop_isr_handler, NULL), TAG,
                        "gpio_isr_handler_add failed");

    s_last_rate_tick = xTaskGetTickCount();
    return ESP_OK;
}

static bool hx711_wait_ready(uint32_t timeout_ms)
{
    uint32_t count = timeout_ms * 100;
    while (gpio_get_level(HX711_DOUT) == 1 && count > 0) {
        esp_rom_delay_us(10);
        count--;
    }
    return (count > 0);
}

static esp_err_t hx711_read_raw(int32_t *out_raw)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!hx711_wait_ready(HX711_READY_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "HX711 timeout waiting ready");
        return ESP_ERR_TIMEOUT;
    }

    int32_t data = 0;
    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_SCK, 1);
        esp_rom_delay_us(2);
        data = (data << 1) | gpio_get_level(HX711_DOUT);
        gpio_set_level(HX711_SCK, 0);
        esp_rom_delay_us(2);
    }

    gpio_set_level(HX711_SCK, 1);
    esp_rom_delay_us(2);
    gpio_set_level(HX711_SCK, 0);
    esp_rom_delay_us(2);

    if (data & 0x800000) {
        data |= ~0xFFFFFF;
    }

    *out_raw = data;
    return ESP_OK;
}

static esp_err_t hx711_read_average(uint32_t samples, int32_t *out_avg)
{
    if (out_avg == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t sum = 0;
    uint32_t ok_count = 0;

    for (uint32_t i = 0; i < samples; i++) {
        int32_t raw = 0;
        if (hx711_read_raw(&raw) == ESP_OK) {
            sum += raw;
            ok_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(HX711_SAMPLE_DELAY_MS));
    }

    if (ok_count == 0) {
        return ESP_FAIL;
    }

    *out_avg = (int32_t)(sum / (int64_t)ok_count);
    return ESP_OK;
}

static esp_err_t hx711_tare(uint32_t samples)
{
    int32_t avg = 0;
    ESP_RETURN_ON_ERROR(hx711_read_average(samples, &avg), TAG, "tare failed");
    s_hx711_offset = avg;
    ESP_LOGI(TAG, "Tare done, offset=%" PRId32, s_hx711_offset);
    return ESP_OK;
}

static uint16_t calc_drops_per_min(void)
{
    TickType_t now_tick = xTaskGetTickCount();
    TickType_t elapsed_tick = now_tick - s_last_rate_tick;

    if (elapsed_tick < pdMS_TO_TICKS(DROP_REPORT_INTERVAL_MS)) {
        return (uint16_t)s_last_drops_per_min;
    }

    uint32_t count = 0;
    portENTER_CRITICAL(&s_drop_mux);
    count = s_drop_count_window;
    s_drop_count_window = 0;
    portEXIT_CRITICAL(&s_drop_mux);

    s_last_rate_tick = now_tick;
    s_last_drops_per_min = count * 6;  // 10 giây -> 1 phút

    if (s_last_drops_per_min > 65535UL) {
        s_last_drops_per_min = 65535UL;
    }

    ESP_LOGI(TAG,
             "DROP | 10s_count=%" PRIu32 " dpm=%" PRIu32 " total=%" PRIu32,
             count, s_last_drops_per_min, (uint32_t)s_drop_total);

    return (uint16_t)s_last_drops_per_min;
}

static esp_err_t loadcell_read(loadcell_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t raw = 0;
    ESP_RETURN_ON_ERROR(hx711_read_average(HX711_SAMPLES, &raw), TAG, "read average failed");

    float weight_g = ((float)(raw - s_hx711_offset)) / LOADCELL_SCALE_FACTOR;
    float volume_ml = weight_g;

    if (volume_ml < 0.0f && volume_ml > NEGATIVE_CLAMP_ML) {
        volume_ml = 0.0f;
    }
    if (volume_ml < 0.0f) {
        volume_ml = 0.0f;
    }
    if (volume_ml > 65535.0f) {
        volume_ml = 65535.0f;
    }

    out->volume_ml = (uint16_t)(volume_ml + 0.5f);
    out->drops_per_min = calc_drops_per_min();
    return ESP_OK;
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

    uint8_t mfg_data[2 + sizeof(loadcell_adv_pkt_t)] = {0};
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
             "ADV LOADCELL | W=%c R=%u B=%u P=%u | ML=%u DPM=%u Seq=%u",
             s_adv_pkt.wing_id,
             s_adv_pkt.room_id,
             s_adv_pkt.bed_id,
             s_adv_pkt.patient_id,
             s_adv_pkt.volume_ml,
             s_adv_pkt.drops_per_min,
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

static void packet_fill_from_reading(const loadcell_data_t *reading)
{
    s_adv_pkt.magic = PACKET_MAGIC;
    s_adv_pkt.wing_id = (uint8_t)WING_ID;
    s_adv_pkt.room_id = ROOM_ID;
    s_adv_pkt.bed_id = BED_ID;
    s_adv_pkt.patient_id = PATIENT_ID;
    s_adv_pkt.node_type = NODE_TYPE_LOADCELL;
    s_adv_pkt.volume_ml = reading->volume_ml;
    s_adv_pkt.drops_per_min = reading->drops_per_min;
    s_adv_pkt.seq = next_seq();
    s_adv_pkt.checksum = calc_checksum((const uint8_t *)&s_adv_pkt, sizeof(s_adv_pkt) - 1);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(hx711_gpio_init());
    ESP_ERROR_CHECK(drop_sensor_init());
    vTaskDelay(pdMS_TO_TICKS(HX711_STARTUP_DELAY_MS));
    ESP_ERROR_CHECK(hx711_tare(HX711_SAMPLES));
    ESP_ERROR_CHECK(ble_init());

    while (1) {
        loadcell_data_t reading;
        if (loadcell_read(&reading) == ESP_OK) {
            packet_fill_from_reading(&reading);
            if (s_ble_synced) {
                ble_start_advertising();
            }
        } else {
            ESP_LOGE(TAG, "loadcell_read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(ADV_UPDATE_INTERVAL_MS));
    }
}