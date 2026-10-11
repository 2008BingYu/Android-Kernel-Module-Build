#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ice_mem.h"
#include "ice_ioctl.h"

static struct task_struct *find_task_by_pid(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();

    return task;
}

int ice_read_mem(COPY_MEMORY *cm)
{
    struct task_struct *task;
    struct mm_struct *mm;
    void *kbuf;
    int ret = 0;

    if (!cm || cm->size == 0)
        return -EINVAL;

    task = find_task_by_pid(cm->pid);
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -EINVAL;
    }

    kbuf = kmalloc(cm->size, GFP_KERNEL);
    if (!kbuf) {
        mmput(mm);
        put_task_struct(task);
        return -ENOMEM;
    }

    if (access_process_vm(task, cm->addr, kbuf, cm->size, 0) != cm->size) {
        ret = -EFAULT;
        goto out;
    }

    if (copy_to_user(cm->buffer, kbuf, cm->size))
        ret = -EFAULT;

out:
    kfree(kbuf);
    mmput(mm);
    put_task_struct(task);
    return ret;
}

int ice_write_mem(COPY_MEMORY *cm)
{
    struct task_struct *task;
    struct mm_struct *mm;
    void *kbuf;
    int ret = 0;

    if (!cm || cm->size == 0)
        return -EINVAL;

    task = find_task_by_pid(cm->pid);
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -EINVAL;
    }

    kbuf = kmalloc(cm->size, GFP_KERNEL);
    if (!kbuf) {
        mmput(mm);
        put_task_struct(task);
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, cm->buffer, cm->size)) {
        ret = -EFAULT;
        goto out;
    }

    if (access_process_vm(task, cm->addr, kbuf, cm->size, FOLL_WRITE) != cm->size)
        ret = -EFAULT;

out:
    kfree(kbuf);
    mmput(mm);
    put_task_struct(task);
    return ret;
}