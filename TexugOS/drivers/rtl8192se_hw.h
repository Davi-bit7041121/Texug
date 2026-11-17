#ifndef TEXUGOS_RTL8192SE_HW_H
#define TEXUGOS_RTL8192SE_HW_H

#include <stdint.h>

struct rtl8192se_firmware;

struct rtl8192se_adapter_config {
    uint8_t mac_address[6];
    uint8_t rf_configuration;
    uint8_t channel_plan;
};

struct rtl8192se_register_io {
    void *context;
    void *bus_context;
    uint8_t (*read8)(void *context, uint16_t offset);
    uint16_t (*read16)(void *context, uint16_t offset);
    uint32_t (*read32)(void *context, uint16_t offset);
    void (*write8)(void *context, uint16_t offset, uint8_t value);
    void (*write16)(void *context, uint16_t offset, uint16_t value);
    void (*write32)(void *context, uint16_t offset, uint32_t value);
    void (*delay_us)(void *context, uint32_t usec);
    int (*enable_bus_mastering)(void *context);
};

int rtl8192se_mac_power_sequence(const struct rtl8192se_register_io *io,
                                 uint32_t command_ring_address);
int rtl8192se_parse_efuse(const uint8_t *physical_map, uint32_t map_size,
                          struct rtl8192se_adapter_config *config);
int rtl8192se_read_adapter_config(
    const struct rtl8192se_register_io *io,
    struct rtl8192se_adapter_config *config);
int rtl8192se_build_rx_ring(uint8_t *ring, uint32_t ring_address,
                            const uint32_t *buffer_addresses,
                            uint32_t descriptor_count,
                            uint32_t buffer_capacity);
int rtl8192se_rearm_rx_descriptor(uint8_t *descriptor,
                                  uint32_t buffer_address,
                                  uint32_t buffer_capacity,
                                  int end_of_ring);
int rtl8192se_program_rx_rings(const struct rtl8192se_register_io *io,
                               uint32_t mpdu_ring_address,
                               uint32_t command_ring_address);
int rtl8192se_build_tx_descriptor(uint8_t *descriptor,
                                  uint32_t buffer_address,
                                  uint32_t frame_length,
                                  uint32_t next_descriptor_address,
                                  uint8_t queue_selection,
                                  uint8_t non_qos);
int rtl8192se_program_tx_rings(const struct rtl8192se_register_io *io,
                               uint32_t data_ring_address,
                               uint32_t management_ring_address);
int rtl8192se_rx_copy_frame(const uint8_t *descriptor,
                            const uint8_t *buffer,
                            uint32_t buffer_capacity, uint8_t *frame,
                            uint32_t frame_capacity, uint32_t *frame_length);
int rtl8192se_upload_firmware(const struct rtl8192se_register_io *io,
                              const struct rtl8192se_firmware *firmware,
                              uint8_t *ring, uint32_t ring_address,
                              uint32_t ring_count, uint8_t *buffer,
                              uint32_t buffer_address,
                              uint32_t buffer_capacity,
                              uint8_t rf_configuration);

#endif
