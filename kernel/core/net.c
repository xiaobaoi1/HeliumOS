#include <net.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <net_proto.h>

static struct net_device *g_devices[NET_MAX_DEVICES];
static int g_count = 0;
static net_rx_handler_fn g_rx_handler = NULL;

void net_init(void) {
    g_count = 0;
    g_rx_handler = NULL;
    memset(g_devices, 0, sizeof(g_devices));
}

int net_register(struct net_device *dev) {
    if (!dev || g_count >= NET_MAX_DEVICES) return -1;
    g_devices[g_count++] = dev;
    kprintf("[NET] Registered %s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            dev->name,
            dev->mac[0], dev->mac[1], dev->mac[2],
            dev->mac[3], dev->mac[4], dev->mac[5]);
    return 0;
}

struct net_device *net_get(const char *name) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_devices[i]->name, name) == 0) return g_devices[i];
    }
    return NULL;
}

int net_send(const char *ifname, const uint8_t *frame, uint32_t len) {
    struct net_device *d = net_get(ifname);
    if (!d || !d->send) return -1;
    return d->send(frame, len);
}

void net_set_rx_handler(net_rx_handler_fn fn) {
    g_rx_handler = fn;
}

void net_deliver(const uint8_t *frame, uint32_t len) {
    if (g_rx_handler) {
        g_rx_handler(frame, len);
        return;
    }
    net_proto_rx(frame, len);
}