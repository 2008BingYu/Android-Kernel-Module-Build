#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/refcount.h>

#include "ice_ioctl.h"
#include "ice_mem.h"
#include "ice_touch.h"

static const char g_secret_key[] = "IceDriver2026";

static ICE_FILE_CTX *ice_get_ctx(struct file *filep)
{
    return (ICE_FILE_CTX *)filep->private_data;
}

static inline struct task_struct *ice_try_get_task_struct(struct task_struct *t)
{
    return refcount_inc_not_zero(&t->usage) ? t : NULL;
}

static pid_t ice_get_pid_by_name(const char *name)
{
    struct task_struct *t;
    pid_t ret = -ESRCH;
    char *buf;
    size_t name_len, read_len;

    name_len = strlen(name);
    if (name_len == 0 || name_len >= 0xFF)
        return -EINVAL;
    read_len = name_len + 1;

    buf = kzalloc(read_len + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    rcu_read_lock();
    for_each_process(t) {
        struct mm_struct *mm;
        unsigned long arg_start, arg_end;
        int len, got;

        if (strncmp(t->comm, name, TASK_COMM_LEN - 1) != 0)
            continue;

        if (!ice_try_get_task_struct(t))
            continue;
        rcu_read_unlock();

        mm = get_task_mm(t);
        if (mm) {
            arg_start = mm->arg_start;
            arg_end   = mm->arg_end;

            if (arg_start && arg_end > arg_start) {
                len = (int)(arg_end - arg_start);
                if (len > (int)read_len)
                    len = (int)read_len;

                got = access_process_vm(t, arg_start, buf, len, FOLL_FORCE);
                if (got > 0) {
                    buf[got] = '\0';
                    if (strcmp(buf, name) == 0)
                        ret = t->pid;
                }
            }

            mmput(mm);
        }

        put_task_struct(t);

        if (ret >= 0) {
            kfree(buf);
            return ret;
        }

        rcu_read_lock();
    }
    rcu_read_unlock();

    kfree(buf);
    return ret;
}

long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    ICE_FILE_CTX *ctx = ice_get_ctx(filep);
    COPY_MEMORY cm;
    GET_PID gp;
    GET_POINTER ptr;
    MODULE_BASE mb;
    TOUCH_IO touch;
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

        cm.pid    = ptr.pid;
        cm.addr   = ptr.addr;
        cm.buffer = &((GET_POINTER __user *)arg)->value;
        cm.size   = sizeof(ptr.value);

        ret = ice_read_mem(&cm);
        if (ret < 0)
            return ret;

        return 0;

    case OP_TOUCH:
        if (!ctx->authorized)
            return -EACCES;

        if (copy_from_user(&touch, (void __user *)arg, sizeof(touch)))
            return -EFAULT;

        if (touch.mode == TOUCH_MODE_CONFIG) {
            ret = ice_touch_config(&touch);
            if (ret == 0 &&
                copy_to_user(&((TOUCH_IO __user *)arg)->x,
                             &touch.x, sizeof(int) * 4))
                return -EFAULT;
            return ret;
        }

        if (touch.mode == TOUCH_MODE_GEOM) {
            ret = ice_touch_geom(&touch);
            if (ret == 0 &&
                copy_to_user(&((TOUCH_IO __user *)arg)->x_max,
                             &touch.x_max, sizeof(int) * 2))
                return -EFAULT;
            return ret;
        }

        if (touch.mode == TOUCH_MODE_EVENT)
            return ice_touch_event(&touch);

        return -EINVAL;

    default:
        return -EINVAL;
    }
}