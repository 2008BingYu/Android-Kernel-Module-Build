#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/string.h>

#include "ice_compat.h"
#include "ice_mem.h"
#include "ice_ioctl.h"

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

static bool ice_vma_perm_match(struct vm_area_struct *vma, const char *perm)
{
    char cur[5];

    if (!perm || perm[0] == '\0')
        return true;

    cur[0] = (vma->vm_flags & VM_READ)   ? 'r' : '-';
    cur[1] = (vma->vm_flags & VM_WRITE)  ? 'w' : '-';
    cur[2] = (vma->vm_flags & VM_EXEC)   ? 'x' : '-';
    cur[3] = (vma->vm_flags & VM_MAYSHARE) ? 's' : 'p';
    cur[4] = '\0';

    return strcmp(cur, perm) == 0;
}

int ice_module_base(MODULE_BASE *mb)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    char path_buf[256];
    int count = 0;
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

    ICE_FOR_EACH_VMA(vma, mm)
        const char *p;

        if (!vma->vm_file)
            continue;

        p = d_path(&vma->vm_file->f_path, path_buf, sizeof(path_buf));
        if (IS_ERR(p) || !p)
            continue;

        if (strcmp(kbasename(p), mb->name) != 0)
            continue;

        if (!ice_vma_perm_match(vma, mb->perm))
            continue;

        count++;
        if (mb->index > 0 && count < mb->index)
            continue;

        mb->base = vma->vm_start;
        ret = 0;
        break;
    ICE_FOR_EACH_VMA_END()

    mmap_read_unlock(mm);
    mmput(mm);
    put_task_struct(task);
    return ret;
}