/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
#include <linux/semaphore.h>
#else
#include <asm/semaphore.h>
#endif
#include <linux/aio.h>

#include <linux/list.h>
#include <linux/uio.h>
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/bitops.h>

#include "include/linux_usb_com.h"
#include "include/linux_usb_dev.h"
#include "include/linux_usb_ioctl.h"
#include "include/linux_usb_extra.h"
#include "include/usb_com.h"

//#define BT_USB_DEBUG
#define BT_USB_NAME      "bt_usb"

/* HCI H4 message type definitions */
#define H4_TYPE_COMMAND         1
#define H4_TYPE_ACL_DATA        2
#define H4_TYPE_SCO_DATA        3
#define H4_TYPE_EVENT           4


static struct class *bt_usb_class;


void *pmalloc(size_t size)
{
    return kmalloc(size, GFP_ATOMIC | GFP_DMA);
}

void *zpmalloc(size_t size)
{
    unsigned char *buf;

    buf = pmalloc(size);
    if (buf != NULL)
    {
    	memset(buf, 0x00, size);
    	return buf;
    }
    return NULL;
}

void pfree(void *ptr)
{
    kfree(ptr);
}

typedef struct read_q_elm_t
{
    uint8_t channel;
    MessageStructure *msg;
    struct read_q_elm_t *next;
} read_q_elm_t;

typedef struct
{
    wait_queue_head_t read_q;
    read_q_elm_t *first;
    read_q_elm_t *last;
    uint16_t count;
    struct semaphore access_sem;
    uint8_t minor;
    struct cdev cdev;
    bool in_use;
    
} bt_usb_instance_t;


struct USBdev {
	struct kobject kobj;
	struct module *owner;
	const struct file_operations *ops;
	struct list_head list;
	dev_t dev;
	unsigned int count;
};

#define BT_USB_MAJOR 0

#ifdef BT_USB_DEBUG
static int callcount;
#endif

static int bt_usb_major = BT_USB_MAJOR;
static bt_usb_instance_t *instances = NULL;
static struct semaphore inst_sem;
static int inst_id = 0;

#ifdef BT_USB_DEBUG
void dumpData(uint8_t *data, uint16_t len)
{
    char buffer[16 * 5];
    int idx;
    int i;

    idx = 0;
	memset(buffer,0x0,sizeof(buffer));

	printk("\n");
    for(i=0;i<len;i++)
    {
		printk("%2x ", data[i]);

    }
	printk("\n");
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
static ssize_t bt_usb_readv(struct kiocb *kiocbp, const struct iovec *io,
                            unsigned long count, loff_t f_pos)
#else
static ssize_t bt_usb_readv(struct file *filp, const struct iovec *io ,
                            unsigned long count, loff_t *f_pos)
#endif
{
    bt_usb_instance_t *inst;
    unsigned int i;
    ssize_t len;
    ssize_t no;
    read_q_elm_t *ptr;
    int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
    inst = (bt_usb_instance_t *)kiocbp->ki_filp->private_data;
#else
    inst = (bt_usb_instance_t *)filp->private_data;
#endif
    if (inst == NULL)
    {
        return -ENODEV;
    }

    
    for (i = 0, len = 0; i < count; i++)
    {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
        if (!access_ok(VERIFY_WRITE, io[i].iov_base, io[i].iov_len))
#else
        if (verify_area(VERIFY_WRITE, io[i].iov_base, io[i].iov_len))
#endif
        {
            return -EFAULT;
        }

        len += io[i].iov_len;
    }

    /* Grab lock and wait for data in queue */
    if (down_interruptible(&inst->access_sem))
    {
        return(-ERESTARTSYS);
    }

    while (inst->first == NULL)
    {
#ifdef BT_USB_DEBUG
        printk("<1>Read queue empty,  count = %d\n",
               inst->count);
#endif
        up(&inst->access_sem);

        if (wait_event_interruptible(inst->read_q,
                                     (inst->first != NULL)))
        {
            return(-ERESTARTSYS);
        }

        if (down_interruptible(&inst->access_sem))
        {
            return(-ERESTARTSYS);
        }
    }

    switch (count)
    {
        case 1: /* Getting size of next message */
            ptr = inst->first;

            no = min(io[0].iov_len,(size_t) sizeof(ptr->msg->buflen));

           ret =  copy_to_user(io[0].iov_base, &ptr->msg->buflen, no);

           if(ret != 0)
           {
               printk(KERN_ERR "BT_USB: copy_to_user Failed !!!\n");
           }
        break;

        case 2: /* Getting actual message (pop off queue) */
            ptr = inst->first;

            inst->first = ptr->next;

            if (inst->last == ptr)
            {
                inst->last = NULL;
            }
            inst->count--;

            no = copy_to_user(io[0].iov_base, &ptr->channel,
                min(io[0].iov_len, (size_t)sizeof(ptr->channel)));
            if (no == 0)
            {
                no = copy_to_user(io[1].iov_base, ptr->msg->buf,
                    min(io[1].iov_len, (size_t)(ptr->msg->buflen)));
            }
            if (no == 0)
            {
#ifdef BT_USB_DEBUG
                printk("Channel=%d\n", ptr->channel);
                //dumpData((uint8_t*)ptr->msg->buf, ptr->msg->buflen);
#endif

                no = min(io[0].iov_len, (size_t)sizeof(ptr->channel)) +
                    min(io[1].iov_len, (size_t)(ptr->msg->buflen));
            }

            pfree(ptr->msg->buf);
            pfree(ptr->msg);
            pfree(ptr);

        break;
        default:
            no = -EINVAL;
        break;
    }

    up(&inst->access_sem);

#ifdef BT_USB_DEBUG
    printk("<1>BT_USB: readv: no = %d\n", no);
#endif
    return (no);
}

uint16_t find_event_opcode(char *buf,int len)
{
	struct complete_event cmp_evt;
	struct status_event   status_evt;
	if(len >= sizeof(struct complete_event))
	{
		if(buf[0] == HCI_COMMAND_COMPLETE_EVT)
		{
			memcpy(&cmp_evt,buf,sizeof(struct complete_event));
			return cmp_evt.opcode;
		}
		if(buf[0] == HCI_COMMAND_STATUS_EVT)
		{
			memcpy(&status_evt,buf,sizeof(struct complete_event));
			return status_evt.opcode;
		}
	}
	return 0;

}

static ssize_t bt_usb_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
    bt_usb_instance_t *inst;
    ssize_t n = -1;
    int ret = -1;
    read_q_elm_t *ptr = NULL;
    csr_dev_t *dv;
    int minor;
    unsigned char channel = 0;
#ifdef NAPLES_HCI_TIMER
	uint16_t opcode = 0;
	int buflen = 0;
#endif

    inst = (bt_usb_instance_t *)file->private_data;
    if (inst == NULL){
        printk("BT_USB: read : error, file->private_data is NULL\n");
        return -ENODEV;
    }
	minor = inst->minor;

#ifdef BT_USB_DEBUG
    printk("BT_USB: read: try to read %d data\n", size);
#endif
	dv = devLookup(minor);
	if(dv == NULL)
	{
		printk("%s: Device not initialized\n",__func__);
		return false;
	}
	up(&dv->devlock);

    /* Grab lock and wait for data in queue */
    if (down_interruptible(&inst->access_sem))
    {
		printk("%s : error, can not down_interruptible access_sem\n",__func__);
        return(-ERESTARTSYS);
    }

    while (inst->first == NULL)
    {
#ifdef BT_USB_DEBUG
        printk("BT_USB: read: Read queue empty,  count = %d\n", inst->count);
#endif
        up(&inst->access_sem);

        if (wait_event_interruptible(inst->read_q,
                                     (inst->first != NULL)))
        {
			printk("%s : error, wait_event_interruptible read_q\n",__func__);
            return(-ERESTARTSYS);
        }

        if (down_interruptible(&inst->access_sem))
        {
			printk("%s : error, down_interruptible access_sem\n",__func__);
            return(-ERESTARTSYS);
        }
    }

    if (size > 1)
    {
        ptr = inst->first;

        inst->first = ptr->next;

        if (inst->last == ptr)
        {
            inst->last = NULL;
        }
        inst->count--;

#ifdef BT_USB_DEBUG
        printk("BT_USB: read: channel %d\n", ptr->channel);
#endif
        switch(ptr->channel)
        {
            case BCSP_CHANNEL_HCI:
                channel = H4_TYPE_EVENT;
#ifdef NAPLES_HCI_TIMER
				buflen = ptr->msg->buflen;
				opcode = find_event_opcode(ptr->msg->buf,ptr->msg->buflen);
				spin_lock(&dv->hcicmd_lock);
#ifdef NAPLES_HCI_TIMER_DEBUG
				printk("%s:event opcode 0x%x, cmd opcode 0x%x\n",
						__func__,opcode,dv->hcicmd.opcode);
#endif
				if(dv->hcicmd.in_use == true && dv->hcicmd.opcode != 0x0 && opcode == dv->hcicmd.opcode)
				{
				//printk("%s: del_timer\n",__func__);

					del_timer_sync(&dv->hcicmd.timer);
					dv->hcicmd.in_use = false;
					dv->hcicmd.opcode = 0x0;
					dv->hcicmd.count = 0x0;
					dv->hcicmd.payload_size = 0x0;
#ifdef NAPLES_HCI_TIMER_DEBUG
					dumpData(ptr->msg->buf,ptr->msg->buflen);
#endif
				}
				spin_unlock(&dv->hcicmd_lock);
#endif //NAPLES_HCI_TIMER

                break;
            case BCSP_CHANNEL_ACL:
                channel = H4_TYPE_ACL_DATA;
                break;
            default:
                printk("BT_USB: read: channel %d not supported\n", ptr->channel);
                pfree(ptr->msg->buf);
                pfree(ptr->msg);
                pfree(ptr);
                ptr = NULL;
                return -ENODEV;
        }
        ret = copy_to_user(buf, &channel, 1);
        if (ret == 0)
        {
            ret = copy_to_user(buf + 1, ptr->msg->buf,
                min(size -1, (size_t)(ptr->msg->buflen)));
        }
        n = ret == 0 ? min(size - 1, (size_t)(ptr->msg->buflen)) + 1 : -1;

        pfree(ptr->msg->buf);
        pfree(ptr->msg);
        pfree(ptr);
        ptr = NULL;
    }

    up(&inst->access_sem);

#ifdef BT_USB_DEBUG
    printk("BT_USB: read: n = %d\n", n);
#endif
    return n;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
static ssize_t bt_usb_writev(struct kiocb *kiocbp, const struct iovec *io,
                             unsigned long count,loff_t f_pos)
#else
static ssize_t bt_usb_writev(struct file *filp, const struct iovec *io,
                             unsigned long count,loff_t *f_pos)
#endif
{
    bt_usb_instance_t *inst;
    int i;
    uint8_t channel;
    ssize_t len=0;
    unsigned char *buf;
    uint16_t length;
    int minor;
    

#ifdef BT_USB_DEBUG
    printk(KERN_ALERT "BT_USB: writev, count = %ld\n",count);
#endif
    down(&inst_sem);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
    inst = (bt_usb_instance_t *)kiocbp->ki_filp->private_data;
#else
    inst = (bt_usb_instance_t *)filp->private_data;
#endif
    if ((inst != NULL) && (inst == &instances[inst->minor]))
    {
    /*inst is relevant. remember the minor number*/
        minor = inst->minor;    
    }
    else
    {
        /*This inst has been freed up. return error*/
        up(&inst_sem);
        return -ENODEV;
    }    
    up(&inst_sem);

    for (i = 0, len = 0; i < count; i++)
    {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
        if (!access_ok(VERIFY_READ, io[i].iov_base, io[i].iov_len))
#else
        if (verify_area(VERIFY_READ, io[i].iov_base, io[i].iov_len))
#endif
        {
            return -EFAULT;
        }

        len+=io[i].iov_len;
    }

    /* Things can go wrong when getting data from userspace */
    i = copy_from_user(&channel,io[0].iov_base,io[0].iov_len);
    if(i == 0)
    {
        length = io[1].iov_len;
        buf = (unsigned char*) pmalloc(io[1].iov_len);
        if (buf == NULL)
        {
          return -ENOMEM;
        }
	
        i = copy_from_user(buf,io[1].iov_base,io[1].iov_len);

        if(i == 0)
        {
#ifdef BT_USB_DEBUG
            printk("Channel=%d\n",channel);
            //dumpData(buf,length);
#endif
            UsbDev_Tx(minor, channel, buf, length);
            i = len;
        }
    }

    return i;
}

static ssize_t bt_usb_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
    unsigned char channel = 0;
    bt_usb_instance_t *inst;
    unsigned char *payload = NULL;
    int payload_size = 0;
    int minor;
    int ret = -1;
    csr_dev_t *dv;

#ifdef BT_USB_DEBUG
    printk("BT_USB: write, count = %d\n",count);
#endif
    down(&inst_sem);

    inst = (bt_usb_instance_t  *)file->private_data;
    if ((inst != NULL) && (inst == &instances[inst->minor]))
    {
    /*inst is relevant. remember the minor number*/
        minor = inst->minor;
    }
    else
    {
        /*This inst has been freed up. return error*/
        printk("BT_USB: read : error, inst has been freed up\n");
        up(&inst_sem);
        return -ENODEV;
    }
    up(&inst_sem);
    dv = devLookup(minor);
    if(dv == NULL)
   {
		printk("%s: Device not initialized\n",__func__);
		return false;
   }
	up(&dv->devlock);

    if (count > 1)
    {
        unsigned char value;
        ret = copy_from_user(&value, buf, 1);

        if (ret == 0)
        {
            switch(value)
            {
                case H4_TYPE_COMMAND:
                    channel = BCSP_CHANNEL_HCI;
                    break;
                case H4_TYPE_ACL_DATA:
                    channel = BCSP_CHANNEL_ACL;
                    break;
                default:
                    printk("BT_USB: read: channel not supported\n");
                    return -ENODEV;
            }

            payload_size = count - 1;
            payload = (unsigned char *)pmalloc(payload_size);
            if (payload == NULL)
            {
                return -ENOMEM;
            }

            ret = copy_from_user(payload, buf + 1, payload_size);
            if (ret == 0)
            {
#ifdef BT_USB_DEBUG
                printk("channel = %d, data dump: \n", channel);
                dumpData(payload,payload_size);
#endif
#ifdef NAPLES_HCI_TIMER
				if(channel == BCSP_CHANNEL_HCI)
				{
					spin_lock(&dv->hcicmd_lock);
					if (dv->hcicmd.in_use == false)
					{
						dv->hcicmd.timer.expires = jiffies + msecs_to_jiffies(BTUSB_EVENT_TIMEOUT);
						dv->hcicmd.in_use = true;
						dv->hcicmd.count = 0x0;
						dv->hcicmd.opcode = buf[1] + (buf[2] << 8);
						memcpy(dv->hcicmd.payload,payload,payload_size);
						dv->hcicmd.payload_size = payload_size;
						add_timer(&dv->hcicmd.timer);
#ifdef NAPLES_HCI_TIMER_DEBUG
						dumpData(payload,payload_size);
						printk("%s: add timer for opcode 0x%x\n",__func__,dv->hcicmd.opcode);
#endif
					}
					spin_unlock(&dv->hcicmd_lock);
				}
#endif //NAPLES_HCI_TIMER

                ret = UsbDev_Tx(minor, channel, payload, payload_size);
            }
            else
            {
                printk("BT_USB: write, copy from user failed\n");
                pfree(payload);
                payload = NULL;
            }
        }
    }
    else
      printk("BT_USB: write, nothing to write\n");


#ifdef BT_USB_DEBUG
    printk("BT_USB: write: %d\n", ret == 0 ? count : -1);
#endif

    return ret == 0 ? count : -1;
}
static int bt_usb_open(struct inode *inode, struct file *filp)
{
    bt_usb_instance_t *inst;
    csr_dev_t *dv;
    int minor;    
    minor = MINOR(inode->i_rdev);

#ifdef BT_USB_DEBUG
    printk(KERN_ALERT "bt_usb%u: Open called count %d\n", minor,callcount++);
#endif

    /* Is device minor within range? */
    if (minor >= BT_USB_COUNT)
    {
        printk("bt_usb: count exceeded\n");
        return -ENXIO;
    }
    
    if(!devExist(minor))
    {
        printk("bt_usb: no device probed yet\n");
        return -ENXIO;
    }
    dv = devLookup(minor);
    if(dv == NULL)
	{
		printk("%s: Device not initialized\n",__func__);
		return false;
	}
	if(!test_bit(LISTEN_STARTED, &dv->flags))
	{
		startListen(dv);
	}
    
	up(&dv->devlock);
    down(&inst_sem);
 #if 0   
    if (&instances[minor] != NULL)
    {
        up(&inst_sem);
        printk("bt_usb%u: still busy %d",minor, callcount);
        return -EBUSY;
    }
#endif
   if(&instances[minor] == NULL)
   	{
   	up(&inst_sem);
        printk("bt_usb%u: is null",minor);
        return -EBUSY;
   	}
    inst = &instances[minor];// = zpmalloc(sizeof(*inst));
    up(&inst_sem);
    if (inst == NULL)
    {
      printk("bt_usb: no instance yet");
    	return -ENOMEM;
    }
    inst->minor = minor;
    inst_id = minor;
    inst->first = NULL;
    inst->last = NULL;
    inst->count = 0;
    inst->in_use = true;
    init_waitqueue_head(&inst->read_q);
    sema_init(&inst->access_sem, 1);

    filp->private_data = inst;
     printk("bt_usb%u: open complete\n",minor);
#ifdef CONFIG_USB_SUSPEND
	/*UsbDev_PmDisable(minor);*/
#endif

    return 0;
}

void bt_usb_cleanup(csr_dev_t *dv)
{
    bt_usb_instance_t *inst;
    read_q_elm_t *ptr;
    read_q_elm_t *next;
	if(!test_bit(LISTEN_STARTED, &dv->flags))
	{
		printk("%s: Device not opened\n",__func__);
		return ;
	}

	if(instances == NULL)
	{
		printk(KERN_ALERT "%s: inst is NULL\n", __func__);
		return;
	}
	inst = &instances[inst_id];

    down(&inst->access_sem);
    ptr = inst->first;
    while(ptr)
    {
        next = ptr->next;

        pfree(ptr->msg->buf);
        pfree(ptr->msg);
        pfree(ptr);

        ptr = next;
    }
    printk("%s:wake up interrupt on release %p %p\n",__func__,&inst, &(inst->read_q));
    inst->first = NULL;
    inst->last = NULL;
    inst->count = 0;   
    inst->in_use = false;
    wake_up_interruptible_all(&inst->read_q);

    up(&inst->access_sem);

}

static int bt_usb_release(struct inode *inode, struct file *filp)
{
    csr_dev_t *dv;

    dv = devLookup(inst_id);
    if(dv == NULL)
	{
		printk("%s: Device not initialized\n",__func__);
		return false;
	}

	if(!test_bit(LISTEN_STARTED, &dv->flags))
	{
		printk("%s: Device not opened\n",__func__);
		up(&dv->devlock);
		return false;
	}
	bt_usb_cleanup(dv);
    usbCleanUrbs(dv);
    QueueFree(dv);
	up(&dv->devlock);

    return 0;
}

static unsigned int bt_usb_poll(struct file *filp,poll_table *wait)
{
    bt_usb_instance_t *inst;
    inst =  (bt_usb_instance_t *)filp->private_data;
    poll_wait(filp,&(inst->read_q) ,wait);
    return (inst->first?(POLLIN | POLLRDNORM):0);
}
                        
#if 0
static int bt_usb_ioctl(struct inode *inode, struct file *filp,
                        unsigned int cmd, unsigned long arg)

{
    int ret;
	bt_usb_instance_t *inst;
	inst = (bt_usb_instance_t *)filp->private_data;

#ifdef BT_USB_DEBUG
    printk(KERN_ALERT "BT_USB: IOCTL called\n");
#endif

    /* Check command */
    if((_IOC_TYPE(cmd) != BT_USB_IOC_MAGIC) ||
       (_IOC_NR(cmd) > BT_USB_IOC_MAX_NO))
    {
        return -ENOTTY;
    }

    /* Handle command */
    ret = -ENOTTY;
    switch(cmd)
    {
#ifdef CSR_BR_USB_USE_DFU_INTF
        case BT_USB_IOC_ENTER_DFU:
            {
                ret = UsbDrv_EnterDFU();
				break;
            }
#endif
        case BT_USB_IOC_RESET:
            {
                ret = 0;
                UsbDev_Reset(inst->minor);
				break;
            }

#ifdef CONFIG_USB_SUSPEND
		case BT_USB_IOC_ENABLE_SUSPEND:
			{
				ret = 0;
				UsbDev_PmEnable(inst->minor);
				break;
			}

		case BT_USB_IOC_DISABLE_SUSPEND:
			{
				ret = 0;
				UsbDev_PmDisable(inst->minor);
				break;
			}
#endif

        default:
            {
                printk(KERN_ALERT "BT_USB: Unknown IOCTL (0x%x)\n", cmd);
                break;
            }
    }

    return ret;
}

#endif
static struct file_operations bt_usb_fops =
{
    owner     : THIS_MODULE,
    //ioctl     : bt_usb_ioctl,
    open      : bt_usb_open,
    release   : bt_usb_release,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
    aio_read  : bt_usb_readv,
    aio_write : bt_usb_writev,
    poll      : bt_usb_poll,
#else
    writev    : bt_usb_writev,
    readv     : bt_usb_readv,
#endif
    write     : bt_usb_write,
    read      : bt_usb_read,
};

void hcCom_ReceiveMsg(void *msg, uint8_t bcspChannel, uint8_t rel)
{
    bt_usb_instance_t *inst;
    read_q_elm_t *p;

    down(&inst_sem);
    inst = &instances[rel];
    
    if (inst == NULL || (inst && inst->in_use == false))
    {
        /* No takers, just toss it. */
        up(&inst_sem);

        pfree(((MessageStructure *)msg)->buf);
        pfree(msg);

        return;
    }

    p = pmalloc(sizeof(read_q_elm_t));
    if (!p)
    {
        up(&inst_sem);
        pfree(((MessageStructure *)msg)->buf);
        pfree(msg);
        return;
    }
    p->channel = bcspChannel;
    p->msg = msg;

#ifdef BT_USB_DEBUG
    printk(KERN_ALERT "bt_usb%u: Received %i bytes for channel %i\n",
           rel, p->msg->buflen, p->channel);
    //dumpData((uint8_t*)p->msg->buf, p->msg->buflen);
#endif

    p->next = NULL;

    if (down_interruptible(&inst->access_sem))
    {
        pfree(p->msg->buf);
        pfree(p->msg);
        pfree(p);
    }
    else
    {
        if (inst->first == NULL)
        {
            inst->count++;
            inst->first = p;
            inst->last = p;
        }
        else
        {
            inst->count++;
            inst->last->next = p;
            inst->last = p;
        }
        wake_up_interruptible(&inst->read_q);

        up(&inst->access_sem);
    }

    up(&inst_sem);
}


static int bt_usb_construct_device(bt_usb_instance_t *dev, int minor, struct class *class)
{
    int err = 0;
    dev_t devno = MKDEV(bt_usb_major, minor);
    struct device *device = NULL;
    
    BUG_ON(dev == NULL || class == NULL);

    /* Memory is to be allocated when the device is opened the first time */    
    cdev_init(&(dev->cdev), &bt_usb_fops);
    dev->cdev.owner = THIS_MODULE;
    
     err = cdev_add(&dev->cdev, devno, 1);
     if (err)
     {
		printk(KERN_WARNING "[target] Error %d while trying to add %s%d",
			err, BT_USB_NAME, minor);
        return err;
     }

    device = device_create(class, NULL, /* no parent device */ 
                                       devno, NULL, /* no additional data */
                                       BT_USB_NAME "%d", minor);

    if (IS_ERR(device)) {
        err = PTR_ERR(device);
        printk(KERN_WARNING "[target] Error %d while trying to create %s%d",
			err, BT_USB_NAME, minor);
        cdev_del(&dev->cdev);
        return err;
    }
	return 0;
}


static int __init bt_usb_init(void)
{
    int result;
    int err;
    int i;
    dev_t dev = 0;


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    printk(KERN_ERR "BT_USB: Driver supports only kernel version >=(2,6,0)\n");
    return -EPERM;
#endif
#ifdef CSR_BR_USB_USE_DFU_INTF 
    printk(KERN_ERR "BT_USB: Feature CSR_BR_USB_USE_DFU_INTF not supported\n");
    return -EPERM;
#endif
#ifdef CSR_BR_USB_USE_SCO_INTF
    printk(KERN_ERR "BT_USB: Feature CSR_BR_USB_USE_SCO_INT not supported\n");
    return -EPERM;
#endif

    init_MUTEX_LOCKED(&inst_sem);
    //memset(instances, 0, sizeof(*instances));

    result = UsbDrv_Start(NULL);
    if (result != true)
    {
        printk(KERN_ERR "BT_USB: USB initialization failed, exiting.\n");
        return result;
    }

    err = alloc_chrdev_region(&dev, 0, BT_USB_COUNT, BT_USB_NAME);
    if(err < 0) {
     	  printk("BT_USB: alloc_chrdev_region failed");
    	  return err;
    }

    bt_usb_major = MAJOR(dev);
    printk("BT_USB: major %d\n",bt_usb_major);
    if(bt_usb_major < 0) {
        printk(KERN_ERR "BT_USB: can't get major %d\n", bt_usb_major);
        UsbDrv_Stop();
        return bt_usb_major;
    }

    /*  udev Initializations */
    bt_usb_class = class_create(THIS_MODULE, BT_USB_NAME);
    if (IS_ERR(bt_usb_class)) {
        printk(KERN_ERR "Failed to create BT USB class\n");
        /* Release device numbers */
        //unregister_chrdev(bt_usb_major,"bt_usb");
        UsbDrv_Stop();
        bt_usb_major = 0;
        return -EINVAL;
    }

    /* Allocate the array of devices */
   instances =  ( bt_usb_instance_t *)kzalloc(
    		                (size_t)BT_USB_COUNT * sizeof(bt_usb_instance_t),
    		                GFP_KERNEL);
   if (instances == NULL) {
    		err = -ENOMEM;
    		return err;
   }

   /* Construct devices */
   for (i = 0; i < BT_USB_COUNT; ++i) {
	err = bt_usb_construct_device(&instances[i], i, bt_usb_class);
       if (err) {
            unregister_chrdev_region(MKDEV(bt_usb_major, 0), BT_USB_COUNT);            
            class_destroy(bt_usb_class);
            UsbDrv_Stop();
            bt_usb_major = 0;
            return -EINVAL;
       }
   }
   up(&inst_sem);
#if 0   
    /* Register the driver with the character subsystem */
	bt_usb_major = register_chrdev(0, BT_USB_NAME, &bt_usb_fops);

    if(bt_usb_major < 0) {
        printk(KERN_ERR "BT_USB: can't get major %d\n", bt_usb_major);
        UsbDrv_Stop();
        return bt_usb_major;
    }
    /*  udev Initializations */
    bt_usb_class = class_create(THIS_MODULE, BT_USB_NAME);
    if (IS_ERR(bt_usb_class)) {
        printk(KERN_ERR "Failed to create BT USB class\n");
        /* Release device numbers */
        unregister_chrdev(bt_usb_major,"bt_usb");
        UsbDrv_Stop();
        bt_usb_major = 0;
        return -EINVAL;
    }

    csrhid_chrdev_device = device_create(bt_usb_class, NULL,
     									MKDEV(bt_usb_major, 0), 
     									NULL, BT_USB_NAME);

    if (IS_ERR(csrhid_chrdev_device)) {
        printk(KERN_ERR "Failed to create BT USB driver\n");
        /* Release device numbers */
        unregister_chrdev(bt_usb_major,"bt_usb");
        class_destroy(bt_usb_class);
        UsbDrv_Stop();
        bt_usb_major = 0;
        return -EINVAL;
    }

    up(&inst_sem);
#endif

    return (0);
}

//#ifdef CONFIG_MODULE_UNLOAD
static void __exit bt_usb_exit(void)
{
    device_destroy(bt_usb_class, MKDEV(bt_usb_major, 0));
    class_destroy(bt_usb_class);
    unregister_chrdev(bt_usb_major,"bt_usb");
    UsbDrv_Stop();

    printk("BlueCore transport stopped.\n");
}
module_exit(bt_usb_exit);
//#endif

module_init(bt_usb_init);
MODULE_LICENSE("GPL v2");
