#ifndef TEXUGOS_RTL8192SE_H
#define TEXUGOS_RTL8192SE_H

#include <stdint.h>

struct rtl8192se_firmware;

struct rtl8192se_status {
    uint16_t device_id;
    uint32_t mmio_base;
    uint32_t command_ring;
    uint32_t rx_mpdu_ring;
    uint32_t rx_command_ring;
    uint32_t tx_data_ring;
    uint32_t tx_management_ring;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t probed;
    uint8_t dma_prepared;
    uint8_t rx_dma_prepared;
    uint8_t tx_dma_prepared;
    uint8_t mac_power_ready;
    uint8_t rx_rings_programmed;
    uint8_t tx_rings_programmed;
    uint8_t adapter_config_read;
    uint8_t mac_address[6];
    uint8_t rf_configuration;
    uint8_t channel_plan;
    uint8_t firmware_uploaded;
};

int rtl8192se_probe(struct rtl8192se_status *status);
int rtl8192se_prepare_command_dma(struct rtl8192se_status *status);
int rtl8192se_build_command_ring(uint8_t *ring, uint32_t physical_address,
                                uint32_t descriptor_count);
int rtl8192se_start_firmware(struct rtl8192se_status *status,
                             const struct rtl8192se_firmware *firmware);
int rtl8192se_send_80211_frame(const uint8_t *frame, uint32_t frame_length,
                               int management_frame);
int rtl8192se_receive_80211_frame(uint8_t *frame, uint32_t frame_capacity,
                                  uint32_t *frame_length);

#endif
