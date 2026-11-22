#ifndef NET_H
#define NET_H

#include <stdint.h>

#define NET_FRAME_MAX  1518
#define NET_MAX_DEVICES 4

struct net_device {
    const char *name;
    uint8_t     mac[6];
    int         (*send)(const uint8_t *frame, uint32_t len);
    int         (*recv)(uint8_t *buf, uint32_t max, uint32_t *out_len);
};

typedef void (*net_rx_handler_fn)(const uint8_t *frame, uint32_t len);

void net_init(void);
int  net_register(struct net_device *dev);
struct net_device *net_get(const char *name);

int  net_send(const char *ifname, const uint8_t *frame, uint32_t len);

/* 注册接收回调。NULL 表示默认 hex dump。 */
void net_set_rx_handler(net_rx_handler_fn fn);

/* 驱动调用：收到一帧时向上报告 */
void net_deliver(const uint8_t *frame, uint32_t len);

#endif