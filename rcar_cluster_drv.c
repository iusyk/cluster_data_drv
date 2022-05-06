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
#include <linux/skbuff.h>
#include <uapi/linux/rpmsg.h>
#include "linux/cdev.h"
#include "r_taurus_cluster_protocol.h"
#include "rcar_cluster_drv.h"
/*#include "rcar_cluster_drv.h"*/

#pragma GCC optimize ("-Og")

static DEFINE_IDA(rpmsg_ctrl_ida);
static DEFINE_IDA(rpmsg_ept_ida);
static DEFINE_IDA(rpmsg_minor_ida);

#define RPMSG_DEV_MAX	(MINORMASK + 1)

#define RCAR_CLUSTER_DRM_NAME     "rcar-cluster-drv"


/**
 * struct rpmsg_ctrldev - control device for instantiating endpoint devices
 * @rpdev:	underlaying rpmsg device
 * @cdev:	cdev for the ctrl device
 * @dev:	device for the ctrl device
 */
struct rpmsg_ctrldev {
	struct rpmsg_device *rpdev;
	struct cdev cdev;
	struct device dev;
};

/**
 * struct rpmsg_eptdev - endpoint device context
 * @dev:	endpoint device
 * @cdev:	cdev for the endpoint device
 * @rpdev:	underlaying rpmsg device
 * @chinfo:	info used to open the endpoint
 * @ept_lock:	synchronization of @ept modifications
 * @ept:	rpmsg endpoint reference, when open
 * @queue_lock:	synchronization of @queue operations
 * @queue:	incoming message queue
 * @readq:	wait object for incoming queue
 */
typedef struct rcar_cluster_eptdev {
	struct device dev;
	struct cdev cdev;

	struct rpmsg_device *rpdev;
	struct rpmsg_channel_info chinfo;

	struct mutex ept_lock;
	struct rpmsg_endpoint *ept;

	spinlock_t queue_lock;
	struct sk_buff_head queue;
	wait_queue_head_t readq;
} rcar_cluster_eptdev_t;

static dev_t rpmsg_major;
static struct class *rpmsg_class;
/*
static DEFINE_IDA(rpmsg_ctrl_ida);
static DEFINE_IDA(rpmsg_ept_ida);
static DEFINE_IDA(rpmsg_minor_ida);
*/
#define dev_to_rcar_eptdev(dev) container_of(dev, rcar_cluster_eptdev_t, dev)
#define cdev_to_rcar_eptdev(i_cdev) container_of(i_cdev, rcar_cluster_eptdev_t, cdev)

#define dev_to_clusterdev(dev) container_of(dev, rcar_cluster_device_t, dev)
#define cdev_to_clusterdev(i_cdev) container_of(i_cdev, rcar_cluster_device_t, cdev)

static int rpmsg_cluster_probe(struct rpmsg_device *rpdev);
static int rpmsg_cluster_cb(struct rpmsg_device* rpdev, void* data, int len,
			void* priv, u32 src);
static void rpmsg_cluster_remove(struct rpmsg_device *rpdev);
static int rpmsg_eptdev_open(struct inode *inode, struct file *filp);
static int rpmsg_eptdev_release(struct inode *inode, struct file *filp);
static ssize_t rpmsg_eptdev_write_iter(struct kiocb *iocb,
				       struct iov_iter *from);
static long rpmsg_eptdev_ioctl(struct file *fp, unsigned int cmd,
			       unsigned long arg);

static int rpmsg_ctrldev_open(struct inode *inode, struct file *filp);
static int rpmsg_ctrldev_release(struct inode *inode, struct file *filp);
static long rpmsg_ctrldev_ioctl(struct file *fp, unsigned int cmd,
				unsigned long arg);	   

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


static const struct file_operations rpmsg_eptdev_fops = {
	.owner = THIS_MODULE,
	.open = rpmsg_eptdev_open,
	.release = rpmsg_eptdev_release,
	/*.read_iter = rpmsg_eptdev_read_iter,*/
	.write_iter = rpmsg_eptdev_write_iter,
	/*.poll = rpmsg_eptdev_poll,*/
	.unlocked_ioctl = rpmsg_eptdev_ioctl,
	/*.compat_ioctl = compat_ptr_ioctl,*/
};

static const struct file_operations rpmsg_ctrldev_fops = {
	.owner = THIS_MODULE,
	.open = rpmsg_ctrldev_open,
	.release = rpmsg_ctrldev_release,
	.unlocked_ioctl = rpmsg_ctrldev_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static atomic_t rpmsg_id_counter = ATOMIC_INIT(0);

static int cluster_taurus_get_uniq_id(void) {
	return atomic_inc_return(&rpmsg_id_counter);
}

static int rpmsg_ctrldev_release(struct inode *inode, struct file *filp)
{
	rcar_cluster_eptdev_t *clusterdev= cdev_to_rcar_eptdev(inode->i_cdev);

	put_device(&clusterdev->dev);

	return 0;
}

static int rpmsg_ctrldev_open(struct inode *inode, struct file *filp)
{
	rcar_cluster_device_t *clusterdev = cdev_to_clusterdev(inode->i_cdev);

	get_device(&clusterdev->dev);
	filp->private_data = clusterdev;

    printk("cluster: !!! rpmsg_ctrldev_open");
	return 0;
}

static void rpmsg_clusterdev_release_device(struct device *dev)
{
	rcar_cluster_device_t *clusterdvc = dev_to_clusterdev(dev);

	ida_simple_remove(&rpmsg_ctrl_ida, dev->id);
	ida_simple_remove(&rpmsg_minor_ida, MINOR(dev->devt));
	kfree(clusterdvc);
}

static int send_msg(struct rpmsg_device *rpdev, taurus_cluster_data_t *data, taurus_cluster_res_msg_t* res_msg) {
	int ret = 0;
	R_TAURUS_CmdMsg_t msg;
	struct taurus_event_list* event;

	printk("cluster !!! Sending msg !!!");

	event = devm_kzalloc(&rpdev->dev, sizeof(*event), GFP_KERNEL);
    if (!event) {
		dev_err(&rpdev->dev, "cluster: %s:%d Can't allocate memory for taurus event\n", __FUNCTION__, __LINE__);
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

	printk("cluster: Init ack!\n");
    
	init_completion(&event->completed);
	
	printk("cluster: Init completed!\n");

    write_lock(&clusterdrv->event_list_lock);
    list_add(&event->list, &clusterdrv->taurus_event_list_head);
    write_unlock(&clusterdrv->event_list_lock);


	printk("cluster !!! Params: %d -> %d!\n",(int)msg.Par2, (int)msg.Par3);

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
	struct taurus_event_list* event = NULL;
	struct taurus_cluster_res_msg* res = (struct taurus_cluster_res_msg*)data;
	struct list_head* i = NULL;
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
/*
static void rpmsg_rcar_cluster_release_device(struct device *dev)
{
	rcar_cluster_device_t *clusterdvc = dev_to_rcar_cluster(dev);

	ida_simple_remove(&rpmsg_ctrl_ida, dev->id);
	ida_simple_remove(&rpmsg_minor_ida, MINOR(dev->devt));
	kfree(clusterdvc);
}
*/
static int rpmsg_cluster_probe(struct rpmsg_device* rpdev)
{
	rcar_cluster_device_t *clusterdvc = NULL;
	int ret = 0;
	R_TAURUS_CmdMsg_t msg;
	int error = 0;
	struct device *dev = NULL;
	taurus_cluster_res_msg_t res_msg;

	taurus_cluster_data_t cluster_data = {
		.rpm = 1000,
		.speed = 40,
	};
	
	printk("rpmsg_cluster_probe!!!!!/n");

	/*
	this condition just because of error during the call of insmode
	probe is called repeatedly
	*/

	clusterdvc = kzalloc(sizeof(*clusterdvc), GFP_KERNEL);

	if (clusterdvc== NULL)
		return -ENOMEM;
	
	clusterdvc->rpdev = rpdev;
	dev = &clusterdvc->dev;
	
	device_initialize(dev);

	dev->parent = &rpdev->dev;
	dev->class = rpmsg_class;
	
	cdev_init(&clusterdvc->cdev, &rpmsg_ctrldev_fops);

	clusterdvc->cdev.owner = THIS_MODULE;
	ret = ida_simple_get(&rpmsg_minor_ida, 0, RPMSG_DEV_MAX, GFP_KERNEL);
	if (ret < 0) {
		goto free_clusterdvc;
	}
	
	clusterdvc->dev.devt = MKDEV(MAJOR(rpmsg_major), ret);
	
	ret = ida_simple_get(&rpmsg_ctrl_ida, 0, 0, GFP_KERNEL);
	if (ret < 0){
		goto free_minor_ida;
	}
	
	clusterdvc->dev.id = ret;

	dev_set_name(&clusterdvc->dev, "rpmsg_ctrl%d", ret);
	
	ret = cdev_device_add(&clusterdvc->cdev, &clusterdvc->dev);
	if (ret) {
		goto free_ctrl_ida;
	}

	/* We can now rely on the function for cleanup */
	
	clusterdvc->dev.release = rpmsg_clusterdev_release_device;
	
	dev_set_drvdata(&rpdev->dev, clusterdvc);
	
    INIT_LIST_HEAD(&clusterdvc->taurus_event_list_head);
    rwlock_init(&clusterdvc->event_list_lock);

	/*send a ping of message with dummy data*/
	send_msg(rpdev, &cluster_data, &res_msg);

	return ret;

free_ctrl_ida:
	ida_simple_remove(&rpmsg_ctrl_ida, clusterdvc->dev.id);
free_minor_ida:
	ida_simple_remove(&rpmsg_minor_ida, MINOR(clusterdvc->dev.devt));
free_clusterdvc:
	put_device(&clusterdvc->dev);
	kfree(clusterdvc);	

	return ret;
}

static int rpmsg_eptdev_destroy(struct device *dev, void *data)
{
	rcar_cluster_eptdev_t *eptdev = dev_to_rcar_eptdev(dev);

	mutex_lock(&eptdev->ept_lock);
	if (eptdev->ept) {
		rpmsg_destroy_ept(eptdev->ept);
		eptdev->ept = NULL;
	}
	mutex_unlock(&eptdev->ept_lock);

	/* wake up any blocked readers */
	wake_up_interruptible(&eptdev->readq);

	cdev_device_del(&eptdev->cdev, &eptdev->dev);
	put_device(&eptdev->dev);

	return 0;
}

static void rpmsg_cluster_remove(struct rpmsg_device *rpdev)
{
	rcar_cluster_device_t *data = dev_get_drvdata(&rpdev->dev);
	int ret = 0;

	printk("rpmsg_cluster_remove");

	ret = device_for_each_child(&data->dev, NULL, rpmsg_eptdev_destroy);
	if (ret)
		dev_warn(&rpdev->dev, "failed to nuke endpoints: %d\n", ret);

	cdev_device_del(&data->cdev, &data->dev);
	put_device(&data->dev);
	
    return;
}

static int /*__init*/ cluster_drv_init(void)
{
	int ret = 0;

    printk("!!! cluster_drv_init!\n");

	ret = alloc_chrdev_region(&rpmsg_major, 0, RPMSG_DEV_MAX, "rpmsg");
	if (ret < 0) {
		pr_err("failed to allocate char dev region\n");
		return ret;
	}

	rpmsg_class = class_create(THIS_MODULE, "rpmsg");
	
	if (IS_ERR(rpmsg_class)) {
		pr_err("failed to create rpmsg class\n");
		unregister_chrdev_region(rpmsg_major, RPMSG_DEV_MAX);
		return PTR_ERR(rpmsg_class);
	}

	printk("!!! cluster_drv_init - class created!\n");

	ret =  register_rpmsg_driver(&rpmsg_cluster_drv);

	if (ret < 0) {
		pr_err("failed to register cluster_drv_init driver\n");
		class_destroy(rpmsg_class);
		unregister_chrdev_region(rpmsg_major, RPMSG_DEV_MAX);
	}

	return ret;
}
postcore_initcall(cluster_drv_init);

static void /*__exit*/ cluster_drv_exit(void)
{
    unregister_rpmsg_driver(&rpmsg_cluster_drv);
	class_destroy(rpmsg_class);
	unregister_chrdev_region(rpmsg_major, RPMSG_DEV_MAX);

    printk("!!! cluster_drv_exit! end\n");
}

/*  */
/*
static void rpmsg_eptdev_release_device(struct device *dev)
{
	struct rpmsg_eptdev *eptdev = dev_to_eptdev(dev);

	ida_simple_remove(&rpmsg_ept_ida, dev->id);
	ida_simple_remove(&rpmsg_minor_ida, MINOR(eptdev->dev.devt));
	kfree(eptdev);
}
*/

static int rpmsg_eptdev_create(struct rpmsg_ctrldev *ctrldev,
			       struct rpmsg_channel_info chinfo)
{
	struct rpmsg_device *rpdev = ctrldev->rpdev;
	rcar_cluster_eptdev_t *eptdev;
	struct device *dev;
	int ret;

	eptdev = kzalloc(sizeof(*eptdev), GFP_KERNEL);
	if (!eptdev)
		return -ENOMEM;

	dev = &eptdev->dev;
	eptdev->rpdev = rpdev;
	eptdev->chinfo = chinfo;

	mutex_init(&eptdev->ept_lock);
	spin_lock_init(&eptdev->queue_lock);
	skb_queue_head_init(&eptdev->queue);
	init_waitqueue_head(&eptdev->readq);

	device_initialize(dev);
	dev->class = rpmsg_class;
	dev->parent = &ctrldev->dev;
	/*dev->groups = rpmsg_eptdev_groups;*//*I am not sure of it is necessary*/
	dev_set_drvdata(dev, eptdev);

	cdev_init(&eptdev->cdev, &rpmsg_eptdev_fops);
	eptdev->cdev.owner = THIS_MODULE;

	ret = ida_simple_get(&rpmsg_minor_ida, 0, RPMSG_DEV_MAX, GFP_KERNEL);
	if (ret < 0)
		goto free_eptdev;
	dev->devt = MKDEV(MAJOR(rpmsg_major), ret);

	ret = ida_simple_get(&rpmsg_ept_ida, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto free_minor_ida;
	dev->id = ret;
	dev_set_name(dev, "rpmsg%d", ret);

	ret = cdev_device_add(&eptdev->cdev, &eptdev->dev);
	if (ret)
		goto free_ept_ida;

	/* We can now rely on the release function for cleanup */
	/*dev->release = rpmsg_eptdev_release_device;*/

	return ret;

free_ept_ida:
	ida_simple_remove(&rpmsg_ept_ida, dev->id);
free_minor_ida:
	ida_simple_remove(&rpmsg_minor_ida, MINOR(dev->devt));
free_eptdev:
	put_device(dev);
	kfree(eptdev);

	return ret;
}

static long rpmsg_ctrldev_ioctl(struct file *fp, unsigned int cmd,
				unsigned long arg)
{
	struct rpmsg_ctrldev *ctrldev = fp->private_data;
	void __user *argp = (void __user *)arg;
	struct rpmsg_endpoint_info eptinfo;
	struct rpmsg_channel_info chinfo;

	if (cmd != RPMSG_CREATE_EPT_IOCTL)
		return -EINVAL;

	if (copy_from_user(&eptinfo, argp, sizeof(eptinfo)))
		return -EFAULT;

	memcpy(chinfo.name, eptinfo.name, RPMSG_NAME_SIZE);
	chinfo.name[RPMSG_NAME_SIZE-1] = '\0';
	chinfo.src = eptinfo.src;
	chinfo.dst = eptinfo.dst;

	return rpmsg_eptdev_create(ctrldev, chinfo);
};

static int rpmsg_eptdev_open(struct inode *inode, struct file *filp)
{
	printk("rpmsg_eptdev_open");
/*	struct rpmsg_eptdev *eptdev = cdev_to_eptdev(inode->i_cdev);
	struct rpmsg_endpoint *ept;
	struct rpmsg_device *rpdev = eptdev->rpdev;
	struct device *dev = &eptdev->dev;

	if (eptdev->ept)
		return -EBUSY;

	get_device(dev);

	ept = rpmsg_create_ept(rpdev, rpmsg_ept_cb, eptdev, eptdev->chinfo);
	if (!ept) {
		dev_err(dev, "failed to open %s\n", eptdev->chinfo.name);
		put_device(dev);
		return -EINVAL;
	}

	eptdev->ept = ept;
	filp->private_data = eptdev;
*/
	return 0;
}


static int rpmsg_eptdev_release(struct inode *inode, struct file *filp)
{
/*	struct rpmsg_eptdev *eptdev = cdev_to_eptdev(inode->i_cdev);
	struct device *dev = &eptdev->dev;

	/ Close the endpoint, if it's not already destroyed by the parent /
	mutex_lock(&eptdev->ept_lock);
	if (eptdev->ept) {
		rpmsg_destroy_ept(eptdev->ept);
		eptdev->ept = NULL;
	}
	mutex_unlock(&eptdev->ept_lock);

	/ Discard all SKBs /
	skb_queue_purge(&eptdev->queue);

	put_device(dev);
*/
	return 0;
}

static ssize_t rpmsg_eptdev_write_iter(struct kiocb *iocb,
				       struct iov_iter *from)
{
	/*struct file *filp = iocb->ki_filp;
	struct rpmsg_eptdev *eptdev = filp->private_data;
	size_t len = iov_iter_count(from);
	void *kbuf;
	int ret;

	kbuf = kzalloc(len, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (!copy_from_iter_full(kbuf, len, from)) {
		ret = -EFAULT;
		goto free_kbuf;
	}

	if (mutex_lock_interruptible(&eptdev->ept_lock)) {
		ret = -ERESTARTSYS;
		goto free_kbuf;
	}

	if (!eptdev->ept) {
		ret = -EPIPE;
		goto unlock_eptdev;
	}

	if (filp->f_flags & O_NONBLOCK)
		ret = rpmsg_trysendto(eptdev->ept, kbuf, len, eptdev->chinfo.dst);
	else
		ret = rpmsg_sendto(eptdev->ept, kbuf, len, eptdev->chinfo.dst);

unlock_eptdev:
	mutex_unlock(&eptdev->ept_lock);

free_kbuf:
	kfree(kbuf);
	return ret < 0 ? ret : len;*/
	return 0;
}

static long rpmsg_eptdev_ioctl(struct file *fp, unsigned int cmd,
			       unsigned long arg)
{
/*	struct rpmsg_eptdev *eptdev = fp->private_data;

	if (cmd != RPMSG_DESTROY_EPT_IOCTL)
		return -EINVAL;

	return rpmsg_eptdev_destroy(&eptdev->dev, NULL);*/
	printk("!!! rpmsg_eptdev_ioctl");
	return 0;
}
/*
static int rpmsg_chrdev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&rpmsg_major, 0, RPMSG_DEV_MAX, "rpmsg");
	if (ret < 0) {
		pr_err("failed to allocate char dev region\n");
		return ret;
	}

	rpmsg_class = class_create(THIS_MODULE, "rpmsg");
	if (IS_ERR(rpmsg_class)) {
		pr_err("failed to create rpmsg class\n");
		unregister_chrdev_region(rpmsg_major, RPMSG_DEV_MAX);
		return PTR_ERR(rpmsg_class);
	}

	ret = register_rpmsg_driver(&rpmsg_chrdev_driver);
	if (ret < 0) {
		pr_err("failed to register rpmsg driver\n");
		class_destroy(rpmsg_class);
		unregister_chrdev_region(rpmsg_major, RPMSG_DEV_MAX);
	}

	return ret;
}
postcore_initcall(rpmsg_chrdev_init);
*/

MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_cluster_id_table);

/*module_rpmsg_driver(rpmsg_cluster_drv);*/

/*module_init(cluster_drv_init);*/
module_exit(cluster_drv_exit);
MODULE_ALIAS("rpmsg_cluster:rpmsg_chrdev");
MODULE_DESCRIPTION("Remote processor messaging cluster driver");
MODULE_LICENSE("GPL v2");



