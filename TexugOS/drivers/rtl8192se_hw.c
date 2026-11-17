#include "rtl8192se_hw.h"
#include "rtl8192se_firmware.h"

#define SYS_FUNC_EN 0x0002u
#define SYS_CLKR 0x0008u
#define AFE_MISC 0x0010u
#define LDOA15_CTRL 0x0020u
#define AFE_XTAL_CTRL 0x0026u
#define AFE_PLL_CTRL 0x0028u
#define SYS_ISO_CTRL 0x0000u
#define EFUSE_CONTROL 0x0030u
#define EFUSE_TEST 0x0034u
#define EFUSE_PHYSICAL_SIZE 512u
#define EFUSE_LOGICAL_SIZE 128u
#define EFUSE_SECTION_COUNT 16u
#define EFUSE_WORD_COUNT 4u
#define RX_DESCRIPTOR_SIZE 32u
#define RX_DESCRIPTOR_COUNT_MAX 64u
#define RX_DESCRIPTOR_OWN (1u << 31)
#define RX_DESCRIPTOR_END_OF_RING (1u << 30)
#define RX_DESCRIPTOR_CRC_ERROR (1u << 14)
#define RX_DESCRIPTOR_ICV_ERROR (1u << 15)
#define RX_DESCRIPTOR_LENGTH_MASK 0x3fffu
#define RX_DESCRIPTOR_INFO_LENGTH_MASK (0x0fu << 16)
#define RX_DESCRIPTOR_BUFFER_SHIFT_MASK (0x03u << 24)
#define RX_DESCRIPTOR_BUFFER_ADDRESS_OFFSET 24u
#define RX_BUFFER_MAX_SIZE 0x3fffu
#define RX_DMA_ADDRESS_LIMIT 0x01000000u
#define RX_MPDU_RING_ADDRESS 0x053cu
#define RX_COMMAND_RING_ADDRESS 0x0538u
#define TX_MANAGEMENT_RING_ADDRESS 0x0518u
#define TX_DATA_RING_ADDRESS 0x052cu
#define TX_POLL 0x0500u
#define EEPROM_SIGNATURE 0x8129u
#define EEPROM_SIGNATURE_OFFSET 0x00u
#define EEPROM_MAC_ADDRESS 0x12u
#define EEPROM_CHANNEL_PLAN 0x7bu
#define EEPROM_BOARD_TYPE 0x7eu
#define RF_POWER_CONTROL 0x0561u
#define COMMAND 0x0040u
#define QUEUE_PAGE_NUMBER 0x00a0u
#define PCI_FUNCTION_CONTROL 0x0503u
#define DEBUG_PORT 0x003au
#define TCR 0x0044u
#define TX_COMMAND_DESCRIPTOR 0x051cu
#define RQPN 0x00a0u
#define FEN_CPUEN (1u << 10)
#define SYS_CPU_CLKSEL (1u << 2)
#define IMEM_RDY (1u << 5)
#define IMEM_CODE_DONE (1u << 0)
#define IMEM_CHECK_REPORT (1u << 1)
#define EMEM_CODE_DONE (1u << 2)
#define EMEM_CHECK_REPORT (1u << 3)
#define DMEM_CODE_DONE (1u << 4)
#define FIRMWARE_READY 0xffu
#define COMMAND_QUEUE_POLL (1u << 5)
#define COMMAND_QUEUE_COUNT 16u
#define COMMAND_DESCRIPTOR_SIZE 64u
#define DMA_CHUNK_SIZE 4096u
#define DMA_ADDRESS_LIMIT 0x01000000u
#define TCR_DMA_INIT_VALUE (IMEM_CHECK_REPORT | (1u << 3))
#define COMMAND_DMA_ENABLE (0x3000u | (1u << 4) | (1u << 2))
#define SYS_FUNC_ENABLE_CPU_BIT (1u << 10)
#define SYS_CLKR_CPU_BIT (1u << 2)

static uint8_t read8(const struct rtl8192se_register_io *io, uint16_t offset)
{
    return io->read8(io->context, offset);
}

static void write8(const struct rtl8192se_register_io *io, uint16_t offset,
                   uint8_t value)
{
    io->write8(io->context, offset, value);
}

static uint16_t read16(const struct rtl8192se_register_io *io, uint16_t offset)
{
    return io->read16(io->context, offset);
}

static void write16(const struct rtl8192se_register_io *io, uint16_t offset,
                    uint16_t value)
{
    io->write16(io->context, offset, value);
}

static uint32_t read32(const struct rtl8192se_register_io *io, uint16_t offset)
{
    return io->read32(io->context, offset);
}

static void write32(const struct rtl8192se_register_io *io, uint16_t offset,
                    uint32_t value)
{
    io->write32(io->context, offset, value);
}

static uint32_t read_le32(const volatile uint8_t *bytes)
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

static int valid_mac_address(const uint8_t *address)
{
    uint8_t any_nonzero = 0u;
    uint8_t all_ones = 0xffu;
    for (uint32_t index = 0u; index < 6u; ++index) {
        any_nonzero |= address[index];
        all_ones &= address[index];
    }
    return any_nonzero != 0u && all_ones != 0xffu &&
           (address[0] & 1u) == 0u;
}

int rtl8192se_parse_efuse(const uint8_t *physical_map, uint32_t map_size,
                          struct rtl8192se_adapter_config *config)
{
    if (physical_map == 0 || map_size != EFUSE_PHYSICAL_SIZE ||
        config == 0) {
        return 0;
    }

    uint8_t logical_map[EFUSE_LOGICAL_SIZE];
    for (uint32_t index = 0u; index < EFUSE_LOGICAL_SIZE; ++index) {
        logical_map[index] = 0xffu;
    }

    uint32_t address = 0u;
    while (address < map_size && physical_map[address] != 0xffu) {
        uint8_t header = physical_map[address++];
        uint32_t section;
        uint8_t word_enable;
        if ((header & 0x1fu) == 0x0fu) {
            if (address >= map_size) {
                return 0;
            }
            uint8_t extended = physical_map[address++];
            if (extended == 0xffu) {
                break;
            }
            if ((extended & 0x0fu) == 0x0fu) {
                return 0;
            }
            section = ((uint32_t)(header & 0xe0u) >> 5) |
                      ((extended & 0xf0u) >> 1);
            word_enable = extended & 0x0fu;
        } else {
            section = (header >> 4) & 0x0fu;
            word_enable = header & 0x0fu;
        }

        for (uint32_t word = 0u; word < EFUSE_WORD_COUNT; ++word) {
            if ((word_enable & (1u << word)) != 0u) {
                continue;
            }
            if (address > map_size - 2u) {
                return 0;
            }
            if (section < EFUSE_SECTION_COUNT) {
                uint32_t destination = section * 8u + word * 2u;
                logical_map[destination] = physical_map[address];
                logical_map[destination + 1u] = physical_map[address + 1u];
            }
            address += 2u;
        }
    }

    uint16_t signature =
        (uint16_t)logical_map[EEPROM_SIGNATURE_OFFSET] |
        ((uint16_t)logical_map[EEPROM_SIGNATURE_OFFSET + 1u] << 8);
    if (signature != EEPROM_SIGNATURE) {
        return 0;
    }

    uint8_t board_type = logical_map[EEPROM_BOARD_TYPE];
    uint8_t rf_configuration;
    if (board_type == 0u) {
        rf_configuration = 0x22u;
    } else if (board_type == 1u) {
        rf_configuration = 0x12u;
    } else if (board_type == 2u) {
        rf_configuration = 0x12u;
    } else if (board_type == 3u) {
        rf_configuration = 0x11u;
    } else {
        return 0;
    }
    if (!valid_mac_address(&logical_map[EEPROM_MAC_ADDRESS])) {
        return 0;
    }

    for (uint32_t index = 0u; index < 6u; ++index) {
        config->mac_address[index] = logical_map[EEPROM_MAC_ADDRESS + index];
    }
    config->rf_configuration = rf_configuration;
    config->channel_plan = logical_map[EEPROM_CHANNEL_PLAN];
    return 1;
}

static int read_efuse_byte(const struct rtl8192se_register_io *io,
                           uint16_t address, uint8_t *value)
{
    if (address >= EFUSE_PHYSICAL_SIZE || value == 0) {
        return 0;
    }
    write8(io, EFUSE_CONTROL + 1u, (uint8_t)address);
    uint8_t high = read8(io, EFUSE_CONTROL + 2u);
    write8(io, EFUSE_CONTROL + 2u,
           (uint8_t)((high & 0xfcu) | ((address >> 8) & 0x03u)));
    write8(io, EFUSE_CONTROL + 3u,
           (uint8_t)(read8(io, EFUSE_CONTROL + 3u) & 0x7fu));

    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        if ((read8(io, EFUSE_CONTROL + 3u) & 0x80u) != 0u) {
            *value = read8(io, EFUSE_CONTROL);
            return 1;
        }
        io->delay_us(io->context, 1u);
    }
    return 0;
}

int rtl8192se_read_adapter_config(
    const struct rtl8192se_register_io *io,
    struct rtl8192se_adapter_config *config)
{
    if (io == 0 || config == 0 || io->read8 == 0 || io->write8 == 0 ||
        io->delay_us == 0) {
        return 0;
    }

    uint8_t value = read8(io, SYS_FUNC_EN + 1u);
    write8(io, SYS_FUNC_EN + 1u, (uint8_t)(value | 0x20u));
    value = read8(io, SYS_ISO_CTRL + 1u);
    write8(io, SYS_ISO_CTRL + 1u, (uint8_t)(value & 0xfeu));
    value = read8(io, EFUSE_TEST + 3u);
    write8(io, EFUSE_TEST + 3u, (uint8_t)(value | 0x80u));
    write8(io, 0x02f8u, 0x03u);
    write8(io, EFUSE_CONTROL + 3u, 0x72u);

    uint8_t physical_map[EFUSE_PHYSICAL_SIZE];
    for (uint16_t address = 0u; address < EFUSE_PHYSICAL_SIZE; ++address) {
        if (!read_efuse_byte(io, address, &physical_map[address])) {
            return 0;
        }
    }
    return rtl8192se_parse_efuse(physical_map, sizeof(physical_map), config);
}

int rtl8192se_build_rx_ring(uint8_t *ring, uint32_t ring_address,
                            const uint32_t *buffer_addresses,
                            uint32_t descriptor_count,
                            uint32_t buffer_capacity)
{
    if (ring == 0 || buffer_addresses == 0 ||
        descriptor_count < 2u ||
        descriptor_count > RX_DESCRIPTOR_COUNT_MAX ||
        (ring_address & 0xffu) != 0u ||
        ring_address >= RX_DMA_ADDRESS_LIMIT ||
        descriptor_count * RX_DESCRIPTOR_SIZE > 4096u ||
        ring_address > RX_DMA_ADDRESS_LIMIT -
                           descriptor_count * RX_DESCRIPTOR_SIZE ||
        buffer_capacity == 0u || buffer_capacity > RX_BUFFER_MAX_SIZE) {
        return 0;
    }

    for (uint32_t index = 0u; index < descriptor_count; ++index) {
        uint32_t buffer_address = buffer_addresses[index];
        if ((buffer_address & 0xfffu) != 0u ||
            buffer_address >= RX_DMA_ADDRESS_LIMIT ||
            buffer_address > RX_DMA_ADDRESS_LIMIT - buffer_capacity) {
            return 0;
        }
    }

    for (uint32_t index = 0u; index < descriptor_count; ++index) {
        uint8_t *descriptor = ring + index * RX_DESCRIPTOR_SIZE;
        for (uint32_t byte = 0u; byte < RX_DESCRIPTOR_SIZE; ++byte) {
            descriptor[byte] = 0u;
        }
        write_le32(descriptor + RX_DESCRIPTOR_BUFFER_ADDRESS_OFFSET,
                   buffer_addresses[index]);
        uint32_t control = buffer_capacity;
        if (index + 1u == descriptor_count) {
            control |= RX_DESCRIPTOR_END_OF_RING;
        }
        __asm__ volatile ("" : : : "memory");
        write_le32(descriptor, control | RX_DESCRIPTOR_OWN);
    }
    return 1;
}

int rtl8192se_rearm_rx_descriptor(uint8_t *descriptor,
                                  uint32_t buffer_address,
                                  uint32_t buffer_capacity,
                                  int end_of_ring)
{
    if (descriptor == 0 || buffer_capacity == 0u ||
        buffer_capacity > RX_BUFFER_MAX_SIZE ||
        (buffer_address & 0xfffu) != 0u ||
        buffer_address >= RX_DMA_ADDRESS_LIMIT ||
        buffer_address > RX_DMA_ADDRESS_LIMIT - buffer_capacity) {
        return 0;
    }

    for (uint32_t byte = 0u; byte < RX_DESCRIPTOR_SIZE; ++byte) {
        descriptor[byte] = 0u;
    }
    write_le32(descriptor + RX_DESCRIPTOR_BUFFER_ADDRESS_OFFSET,
               buffer_address);
    uint32_t control = buffer_capacity;
    if (end_of_ring != 0) {
        control |= RX_DESCRIPTOR_END_OF_RING;
    }
    __asm__ volatile ("" : : : "memory");
    write_le32(descriptor, control | RX_DESCRIPTOR_OWN);
    return 1;
}

int rtl8192se_program_rx_rings(const struct rtl8192se_register_io *io,
                               uint32_t mpdu_ring_address,
                               uint32_t command_ring_address)
{
    if (io == 0 || io->write32 == 0 || io->read32 == 0 ||
        (mpdu_ring_address & 0xffu) != 0u ||
        (command_ring_address & 0xffu) != 0u ||
        mpdu_ring_address == 0u || command_ring_address == 0u ||
        mpdu_ring_address >= RX_DMA_ADDRESS_LIMIT ||
        command_ring_address >= RX_DMA_ADDRESS_LIMIT) {
        return 0;
    }

    write32(io, RX_MPDU_RING_ADDRESS, mpdu_ring_address);
    write32(io, RX_COMMAND_RING_ADDRESS, command_ring_address);
    return read32(io, RX_MPDU_RING_ADDRESS) == mpdu_ring_address &&
           read32(io, RX_COMMAND_RING_ADDRESS) == command_ring_address;
}

int rtl8192se_build_tx_descriptor(uint8_t *descriptor,
                                  uint32_t buffer_address,
                                  uint32_t frame_length,
                                  uint32_t next_descriptor_address,
                                  uint8_t queue_selection,
                                  uint8_t non_qos)
{
    if (descriptor == 0 || buffer_address == 0u ||
        buffer_address >= RX_DMA_ADDRESS_LIMIT ||
        frame_length < 10u || frame_length > 4096u ||
        next_descriptor_address == 0u ||
        (next_descriptor_address & 0x3fu) != 0u ||
        next_descriptor_address >= RX_DMA_ADDRESS_LIMIT ||
        queue_selection > 0x1fu || non_qos > 1u) {
        return 0;
    }

    for (uint32_t index = 0u; index < 64u; ++index) {
        descriptor[index] = 0u;
    }
    write_le32(descriptor + 4u,
               ((uint32_t)queue_selection << 8) |
                   ((uint32_t)non_qos << 16));
    write_le32(descriptor + 16u, 1u << 31);
    write_le32(descriptor + 28u, frame_length);
    write_le32(descriptor + 32u, buffer_address);
    write_le32(descriptor + 36u, next_descriptor_address);
    __asm__ volatile ("" : : : "memory");
    uint32_t flags = frame_length | (32u << 16) | (1u << 26) |
                     (1u << 27) | (1u << 31);
    write_le32(descriptor, flags);
    return 1;
}

int rtl8192se_program_tx_rings(const struct rtl8192se_register_io *io,
                               uint32_t data_ring_address,
                               uint32_t management_ring_address)
{
    if (io == 0 || io->write32 == 0 || io->read32 == 0 ||
        (data_ring_address & 0xffu) != 0u ||
        (management_ring_address & 0xffu) != 0u ||
        data_ring_address == 0u || management_ring_address == 0u ||
        data_ring_address >= RX_DMA_ADDRESS_LIMIT ||
        management_ring_address >= RX_DMA_ADDRESS_LIMIT) {
        return 0;
    }

    write32(io, TX_DATA_RING_ADDRESS, data_ring_address);
    write32(io, TX_MANAGEMENT_RING_ADDRESS, management_ring_address);
    return read32(io, TX_DATA_RING_ADDRESS) == data_ring_address &&
           read32(io, TX_MANAGEMENT_RING_ADDRESS) ==
               management_ring_address;
}

int rtl8192se_rx_copy_frame(const uint8_t *descriptor,
                            const uint8_t *buffer,
                            uint32_t buffer_capacity, uint8_t *frame,
                            uint32_t frame_capacity, uint32_t *frame_length)
{
    if (descriptor == 0 || buffer == 0 || frame == 0 ||
        frame_length == 0) {
        return 0;
    }
    uint32_t control = (uint32_t)descriptor[0] |
                      ((uint32_t)descriptor[1] << 8) |
                      ((uint32_t)descriptor[2] << 16) |
                      ((uint32_t)descriptor[3] << 24);
    uint32_t length = control & RX_DESCRIPTOR_LENGTH_MASK;
    uint32_t info_length =
        ((control & RX_DESCRIPTOR_INFO_LENGTH_MASK) >> 16) * 8u;
    uint32_t buffer_shift =
        (control & RX_DESCRIPTOR_BUFFER_SHIFT_MASK) >> 24;
    uint32_t frame_offset = info_length + buffer_shift;
    if ((control & RX_DESCRIPTOR_OWN) != 0u ||
        (control & (RX_DESCRIPTOR_CRC_ERROR | RX_DESCRIPTOR_ICV_ERROR)) !=
            0u ||
        length == 0u || length > buffer_capacity ||
        frame_offset >= length ||
        length - frame_offset > frame_capacity) {
        return 0;
    }

    uint32_t payload_length = length - frame_offset;
    for (uint32_t index = 0u; index < payload_length; ++index) {
        frame[index] = buffer[frame_offset + index];
    }
    *frame_length = payload_length;
    return 1;
}

static int switch_to_mac_clock(const struct rtl8192se_register_io *io,
                               uint8_t value)
{
    write8(io, SYS_CLKR + 1u, value);
    io->delay_us(io->context, 400u);
    if ((read8(io, SYS_CLKR + 1u) & 0x80u) != (value & 0x80u)) {
        return 0;
    }

    if ((value & 0xc0u) == 0u) {
        for (uint32_t attempt = 0u; attempt < 100u; ++attempt) {
            if ((read8(io, SYS_CLKR + 1u) & 0x40u) != 0u) {
                return 1;
            }
            io->delay_us(io->context, 10u);
        }
        return 0;
    }
    return 1;
}

int rtl8192se_mac_power_sequence(const struct rtl8192se_register_io *io,
                                 uint32_t command_ring_address)
{
    if (io == 0 || io->read8 == 0 || io->read16 == 0 || io->read32 == 0 ||
        io->write8 == 0 || io->write16 == 0 || io->write32 == 0 ||
        io->delay_us == 0 || command_ring_address == 0u ||
        (command_ring_address & 0xffu) != 0u ||
        command_ring_address >= DMA_ADDRESS_LIMIT ||
        command_ring_address > DMA_ADDRESS_LIMIT -
            COMMAND_QUEUE_COUNT * COMMAND_DESCRIPTOR_SIZE) {
        return 0;
    }

    uint8_t value8 = read8(io, SYS_FUNC_EN + 1u);
    write8(io, SYS_FUNC_EN + 1u, (uint8_t)(value8 & 0xfeu));
    io->delay_us(io->context, 1u);
    write8(io, SYS_FUNC_EN + 1u, (uint8_t)(value8 | 0x01u));

    value8 = read8(io, SYS_CLKR + 1u);
    if ((value8 & 0x80u) != 0u &&
        !switch_to_mac_clock(io, (uint8_t)(value8 & 0x3fu))) {
        return 0;
    }

    write8(io, AFE_PLL_CTRL, 0u);
    io->delay_us(io->context, 50u);
    write8(io, LDOA15_CTRL, 0x34u);
    io->delay_us(io->context, 50u);
    write8(io, RF_POWER_CONTROL, 0u);

    value8 = (uint8_t)(read8(io, SYS_FUNC_EN + 1u) & 0x73u);
    write8(io, SYS_FUNC_EN + 1u, value8);
    io->delay_us(io->context, 1000u);
    write16(io, COMMAND, 0u);
    write32(io, TCR, 0u);

    value8 = (uint8_t)(read8(io, 0x0562u) | 0x08u);
    write8(io, 0x0562u, value8);
    write8(io, 0x0562u, (uint8_t)(value8 & 0xf7u));

    write8(io, AFE_XTAL_CTRL,
           (uint8_t)(read8(io, AFE_XTAL_CTRL) | 0x01u));
    io->delay_us(io->context, 2000u);
    write8(io, AFE_XTAL_CTRL + 1u,
           (uint8_t)(read8(io, AFE_XTAL_CTRL + 1u) & 0xfbu));

    write8(io, AFE_MISC, (uint8_t)(read8(io, AFE_MISC) | 0x01u));
    io->delay_us(io->context, 1000u);
    write8(io, AFE_MISC, (uint8_t)(read8(io, AFE_MISC) | 0x02u));
    io->delay_us(io->context, 1000u);
    write8(io, LDOA15_CTRL,
           (uint8_t)(read8(io, LDOA15_CTRL) | 0x01u));

    write16(io, SYS_ISO_CTRL,
            (uint16_t)(read16(io, SYS_ISO_CTRL) | (1u << 11)));
    write16(io, SYS_FUNC_EN,
            (uint16_t)(read16(io, SYS_FUNC_EN) | (1u << 13)));
    write8(io, SYS_ISO_CTRL + 1u, 0x68u);

    io->delay_us(io->context, 200u);
    value8 = read8(io, AFE_PLL_CTRL);
    write8(io, AFE_PLL_CTRL, (uint8_t)(value8 | 0x11u));
    io->delay_us(io->context, 100u);
    write8(io, AFE_PLL_CTRL, (uint8_t)(value8 | 0x51u));
    io->delay_us(io->context, 10u);
    write8(io, AFE_PLL_CTRL, (uint8_t)(value8 | 0x11u));
    io->delay_us(io->context, 10u);
    write8(io, AFE_PLL_CTRL + 1u,
           (uint8_t)(read8(io, AFE_PLL_CTRL + 1u) | 0x01u));
    io->delay_us(io->context, 1000u);

    write8(io, SYS_ISO_CTRL, 0xa6u);
    write16(io, SYS_CLKR,
            (uint16_t)(read16(io, SYS_CLKR) | (1u << 12) | (1u << 11)));
    write16(io, SYS_FUNC_EN,
            (uint16_t)(read16(io, SYS_FUNC_EN) | (1u << 11)));
    write8(io, SYS_FUNC_EN + 1u,
           (uint8_t)(read8(io, SYS_FUNC_EN + 1u) & 0x7fu));
    write16(io, SYS_FUNC_EN,
            (uint16_t)(read16(io, SYS_FUNC_EN) | (1u << 11) | (1u << 15)));
    write16(io, SYS_CLKR, (uint16_t)(read16(io, SYS_CLKR) & ~(1u << 2)));

    value8 = (uint8_t)((read8(io, SYS_CLKR + 1u) | 0x80u) & 0xbfu);
    if (!switch_to_mac_clock(io, value8)) {
        return 0;
    }

    write16(io, COMMAND, 0u);
    write32(io, RQPN, 0xffffffffu);
    write32(io, RQPN + 4u, 0xffffffffu);
    write8(io, RQPN + 8u, 0xffu);
    write8(io, QUEUE_PAGE_NUMBER + 11u, 0x80u);
    write8(io, PCI_FUNCTION_CONTROL, 0x22u);
    write8(io, DEBUG_PORT, 0x91u);

    write32(io, TX_COMMAND_DESCRIPTOR, command_ring_address);
    write16(io, COMMAND, COMMAND_DMA_ENABLE);
    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        if ((read32(io, TCR) & TCR_DMA_INIT_VALUE) ==
            TCR_DMA_INIT_VALUE) {
            return 1;
        }
        io->delay_us(io->context, 5u);
    }
    return 0;
}

static int wait_for_tcr(const struct rtl8192se_register_io *io,
                        uint32_t mask, uint32_t expected,
                        uint32_t attempts, uint32_t delay_us)
{
    for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        if ((read32(io, TCR) & mask) == expected) {
            return 1;
        }
        io->delay_us(io->context, delay_us);
    }
    return 0;
}

static int download_segment(const struct rtl8192se_register_io *io,
                            const uint8_t *source, uint32_t size,
                            uint8_t *ring, uint32_t ring_address,
                            uint32_t ring_count, uint8_t *buffer,
                            uint32_t buffer_address, uint32_t capacity,
                            uint32_t *descriptor_index, int *bus_master_enabled)
{
    uint32_t offset = 0u;
    while (offset < size) {
        uint32_t length = size - offset;
        if (length > capacity) {
            length = capacity;
        }
        uint8_t *descriptor =
            ring + *descriptor_index * COMMAND_DESCRIPTOR_SIZE;
        for (uint32_t index = 0u; index < length; ++index) {
            buffer[index] = source[offset + index];
        }

        uint32_t next_index = (*descriptor_index + 1u) % ring_count;
        uint32_t next_address =
            ring_address + next_index * COMMAND_DESCRIPTOR_SIZE;
        if (!rtl8192se_firmware_build_download_descriptor(
                descriptor, buffer_address, length, next_address,
                (uint8_t)(offset + length == size))) {
            return 0;
        }
        __asm__ volatile ("" : : : "memory");
        if (!*bus_master_enabled) {
            if (io->enable_bus_mastering == 0 ||
                !io->enable_bus_mastering(io->bus_context)) {
                return 0;
            }
            *bus_master_enabled = 1;
        }

        write16(io, TX_POLL, COMMAND_QUEUE_POLL);
        uint32_t attempts = 0u;
        while ((read_le32(descriptor) & 0x80000000u) != 0u &&
               attempts < 1000u) {
            io->delay_us(io->context, 5u);
            ++attempts;
        }
        if ((read_le32(descriptor) & 0x80000000u) != 0u) {
            return 0;
        }

        offset += length;
        *descriptor_index = next_index;
    }
    return 1;
}

int rtl8192se_upload_firmware(const struct rtl8192se_register_io *io,
                              const struct rtl8192se_firmware *firmware,
                              uint8_t *ring, uint32_t ring_address,
                              uint32_t ring_count, uint8_t *buffer,
                              uint32_t buffer_address,
                              uint32_t buffer_capacity,
                              uint8_t rf_configuration)
{
    if (io == 0 || io->read16 == 0 || io->read32 == 0 ||
        io->write16 == 0 || io->delay_us == 0 ||
        io->enable_bus_mastering == 0 || firmware == 0 ||
        firmware->bytes == 0 ||
        ring == 0 || buffer == 0 || ring_count != COMMAND_QUEUE_COUNT ||
        (ring_address & 0xffu) != 0u ||
        ring_address > DMA_ADDRESS_LIMIT -
            ring_count * COMMAND_DESCRIPTOR_SIZE ||
        buffer_address == 0u || buffer_address >= DMA_ADDRESS_LIMIT ||
        buffer_capacity == 0u || buffer_capacity > DMA_CHUNK_SIZE ||
        buffer_address > DMA_ADDRESS_LIMIT - buffer_capacity ||
        firmware->imem_size == 0u || firmware->emem_size == 0u ||
        firmware->imem_offset > firmware->size ||
        firmware->imem_size > firmware->size - firmware->imem_offset ||
        firmware->emem_offset > firmware->size ||
        firmware->emem_size > firmware->size - firmware->emem_offset ||
        firmware->dmem_size != sizeof(uint8_t) * 48u ||
        firmware->dmem_offset > firmware->size ||
        firmware->dmem_size > firmware->size - firmware->dmem_offset) {
        return 0;
    }

    if (rf_configuration != 0x11u && rf_configuration != 0x12u &&
        rf_configuration != 0x22u) {
        return 0;
    }
    uint8_t private_data[48];
    if (!rtl8192se_firmware_prepare_dmem(firmware, rf_configuration,
                                         private_data,
                                         sizeof(private_data))) {
        return 0;
    }

    uint32_t descriptor_index = 0u;
    int bus_master_enabled = 0;
    if (!download_segment(io, &firmware->bytes[firmware->imem_offset],
                          firmware->imem_size, ring, ring_address,
                          ring_count, buffer, buffer_address, buffer_capacity,
                          &descriptor_index, &bus_master_enabled) ||
        !wait_for_tcr(io, IMEM_CODE_DONE | IMEM_CHECK_REPORT,
                      IMEM_CODE_DONE | IMEM_CHECK_REPORT, 1000u, 5u)) {
        return 0;
    }

    if (!download_segment(io, &firmware->bytes[firmware->emem_offset],
                          firmware->emem_size, ring, ring_address,
                          ring_count, buffer, buffer_address, buffer_capacity,
                          &descriptor_index, &bus_master_enabled) ||
        !wait_for_tcr(io, EMEM_CODE_DONE | EMEM_CHECK_REPORT,
                      EMEM_CODE_DONE | EMEM_CHECK_REPORT, 1000u, 5u)) {
        return 0;
    }

    write16(io, SYS_CLKR,
            (uint16_t)(read16(io, SYS_CLKR) | SYS_CPU_CLKSEL));
    write16(io, SYS_FUNC_EN,
            (uint16_t)(read16(io, SYS_FUNC_EN) | FEN_CPUEN));
    if (!wait_for_tcr(io, IMEM_RDY, IMEM_RDY, 1000u, 100u)) {
        return 0;
    }

    if (!download_segment(io, private_data, sizeof(private_data), ring,
                          ring_address, ring_count, buffer, buffer_address,
                          buffer_capacity, &descriptor_index,
                          &bus_master_enabled) ||
        !wait_for_tcr(io, DMEM_CODE_DONE, DMEM_CODE_DONE, 1000u, 5u) ||
        !wait_for_tcr(io, FIRMWARE_READY, FIRMWARE_READY, 2000u, 40u)) {
        return 0;
    }
    return 1;
}
