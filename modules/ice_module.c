#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ice_module.h"
#include "ice_ioctl.h"

int ice_module_base(MODULE_BASE *mb)
{
    if (!mb)
        return -EINVAL;

    mb->base = 0;
    return 0;
}