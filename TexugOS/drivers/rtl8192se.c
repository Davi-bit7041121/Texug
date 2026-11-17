#include <stdint.h>

#include "frame_allocator.h"
#include "paging.h"
#include "pci.h"
#include "rtl8192se.h"
#include "rtl8192se_firmware.h"
#include "rtl8192se_hw.h"

#define RTL8192SE_VENDOR_ID 0x10ecu
#define RTL8192SE_REGISTER_BAR 1u
#define RTL8192SE_REGISTER_WINDOW_SIZE 0x4000u
#define RTL8192SE_DMA_LIMIT 0x01000000ull
#define RTL8192SE_PAGE_SIZE 4096u
#define RTL8192SE_COMMAND_RING_COUNT 16u
#define RTL8192SE_DESCRIPTOR_SIZE 64u
#define RTL8192SE_DESCRIPTOR_NEXT_OFFSET 36u
#define RTL8192SE_RX_RING_COUNT 16u
#define RTL8192SE_RX_DESCRIPTOR_SIZE 32u
#define RTL8192SE_RX_BUFFER_SIZE RTL8192SE_PAGE_SIZE
#define RTL8192SE_TX_RING_COUNT 16u
#define RTL8192SE_TX_BUFFER_SIZE RTL8192SE_PAGE_SIZE
#define RTL8192SE_TX_POLL_REGISTER 0x0500u
#define RTL8192SE_TX_DATA_POLL (1u << 1)
#define RTL8192SE_TX_MANAGEMENT_POLL (1u << 6)
#define RTL8192SE_TX_DATA_QUEUE_SELECTION 0u
#define RTL8192SE_TX_MANAGEMENT_QUEUE_SELECTION 0x12u

static struct rtl8192se_status adapter_status;
static uint32_t command_ring_frame = FRAME_INVALID;
static uint32_t command_buffer_frame = FRAME_INVALID;
static uint32_t rx_mpdu_ring_frame = FRAME_INVALID;
static uint32_t rx_command_ring_frame = FRAME_INVALID;
static uint32_t tx_data_ring_frame = FRAME_INVALID;
static uint32_t tx_management_ring_frame = FRAME_INVALID;
static uint32_t rx_mpdu_buffer_frames[RTL8192SE_RX_RING_COUNT];
static uint32_t rx_command_buffer_frames[RTL8192SE_RX_RING_COUNT];
static uint32_t tx_data_buffer_frames[RTL8192SE_TX_RING_COUNT];
static uint32_t tx_management_buffer_frames[RTL8192SE_TX_RING_COUNT];
static uint32_t tx_data_index;
static uint32_t tx_management_index;
static uint32_t rx_mpdu_index;
static uint8_t command_dma_ready;
static uint8_t rx_dma_ready;
static uint8_t tx_dma_ready;
static uint8_t command_bus_master_enabled;

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

int rtl8192se_build_command_ring(uint8_t *ring, uint32_t physical_address,
                                uint32_t descriptor_count)
{
    if (ring == 0 || descriptor_count < 2u ||
        descriptor_count > RTL8192SE_COMMAND_RING_COUNT ||
        (physical_address & 0xffu) != 0u ||
        physical_address >= RTL8192SE_DMA_LIMIT ||
        descriptor_count * RTL8192SE_DESCRIPTOR_SIZE >
            RTL8192SE_PAGE_SIZE ||
        physical_address >
            RTL8192SE_DMA_LIMIT -
                descriptor_count * RTL8192SE_DESCRIPTOR_SIZE) {
        return 0;
    }

    for (uint32_t index = 0u; index < descriptor_count; ++index) {
        uint8_t *descriptor = ring + index * RTL8192SE_DESCRIPTOR_SIZE;
        for (uint32_t byte = 0u; byte < RTL8192SE_DESCRIPTOR_SIZE; ++byte) {
            descriptor[byte] = 0u;
        }
        uint32_t next = physical_address +
            ((index + 1u) % descriptor_count) * RTL8192SE_DESCRIPTOR_SIZE;
        write_le32(&descriptor[RTL8192SE_DESCRIPTOR_NEXT_OFFSET], next);
    }
    return 1;
}

static void release_command_dma(void)
{
    if (command_buffer_frame != FRAME_INVALID) {
        frame_free(command_buffer_frame);
        command_buffer_frame = FRAME_INVALID;
    }
    if (command_ring_frame != FRAME_INVALID) {
        frame_free(command_ring_frame);
        command_ring_frame = FRAME_INVALID;
    }
    command_dma_ready = 0u;
}

int rtl8192se_prepare_command_dma(struct rtl8192se_status *status)
{
    if (status == 0 || status->probed == 0u) {
        return 0;
    }
    if (command_dma_ready != 0u) {
        status->command_ring = command_ring_frame;
        status->dma_prepared = 1u;
        return 1;
    }

    command_ring_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    if (command_ring_frame == FRAME_INVALID) {
        return 0;
    }

    uint8_t *ring = (uint8_t *)(uintptr_t)command_ring_frame;
    if (!rtl8192se_build_command_ring(ring, command_ring_frame,
                                      RTL8192SE_COMMAND_RING_COUNT)) {
        release_command_dma();
        return 0;
    }
    command_buffer_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    if (command_buffer_frame == FRAME_INVALID) {
        release_command_dma();
        return 0;
    }
    uint8_t *buffer = (uint8_t *)(uintptr_t)command_buffer_frame;
    for (uint32_t byte = 0u; byte < RTL8192SE_PAGE_SIZE; ++byte) {
        buffer[byte] = 0u;
    }

    __asm__ volatile ("" : : : "memory");
    command_dma_ready = 1u;
    status->command_ring = command_ring_frame;
    status->dma_prepared = 1u;
    return 1;
}

static void release_rx_dma(void)
{
    for (uint32_t index = 0u; index < RTL8192SE_RX_RING_COUNT; ++index) {
        if (rx_mpdu_buffer_frames[index] != FRAME_INVALID) {
            frame_free(rx_mpdu_buffer_frames[index]);
            rx_mpdu_buffer_frames[index] = FRAME_INVALID;
        }
        if (rx_command_buffer_frames[index] != FRAME_INVALID) {
            frame_free(rx_command_buffer_frames[index]);
            rx_command_buffer_frames[index] = FRAME_INVALID;
        }
    }
    if (rx_mpdu_ring_frame != FRAME_INVALID) {
        frame_free(rx_mpdu_ring_frame);
        rx_mpdu_ring_frame = FRAME_INVALID;
    }
    if (rx_command_ring_frame != FRAME_INVALID) {
        frame_free(rx_command_ring_frame);
        rx_command_ring_frame = FRAME_INVALID;
    }
    rx_dma_ready = 0u;
}

static int rtl8192se_prepare_rx_dma(struct rtl8192se_status *status)
{
    if (status == 0 || status->probed == 0u) {
        return 0;
    }
    if (rx_dma_ready != 0u) {
        status->rx_mpdu_ring = rx_mpdu_ring_frame;
        status->rx_command_ring = rx_command_ring_frame;
        status->rx_dma_prepared = 1u;
        return 1;
    }

    for (uint32_t index = 0u; index < RTL8192SE_RX_RING_COUNT; ++index) {
        rx_mpdu_buffer_frames[index] = FRAME_INVALID;
        rx_command_buffer_frames[index] = FRAME_INVALID;
    }
    rx_mpdu_ring_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    rx_command_ring_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    if (rx_mpdu_ring_frame == FRAME_INVALID ||
        rx_command_ring_frame == FRAME_INVALID) {
        release_rx_dma();
        return 0;
    }

    uint32_t mpdu_buffers[RTL8192SE_RX_RING_COUNT];
    uint32_t command_buffers[RTL8192SE_RX_RING_COUNT];
    for (uint32_t index = 0u; index < RTL8192SE_RX_RING_COUNT; ++index) {
        rx_mpdu_buffer_frames[index] =
            frame_allocate_below(RTL8192SE_DMA_LIMIT);
        rx_command_buffer_frames[index] =
            frame_allocate_below(RTL8192SE_DMA_LIMIT);
        if (rx_mpdu_buffer_frames[index] == FRAME_INVALID ||
            rx_command_buffer_frames[index] == FRAME_INVALID) {
            release_rx_dma();
            return 0;
        }
        mpdu_buffers[index] = rx_mpdu_buffer_frames[index];
        command_buffers[index] = rx_command_buffer_frames[index];
        uint8_t *mpdu_buffer =
            (uint8_t *)(uintptr_t)rx_mpdu_buffer_frames[index];
        uint8_t *command_buffer =
            (uint8_t *)(uintptr_t)rx_command_buffer_frames[index];
        for (uint32_t byte = 0u; byte < RTL8192SE_RX_BUFFER_SIZE; ++byte) {
            mpdu_buffer[byte] = 0u;
            command_buffer[byte] = 0u;
        }
    }

    uint8_t *mpdu_ring = (uint8_t *)(uintptr_t)rx_mpdu_ring_frame;
    uint8_t *command_ring = (uint8_t *)(uintptr_t)rx_command_ring_frame;
    if (!rtl8192se_build_rx_ring(mpdu_ring, rx_mpdu_ring_frame,
                                 mpdu_buffers, RTL8192SE_RX_RING_COUNT,
                                 RTL8192SE_RX_BUFFER_SIZE) ||
        !rtl8192se_build_rx_ring(command_ring, rx_command_ring_frame,
                                 command_buffers, RTL8192SE_RX_RING_COUNT,
                                 RTL8192SE_RX_BUFFER_SIZE)) {
        release_rx_dma();
        return 0;
    }

    __asm__ volatile ("" : : : "memory");
    rx_dma_ready = 1u;
    rx_mpdu_index = 0u;
    status->rx_mpdu_ring = rx_mpdu_ring_frame;
    status->rx_command_ring = rx_command_ring_frame;
    status->rx_dma_prepared = 1u;
    return 1;
}

static void release_tx_dma(void)
{
    for (uint32_t index = 0u; index < RTL8192SE_TX_RING_COUNT; ++index) {
        if (tx_data_buffer_frames[index] != FRAME_INVALID) {
            frame_free(tx_data_buffer_frames[index]);
            tx_data_buffer_frames[index] = FRAME_INVALID;
        }
        if (tx_management_buffer_frames[index] != FRAME_INVALID) {
            frame_free(tx_management_buffer_frames[index]);
            tx_management_buffer_frames[index] = FRAME_INVALID;
        }
    }
    if (tx_data_ring_frame != FRAME_INVALID) {
        frame_free(tx_data_ring_frame);
        tx_data_ring_frame = FRAME_INVALID;
    }
    if (tx_management_ring_frame != FRAME_INVALID) {
        frame_free(tx_management_ring_frame);
        tx_management_ring_frame = FRAME_INVALID;
    }
    tx_dma_ready = 0u;
}

static int rtl8192se_prepare_tx_dma(struct rtl8192se_status *status)
{
    if (status == 0 || status->probed == 0u) {
        return 0;
    }
    if (tx_dma_ready != 0u) {
        status->tx_data_ring = tx_data_ring_frame;
        status->tx_management_ring = tx_management_ring_frame;
        status->tx_dma_prepared = 1u;
        return 1;
    }

    for (uint32_t index = 0u; index < RTL8192SE_TX_RING_COUNT; ++index) {
        tx_data_buffer_frames[index] = FRAME_INVALID;
        tx_management_buffer_frames[index] = FRAME_INVALID;
    }
    tx_data_ring_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    tx_management_ring_frame = frame_allocate_below(RTL8192SE_DMA_LIMIT);
    if (tx_data_ring_frame == FRAME_INVALID ||
        tx_management_ring_frame == FRAME_INVALID) {
        release_tx_dma();
        return 0;
    }

    uint8_t *data_ring = (uint8_t *)(uintptr_t)tx_data_ring_frame;
    uint8_t *management_ring =
        (uint8_t *)(uintptr_t)tx_management_ring_frame;
    if (!rtl8192se_build_command_ring(data_ring, tx_data_ring_frame,
                                     RTL8192SE_TX_RING_COUNT) ||
        !rtl8192se_build_command_ring(management_ring,
                                     tx_management_ring_frame,
                                     RTL8192SE_TX_RING_COUNT)) {
        release_tx_dma();
        return 0;
    }

    for (uint32_t index = 0u; index < RTL8192SE_TX_RING_COUNT; ++index) {
        tx_data_buffer_frames[index] =
            frame_allocate_below(RTL8192SE_DMA_LIMIT);
        tx_management_buffer_frames[index] =
            frame_allocate_below(RTL8192SE_DMA_LIMIT);
        if (tx_data_buffer_frames[index] == FRAME_INVALID ||
            tx_management_buffer_frames[index] == FRAME_INVALID) {
            release_tx_dma();
            return 0;
        }
        uint8_t *data_buffer =
            (uint8_t *)(uintptr_t)tx_data_buffer_frames[index];
        uint8_t *management_buffer =
            (uint8_t *)(uintptr_t)tx_management_buffer_frames[index];
        for (uint32_t byte = 0u; byte < RTL8192SE_TX_BUFFER_SIZE; ++byte) {
            data_buffer[byte] = 0u;
            management_buffer[byte] = 0u;
        }
    }

    __asm__ volatile ("" : : : "memory");
    tx_data_index = 0u;
    tx_management_index = 0u;
    tx_dma_ready = 1u;
    status->tx_data_ring = tx_data_ring_frame;
    status->tx_management_ring = tx_management_ring_frame;
    status->tx_dma_prepared = 1u;
    return 1;
}

static uint8_t rtl8192se_mmio_read8(void *context, uint16_t offset)
{
    volatile uint8_t *registers = (volatile uint8_t *)context;
    return registers[offset];
}

static void rtl8192se_mmio_write8(void *context, uint16_t offset,
                                  uint8_t value)
{
    volatile uint8_t *registers = (volatile uint8_t *)context;
    registers[offset] = value;
}

static uint16_t rtl8192se_mmio_read16(void *context, uint16_t offset)
{
    volatile uint16_t *registers =
        (volatile uint16_t *)((volatile uint8_t *)context + offset);
    return *registers;
}

static uint32_t rtl8192se_mmio_read32(void *context, uint16_t offset)
{
    volatile uint32_t *registers =
        (volatile uint32_t *)((volatile uint8_t *)context + offset);
    return *registers;
}

static void rtl8192se_mmio_write16(void *context, uint16_t offset,
                                   uint16_t value)
{
    volatile uint16_t *registers =
        (volatile uint16_t *)((volatile uint8_t *)context + offset);
    *registers = value;
}

static void rtl8192se_mmio_write32(void *context, uint16_t offset,
                                   uint32_t value)
{
    volatile uint32_t *registers =
        (volatile uint32_t *)((volatile uint8_t *)context + offset);
    *registers = value;
}

static void rtl8192se_delay_us(void *context, uint32_t usec)
{
    (void)context;
    for (volatile uint32_t count = 0u; count < usec * 1024u; ++count) {
        __asm__ volatile ("pause");
    }
}

static int rtl8192se_enable_bus_mastering(void *context)
{
    const struct rtl8192se_status *status = context;
    if (status == 0 || status->probed == 0u) {
        return 0;
    }
    if (command_bus_master_enabled != 0u) {
        return 1;
    }
    struct pci_device device = {
        .bus = status->bus,
        .slot = status->slot,
        .function = status->function,
        .vendor_id = RTL8192SE_VENDOR_ID,
        .device_id = status->device_id,
    };
    if (!pci_enable_bus_mastering(&device)) {
        return 0;
    }
    command_bus_master_enabled = 1u;
    return 1;
}

int rtl8192se_start_firmware(struct rtl8192se_status *status,
                             const struct rtl8192se_firmware *firmware)
{
    if (status == 0 || status->probed == 0u || firmware == 0 ||
        status->mmio_base > UINT32_MAX - RTL8192SE_REGISTER_WINDOW_SIZE ||
        !rtl8192se_prepare_command_dma(status) ||
        !rtl8192se_prepare_rx_dma(status) ||
        !rtl8192se_prepare_tx_dma(status)) {
        return 0;
    }
    struct pci_device device = {
        .bus = status->bus,
        .slot = status->slot,
        .function = status->function,
        .vendor_id = RTL8192SE_VENDOR_ID,
        .device_id = status->device_id,
    };
    if (!pci_enable_memory_space(&device)) {
        return 0;
    }
    struct rtl8192se_register_io io = {
        .context = (void *)(uintptr_t)status->mmio_base,
        .bus_context = status,
        .read8 = rtl8192se_mmio_read8,
        .read16 = rtl8192se_mmio_read16,
        .read32 = rtl8192se_mmio_read32,
        .write8 = rtl8192se_mmio_write8,
        .write16 = rtl8192se_mmio_write16,
        .write32 = rtl8192se_mmio_write32,
        .delay_us = rtl8192se_delay_us,
        .enable_bus_mastering = rtl8192se_enable_bus_mastering,
    };
    uint8_t *ring = (uint8_t *)(uintptr_t)command_ring_frame;
    uint8_t *buffer = (uint8_t *)(uintptr_t)command_buffer_frame;
    struct rtl8192se_adapter_config config;
    if (!rtl8192se_mac_power_sequence(&io, command_ring_frame)) {
        return 0;
    }
    status->mac_power_ready = 1u;
    if (!rtl8192se_read_adapter_config(&io, &config)) {
        return 0;
    }
    for (uint32_t index = 0u; index < 6u; ++index) {
        status->mac_address[index] = config.mac_address[index];
    }
    status->rf_configuration = config.rf_configuration;
    status->channel_plan = config.channel_plan;
    status->adapter_config_read = 1u;
    if (!rtl8192se_upload_firmware(
            &io, firmware, ring, command_ring_frame,
            RTL8192SE_COMMAND_RING_COUNT, buffer, command_buffer_frame,
            RTL8192SE_PAGE_SIZE, config.rf_configuration)) {
        return 0;
    }
    status->firmware_uploaded = 1u;
    if (!rtl8192se_program_rx_rings(
            &io, rx_mpdu_ring_frame, rx_command_ring_frame)) {
        return 0;
    }
    status->rx_rings_programmed = 1u;
    if (!rtl8192se_program_tx_rings(
            &io, tx_data_ring_frame, tx_management_ring_frame)) {
        return 0;
    }
    status->tx_rings_programmed = 1u;
    adapter_status = *status;
    return 1;
}

int rtl8192se_send_80211_frame(const uint8_t *frame, uint32_t frame_length,
                               int management_frame)
{
    if (frame == 0 || frame_length < 10u ||
        frame_length > RTL8192SE_TX_BUFFER_SIZE ||
        tx_dma_ready == 0u || adapter_status.tx_rings_programmed == 0u) {
        return 0;
    }

    uint32_t *buffer_frames;
    uint8_t *ring;
    uint32_t *next_index;
    uint8_t queue_selection;
    uint16_t poll_bit;
    uint32_t ring_address;
    if (management_frame != 0) {
        buffer_frames = tx_management_buffer_frames;
        ring_address = tx_management_ring_frame;
        ring = (uint8_t *)(uintptr_t)tx_management_ring_frame;
        next_index = &tx_management_index;
        queue_selection = RTL8192SE_TX_MANAGEMENT_QUEUE_SELECTION;
        poll_bit = RTL8192SE_TX_MANAGEMENT_POLL;
    } else {
        buffer_frames = tx_data_buffer_frames;
        ring_address = tx_data_ring_frame;
        ring = (uint8_t *)(uintptr_t)tx_data_ring_frame;
        next_index = &tx_data_index;
        queue_selection = RTL8192SE_TX_DATA_QUEUE_SELECTION;
        poll_bit = RTL8192SE_TX_DATA_POLL;
    }

    uint32_t index = *next_index;
    uint8_t *descriptor =
        ring + index * RTL8192SE_DESCRIPTOR_SIZE;
    if ((read_le32(descriptor) & (1u << 31)) != 0u) {
        return 0;
    }

    uint8_t *buffer = (uint8_t *)(uintptr_t)buffer_frames[index];
    for (uint32_t byte = 0u; byte < frame_length; ++byte) {
        buffer[byte] = frame[byte];
    }
    uint32_t next_descriptor_address = ring_address +
        ((index + 1u) % RTL8192SE_TX_RING_COUNT) *
            RTL8192SE_DESCRIPTOR_SIZE;
    uint8_t qos_data = frame_length >= 2u &&
        (frame[0] & 0x0cu) == 0x08u && (frame[0] & 0x80u) != 0u;
    if (!rtl8192se_build_tx_descriptor(
            descriptor, buffer_frames[index], frame_length,
            next_descriptor_address, queue_selection,
            (uint8_t)(qos_data == 0u))) {
        return 0;
    }

    __asm__ volatile ("" : : : "memory");
    rtl8192se_mmio_write16(
        (void *)(uintptr_t)adapter_status.mmio_base,
        RTL8192SE_TX_POLL_REGISTER, poll_bit);
    *next_index = (index + 1u) % RTL8192SE_TX_RING_COUNT;
    return 1;
}

int rtl8192se_receive_80211_frame(uint8_t *frame, uint32_t frame_capacity,
                                  uint32_t *frame_length)
{
    if (frame == 0 || frame_capacity == 0u || frame_length == 0 ||
        rx_dma_ready == 0u || adapter_status.rx_rings_programmed == 0u) {
        return 0;
    }

    uint8_t *ring = (uint8_t *)(uintptr_t)rx_mpdu_ring_frame;
    for (uint32_t scanned = 0u; scanned < RTL8192SE_RX_RING_COUNT; ++scanned) {
        uint32_t index = rx_mpdu_index;
        uint8_t *descriptor =
            ring + index * RTL8192SE_RX_DESCRIPTOR_SIZE;
        if ((read_le32(descriptor) & (1u << 31)) != 0u) {
            return 0;
        }

        uint32_t received_length = 0u;
        uint8_t *buffer =
            (uint8_t *)(uintptr_t)rx_mpdu_buffer_frames[index];
        int frame_valid = rtl8192se_rx_copy_frame(
            descriptor, buffer, RTL8192SE_RX_BUFFER_SIZE, frame,
            frame_capacity, &received_length);
        int end_of_ring = index + 1u == RTL8192SE_RX_RING_COUNT;
        if (!rtl8192se_rearm_rx_descriptor(
                descriptor, rx_mpdu_buffer_frames[index],
                RTL8192SE_RX_BUFFER_SIZE, end_of_ring)) {
            return 0;
        }
        rx_mpdu_index = (index + 1u) % RTL8192SE_RX_RING_COUNT;
        if (frame_valid) {
            *frame_length = received_length;
            return 1;
        }
    }
    return 0;
}

static int rtl8192se_device_supported(uint16_t device_id)
{
    return device_id == 0x8192u ||
           (device_id >= 0x8171u && device_id <= 0x8174u);
}

int rtl8192se_probe(struct rtl8192se_status *status)
{
    adapter_status = (struct rtl8192se_status){0};
    struct pci_device device;
    for (uint32_t index = 0u; index < pci_device_count(); ++index) {
        if (!pci_get_device(index, &device) ||
            device.vendor_id != RTL8192SE_VENDOR_ID ||
            !rtl8192se_device_supported(device.device_id)) {
            continue;
        }

        struct pci_bar bar;
        if (!pci_read_bar(&device, RTL8192SE_REGISTER_BAR, &bar) ||
            bar.is_io != 0u || bar.address > UINT32_MAX ||
            bar.address > UINT32_MAX - RTL8192SE_REGISTER_WINDOW_SIZE) {
            break;
        }

        uint32_t base = (uint32_t)bar.address;
        if (!paging_map_mmio(base, RTL8192SE_REGISTER_WINDOW_SIZE)) {
            break;
        }

        adapter_status.device_id = device.device_id;
        adapter_status.mmio_base = base;
        adapter_status.bus = device.bus;
        adapter_status.slot = device.slot;
        adapter_status.function = device.function;
        adapter_status.probed = 1u;
        break;
    }

    if (status != 0) {
        *status = adapter_status;
    }
    return adapter_status.probed != 0u;
}
