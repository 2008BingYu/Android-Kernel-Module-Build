#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "ice_ioctl.h"
#include "ice_mem.h"

static const char g_secret_key[] = "IceDriver2026";

static ICE_FILE_CTX *ice_get_ctx(struct file *filep)
{
    return (ICE_FILE_CTX *)filep->private_data;
}

/* 按线程组组长(进程)名查 pid，找不到返回 -ESRCH */
static pid_t ice_get_pid_by_name(const char *name)
{
    struct task_struct *t;

    for_each_process(t) {
        if (strcmp(t->comm, name) == 0)
            return t->pid;
    }

    return -ESRCH;
}

long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    ICE_FILE_CTX *ctx = ice_get_ctx(filep);
    COPY_MEMORY cm;
    GET_PID gp;
    GET_POINTER ptr;
    MODULE_BASE mb;
    char name_buf[256];
    char user_key[0x100];
    unsigned long user_name;
    int ret;

    if (!ctx)
        return -EINVAL;

    switch (cmd) {
    case OP_INIT_KEY:
        memset(user_key, 0, sizeof(user_key));

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

    case OP_GET_PID:
        if (!ctx->authorized)
            return -EACCES;

        memset(name_buf, 0, sizeof(name_buf));

        if (copy_from_user(&gp, (void __user *)arg, sizeof(gp)))
            return -EFAULT;

        user_name = (unsigned long)gp.name;
        if (!user_name)
            return -EINVAL;

        if (strncpy_from_user(name_buf, (void __user *)user_name,
                              sizeof(name_buf) - 1) < 0)
            return -EFAULT;

        ret = ice_get_pid_by_name(name_buf);
        if (ret < 0)
            return ret;

        if (copy_to_user(&((GET_PID __user *)arg)->pid,
                         &ret, sizeof(ret)))
            return -EFAULT;

        return 0;

    case OP_MODULE_BASE:
        if (!ctx->authorized)
            return -EACCES;

        memset(name_buf, 0, sizeof(name_buf));

        if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)))
            return -EFAULT;

        user_name = (unsigned long)mb.name;
        if (!user_name)
            return -EINVAL;

        if (strncpy_from_user(name_buf, (void __user *)user_name,
                              sizeof(name_buf) - 1) < 0)
            return -EFAULT;

        mb.name = name_buf;

        ret = ice_module_base(&mb);
        if (ret < 0)
            return ret;

        if (copy_to_user(&((MODULE_BASE __user *)arg)->base,
                         &mb.base, sizeof(mb.base)))
            return -EFAULT;

        return 0;

    case OP_READ_MEM:
        if (!ctx->authorized)
            return -EACCES;

        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_read_mem(&cm);

    case OP_WRITE_MEM:
        if (!ctx->authorized)
            return -EACCES;

        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)))
            return -EFAULT;
        return ice_write_mem(&cm);

    case OP_GET_POINTER:
        if (!ctx->authorized)
            return -EACCES;

        if (copy_from_user(&ptr, (void __user *)arg, sizeof(ptr)))
            return -EFAULT;

        /* 直接复用 ice_read_mem：buffer 指向用户结构体里的
         * value 字段，读出的指针值由它 copy_to_user 写回 */
        cm.pid    = ptr.pid;
        cm.addr   = ptr.addr;
        cm.buffer = &((GET_POINTER __user *)arg)->value;
        cm.size   = sizeof(ptr.value);

        ret = ice_read_mem(&cm);
        if (ret < 0)
            return ret;

        return 0;

    default:
        return -EINVAL;
    }
}