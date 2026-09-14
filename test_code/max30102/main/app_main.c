#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "ble_node.h"
#include "sensor_node.h"

static const char *TAG = "APP";

#define WING_ID         'B'
#define ROOM_ID         1
#define BED_ID          3
#define PATIENT_ID      2
#define NODE_TYPE       1

void app_main(void)
{
    ESP_ERROR_CHECK(sensor_node_init());
    ESP_ERROR_CHECK(ble_node_init("VITAL_A_R01_B02"));

    uint8_t seq = 0;

    while (1) {
        vital_data_t vd;
        if (sensor_node_read(&vd) == ESP_OK) {
            vital_adv_pkt_t pkt = {0};

            pkt.magic       = 0xA5;
            pkt.wing_id     = WING_ID;
            pkt.room_id     = ROOM_ID;
            pkt.bed_id      = BED_ID;
            pkt.patient_id  = PATIENT_ID;
            pkt.node_type   = NODE_TYPE;
            pkt.hr          = vd.hr;
            pkt.spo2        = vd.spo2;
            pkt.temp_x100   = vd.temp_x100;
            pkt.status      = vd.status;
            pkt.battery     = vd.battery;
            pkt.seq         = seq++;
            pkt.checksum    = ble_calc_checksum((uint8_t *)&pkt, sizeof(pkt) - 1);

            ESP_LOGI(TAG,
                     "ADV pkt | Wing=%c Room=%u Bed=%u Patient=%u | HR=%u SpO2=%u Temp=%.2fC Bat=%u Seq=%u",
                     pkt.wing_id,
                     pkt.room_id,
                     pkt.bed_id,
                     pkt.patient_id,
                     pkt.hr,
                     pkt.spo2,
                     pkt.temp_x100 / 100.0f,
                     pkt.battery,
                     pkt.seq);

            ble_node_update_adv_packet(&pkt);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}