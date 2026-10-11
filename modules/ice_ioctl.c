#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "ice_ioctl.h"
#include "ice_mem.h"
#include "ice_module.h"

long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case OP_READ_MEM: {
        COPY_MEMORY cm;
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_read_mem(&cm);
    }
    case OP_WRITE_MEM: {
        COPY_MEMORY cm;
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_write_mem(&cm);
    }
    case OP_MODULE_BASE: {
        MODULE_BASE mb;
        if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)))
            return -EFAULT;
        return ice_module_base(&mb);
    }
    case OP_INIT_KEY:
        return 0;
    default:
        return -EINVAL;
    }
}