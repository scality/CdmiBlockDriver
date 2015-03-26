/*
 * Copyright (C) 2014 SCALITY SA - http://www.scality.com
 *
 * This file is part of ScalityRestBlock.
 *
 * ScalityRestBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ScalityRestBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ScalityRestBlock.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Linux Kernel/LKM headers: module.h is needed by all modules and
// kernel.h is needed for KERN_INFO.

#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/version.h>

#include <srb/srb-cdmi.h>
#include <srb/srb-log.h>

#include "srb.h"

#define DEV_MAX			64
#define DEV_MINORS		256

/*
 * Default values for ScalityRestBlock LKM parameters
 */
#define SRB_REQ_TIMEOUT_DFLT		30
#define SRB_NB_REQ_RETRIES_DFLT	3
#define SRB_CONN_TIMEOUT_DFLT		30
#define SRB_LOG_LEVEL_DFLT		SRB_INFO
#define SRB_THREAD_POOL_SIZE_DFLT	8

// LKM information
MODULE_AUTHOR("Laurent Meyer <laurent.meyer@digitam.net>");
MODULE_AUTHOR("David Pineau <david.pineau@scality.com>");
MODULE_DESCRIPTION("Block Device Driver for REST-based storage");
MODULE_LICENSE("GPL");
MODULE_VERSION(DEV_REL_VERSION);

typedef struct srb_server_s {
	struct srb_server_s   	*next;
	struct srb_cdmi_desc	cdmi_desc;
} srb_server_t;

/* Device state (reduce spinlock section and avoid multiple operation on same device) */
enum device_state {
        DEV_IN_USE,
        DEV_UNUSED,
};

struct srb_device {
	/* Device subsystem related data */
	int			id;		/* device ID */
	int			major;		/* blkdev assigned major */

	/* NOTE: use const from ./linux/genhd.h */
	char			name[DISK_NAME_LEN];	/* blkdev name, e.g. srba */
	struct gendisk		*disk;
	uint64_t		disk_size;	/* Size in bytes */
	atomic_t                users;		/* Number of users who
						 * opened dev */
	enum device_state			state; 		/* for create extend attach detach destroy purpose */

	struct request_queue	*q;
	spinlock_t		rq_lock;	/* request queue lock */

	struct task_struct	**thread;	/* allow dynamic allocation during device creation */
	int			nb_threads;

	/* Dewpoint specific data */
	struct srb_cdmi_desc	 **thread_cdmi_desc;	/* allow dynamic allocation during device creation*/

	/*
	** List of requests received by the drivers, but still to be
	** processed. This due to network latency.
	*/
	spinlock_t		waiting_lock;	/* wait_queue lock */
	wait_queue_head_t	waiting_wq;
	struct list_head	waiting_queue;  /* Requests to be sent */

	/* Debug traces */
	srb_debug_t		debug;
};

const srb_debug_t * srb_device_get_debug(const struct srb_device *dev) {
        return &dev->debug;
}

int srb_device_get_major(const struct srb_device *dev) {
        return dev->major;
}

const char * srb_device_get_name(const struct srb_device *dev) {
        return dev->name;
}

int srb_device_get_debug_level(const struct srb_device *dev) {
        return dev->debug.level;
}
void srb_device_set_debug_level(struct srb_device *dev, int level) {
        dev->debug.level = level;
}

struct srb_cdmi_desc * srb_device_get_thread_cdmi_desc(const struct srb_device *dev, int idx) {
        return dev->thread_cdmi_desc[idx];
}

struct gendisk * srb_device_get_disk(const struct srb_device *dev) {
        return dev->disk;
}

static srb_device_t	devtab[DEV_MAX];
static srb_server_t	*servers = NULL;
static DEFINE_SPINLOCK(devtab_lock);

/* Module parameters (LKM parameters)
 */
unsigned short srb_log = SRB_LOG_LEVEL_DFLT;
unsigned short req_timeout = SRB_REQ_TIMEOUT_DFLT;
unsigned short nb_req_retries = SRB_NB_REQ_RETRIES_DFLT;
unsigned short server_conn_timeout = SRB_CONN_TIMEOUT_DFLT;
unsigned int thread_pool_size = SRB_THREAD_POOL_SIZE_DFLT;
MODULE_PARM_DESC(debug, "Global log level for ScalityRestBlock LKM");
module_param_named(debug, srb_log, ushort, 0644);

MODULE_PARM_DESC(req_timeout, "Global timeout for request");
module_param(req_timeout, ushort, 0644);

MODULE_PARM_DESC(nb_req_retries, "Global number of retries for request");
module_param(nb_req_retries, ushort, 0644);

MODULE_PARM_DESC(server_conn_timeout, "Global timeout for connection to server(s)");
module_param(server_conn_timeout, ushort, 0644);

MODULE_PARM_DESC(thread_pool_size, "Size of the thread pool");
module_param(thread_pool_size, uint, 0444);

/* XXX: Request mapping
 */
static char *req_code_to_str(int code)
{
	switch (code) {
		case READ: return "READ"; break;
		case WRITE: return "WRITE"; break;
		case WRITE_FLUSH: return "WRITE_FLUSH"; break;
		case WRITE_FUA: return "WRITE_FUA"; break;
		case WRITE_FLUSH_FUA: return "WRITE_FLUSH_FUA"; break;
		default: return "UNKNOWN";
	}
}

static int req_flags_to_str(int flags, char *buff)
{
	//char buff[128];
	int size = 0;
	buff[0] = '\0';

	// detect common flags
	if (flags == REQ_COMMON_MASK) {
		strncpy(buff, "REQ_COMMON_MASK", 15);
		buff[16] = '\0';
		return 16;
	}
	if (flags == REQ_FAILFAST_MASK) {
		strncpy(buff, "REQ_FAILFAST_MASK", 17);
		buff[18] = '\0';
		return 18;
	}
	if (flags == REQ_NOMERGE_FLAGS) {
		strncpy(buff, "REQ_NOMERGE_FLAGS", 17);
		buff[18] = '\0';
		return 18;
	}

	if (flags & REQ_WRITE) {
		strncpy(&buff[size], "REQ_WRITE|", 10);
		size += 10;
	}
	if (flags & REQ_FAILFAST_DEV) {
		strncpy(&buff[size], "REQ_FAILFAST_DEV|", 17);
		size += 17;
	}
	if (flags & REQ_FAILFAST_TRANSPORT) {
		strncpy(&buff[size], "REQ_FAILFAST_TRANSPORT|", 23);
		size += 23;
	}
	if (flags & REQ_FAILFAST_DRIVER) {
		strncpy(&buff[size], "REQ_FAILFAST_DRIVER|", 20);
		size += 20;
	}
	if (flags & REQ_SYNC) {
		strncpy(&buff[size], "REQ_SYNC|", 9);
		size += 9;
	}
	if (flags & REQ_META) {
		strncpy(&buff[size], "REQ_META|", 9);
		size += 9;
	}
	if (flags & REQ_PRIO) {
		strncpy(&buff[size], "REQ_PRIO|", 9);
		size += 9;
	}
	if (flags & REQ_DISCARD) {
		strncpy(&buff[size], "REQ_DISCARD|", 13);
		size += 13;
	}
	if (flags & REQ_WRITE_SAME) {
		strncpy(&buff[size], "REQ_WRITE_SAME|", 16);
		size += 16;
	}
	if (flags & REQ_NOIDLE) {
		strncpy(&buff[size], "REQ_NOIDLE|", 11);
		size += 11;
	}
	if (flags & REQ_RAHEAD) {
		strncpy(&buff[size], "REQ_RAHEAD|", 11);
		size += 11;
	}
	if (flags & REQ_THROTTLED) {
		strncpy(&buff[size], "REQ_THROTTLED|", 14);
		size += 14;
	}
	if (flags & REQ_SORTED) {
		strncpy(&buff[size], "REQ_SORTED|", 11);
		size += 11;
	}
	if (flags & REQ_SOFTBARRIER) {
		strncpy(&buff[size], "REQ_SOFTBARRIER|", 16);
		size += 16;
	}
	if (flags & REQ_FUA) {
		strncpy(&buff[size], "REQ_FUA|", 8);
		size += 8;
	}
	if (flags & REQ_NOMERGE) {
		strncpy(&buff[size], "REQ_NOMERGE|", 12);
		size += 12;
	}
	if (flags & REQ_STARTED) {
		strncpy(&buff[size], "REQ_STARTED|", 12);
		size += 12;
	}
	if (flags & REQ_DONTPREP) {
		strncpy(&buff[size], "REQ_DONTPREP|", 13);
		size += 13;
	}
	if (flags & REQ_QUEUED) {
		strncpy(&buff[size], "REQ_QUEUED|", 11);
		size += 11;
	}
	if (flags & REQ_ELVPRIV) {
		strncpy(&buff[size], "REQ_ELVPRIV|", 12);
		size += 12;
	}
	if (flags & REQ_FAILED) {
		strncpy(&buff[size], "REQ_FAILED|", 11);
		size += 11;
	}
	if (flags & REQ_QUIET) {
		strncpy(&buff[size], "REQ_QUIET|", 10);
		size += 10;
	}
	if (flags & REQ_PREEMPT) {
		strncpy(&buff[size], "REQ_PREEMPT|", 12);
		size += 12;
	}
	if (flags & REQ_ALLOCED) {
		strncpy(&buff[size], "REQ_ALLOCED|", 12);
		size += 12;
	}
	if (flags & REQ_COPY_USER) {
		strncpy(&buff[size], "REQ_COPY_USER|", 14);
		size += 14;
	}
	if (flags & REQ_FLUSH) {
		strncpy(&buff[size], "REQ_FLUSH|", 10);
		size += 10;
	}
	if (flags & REQ_FLUSH_SEQ) {
		strncpy(&buff[size], "REQ_FLUSH_SEQ|", 14);
		size += 14;
	}
	if (flags & REQ_IO_STAT) {
		strncpy(&buff[size], "REQ_IO_STAT|", 12);
		size += 12;
	}
	if (flags & REQ_MIXED_MERGE) {
		strncpy(&buff[size], "REQ_MIXED_MERGE|", 16);
		size += 16;
	}
	if (flags & REQ_SECURE) {
		strncpy(&buff[size], "REQ_SECURE|", 11);
		size += 11;
	}
#ifdef REQ_KERNEL /* Gone in 3.18 */
	if (flags & REQ_KERNEL) {
		strncpy(&buff[size], "REQ_KERNEL|", 11);
		size += 11;
	}
#endif
	if (flags & REQ_PM) {
		strncpy(&buff[size], "REQ_PM|", 7);
		size += 7;
	}
#ifdef REQ_END	/* appears in 3.6.10 in linux/blk_types.h */
	if (flags & REQ_END) {
		strncpy(&buff[size], "REQ_END|", 8);
		size += 8;
	}
#endif
	if (size != 0)
		buff[size-1] = '\0';

	return size;
}


/*
 * Handle an I/O request.
 */
static int srb_xfer_scl(struct srb_device *dev,
		struct srb_cdmi_desc *desc,
		struct request *req)
{
	int ret = 0;
	struct timeval tv_start;
	struct timeval tv_end;

	SRBDEV_LOG_DEBUG(dev, "CDMI request %p (%s) with cdmi_desc %p",
			 req, req_code_to_str(rq_data_dir(req)), desc);

	if (SRB_DEBUG <= dev->debug.level)
		do_gettimeofday(&tv_start);

	if (rq_data_dir(req) == WRITE) {
		ret = srb_cdmi_putrange(&dev->debug,
					desc,
					blk_rq_pos(req) * 512ULL,
					blk_rq_sectors(req) * 512ULL);
	}
	else {
		ret = srb_cdmi_getrange(&dev->debug,
					desc,
					blk_rq_pos(req) * 512ULL,
					blk_rq_sectors(req) * 512ULL);
	}

	if (SRB_DEBUG <= dev->debug.level) {
		do_gettimeofday(&tv_end);
		SRBDEV_LOG_DEBUG(dev, "Request took %ldms",
				 (tv_end.tv_sec - tv_start.tv_sec)*1000 +
				 (tv_end.tv_usec - tv_start.tv_usec)/1000);
	}

	if (ret) {
		SRBDEV_LOG_ERR(dev, "CDMI Request using scatterlist failed"
			       " with IO error: %d", ret);
		return -EIO;
	}

	return ret;
}

/*
 * Free internal disk
 */
static int srb_free_disk(struct srb_device *dev)
{
	struct gendisk *disk = NULL;

	disk = dev->disk;
	if (!disk) {
		SRBDEV_LOG_ERR(dev, "Disk is not available anymore (%s)", dev->name);
		return -EINVAL;
	}
	dev->disk = NULL;

	/* free disk */
	if (disk->flags & GENHD_FL_UP) {
		del_gendisk(disk);
		if (disk->queue)
			blk_cleanup_queue(disk->queue);
	}

	put_disk(disk);

	return 0;
}

/*
 * Thread for srb
 */
static int srb_thread(void *data)
{
	struct srb_device *dev;
	struct request *req;
	unsigned long flags;
	int th_id;
	int th_ret = 0;
	char buff[256];
	struct req_iterator iter;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
	struct bio_vec *bvec;
#else
	struct bio_vec bvec;
#endif
	struct srb_cdmi_desc *cdmi_desc;

	SRBDEV_LOG_DEBUG(((struct srb_device *)data), "Thread started with device %p", data);

	dev = data;

	/* Init thread specific values */
	spin_lock(&devtab_lock);
	th_id = dev->nb_threads;
	dev->nb_threads++;
	spin_unlock(&devtab_lock);

	set_user_nice(current, -20);
	while (!kthread_should_stop() || !list_empty(&dev->waiting_queue)) {
		/* wait for something to do */
		wait_event_interruptible(dev->waiting_wq,
					kthread_should_stop() ||
					!list_empty(&dev->waiting_queue));

		/* TODO: improve kthread termination, otherwise calling we can not
		  terminate a kthread calling kthread_stop() */
		/* if (kthread_should_stop()) {
			printk(KERN_INFO "srb_thread: immediate kthread exit\n");
			do_exit(0);
		} */

		spin_lock_irqsave(&dev->waiting_lock, flags);
		/* extract request */
		if (list_empty(&dev->waiting_queue)) {
			spin_unlock_irqrestore(&dev->waiting_lock, flags);
			continue;
		}
		req = list_entry(dev->waiting_queue.next, struct request,
				queuelist);
		list_del_init(&req->queuelist);
		spin_unlock_irqrestore(&dev->waiting_lock, flags);
		
		if (blk_rq_sectors(req) == 0) {
			blk_end_request_all(req, 0);
			continue;
		}

		req_flags_to_str(req->cmd_flags, buff);
		SRBDEV_LOG_DEBUG(dev, "thread %d: New REQ of type %s (%d) flags: %s (%llu)",
				 th_id, req_code_to_str(rq_data_dir(req)), rq_data_dir(req), buff,
                                 (unsigned long long)req->cmd_flags);
		if (req->cmd_flags & REQ_FLUSH) {
			SRBDEV_LOG_DEBUG(dev, "DEBUG CMD REQ_FLUSH\n");
		}
		/* XXX: Use iterator instead of internal function (cf linux/blkdev.h)
		 *  __rq_for_each_bio(bio, req) {
		 */
		rq_for_each_segment(bvec, req, iter) {
			if (iter.bio->bi_rw & REQ_FLUSH) {
				SRBDEV_LOG_DEBUG(dev, "DEBUG VR BIO REQ_FLUSH\n");
			}
		}

		/* Create scatterlist */
		cdmi_desc = srb_device_get_thread_cdmi_desc(dev, th_id);
		sg_init_table(cdmi_desc->sgl, DEV_NB_PHYS_SEGS);
		dev->thread_cdmi_desc[th_id]->sgl_size = blk_rq_map_sg(dev->q, req, dev->thread_cdmi_desc[th_id]->sgl);

		SRBDEV_LOG_DEBUG(dev, "scatter_list size %d [nb_seg = %d,"
		                 " sector = %lu, nr_sectors=%u w=%d]",
		                 DEV_NB_PHYS_SEGS,
		                 dev->thread_cdmi_desc[th_id]->sgl_size,
		                 blk_rq_pos(req), blk_rq_sectors(req),
		                 rq_data_dir(req) == WRITE);

		/* Call scatter function */
		th_ret = srb_xfer_scl(dev, dev->thread_cdmi_desc[th_id], req);

		SRBDEV_LOG_DEBUG(dev, "thread %d: REQ done with returned code %d",
		                 th_id, th_ret);
	
		/* No IO error testing for the moment */
		blk_end_request_all(req, 0);
	}

	return 0;
}


static void srb_rq_fn(struct request_queue *q)
{
	struct srb_device *dev = q->queuedata;	
	struct request *req;
	unsigned long flags;

	while ((req = blk_fetch_request(q)) != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			SRBDEV_LOG_DEBUG(dev, "Skip non-CMD request");

			__blk_end_request_all(req, -EIO);
			continue;
		}

		spin_lock_irqsave(&dev->waiting_lock, flags);
		list_add_tail(&req->queuelist, &dev->waiting_queue);
		spin_unlock_irqrestore(&dev->waiting_lock, flags);
		wake_up_nr(&dev->waiting_wq, 1);
	}
}

static int srb_open(struct block_device *bdev, fmode_t mode)
{
	srb_device_t *dev = (srb_device_t*)bdev->bd_disk->private_data;
	int ret = 0;

	SRBDEV_LOG_INFO(dev, "Opening device (%s)", bdev->bd_disk->disk_name);

	/* Need to check if a detach command is in progress for this
	   device*/	
	spin_lock(&devtab_lock);
	if (dev->state == DEV_IN_USE) {
	      SRBDEV_LOG_INFO(dev, 
			      "Tried to open device (%s) while a detach command is in progress", 
			      bdev->bd_disk->disk_name);
	      ret = -ENOENT;
	      goto out;
	}

        atomic_inc(&dev->users);
 out:
	spin_unlock(&devtab_lock);
	return ret;
}

/*
 * After linux kernel v3.10, this function stops returning anything
 * (becomes void). For simplicity, we currently don't support earlier kernels.
 */
static void srb_release(struct gendisk *disk, fmode_t mode)
{
	srb_device_t *dev;

	dev = disk->private_data;
	SRBDEV_LOG_INFO(dev, "Releasing device (%s)", disk->disk_name);
	spin_lock(&devtab_lock);
        atomic_dec(&dev->users);
	spin_unlock(&devtab_lock);
}

static const struct block_device_operations srb_fops =
{
	.owner   =	THIS_MODULE,
	.open	 =	srb_open,
	.release =	srb_release,
};

static int srb_init_disk(struct srb_device *dev)
{
	struct gendisk *disk = NULL;
	struct request_queue *q;
	unsigned int i;
	int ret = 0;

	SRB_LOG_INFO(srb_log, "srb_init_disk: initializing disk for device: %s", dev->name);

	/* create gendisk info */
	disk = alloc_disk(DEV_MINORS);
	if (!disk) {
		SRB_LOG_WARN(srb_log, "srb_init_disk: unable to allocate memory for disk for device: %s",
			dev->name);
		return -ENOMEM;
	}
	SRB_LOG_DEBUG(srb_log, "Creating new disk: %p", disk);

	strcpy(disk->disk_name, dev->name);
	disk->major	   = dev->major;
	disk->first_minor  = 0;
	disk->fops	   = &srb_fops;
	disk->private_data = dev;

	/* init rq */
	q = blk_init_queue(srb_rq_fn, &dev->rq_lock);
	if (!q) {
		SRB_LOG_WARN(srb_log, "srb_init_disk: unable to init block queue for device: %p, disk: %p",
			dev, disk);
		srb_free_disk(dev);
		return -ENOMEM;
	}

	blk_queue_max_hw_sectors(q, DEV_NB_PHYS_SEGS);
	q->queuedata	= dev;

	dev->disk	= disk;
	dev->q		= disk->queue = q;
	dev->nb_threads = 0;
	//blk_queue_flush(q, REQ_FLUSH | REQ_FUA);
	//blk_queue_max_phys_segments(q, DEV_NB_PHYS_SEGS);

	//TODO: Enable flush and bio (Issue #21)
	//blk_queue_flush(q, REQ_FLUSH);

	for (i = 0; i < thread_pool_size; i++) {
		//if ((ret = srb_cdmi_connect(&dev->debug, &dev->thread_cdmi_desc[i]))) {
		if ((ret = srb_cdmi_connect(&dev->debug, dev->thread_cdmi_desc[i]))) {
			SRB_LOG_ERR(srb_log, "Unable to connect to CDMI endpoint: %d",
				ret);
			srb_free_disk(dev);
			return -EIO;
		}
	}
	/* Caution: be sure to call this before spawning threads */
	ret = srb_cdmi_getsize(&dev->debug, dev->thread_cdmi_desc[0], &dev->disk_size);
	if (ret != 0) {
		SRB_LOG_ERR(srb_log, "Could not retrieve volume size.");
		srb_free_disk(dev);
		return ret;
	}

	set_capacity(disk, dev->disk_size / 512ULL);

	for (i = 0; i < thread_pool_size; i++) {
		dev->thread[i] = kthread_create(srb_thread, dev, "%s",
						dev->disk->disk_name);
		if (IS_ERR(dev->thread[i])) {
			SRB_LOG_ERR(srb_log, "Unable to create worker thread (id %d)", i);
			dev->thread[i] = NULL;
			srb_free_disk(dev);
			goto err_kthread;
		}
		wake_up_process(dev->thread[i]);
	}
	add_disk(disk);

	SRBDEV_LOG_INFO(dev, "Attached volume %s of size 0x%llx",
	                disk->disk_name, (unsigned long long)dev->disk_size);

	return 0;

err_kthread:
	for (i = 0; i < thread_pool_size; i++) {
		if (dev->thread[i] != NULL)
			kthread_stop(dev->thread[i]);
	}

	return -EIO;
}
#define device_free_slot(X) ((X)->name[0] == 0)


/* This function gets the next free slot in device tab (devtab)
** and sets its name and id.
** Note : all the remaining fields states are undefived, it is
** the caller responsability to set them.
*/
static int srb_device_new(const char *devname, srb_device_t *dev)
{
	int ret = -EINVAL;
	unsigned int i;

	SRB_LOG_INFO(srb_log, "srb_device_new: creating new device %s"
		      " with %d threads", devname, thread_pool_size);

	if (NULL == dev) {
		ret = -EINVAL;
		goto out;
	}

	if (NULL == devname || strlen(devname) >= DISK_NAME_LEN) {
		SRB_LOG_ERR(srb_log, "srb_device_new: "
			     "Invalid (or too long) device name '%s'",
			     devname == NULL ? "" : devname);
		ret = -EINVAL;
		goto out;
	}

	/* Lock table to protect against concurrent devices
	 * creation
	 */
	dev->debug.name = &dev->name[0];
	dev->debug.level = srb_log;
        atomic_set(&dev->users, 0);
	strncpy(dev->name, devname, strlen(devname));

	/* XXX: dynamic allocation of thread pool and cdmi connection pool
	 * NB: The memory allocation for the thread is an array of pointer
	 *     whereas the allocation for the cdmi connection pool is an array
	 *     of cdmi connection structure
	 */
	dev->thread_cdmi_desc = vmalloc(thread_pool_size * sizeof(struct srb_cdmi_desc_s *));
	if (dev->thread_cdmi_desc == NULL) {
		SRB_LOG_CRIT(srb_log, "srb_device_new: Unable to allocate memory for CDMI struct pointer");
		ret = -ENOMEM;
		goto err_mem;
	}
	for (i = 0; i < thread_pool_size; i++) {
	    dev->thread_cdmi_desc[i] = vmalloc(sizeof(struct srb_cdmi_desc));
		if (dev->thread_cdmi_desc[i] == NULL) {
			SRB_LOG_CRIT(srb_log, "srb_device_new: Unable to allocate memory for CDMI struct, step %d", i);
			ret = -ENOMEM;
			goto err_mem;
		}
	}
	dev->thread = vmalloc(thread_pool_size * sizeof(struct task_struct *));
	if (dev->thread == NULL) {
		SRB_LOG_CRIT(srb_log, "srb_device_new: Unable to allocate memory for kernel thread struct");
		ret = -ENOMEM;
		goto err_mem;
	}

	return 0;

err_mem:
	if (NULL != dev && NULL != dev->thread_cdmi_desc) {
		for (i = 0; i < thread_pool_size; i++) {
			if (dev->thread_cdmi_desc[i])
				vfree(dev->thread_cdmi_desc[i]);
		}
		vfree(dev->thread_cdmi_desc);
	}
out:
	return ret;
}

/* This helper marks the given device slot as empty
** CAUTION: the devab lock must be held
*/
static void __srb_device_free(srb_device_t *dev)
{
	SRB_LOG_INFO(srb_log, "__srb_device_free: freeing device: %s", dev->name);

	memset(dev->name, 0, DISK_NAME_LEN);
	dev->major = 0;
	dev->id = -1;
}

static void srb_device_free(srb_device_t *dev)
{
	unsigned int i = 0;

	SRB_LOG_INFO(srb_log, "srb_device_free: freeing device: %s", dev->name);

	__srb_device_free(dev);

	if (dev->thread_cdmi_desc) {
		for (i = 0; i < thread_pool_size; i++) {
			if (dev->thread_cdmi_desc[i])
			{
				srb_cdmi_disconnect(&dev->debug,
				                    dev->thread_cdmi_desc[i]);
				vfree(dev->thread_cdmi_desc[i]);
			}
		}
		vfree(dev->thread_cdmi_desc);
	}
	if (dev->thread)
		vfree(dev->thread);
}

static int _srb_reconstruct_url(char *url, char *name,
				 const char *baseurl, const char *basepath,
				 const char *filename)
{
	int urllen = 0;
	int namelen = 0;
	int seplen = 0;

	SRB_LOG_DEBUG(srb_log, "_srb_reconstruct_url: construction of URL with url: %s, name: %s, baseurl: %s, basepath: %p, filename: %s",
		url, name, baseurl, basepath, filename);

	urllen = strlen(baseurl);
	if (baseurl[urllen - 1] != '/')
		seplen = 1;

	if (seplen)
	{
		urllen = snprintf(url, SRB_CDMI_URL_SIZE, "%s/%s", baseurl, filename);
		namelen = snprintf(name, SRB_CDMI_URL_SIZE, "%s/%s", basepath, filename);
	}
	else
	{
		urllen = snprintf(url, SRB_CDMI_URL_SIZE, "%s%s", baseurl, filename);
		namelen = snprintf(name, SRB_CDMI_URL_SIZE, "%s%s", basepath, filename);
	}

	if (urllen >= SRB_CDMI_URL_SIZE || namelen >= SRB_CDMI_URL_SIZE)
		return -EINVAL;

	return 0;
}

static int __srb_device_detach(srb_device_t *dev)
{
	unsigned int i, users = 0;
	int ret = 0;

	SRB_LOG_DEBUG(srb_log, "detaching device (%p)", dev);

	if (!dev) {
		SRB_LOG_WARN(srb_log, "__srb_device_detach: empty device");
		return -EINVAL;
	}

	SRBDEV_LOG_DEBUG(dev, "Detaching device (%s)", dev->name);

        users = atomic_read(&dev->users);
	if (users > 0) {
		SRBDEV_LOG_ERR(dev, "Unable to remove, device still opened (#users: %d)", users);
		return -EBUSY;
	}

	if (!dev->disk) {
		SRBDEV_LOG_ERR(dev, "Device %s is not available anymore", dev->name);
		return -EINVAL;
	}

	SRBDEV_LOG_INFO(dev, "Stopping device's background processes");
	for (i = 0; i < thread_pool_size; i++) {
		if (dev->thread[i])
			kthread_stop(dev->thread[i]);
	}

	/* free disk */
	ret = srb_free_disk(dev);
	if (0 != ret) {
		SRBDEV_LOG_WARN(dev, "Failed to remove device: %d", ret);
	}

	SRB_LOG_INFO(srb_log, "Unregistering device from BLOCK Subsystem");

	/* Remove device */
	unregister_blkdev(dev->major, DEV_NAME);

	/* Mark slot as empty */
	if (NULL != dev)
		srb_device_free(dev);

	return 0;
}

static int _srb_detach_devices(void)
{
	int ret;
	int i = 0;
	int errcount = 0;
	int dev_in_use[DEV_MAX];

	SRB_LOG_INFO(srb_log, "_srb_detach_devices: detaching devices");

	/* mark all device that are not used */
	spin_lock(&devtab_lock);
	for (i = 0; i < DEV_MAX; ++i) {
		dev_in_use[i] = 0;
		if (!device_free_slot(&devtab[i])) {
			if (devtab[i].state == DEV_IN_USE) {
				dev_in_use[i] = 1;	
			} else {
				devtab[i].state = DEV_IN_USE;
				dev_in_use[i] = 2;
			}
		}
	}
	spin_unlock(&devtab_lock);

	/* detach all marked devices */
	for (i = 0; i < DEV_MAX; ++i) {
		if (2 == dev_in_use[i]) {
			ret = __srb_device_detach(&devtab[i]);
			if (ret != 0) {
				SRBDEV_LOG_ERR(&devtab[i],
				        "Cannot remove device %s for volume %s"
				        " on module unload: %i",
				        devtab[i].name,
				        devtab[i].thread_cdmi_desc ?
				                devtab[i].thread_cdmi_desc[0]->filename
				                : "NULL",
				         ret);
				errcount++;
			}
		}
	}

	/* mark all device that are not used */
	spin_lock(&devtab_lock);
	for (i = 0; i < DEV_MAX; ++i) {
		if (2 == dev_in_use[i]) {
			devtab[i].state = DEV_UNUSED;
		}
	}
	spin_unlock(&devtab_lock);

	return errcount;
}

static void _srb_server_free(srb_server_t *server)
{
	SRB_LOG_DEBUG(srb_log, "_srb_server_free: deleting server url %s (%p)", server->cdmi_desc.url, server);

	if (server) {
		server->next = NULL;
		vfree(server);
		server = NULL;
	}
}

static int _srb_server_new(srb_debug_t *dbg, const char *url, srb_server_t **server)
{
	srb_server_t	*new = NULL;
	int		ret = 0;

	SRB_LOG_DEBUG(dbg->level, "_srb_server_new: creating server with url: %s, servers: %p", url, *server);

	new = vmalloc(sizeof(struct srb_server_s));
	if (new == NULL) {
		SRB_LOG_ERR(dbg->level, "Cannot allocate memory to add a new server.");
		ret = -ENOMEM;
		goto end;
	}
	memset(new, 0, sizeof(struct srb_server_s));

	ret = srb_cdmi_init(dbg, &new->cdmi_desc, url);
	if (ret != 0) {
		SRB_LOG_ERR(dbg->level, "Could not initialize server descriptor (parse URL).");
		goto end;
	}

	if (server) {
		new->next = NULL;
		*server = new;
	}

	return 0;
end:
	_srb_server_free(new);

	return ret;
}


/*
 * XXX NOTE XXX: #13 This function picks only one server that has enough free
 * space in the URL buffer to append the filename.
 */
static int _srb_server_pick(const char *filename, struct srb_cdmi_desc *pick)
{
	char url[SRB_CDMI_URL_SIZE];
	char name[SRB_CDMI_URL_SIZE];
	int ret;
	int found = 0;
	srb_server_t *server = NULL;

	SRB_LOG_DEBUG(srb_log, "_srb_server_pick: picking server with filename: %s, with CDMI pick %p", filename, pick);

	spin_lock(&devtab_lock);
	server = servers;
	while (server != NULL) {
		SRB_LOG_INFO(srb_log, "Browsing server: %s", server->cdmi_desc.url);
		ret = _srb_reconstruct_url(url, name,
					    server->cdmi_desc.url,
					    server->cdmi_desc.filename,
					    filename);
		SRB_LOG_INFO(srb_log, "Dewb reconstruct url yielded %s, %i", url, ret);
		if (ret == 0) {
			memcpy(pick, &server->cdmi_desc, sizeof(struct srb_cdmi_desc));
			strncpy(pick->url, url, SRB_CDMI_URL_SIZE);
			strncpy(pick->filename, name, SRB_CDMI_URL_SIZE);
			SRB_LOG_INFO(srb_log, "Copied into pick: url=%s, name=%s", pick->url, pick->filename);
			found = 1;
			break ;
		}
		server = server->next;
	}
	spin_unlock(&devtab_lock);

	SRB_LOG_INFO(srb_log, "Browsed all servers");

	if (!found) {
		SRB_LOG_ERR(srb_log, "Could not match any server for filename %s", filename);
		// No such device or adress seems to match 'missing server'
		ret = -ENXIO;
		goto end;
	}

	ret = 0;
end:
	return ret;
}

/* XXX: Respect ISO C90
 *      Fix compilation warning: ISO C90 forbids mixed declarations and code
 *      Fix design: only create new server if not found in the list
 */
int srb_server_add(const char *url)
{
	int		ret = 0;
	int		found = 0;
	srb_server_t	*cur = NULL;
	srb_server_t	*last = NULL;
	srb_server_t	*new = NULL;
	srb_debug_t	debug;

	SRB_LOG_INFO(srb_log, "srb_server_add: adding server %s", url);

	debug.name = "<Server-Url-Adder>";
	debug.level = srb_log;

	if (strlen(url) >= SRB_CDMI_URL_SIZE) {
		SRB_LOG_ERR(srb_log, "Url too big: '%s'", url);
		ret = -EINVAL;
		goto err_out_dev;
	}

	ret = _srb_server_new(&debug, url, &new);
	if (ret != 0)
		goto err_out_dev;

	spin_lock(&devtab_lock);
	cur = servers;
	while (cur != NULL) {
		if (strcmp(url, cur->cdmi_desc.url) == 0) {
			found = 1;
			break;
		}
		last = cur;
		cur = cur->next;
	}
	if (found == 0) {
		if (last != NULL)
			last->next = new;
		else
			servers = new;
		new = NULL;
	}
	spin_unlock(&devtab_lock);

	if (found)
		_srb_server_free(new);

	return 0;

err_out_dev:

	return ret;
}

static int _locked_server_remove(const char *url)
{
	int		ret = 0;
	int		i;
	int		found = 0;
	srb_server_t	*cur = NULL;
	srb_server_t	*prev = NULL;

	cur = servers;
	while (cur != NULL) {
		if (strcmp(url, cur->cdmi_desc.url) == 0) {
			found = 1;
			break;
		}
		prev = cur;
		cur = cur->next;
	}

	if (found == 0) {
		SRB_LOG_ERR(srb_log, "Cannot remove server: "
			     "Url is not part of servers");
		ret = -ENOENT;
		goto end;
	}

	/* Only one server == Last server, make sure there are no devices
	 * attached anymore before removing */
	if (servers != NULL && servers->next == NULL) {
		for (i = 0; i < DEV_MAX; ++i) {
			if (!device_free_slot(&devtab[i])) {
				SRB_LOG_ERR(srb_log,
					     "Could not remove all devices; "
					     "not removing server.");
				ret = -EBUSY;
				goto end;
			}
		}
	}


	if (prev)
		prev->next = cur->next;
	else
		servers = cur->next;

	_srb_server_free(cur);

	ret = 0;

end:
	return ret;
}

int srb_server_remove(const char *url)
{
	int		ret = 0;

	SRB_LOG_INFO(srb_log, "srb_server_remove: removing server %s", url);

	if (strlen(url) >= SRB_CDMI_URL_SIZE) {
		SRB_LOG_ERR(srb_log, "Url too big: '%s'", url);
		ret = -EINVAL;
		goto end;
	}

	spin_lock(&devtab_lock);
	ret = _locked_server_remove(url);
	spin_unlock(&devtab_lock);

end:
	return ret;
}

ssize_t srb_servers_dump(char *buf, ssize_t max_size)
{
	srb_server_t	*cur = NULL;
	ssize_t		printed = 0;
	ssize_t		len = 0;
	ssize_t		ret = 0;

	SRB_LOG_INFO(srb_log, "srb_servers_dump: dumping servers: buf: %p, max_size: %ld", buf, max_size);

	spin_lock(&devtab_lock);
	cur = servers;
	while (cur) {
		if (printed != 0) {
			len = snprintf(buf + printed, max_size - printed, ",");
			if (len == -1 || len != 1) {
				SRB_LOG_ERR(srb_log, "Not enough space to print servers list in buffer.");
				ret = -ENOMEM;
				break;
			}
			printed += len;
		}

		len = snprintf(buf + printed, max_size - printed, "%s", cur->cdmi_desc.url);
		if (len == -1 || len > (max_size - printed)) {
			SRB_LOG_ERR(srb_log, "Not enough space to print servers list in buffer.");
			ret = -ENOMEM;
			break;
		}
		printed += len;

		cur = cur->next;
	}
	spin_unlock(&devtab_lock);

	len = snprintf(buf + printed, max_size - printed, "\n");
	if (len == -1 || len != 1) {
		SRB_LOG_ERR(srb_log, "Not enough space to print servers list in buffer.");
		ret = -ENOMEM;
	}
	printed += len;

	return ret < 0 ? ret : printed;
}

int srb_device_detach(const char *devname)
{
	int ret = 0;
	int i;
	int found = 0;
	struct srb_device *dev = NULL;

	SRB_LOG_INFO(srb_log, "srb_device_detach: detaching device name %s",
		      devname);

	/* get device to detach and mark it */
	spin_lock(&devtab_lock);
	for (i = 0; i < DEV_MAX; ++i) {
		if (!device_free_slot(&devtab[i])) {
			if (strcmp(devname, devtab[i].name) == 0) {
				found = 1;
				dev = &devtab[i];
				if (atomic_read(&devtab[i].users) > 0)
				        ret = -EBUSY;
				else if (devtab[i].state == DEV_IN_USE) {
					ret = -EBUSY;
				} else {
					/* mark it as ongoing operation */
					dev->state = DEV_IN_USE;
					ret = 0;
				}
				break;
			}
		}
	}
	spin_unlock(&devtab_lock);

	/* check status */
	if (1 == found && 0 != ret) {
		SRBDEV_LOG_ERR(dev, "Device %s is in use", devname);
		return ret;
	}

	/* real stuff */
	if (1 == found) {
		ret = __srb_device_detach(dev);
		if (ret != 0) {
			SRBDEV_LOG_ERR(dev, "Cannot detach device %s", devname);
		}
	} else {
	        SRB_LOG_ERR(srb_log, "Device %s was not found as attached", devname);
		return -EINVAL;
	}

	/* mark device as unsued == available */
	spin_lock(&devtab_lock);
	dev->state = DEV_UNUSED;
	spin_unlock(&devtab_lock);

	return ret;
}

/* TODO: Remove useless memory allocation (cdmi_desc)
 */
int srb_device_attach(const char *filename, const char *devname)
{
	srb_device_t *dev = NULL;
	int rc = 0;
	unsigned int i;
	int do_unregister = 0;
	struct srb_cdmi_desc *cdmi_desc = NULL;
	int found = 0;
	const char *fname;

	SRB_LOG_INFO(srb_log, "srb_device_attach: attaching "
		      "filename %s as device %s",
		      filename, devname);

	/* check if volume is already attached, otherwise use the first empty slot */
	spin_lock(&devtab_lock);
	for (i = 0; i < DEV_MAX; ++i) {
		if (!device_free_slot(&devtab[i])) {
			fname = kbasename(devtab[i].thread_cdmi_desc[0]->filename);
			if (strlen(fname) == strlen(filename) && strncmp(fname, filename, strlen(filename)) == 0) {
				found = 1;
				dev = &devtab[i];
				break;
			}
		}
	}
	if (0 == found) {
		for (i = 0; i < DEV_MAX; ++i) {
			if (device_free_slot(&devtab[i]) && 
			    (devtab[i].state != DEV_IN_USE)) {
				dev = &devtab[i];
				dev->id = i;
				dev->state = DEV_IN_USE;
				break;
			}
		}
	}
	spin_unlock(&devtab_lock);

	if (dev == NULL) {
		SRB_LOG_ERR(srb_log, "No device slot available to attach volume"
		            " %s as device %s.", filename, devname);
		rc = -ENOMEM;
		goto cleanup;
	}

	if (1 == found) {
		SRBDEV_LOG_ERR(dev, "Volume %s already attached as device %s", filename, dev->name);
		// Don't release dev.
		dev = NULL;
		rc = -EEXIST;
		goto cleanup;
	}

	SRB_LOG_INFO(srb_log, "Volume %s not attached yet, using device slot %d", filename, dev->id);

	cdmi_desc = vmalloc(sizeof(struct srb_cdmi_desc));
	if (cdmi_desc == NULL) {
		SRB_LOG_ERR(srb_log, "Unable to allocate memory for cdmi struct");
		rc = -ENOMEM;
		goto cleanup;
	}

	/* Allocate dev structure */
	rc = srb_device_new(devname, dev);
	if (rc != 0) {
		SRB_LOG_ERR(srb_log, "Unable to create new device: %i", rc);
		goto cleanup;
	} else {
		SRB_LOG_INFO(srb_log, "New device created for %s", devname);
	}

	init_waitqueue_head(&dev->waiting_wq);
	INIT_LIST_HEAD(&dev->waiting_queue);
	spin_lock_init(&dev->waiting_lock);

	/* Pick a convenient server to get srb_cdmi_desc
	 * TODO: #13 We need to manage failover by using every server
	 * NB: _srb_server_pick fills the cdmi_desc sruct
	 */
	rc = _srb_server_pick(filename, cdmi_desc);
	if (rc != 0) {
		SRB_LOG_ERR(srb_log, "Unable to get server: %i", rc);
		goto cleanup;
	}
	SRB_LOG_INFO(srb_log, "Attaching Device: Picked server "
		      "[ip=%s port=%d fullpath=%s]",
		      cdmi_desc->ip_addr, cdmi_desc->port,
		      cdmi_desc->filename);

	/* set timeout value */
	cdmi_desc->timeout.tv_sec = req_timeout;
	cdmi_desc->timeout.tv_usec = 0;

	for (i = 0; i < thread_pool_size; i++) {	
		memcpy(dev->thread_cdmi_desc[i], cdmi_desc,
		       sizeof(struct srb_cdmi_desc));
	}
	rc = register_blkdev(0, DEV_NAME);
	if (rc < 0) {
		SRB_LOG_ERR(srb_log, "Could not register_blkdev()");
		goto cleanup;
	}
	dev->major = rc;

	rc = srb_init_disk(dev);
	if (rc < 0) {
		do_unregister = 1;
		goto cleanup;
	}

	srb_sysfs_device_init(dev);

	SRBDEV_LOG_INFO(dev, "Attached device %s (id: %d) for server "
		      "[ip=%s port=%d fullpath=%s]",
		      dev->name, dev->id, cdmi_desc->ip_addr,
		      cdmi_desc->port, cdmi_desc->filename);

	/* mark device as unsued == available */
	spin_lock(&devtab_lock);
	dev->state = DEV_UNUSED;
	spin_unlock(&devtab_lock);

	// Prevent releasing device <=> Validate operation
	dev = NULL;

	rc = 0;

cleanup:
	if (do_unregister)
		//unregister_blkdev(dev->major, dev->name);
		unregister_blkdev(dev->major, DEV_NAME);
	if (NULL != dev) {
		srb_device_free(dev);
		/* mark device as unused == available */
		spin_lock(&devtab_lock);
		dev->state = DEV_UNUSED;
		spin_unlock(&devtab_lock);
	}
	if (NULL != cdmi_desc)
		vfree(cdmi_desc);

	if (rc < 0)
		SRB_LOG_ERR(srb_log, "Error adding device %s", filename);

	return rc;
}

static int __init srb_init(void)
{
	int rc;

	SRB_LOG_NOTICE(srb_log, "Initializing %s block device driver version %s", DEV_NAME, DEV_REL_VERSION);

	/* Zeroing device tab */
	memset(devtab, 0, sizeof(devtab));

	rc = srb_sysfs_init();
	if (rc) {
		SRB_LOG_ERR(srb_log, "Failed to initialize with code: %d", rc);
		return rc;
	}

	return 0;
}

static void __exit srb_cleanup(void)
{
	SRB_LOG_NOTICE(srb_log, "Cleaning up %s block device driver", DEV_NAME);

	_srb_detach_devices();

	srb_sysfs_cleanup();
}

module_init(srb_init);
module_exit(srb_cleanup);
