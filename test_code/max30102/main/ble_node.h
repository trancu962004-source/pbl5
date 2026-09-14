#ifndef BLE_NODE_H
#define BLE_NODE_H

#include <stdint.h>
#include "esp_err.h"

typedef struct __attribute__((packed)) {
    uint8_t  magic;        // 0xA5
    uint8_t  wing_id;      // 'A' hoặc 'B'
    uint8_t  room_id;
    uint8_t  bed_id;
    uint8_t  patient_id;
    uint8_t  node_type;    // 1 = vital
    uint16_t hr;
    uint16_t spo2;
    uint16_t temp_x100;
    uint8_t  status;
    uint8_t  battery;
    uint8_t  seq;
    uint8_t  checksum;
} vital_adv_pkt_t;

esp_err_t ble_node_init(const char *device_name);
esp_err_t ble_node_update_adv_packet(const vital_adv_pkt_t *pkt);
uint8_t ble_calc_checksum(const uint8_t *data, uint16_t len);

#endif