/*
 * rcar_cluster_drv.c  --  R-Car Cluster driver
 *
 */

//#define DEBUG

#include <linux/device.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/dma-mapping.h>

#include <linux/rpmsg.h>
#include <linux/of_reserved_mem.h>
#include <linux/atomic.h>

#include "r_taurus_cluster_protocol.h"
#include "rcar_cluster_drv.h"

#pragma GCC optimize ("-Og")

#define RPMSG_DEV_MAX	(MINORMASK + 1)

#define RCAR_CLUSTER_DRM_NAME     "rcar-cluster-drv"

static int rpmsg_cluster_probe(struct rpmsg_device *rpdev);
static int rpmsg_cluster_cb(struct rpmsg_device* rpdev, void* data, int len,
			void* priv, u32 src);
static void rpmsg_cluster_remove(struct rpmsg_device *rpdev);			

static struct rpmsg_device_id rpmsg_driver_cluster_id_table[] = {
	{ .name	= "taurus-cluster" },
	{ },
};

static struct rpmsg_driver rpmsg_cluster_drv = {
	.drv.name	= "rpmsg_chrdev",/*KBUILD_MODNAME,*/
	.drv.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	.id_table	= rpmsg_driver_cluster_id_table,
	.probe		= rpmsg_cluster_probe,
	.callback	= rpmsg_cluster_cb,
	.remove		= rpmsg_cluster_remove,
};

static atomic_t rpmsg_id_counter = ATOMIC_INIT(0);

static int cluster_taurus_get_uniq_id(void) {
	return atomic_inc_return(&rpmsg_id_counter);
}

static int send_msg(struct rpmsg_device *rpdev, taurus_cluster_data_t *data, taurus_cluster_res_msg_t* res_msg) {
	int ret = 0;
	R_TAURUS_CmdMsg_t msg;
	struct taurus_event_list* event;

	printk("!!! Sending msg !!!");

	event = devm_kzalloc(&rpdev->dev, sizeof(*event), GFP_KERNEL);
    if (!event) {
		dev_err(&rpdev->dev, "%s:%d Can't allocate memory for taurus event\n", __FUNCTION__, __LINE__);
		return -ENOMEM;
    }
	event->result = devm_kzalloc(&rpdev->dev, sizeof(*event->result), GFP_KERNEL);
    
	if (!event->result) {
        dev_err(&rpdev->dev, "%s:%d Can't allocate memory for taurus event->result\n", __FUNCTION__, __LINE__);
		devm_kfree(&rpdev->dev, event);
		return -ENOMEM;
    }

	rcar_cluster_device_t *clusterdrv = (rcar_cluster_device_t*)dev_get_drvdata(&rpdev->dev);

	if(!clusterdrv){
		dev_err(&rpdev->dev, "%s:%d Can't get data type rcar_cluster_device*\n", __FUNCTION__, __LINE__);
		devm_kfree(&rpdev->dev, event->result);
		devm_kfree(&rpdev->dev, event);		
		return -ENOMEM;
	}

	msg.Id = cluster_taurus_get_uniq_id();
	msg.Channel = 0x80;
	msg.Cmd = R_TAURUS_CMD_IOCTL;
	msg.Par1 = 2;
	msg.Par2 = 4;
	msg.Par3 = 6;

    event->id = msg.Id;
    init_completion(&event->ack);

	printk("Init ack!\n");
    
	init_completion(&event->completed);
	
	printk("Init completed!\n");

    write_lock(&clusterdrv->event_list_lock);
    list_add(&event->list, &clusterdrv->taurus_event_list_head);
    write_unlock(&clusterdrv->event_list_lock);


	printk("!!! Params: %d -> %d!\n",(int)msg.Par2, (int)msg.Par3);

	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
	
	if (ret){
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		printk("Send error ack!\n");
	}
	
	ret = wait_for_completion_interruptible(&event->ack);
	if (ret == -ERESTARTSYS) {
		/* we were interrupted */
        dev_err(&rpdev->dev, "%s:%d Interrupted while waiting taurus ACK (%d)\n", __FUNCTION__, __LINE__, ret);
		printk("!!! FAIL ack!\n");
	}
	else if(wait_for_completion_interruptible(&event->completed) == -ERESTARTSYS){
		dev_err(&rpdev->dev, "%s:%d Interrupted while waiting taurus response (%d)\n", __FUNCTION__, __LINE__, ret);
		printk("!!! FAIL completes!\n");
	}
	else {
		printk("!!! Copying result!\n");
		memcpy(res_msg, event->result, sizeof(taurus_cluster_res_msg_t));
		printk("!!! Copied result!\n");
	}

	write_lock(&clusterdrv->event_list_lock);
	list_del(&event->list);
	write_unlock(&clusterdrv->event_list_lock);

	devm_kfree(&rpdev->dev, event->result);
	devm_kfree(&rpdev->dev, event);	

	printk("!!! CLUSTER UNLOK MSG");

	return ret;
}

/* -----------------------------------------------------------------------------
 * RPMSG operations
 */

static int rpmsg_cluster_cb(struct rpmsg_device* rpdev, void* data, int len,
			void* priv, u32 src) {
	int ret = 0;
	R_TAURUS_CmdMsg_t msg;
	struct taurus_event_list* event;
	struct taurus_cluster_res_msg* res = (struct taurus_cluster_res_msg*)data;
	struct list_head* i;
	rcar_cluster_device_t* clusterdrv = (rcar_cluster_device_t*)dev_get_drvdata(&rpdev->dev);
	uint32_t res_id = res->hdr.Id;
	printk("!!! CLUSTER CALLBACK res %d!\n", res->hdr.Result);

	if ((res->hdr.Result == R_TAURUS_CMD_NOP) && (res_id == 0)) {/*necessary send data to cluster*/
	}
	else {/*send ACK message*/
		read_lock(&clusterdrv->event_list_lock);
		
		list_for_each_prev(i, &clusterdrv->taurus_event_list_head) {
			event = list_entry(i, struct taurus_event_list, list);
			if (event->id == res_id) {

				memcpy(event->result, data, len);
		
				printk("!! Copy data for id %d", res_id);
		
				if (event->ack_received) {
					printk("!! COMPLETED");
					complete(&event->completed);
				} else {
					event->ack_received = 1;
					complete(&event->ack);
				}
			}
		}
		read_unlock(&clusterdrv->event_list_lock);
	}
	return 0;
}

static int rpmsg_cluster_probe(struct rpmsg_device* rpdev)
{
	rcar_cluster_device_t *clusterdvc = NULL;
	int ret;
	R_TAURUS_CmdMsg_t msg;
	int error = 0;
	taurus_cluster_res_msg_t res_msg;
	struct rpmsg_ctrldev *ctrldev;
	taurus_cluster_data_t cluster_data = {
		.rpm = 100,
		.speed = 200,
	};
	printk("rpmsg_cluster_probe!!!!!");
	/*
	this condition just because of error during the call of insmode
	probe is called repeatedly
	*/

	clusterdvc = devm_kzalloc(&rpdev->dev, sizeof(rcar_cluster_device_t), GFP_KERNEL);
	
	if (clusterdvc== NULL)
			return -ENOMEM;

	dev_set_drvdata(&rpdev->dev, clusterdvc);

	clusterdvc->dev = &rpdev->dev;
    clusterdvc->rpdev = rpdev;
    INIT_LIST_HEAD(&clusterdvc->taurus_event_list_head);
    rwlock_init(&clusterdvc->event_list_lock);

	dev_set_name(&rpdev->dev, "rpmsg_cluster%d", ret);
	
	send_msg(rpdev, &cluster_data, &res_msg);
	
	printk("Sent !!!");
	return 0;
}

static void rpmsg_cluster_remove(struct rpmsg_device *rpdev)
{
	void *data = NULL;

	printk("!!!! rpmsg_cluster_remove");
    
	data = dev_get_drvdata(&rpdev->dev);

	if(data){
		devm_kfree(&rpdev->dev, data);
	};

    return;
}

static int __init cluster_drv_init(void)
{
        printk("!!! cluster_drv_init!\n");

		return register_rpmsg_driver(&rpmsg_cluster_drv);
}

static void __exit cluster_drv_exit(void)
{
        unregister_rpmsg_driver(&rpmsg_cluster_drv);

        printk("!!! cluster_drv_exit! end\n");
}	


MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_cluster_id_table);

/*module_rpmsg_driver(rpmsg_cluster_drv);*/

module_init(cluster_drv_init);
module_exit(cluster_drv_exit);
MODULE_ALIAS("rpmsg_cluster:rpmsg_chrdev");
MODULE_DESCRIPTION("Remote processor messaging cluster driver");
MODULE_LICENSE("GPL v2");



