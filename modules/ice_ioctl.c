#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "ice_ioctl.h"
#include "ice_mem.h"
#include "ice_module.h"

static const char g_secret_key[] = "IceDriver2026";

static ICE_FILE_CTX *ice_get_ctx(struct file *filep)
{
    return (ICE_FILE_CTX *)filep->private_data;
}

long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    ICE_FILE_CTX *ctx = ice_get_ctx(filep);
    if (!ctx)
        return -EINVAL;

    switch (cmd) {
    case OP_INIT_KEY: {
        char user_key[0x100] = {0};

        if (copy_from_user(user_key, (void __user *)arg,
                           sizeof(user_key) - 1))
            return -EFAULT;

        if (strcmp(user_key, g_secret_key) == 0) {
            ctx->authorized = true;
            pr_info("[IceDriver] fd %p authorized\n", filep);
            return 0;
        }

        pr_warn("[IceDriver] bad key\n");
        return -EACCES;
    }

    case OP_READ_MEM: {
        if (!ctx->authorized)
            return -EACCES;

        COPY_MEMORY cm;
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_read_mem(&cm);
    }

    case OP_WRITE_MEM: {
        if (!ctx->authorized)
            return -EACCES;

        COPY_MEMORY cm;
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_write_mem(&cm);
    }

    case OP_MODULE_BASE: {
        if (!ctx->authorized)
            return -EACCES;

        MODULE_BASE mb;
        char name_buf[256] = {0};
        unsigned long user_name;

        if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)))
            return -EFAULT;

        user_name = (unsigned long)mb.name;
        if (!user_name)
            return -EINVAL;

        if (strncpy_from_user(name_buf, (void __user *)user_name,
                              sizeof(name_buf) - 1) < 0)
            return -EFAULT;

        mb.name = name_buf;

        int ret = ice_module_base(&mb);
        if (ret < 0)
            return ret;

        if (copy_to_user(&((MODULE_BASE __user *)arg)->base,
                         &mb.base, sizeof(mb.base)))
            return -EFAULT;

        return 0;
    }

    default:
        return -EINVAL;
    }
}