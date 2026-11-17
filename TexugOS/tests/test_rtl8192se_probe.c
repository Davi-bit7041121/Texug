#include <stdint.h>
#include <stdio.h>

#include "frame_allocator.h"
#include "paging.h"
#include "pci.h"
#include "rtl8192se.h"
#include "rtl8192se_firmware.h"
#include "rtl8192se_hw.h"

static struct pci_device test_device;
static struct pci_bar test_bar;
static uint32_t mapped_address;
static uint32_t mapped_size;
static int map_succeeds = 1;
static uint8_t mock_registers[0x4000];
static uint8_t mock_efuse[512];
static int mock_efuse_ready = 1;
static uint32_t delay_call_count;
static int fail_clock_switch;
static uint32_t mock_tcr;
static uint32_t mock_poll_count;
static int suppress_dma_completion;
static int bus_master_enabled;
static uint8_t test_ring[16u * 64u] __attribute__((aligned(256)));
static uint8_t *test_dma_buffer;

static uint32_t read_le32(const uint8_t *bytes);

uint32_t frame_allocate_below(uint64_t address_limit)
{
    (void)address_limit;
    return FRAME_INVALID;
}

int frame_free(uint32_t physical_address)
{
    (void)physical_address;
    return 0;
}

uint32_t fat32_root_cluster(void)
{
    return 0u;
}

int fat32_read_file(uint32_t directory, const char *name, void *buffer,
                    uint32_t capacity, uint32_t *size)
{
    (void)directory;
    (void)name;
    (void)buffer;
    (void)capacity;
    (void)size;
    return 0;
}

uint32_t pci_device_count(void)
{
    return 1u;
}

int pci_get_device(uint32_t index, struct pci_device *device)
{
    if (index != 0u || device == 0) {
        return 0;
    }
    *device = test_device;
    return 1;
}

int pci_read_bar(const struct pci_device *device, uint8_t bar_index,
                 struct pci_bar *bar)
{
    if (device == 0 || bar == 0 || bar_index != 1u) {
        return 0;
    }
    *bar = test_bar;
    return 1;
}

int pci_enable_memory_space(const struct pci_device *device)
{
    return device != 0;
}

int pci_enable_bus_mastering(const struct pci_device *device)
{
    if (device == 0) {
        return 0;
    }
    bus_master_enabled = 1;
    return 1;
}

int paging_map_mmio(uint32_t address, uint32_t size)
{
    mapped_address = address;
    mapped_size = size;
    return map_succeeds;
}

static uint8_t mock_read8(void *context, uint16_t offset)
{
    (void)context;
    if (offset == 0x0030u) {
        uint16_t address = (uint16_t)mock_registers[0x0031u] |
            (uint16_t)((mock_registers[0x0032u] & 0x03u) << 8);
        return mock_efuse[address];
    }
    if (offset == 0x0033u) {
        return mock_efuse_ready ? 0x80u : 0u;
    }
    if (fail_clock_switch && offset == 0x0009u) {
        return 0u;
    }
    return mock_registers[offset];
}

static uint16_t mock_read16(void *context, uint16_t offset)
{
    (void)context;
    return (uint16_t)(mock_registers[offset] |
                      ((uint16_t)mock_registers[offset + 1u] << 8));
}

static uint32_t mock_read32(void *context, uint16_t offset)
{
    (void)context;
    if (offset == 0x0044u) {
        return mock_tcr;
    }
    return (uint32_t)mock_registers[offset] |
           ((uint32_t)mock_registers[offset + 1u] << 8) |
           ((uint32_t)mock_registers[offset + 2u] << 16) |
           ((uint32_t)mock_registers[offset + 3u] << 24);
}

static void mock_write8(void *context, uint16_t offset, uint8_t value)
{
    (void)context;
    mock_registers[offset] = value;
}

static void mock_write16(void *context, uint16_t offset, uint16_t value)
{
    (void)context;
    mock_registers[offset] = (uint8_t)value;
    mock_registers[offset + 1u] = (uint8_t)(value >> 8);
    if (offset == 0x0040u && value == 0x3014u) {
        mock_tcr = 0x0au;
    } else if (offset == 0x0500u) {
        uint32_t descriptor_index = mock_poll_count % 16u;
        if (!suppress_dma_completion) {
            uint8_t *descriptor =
                &test_ring[descriptor_index * 64u];
            descriptor[3u] &= 0x7fu;
        }
        if (mock_poll_count == 0u) {
            mock_tcr = 0x03u;
        } else if (mock_poll_count == 1u) {
            mock_tcr = 0x0cu;
        } else {
            mock_tcr = 0xffu;
        }
        ++mock_poll_count;
    } else if (offset == 0x0002u || offset == 0x0008u) {
        mock_tcr |= 0x20u;
    }
}

static void mock_write32(void *context, uint16_t offset, uint32_t value)
{
    (void)context;
    mock_registers[offset] = (uint8_t)value;
    mock_registers[offset + 1u] = (uint8_t)(value >> 8);
    mock_registers[offset + 2u] = (uint8_t)(value >> 16);
    mock_registers[offset + 3u] = (uint8_t)(value >> 24);
    if (offset == 0x0044u) {
        mock_tcr = value;
    }
}

static void mock_delay(void *context, uint32_t usec)
{
    (void)context;
    if (usec != 0u) {
        ++delay_call_count;
    }
}

static int mock_enable_bus_mastering(void *context)
{
    (void)context;
    if ((test_ring[3u] & 0x80u) == 0u ||
        read_le32(&test_ring[28u]) == 0u ||
        read_le32(&test_ring[32u]) != 0x00200000u ||
        test_dma_buffer == 0) {
        return 0;
    }
    bus_master_enabled = 1;
    return 1;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void reset_test_device(uint16_t vendor_id, uint16_t device_id)
{
    test_device = (struct pci_device) {
        .bus = 2u,
        .slot = 3u,
        .function = 1u,
        .vendor_id = vendor_id,
        .device_id = device_id,
    };
    test_bar = (struct pci_bar) {
        .address = 0xf0000000u,
        .is_io = 0u,
        .is_64bit = 0u,
    };
    mapped_address = 0u;
    mapped_size = 0u;
    map_succeeds = 1;
}

int main(void)
{
    struct rtl8192se_status status;
    uint8_t ring[16u * 64u];
    if (!rtl8192se_build_command_ring(ring, 0x00100000u, 16u) ||
        read_le32(&ring[36u]) != 0x00100040u ||
        read_le32(&ring[15u * 64u + 36u]) != 0x00100000u ||
        rtl8192se_build_command_ring(ring, 0x00100001u, 16u) ||
        rtl8192se_build_command_ring(ring, 0x00100000u, 17u) ||
        rtl8192se_build_command_ring(ring, 0x00ffffc0u, 16u)) {
        fputs("RTL8191SE command DMA ring layout is invalid\n", stderr);
        return 1;
    }

    uint8_t rx_ring[16u * 32u] __attribute__((aligned(256)));
    uint32_t rx_buffer_addresses[16u];
    for (uint32_t index = 0u; index < 16u; ++index) {
        rx_buffer_addresses[index] = 0x00300000u + index * 4096u;
    }
    if (!rtl8192se_build_rx_ring(rx_ring, 0x00110000u,
                                 rx_buffer_addresses, 16u, 4096u) ||
        read_le32(&rx_ring[0u]) != 0x80001000u ||
        read_le32(&rx_ring[24u]) != rx_buffer_addresses[0u] ||
        read_le32(&rx_ring[15u * 32u]) != 0xc0001000u ||
        read_le32(&rx_ring[15u * 32u + 24u]) !=
            rx_buffer_addresses[15u]) {
        fputs("RTL8191SE RX descriptor ring layout is invalid\n", stderr);
        return 1;
    }
    if (!rtl8192se_rearm_rx_descriptor(
            &rx_ring[3u * 32u], rx_buffer_addresses[3u], 4096u, 1) ||
        read_le32(&rx_ring[3u * 32u]) != 0xc0001000u ||
        read_le32(&rx_ring[3u * 32u + 24u]) != rx_buffer_addresses[3u] ||
        rtl8192se_rearm_rx_descriptor(&rx_ring[3u * 32u],
                                      0x01000000u, 4096u, 0)) {
        fputs("RTL8191SE RX descriptor rearm failed\n", stderr);
        return 1;
    }
    rx_buffer_addresses[3u] = 0x01000000u;
    if (rtl8192se_build_rx_ring(rx_ring, 0x00110000u,
                                rx_buffer_addresses, 16u, 4096u)) {
        fputs("RTL8191SE RX ring accepted an out-of-range DMA buffer\n",
              stderr);
        return 1;
    }

    uint8_t tx_descriptor[64u] __attribute__((aligned(64)));
    if (!rtl8192se_build_tx_descriptor(tx_descriptor, 0x00300000u, 128u,
                                       0x00120040u, 0x12u, 1u) ||
        read_le32(tx_descriptor) != 0x8c200080u ||
        read_le32(&tx_descriptor[4u]) != 0x00011200u ||
        read_le32(&tx_descriptor[16u]) != 0x80000000u ||
        read_le32(&tx_descriptor[28u]) != 128u ||
        read_le32(&tx_descriptor[32u]) != 0x00300000u ||
        read_le32(&tx_descriptor[36u]) != 0x00120040u ||
        !rtl8192se_build_tx_descriptor(tx_descriptor, 0x00300000u, 128u,
                                       0x00120040u, 0x12u, 0u) ||
        read_le32(&tx_descriptor[4u]) != 0x00001200u ||
        rtl8192se_build_tx_descriptor(tx_descriptor, 0x01000000u, 128u,
                                      0x00120040u, 0x12u, 1u) ||
        rtl8192se_build_tx_descriptor(tx_descriptor, 0x00300000u, 4097u,
                                      0x00120040u, 0x12u, 1u)) {
        fputs("RTL8191SE TX descriptor construction failed\n", stderr);
        return 1;
    }

    uint8_t rx_descriptor[32u] = {0};
    uint8_t rx_buffer[64u] = {0};
    uint8_t received_frame[32u] = {0};
    uint32_t received_length = 0u;
    for (uint32_t index = 0u; index < 14u; ++index) {
        rx_buffer[index + 10u] = (uint8_t)(0xa0u + index);
    }
    write_le32(rx_descriptor, (1u << 16) | (2u << 24) | 24u);
    if (!rtl8192se_rx_copy_frame(rx_descriptor, rx_buffer,
                                 sizeof(rx_buffer), received_frame,
                                 sizeof(received_frame), &received_length) ||
        received_length != 14u || received_frame[0] != 0xa0u ||
        received_frame[13] != 0xadu) {
        fputs("RTL8191SE RX frame extraction failed\n", stderr);
        return 1;
    }
    write_le32(rx_descriptor, 0x80000018u);
    if (rtl8192se_rx_copy_frame(rx_descriptor, rx_buffer,
                                sizeof(rx_buffer), received_frame,
                                sizeof(received_frame), &received_length)) {
        fputs("RTL8191SE RX frame extraction ignored descriptor ownership\n",
              stderr);
        return 1;
    }
    write_le32(rx_descriptor, (1u << 14) | 24u);
    if (rtl8192se_rx_copy_frame(rx_descriptor, rx_buffer,
                                sizeof(rx_buffer), received_frame,
                                sizeof(received_frame), &received_length)) {
        fputs("RTL8191SE RX frame extraction accepted a CRC error\n",
              stderr);
        return 1;
    }

    struct rtl8192se_register_io io = {
        .read8 = mock_read8,
        .read16 = mock_read16,
        .read32 = mock_read32,
        .write8 = mock_write8,
        .write16 = mock_write16,
        .write32 = mock_write32,
        .delay_us = mock_delay,
        .enable_bus_mastering = mock_enable_bus_mastering,
    };
    if (!rtl8192se_program_rx_rings(&io, 0x00110000u, 0x00120000u) ||
        read_le32(&mock_registers[0x053cu]) != 0x00110000u ||
        read_le32(&mock_registers[0x0538u]) != 0x00120000u ||
        rtl8192se_program_rx_rings(&io, 0x00110001u, 0x00120000u)) {
        fputs("RTL8191SE RX ring base programming failed\n", stderr);
        return 1;
    }
    if (!rtl8192se_program_tx_rings(&io, 0x00130000u, 0x00140000u) ||
        read_le32(&mock_registers[0x052cu]) != 0x00130000u ||
        read_le32(&mock_registers[0x0518u]) != 0x00140000u ||
        rtl8192se_program_tx_rings(&io, 0x00130001u, 0x00140000u)) {
        fputs("RTL8191SE TX ring base programming failed\n", stderr);
        return 1;
    }
    if (!rtl8192se_build_command_ring(test_ring, 0x00100000u, 16u) ||
        !rtl8192se_mac_power_sequence(&io, 0x00100000u) ||
        delay_call_count == 0u || mock_registers[0x0040u] != 0x14u ||
        mock_registers[0x0041u] != 0x30u ||
        mock_registers[0x051cu] != 0x00u ||
        mock_registers[0x051du] != 0x00u ||
        mock_registers[0x051eu] != 0x10u ||
        mock_registers[0x051fu] != 0x00u ||
        mock_registers[0x00a0u] != 0xffu ||
        mock_registers[0x00a8u] != 0xffu ||
        mock_registers[0x00abu] != 0x80u ||
        mock_registers[0x0503u] != 0x22u ||
        mock_registers[0x003au] != 0x91u) {
        fputs("RTL8191SE MAC power preparation failed\n", stderr);
        return 1;
    }

    for (uint32_t index = 0u; index < sizeof(mock_registers); ++index) {
        mock_registers[index] = 0u;
    }
    fail_clock_switch = 1;
    if (rtl8192se_mac_power_sequence(&io, 0x00100000u)) {
        fputs("RTL8191SE ignored a failed MAC clock switch\n", stderr);
        return 1;
    }
    fail_clock_switch = 0;

    uint8_t fake_firmware[135] = {0};
    fake_firmware[38u] = 0x22u;
    for (uint32_t index = 80u; index < sizeof(fake_firmware); ++index) {
        fake_firmware[index] = (uint8_t)(index + 1u);
    }
    struct rtl8192se_firmware firmware = {
        .bytes = fake_firmware,
        .size = sizeof(fake_firmware),
        .version = 1u,
        .imem_offset = 80u,
        .imem_size = 4u,
        .emem_offset = 84u,
        .emem_size = 3u,
        .dmem_offset = 32u,
        .dmem_size = 48u,
    };
    test_dma_buffer = fake_firmware;
    for (uint32_t index = 0u; index < sizeof(mock_registers); ++index) {
        mock_registers[index] = 0u;
    }
    mock_tcr = 0x0au;
    mock_poll_count = 0u;
    bus_master_enabled = 0;
    if (!rtl8192se_build_command_ring(test_ring, 0x00100000u, 16u) ||
        !rtl8192se_upload_firmware(&io, &firmware, test_ring,
                                   0x00100000u, 16u, fake_firmware,
                                   0x00200000u, 4096u, 0x22u) ||
        !bus_master_enabled || mock_poll_count != 3u) {
        fputs("RTL8191SE firmware download sequence failed\n", stderr);
        return 1;
    }

    mock_tcr = 0x0au;
    mock_poll_count = 0u;
    bus_master_enabled = 0;
    suppress_dma_completion = 1;
    if (rtl8192se_upload_firmware(&io, &firmware, test_ring,
                                  0x00100000u, 16u, fake_firmware,
                                  0x00200000u, 4096u, 0x22u)) {
        fputs("RTL8191SE firmware upload ignored a stuck DMA descriptor\n",
              stderr);
        return 1;
    }
    suppress_dma_completion = 0;

    for (uint32_t index = 0u; index < sizeof(mock_efuse); ++index) {
        mock_efuse[index] = 0xffu;
    }
    mock_efuse[0u] = 0x0eu;
    mock_efuse[1u] = 0x29u;
    mock_efuse[2u] = 0x81u;
    mock_efuse[3u] = 0x21u;
    mock_efuse[4u] = 0x00u;
    mock_efuse[5u] = 0x11u;
    mock_efuse[6u] = 0x22u;
    mock_efuse[7u] = 0x33u;
    mock_efuse[8u] = 0x44u;
    mock_efuse[9u] = 0x55u;
    mock_efuse[10u] = 0xf5u;
    mock_efuse[11u] = 0x00u;
    mock_efuse[12u] = 0x34u;
    mock_efuse[13u] = 0x01u;
    mock_efuse[14u] = 0x00u;
    mock_efuse[15u] = 0xffu;
    struct rtl8192se_adapter_config adapter_config;
    if (!rtl8192se_parse_efuse(mock_efuse, sizeof(mock_efuse),
                               &adapter_config) ||
        adapter_config.mac_address[0] != 0x00u ||
        adapter_config.mac_address[5] != 0x55u ||
        adapter_config.channel_plan != 0x34u ||
        adapter_config.rf_configuration != 0x12u) {
        fputs("RTL8191SE eFuse logical map parsing failed\n", stderr);
        return 1;
    }
    mock_efuse[1u] = 0u;
    if (rtl8192se_parse_efuse(mock_efuse, sizeof(mock_efuse),
                              &adapter_config)) {
        fputs("RTL8191SE accepted an invalid eFuse signature\n", stderr);
        return 1;
    }
    mock_efuse[1u] = 0x29u;
    if (!rtl8192se_read_adapter_config(&io, &adapter_config) ||
        adapter_config.mac_address[0] != 0x00u ||
        adapter_config.mac_address[5] != 0x55u ||
        adapter_config.channel_plan != 0x34u ||
        adapter_config.rf_configuration != 0x12u) {
        fputs("RTL8191SE eFuse adapter configuration parsing failed\n",
              stderr);
        return 1;
    }
    mock_efuse[13u] = 0x02u;
    if (!rtl8192se_parse_efuse(mock_efuse, sizeof(mock_efuse),
                               &adapter_config) ||
        adapter_config.rf_configuration != 0x12u) {
        fputs("RTL8191SE mapped a one-TX/two-RX RF board incorrectly\n",
              stderr);
        return 1;
    }
    mock_efuse[13u] = 0x03u;
    if (!rtl8192se_parse_efuse(mock_efuse, sizeof(mock_efuse),
                               &adapter_config) ||
        adapter_config.rf_configuration != 0x11u) {
        fputs("RTL8191SE mapped a one-TX/one-RX RF board incorrectly\n",
              stderr);
        return 1;
    }
    mock_efuse[13u] = 0x01u;
    mock_efuse[13u] = 0xffu;
    if (rtl8192se_read_adapter_config(&io, &adapter_config)) {
        fputs("RTL8191SE accepted an invalid eFuse board type\n", stderr);
        return 1;
    }
    mock_efuse[13u] = 0x01u;
    mock_efuse_ready = 0;
    if (rtl8192se_read_adapter_config(&io, &adapter_config)) {
        fputs("RTL8191SE ignored an eFuse read timeout\n", stderr);
        return 1;
    }
    mock_efuse_ready = 1;

    reset_test_device(0x10ecu, 0x8171u);
    if (!rtl8192se_probe(&status) || status.probed == 0u ||
        status.device_id != 0x8171u || status.mmio_base != 0xf0000000u ||
        status.bus != 2u || status.slot != 3u || status.function != 1u ||
        mapped_address != 0xf0000000u || mapped_size != 0x4000u) {
        fputs("RTL8191SE PCI/MMIO probe rejected supported device\n", stderr);
        return 1;
    }

    reset_test_device(0x10ecu, 0x8175u);
    if (rtl8192se_probe(&status) || status.probed != 0u ||
        mapped_address != 0u) {
        fputs("RTL8192SE probe accepted unsupported device ID\n", stderr);
        return 1;
    }

    reset_test_device(0x10ecu, 0x8171u);
    test_bar.is_io = 1u;
    if (rtl8192se_probe(&status) || status.probed != 0u ||
        mapped_address != 0u) {
        fputs("RTL8192SE probe accepted an I/O BAR for MMIO\n", stderr);
        return 1;
    }

    reset_test_device(0x10ecu, 0x8171u);
    test_bar.address = 0x100000000ull;
    if (rtl8192se_probe(&status) || status.probed != 0u ||
        mapped_address != 0u) {
        fputs("RTL8192SE probe accepted an unmappable 64-bit BAR\n", stderr);
        return 1;
    }

    reset_test_device(0x10ecu, 0x8171u);
    map_succeeds = 0;
    if (rtl8192se_probe(&status) || status.probed != 0u) {
        fputs("RTL8192SE probe ignored MMIO mapping failure\n", stderr);
        return 1;
    }
    return 0;
}
