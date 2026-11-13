#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

#include "ice_compat.h"
#include "ice_ioctl.h"
#include "ice_device.h"
#include "ice_mem.h"
#include "ice_sym.h"
#include "ice_touch.h"

#define ICE_NAME_LEN 8

extern long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

static int    major_number;
static struct class*  ice_class  = NULL;
static struct device* ice_device = NULL;
static struct cdev   ice_cdev;
static dev_t         ice_dev;
static char          ice_name[ICE_NAME_LEN + 1];

static int ice_open(struct inode *inodep, struct file *filep)
{
    ICE_FILE_CTX *ctx = kzalloc(sizeof(ICE_FILE_CTX), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->authorized = false;
    filep->private_data = ctx;

    pr_info("[IceDriver] fd %p opened\n", filep);
    return 0;
}

static int ice_release(struct inode *inodep, struct file *filep)
{
    ICE_FILE_CTX *ctx = (ICE_FILE_CTX *)filep->private_data;
    if (ctx) {
        kfree(ctx);
        filep->private_data = NULL;
    }
    pr_info("[IceDriver] fd %p closed\n", filep);
    return 0;
}

static ssize_t ice_read(struct file *filep, char __user *buf, size_t len, loff_t *offset)
{
    return 0;
}

static ssize_t ice_write(struct file *filep, const char __user *buf, size_t len, loff_t *offset)
{
    return len;
}

static long ice_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    return ice_ioctl(filep, cmd, arg);
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = ice_open,
    .release        = ice_release,
    .read           = ice_read,
    .write          = ice_write,
    .unlocked_ioctl = ice_unlocked_ioctl,
};

static void ice_make_name(void)
{
    static const char tbl[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned long seed = (unsigned long)jiffies ^ (unsigned long)&ice_cdev;
    int i;

    for (i = 0; i < ICE_NAME_LEN; i++) {
        seed = seed * 1103515245UL + 12345UL;
        ice_name[i] = tbl[(seed >> 8) % 36];
    }
    ice_name[ICE_NAME_LEN] = '\0';
}

static int __init ice_init(void)
{
    ice_make_name();

    if (ice_sym_init() < 0) {
        pr_err("[IceDriver] ice_sym_init failed\n");
        return -1;
    }

    if (ice_mem_init() < 0) {
        pr_err("[IceDriver] ice_mem_init failed\n");
        ice_sym_exit();
        return -1;
    }

    if (ice_touch_init() < 0)
        pr_warn("[IceDriver] touch init failed, touch disabled\n");

    if (alloc_chrdev_region(&ice_dev, 0, 1, ice_name) < 0) {
        pr_err("[IceDriver] failed to allocate device number\n");
        ice_touch_exit();
        ice_mem_exit();
        ice_sym_exit();
        return -1;
    }
    major_number = MAJOR(ice_dev);

    cdev_init(&ice_cdev, &fops);
    if (cdev_add(&ice_cdev, ice_dev, 1) < 0) {
        unregister_chrdev_region(ice_dev, 1);
        ice_touch_exit();
        ice_mem_exit();
        ice_sym_exit();
        return -1;
    }

    {
        int tries;

        for (tries = 0; tries < 8; tries++) {
            ice_class = ICE_CLASS_CREATE(ice_name);
            if (!IS_ERR(ice_class))
                break;
            if (PTR_ERR(ice_class) != -EEXIST)
                break;
            ice_make_name();
        }
    }

    if (IS_ERR(ice_class)) {
        cdev_del(&ice_cdev);
        unregister_chrdev_region(ice_dev, 1);
        ice_touch_exit();
        ice_mem_exit();
        ice_sym_exit();
        return -1;
    }

    ice_device = device_create(ice_class, NULL, ice_dev, NULL, ice_name);
    if (IS_ERR(ice_device)) {
        class_destroy(ice_class);
        cdev_del(&ice_cdev);
        unregister_chrdev_region(ice_dev, 1);
        ice_touch_exit();
        ice_mem_exit();
        ice_sym_exit();
        return -1;
    }

    pr_info("[IceDriver] loaded, major=%d\n", major_number);
    return 0;
}

static void __exit ice_exit(void)
{
    device_destroy(ice_class, ice_dev);
    class_destroy(ice_class);
    cdev_del(&ice_cdev);
    unregister_chrdev_region(ice_dev, 1);
    ice_touch_exit();
    ice_mem_exit();
    ice_sym_exit();
    pr_info("[IceDriver] unloaded\n");
}

module_init(ice_init);
module_exit(ice_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("by.function");
MODULE_DESCRIPTION("IceDriver - force memory read/write");