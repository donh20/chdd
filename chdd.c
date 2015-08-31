/*************************************************************************
	> File Name: chdd.c
	> Author: Dong Hao
	> Mail: haodong.donh@gmail.com
	> Created Time: 2015年08月12日 星期三 16时53分25秒
 ************************************************************************/

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <linux/math64.h>
//#include <asm/system.h>
#include "chdd.h"
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Dong Hao");


#define MEM_SIZE 0x1000 /*4KB*/
#define CHDD_MAJOR 250
#define CHDD_QUANTUM 0x1000 /*4KB*/
#define CHDD_QUANTUM_SET 1000
#define MEM_CLEAR 0x01
#define CHDD_NR_DEVS 3

static int chdd_quantum = CHDD_QUANTUM;
static int chdd_quantum_set = CHDD_QUANTUM_SET;
static int chdd_major = CHDD_MAJOR;
static int chdd_minor = 0;
static int chdd_nr_devs = CHDD_NR_DEVS;

/* Actually stores the data of the device */
struct chdd_qset {
    /* each of the **data holds quantum * qset memory */
    void **data;
    struct chdd_qset *next;
};

struct chdd {
    struct chdd_qset *data;     /* Pointer to first quantum set */
    int quantum;                /* the current quantum size */
    int qset;                   /* the current array size */
    unsigned long long size;    /* amount of data stored here */
    struct semaphore sem;       /* mutual exclusion semaphore */
    struct timer_list timer;
    unsigned int nreaders;
    unsigned int nwriters;
    struct cdev cdev;
    unsigned char mem[MEM_SIZE];
};

/*device structure pointer*/
struct chdd *chddp;

int chdd_release(struct inode *inode, struct file *filp)
{
    struct chdd *dev= filp->private_data;
    down(&dev->sem);

    if (filp->f_mode & FMODE_READ) {
        dev->nreaders--;
        PDEBUG("Device %d nreaders: %d.\n", DEV_NTH(dev), dev->nreaders);
    }

    if (filp->f_mode & FMODE_WRITE) {
        if (dev->nwriters > 0) {
            dev->nreaders--;
            PDEBUG("Device %d nwriters: %d.\n", DEV_NTH(dev), dev->nwriters);
        }
    }

    up(&dev->sem);
    kfree(dev);
    return 0;
}

int chdd_trim(struct chdd *dev) {
    struct chdd_qset *next, *dptr;
    int qset = dev->qset;
    int i;

    for (dptr = dev->data; dptr; dptr = next) {
        if (dptr->data) {
            for (i = 0; i < qset; i++) {
                kfree(dptr->data[i]);/*clear one quantum each time */
                dptr->data = NULL;
            }
            /* quantum * qset */
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = chdd_quantum; 
    dev->qset = chdd_quantum_set;
    dev->data = NULL;

    return 0;
}

int chdd_open(struct inode *inode, struct file *filp)
{
    /*filp->private_data = dev;*/
    struct chdd *dev;

    /*container_of returns the pointer of the upper structure 
    * struct demo_struct { 
    *   type1 member1; 
    *   type2 member2; 
    *   type3 member3; 
    *   type4 member4; 
    * };      
    * 
    * struct demo_struct *demo_p;
    * struct type3 *member3_p = get_member_pointer_from_somewhere();
    * demo_p = container_of(member3_p, struct demo_struct, member3);
    *
    * */
    dev = container_of(inode->i_cdev, struct chdd, cdev);
    filp->private_data = dev;

    /*trim the length of the device if open was write-only */
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        chdd_trim(dev);
    }
    del_timer(&dev->timer);

    if (filp->f_mode & FMODE_READ) {
        dev->nreaders++;
        PDEBUG("Device %d nreaders: %d.\n", DEV_NTH(dev), dev->nreaders);
    }

    if (filp->f_mode & FMODE_WRITE) {
        if (dev->nwriters > 0) {
            up(&dev->sem);
            PDEBUG("Device %d busy writing.\n", DEV_NTH(dev));
            return -EBUSY;
        } else {
        dev->nwriters++;
        PDEBUG("Device %d nwriters: %d.\n", DEV_NTH(dev), dev->nwriters);
        }
    }
    up(&dev->sem);
    return 0;
}

/* follow the list up to the right position */
struct chdd_qset *chdd_follow(struct chdd *dev, int item) {
    struct chdd_qset *dptr;
    int i;
    dptr = dev->data;
    for (i = 1; i <= item; i++) {
        if (!dptr) {
            goto out;
        }
        dptr = dptr -> next;
    }

out:
    return dptr;

}

ssize_t chdd_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
    unsigned long p = *ppos;
    unsigned int count = size;
    ssize_t ret = 0;
    struct chdd *dev = filp->private_data;
    /*we let this parameter points to chdd structure when we do chdd_open*/

    struct chdd_qset *dptr; /*the first linklist item*/
    int quantum = dev->quantum;
    int qset = dev->qset;
    int itemsize = quantum * qset; /* bytes in this linklist */
    int item, s_pos, q_pos, rest;

    if (p >= dev->size) {
        goto out;
    }

    if (p + count > dev->size) {
        count = dev->size - p;
    }

    item = (long)*ppos / itemsize;    // how many items(qsets) we can jump. 
    rest = (long)*ppos % itemsize;    // offset in the last item(qset). 
    s_pos = rest / quantum; // how many quantums we can jump. 
    q_pos = rest % quantum; // offset in the last quantum. 

    dptr = chdd_follow(dev, item);

    if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out;

    /* read only up to the end of this quantum */
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dev->mem + p, count)) {
        ret = -EFAULT;
        goto out;
    } else {
        *ppos += count;
        ret = count;
        printk(KERN_ALERT "read %u bytes(s) from %lu", count, p);
    }
out:
    return ret;
}

ssize_t chdd_write(struct file *filp, const char __user *buf, size_t size, loff_t *ppos)
{
    struct chdd *dev = filp->private_data;
    struct chdd_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    loff_t p = *ppos;
    size_t count = size;
    ssize_t ret = -ENOMEM;
    
    int item, s_pos, q_pos, rest;

    if (p > dev->size) {
        goto out;
    }

    item = (long)p / itemsize;
    rest = (long)p % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = chdd_follow(dev, item);

    if (!dptr)
        goto out;
    
    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;

        memset(dptr->data, 0, qset * sizeof(char *));
    }
    
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;

        memset(dptr->data[s_pos], 0, quantum);
    }

    /* write only up to the end of this quantum */
    if (count > quantum - q_pos) {
        count = quantum - q_pos;
    }

    if (copy_from_user(dptr->data[s_pos]+q_pos, buf, count)) {
        ret = -EFAULT;
        goto out;
    }
    
    *ppos += count;
    ret = count;

    if (dev->size < *ppos)
        dev->size = *ppos;

out:
    return ret;
}

/*
int chdd_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
              unsigned long arg)
{
    struct chdd *dev = filp->private_data;

    switch (cmd) {
        case MEM_CLEAR:
            memset(dev->mem, 0, MEM_SIZE);
            printk(KERN_ALERT "chdd memory is set to zero");
            break;

        default:
            return -EINVAL;
    }
    return 0; 
}
*/
loff_t chdd_llseek(struct chdd *dev, loff_t *ppos, int whence)
{
    loff_t ret = 0;
    return ret;
}
const struct file_operations chdd_fops = {
    .owner = THIS_MODULE,
    .read = chdd_read,
    .write = chdd_write,
//    .ioctl = chdd_ioctl,
    .open = chdd_open,
    .release = chdd_release,
//    .llseek = chdd_llseek,
};

static void chdd_exit(void)
{
    dev_t devno = MKDEV(chdd_major, 0);
    int i = 0;

    printk(KERN_INFO "Goodbye cruel world!\n");

    if (chddp) {
        for(i = 0; i<chdd_nr_devs; i++) {
            cdev_del(&chddp[i].cdev);
            PDEBUG("delete device %d", i+1);
            kfree(chddp[i].data);
        }
        
        kfree(chddp);
    }
    unregister_chrdev_region(devno, chdd_nr_devs);
}

static void chdd_setup_cdev(struct chdd *dev, int index) 
{
    int err, devno = MKDEV(chdd_major, chdd_minor + index);

    cdev_init(&dev->cdev, &chdd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &chdd_fops;// I don't know whether I need it or not.
    err = cdev_add(&dev->cdev, devno, 1);

    if (err) {
        printk(KERN_ALERT "Error %d adding chdd %d\n", err, index);
        printk(KERN_ALERT "Failed in setup_cdev!\n");
 //       chdd_exit();
    }
}

static int chdd_init(void)
{
    int result;
    int i;
    dev_t devno = MKDEV(chdd_major, 0);

    /*apply for the device number*/
    if (chdd_major) {
        result = register_chrdev_region(devno, chdd_nr_devs, "chdd");
    } else {
        result = alloc_chrdev_region(&devno, chdd_minor, chdd_nr_devs, "chdd");
        chdd_major = MKDEV(devno, 0);
    }

    if (result < 0) {
        printk(KERN_ALERT "chdd cannot get major %d\n, minor count %d"
                            , chdd_major, chdd_nr_devs);
        return result;
    }
    printk(KERN_ALERT "chdd get major %d\n", chdd_major);

    /*apply memory for device struct*/
    chddp = kmalloc(chdd_nr_devs * sizeof(struct chdd), GFP_KERNEL);
    if (!chddp) {
        PDEBUG("Allocate device error!");
        result = -ENOMEM;
        goto fail;
    } 

    memset(chddp, 0, chdd_nr_devs * sizeof(struct chdd));

    for(i = 0; i < chdd_nr_devs; i++) {
        chdd_setup_cdev(&chddp[i], i);
        PDEBUG("Initialized device %d, size: %d.", i+1, chddp[i].size);
    }
    
    printk(KERN_ALERT "Hello, world!\n");
    return 0;

fail:
    chdd_exit();
    //unregister_chrdev_region(devno, 1);
    return result;
}

module_init(chdd_init);
module_exit(chdd_exit);
