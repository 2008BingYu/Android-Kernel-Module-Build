#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEVICE_NAME "IceDriver"
#define CLASS_NAME  "IceDriver"

static int    major_number;
static struct class*  ice_class  = NULL;
static struct device* ice_device = NULL;
static struct cdev   ice_cdev;
static dev_t         ice_dev;

static int ice_open(struct inode *inodep, struct file *filep) {
    pr_info("[IceDriver] opened\n");
    return 0;
}

static int ice_release(struct inode *inodep, struct file *filep) {
    pr_info("[IceDriver] closed\n");
    return 0;
}

static ssize_t ice_read(struct file *filep, char __user *buf, size_t len, loff_t *offset) {
    pr_info("[IceDriver] read called\n");
    return 0;
}

static ssize_t ice_write(struct file *filep, const char __user *buf, size_t len, loff_t *offset) {
    pr_info("[IceDriver] write called\n");
    return len;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = ice_open,
    .release = ice_release,
    .read    = ice_read,
    .write   = ice_write,
};

static int __init ice_init(void)
{
    if (alloc_chrdev_region(&ice_dev, 0, 1, DEVICE_NAME) < 0) {
        pr_err("[IceDriver] failed to allocate device number\n");
        return -1;
    }
    major_number = MAJOR(ice_dev);

    cdev_init(&ice_cdev, &fops);
    if (cdev_add(&ice_cdev, ice_dev, 1) < 0) {
        unregister_chrdev_region(ice_dev, 1);
        return -1;
    }

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
        ice_class = class_create(CLASS_NAME);
    #else
        ice_class = class_create(THIS_MODULE, CLASS_NAME);
    #endif

    if (IS_ERR(ice_class)) {
        cdev_del(&ice_cdev);
        unregister_chrdev_region(ice_dev, 1);
        return -1;
    }

    ice_device = device_create(ice_class, NULL, ice_dev, NULL, DEVICE_NAME);
    if (IS_ERR(ice_device)) {
        class_destroy(ice_class);
        cdev_del(&ice_cdev);
        unregister_chrdev_region(ice_dev, 1);
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
    pr_info("[IceDriver] unloaded\n");
}

module_init(ice_init);
module_exit(ice_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("by.function");
MODULE_DESCRIPTION("Memory read and write driver");
