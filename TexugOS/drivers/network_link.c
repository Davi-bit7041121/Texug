#include "link.h"
#include "rtl8139.h"

static const struct network_link rtl8139_link = {
    .name = "RTL8139",
    .send = rtl8139_send,
    .receive = rtl8139_receive,
    .get_mac = rtl8139_get_mac,
};

const struct network_link *network_link_initialize(void)
{
    return rtl8139_initialize() ? &rtl8139_link : 0;
}
