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
        struct device dev;
        struct cdev cdev;
        struct rpmsg_device* rpdev;

        struct rpmsg_channel_info chinfo;

	    struct mutex ept_lock;
	    struct rpmsg_endpoint *ept;

        struct list_head taurus_event_list_head;
        rwlock_t event_list_lock;


        /* ?? */
        spinlock_t queue_lock;
	    struct sk_buff_head queue;
	    wait_queue_head_t readq;

} rcar_cluster_device_t;

#endif /* __RCAR_CLUSTER_DRV_H__ */