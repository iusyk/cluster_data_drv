#ifndef __RCAR_CLUSTER_DRV_H__
#define __RCAR_CLUSTER_DRV_H__

#include <linux/kernel.h>
#include <linux/list.h>
struct taurus_rvgc_res_msg;

typedef struct taurus_event_list {
        uint32_t id;
        struct taurus_cluster_res_msg* result;
        struct list_head list;
        struct completion ack;
        bool ack_received;
        struct completion completed;
}taurus_event_list_t;

typedef struct rcar_cluster_device {
        struct device* dev;

        struct rpmsg_device* rpdev;

        struct list_head taurus_event_list_head;
        rwlock_t event_list_lock;

}rcar_cluster_device_t;

#endif /* __RCAR_CLUSTER_DRV_H__ */