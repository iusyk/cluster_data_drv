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
#include "r_taurus_cluster_protocol.h"

#include <linux/atomic.h>

#pragma GCC optimize ("-Og")

#define RCAR_CLUSTER_DRM_NAME     "rcar-cluster-drv"

static atomic_t rpmsg_id_counter = ATOMIC_INIT(0);

static int cluster_taurus_get_uniq_id(void) {
	return atomic_inc_return(&rpmsg_id_counter);
}

/* -----------------------------------------------------------------------------
 * RPMSG operations
 */

static int rcar_cluster_cb(struct rpmsg_device* rpdev, void* data, int len,
			void* priv, u32 src) {
	taurus_cluster_data_t * params = (taurus_cluster_data_t*)data;

	dev_dbg(&rpdev->dev, "%s():%d\n", __FUNCTION__, __LINE__);

        R_TAURUS_CmdMsg_t msg;
	msg.Id = cluster_taurus_get_uniq_id();
	msg.Per = R_TAURUS_CLUSTER_PERI_ID;
	msg.Channel = 0xff;
	msg.Cmd = R_TAURUS_CMD_IOCTL;
	msg.Par1 = 0;
	msg.Par2 = (int)params.speed;
	msg.Par3 = params.rpm;

	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
	if (ret)
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);

	return 0;
}

static int rpmsg_cluster_probe(struct rpmsg_device *rpdev)
{
	int ret;
	struct instance_data *idata;

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n",
					rpdev->src, rpdev->dst);

	R_TAURUS_CmdMsg_t msg;
        msg.Id = cluster_taurus_get_uniq_id();
        msg.Channel = 0xff;
        msg.Cmd = R_TAURUS_CMD_IOCTL;
        msg.Par1 = 0;
        msg.Par2 = (int)params.speed;
        msg.Par3 = params.rpm;
	/* send a message to our remote processor */
	ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void rpmsg_cluster_remove(struct rpmsg_device *rpdev)
{
}

static struct rpmsg_device_id rpmsg_driver_cluster_id_table[] = {
	{ .name	= "taurus-cluster" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_cluster_id_table);

static struct rpmsg_driver rpmsg_cluster_drv = {
	.drv.name	= KBUILD_MODNAME,
	.id_table	= rpmsg_driver_cluster_id_table,
	.probe		= rpmsg_cluster_probe,
	.callback	= rpmsg_cluster_cb,
	.remove		= rpmsg_cluster_remove,
};

module_rpmsg_driver(rpmsg_cluster_drv);

MODULE_DESCRIPTION("Remote processor messaging cluster driver");
MODULE_LICENSE("GPL v2");



