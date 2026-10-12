#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/version.h>

#include "ice_module.h"
#include "ice_ioctl.h"
#include "ice_sym.h"

static struct task_struct *ice_mod_find_task(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();

    return task;
}

int ice_module_base(MODULE_BASE *mb)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    char path_buf[256];
    int ret = -ENOENT;

    if (!mb || !mb->name)
        return -EINVAL;

    task = ice_mod_find_task(mb->pid);
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -EINVAL;
    }

    mb->base = 0;

    mmap_read_lock(mm);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    {
        VMA_ITERATOR(vmi, mm, 0);
        for_each_vma(vmi, vma) {
#else
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
        const char *p;

        if (!vma->vm_file)
            continue;

        if (p_d_path) {
            p = p_d_path(&vma->vm_file->f_path, path_buf, sizeof(path_buf));
        } else {
            continue;
        }

        if (IS_ERR(p) || !p)
            continue;

        if (strstr(p, mb->name)) {
            mb->base = vma->vm_start;
            ret = 0;
            break;
        }
    }

    mmap_read_unlock(mm);
    mmput(mm);
    put_task_struct(task);
    return ret;
}