#ifndef ICE_MEM_H
#define ICE_MEM_H

#include "ice_ioctl.h"

int ice_module_base(MODULE_BASE *mb);

int  ice_mem_init(void);
void ice_mem_exit(void);

int ice_rw_unrestricted(int is_user, pid_t pid, uintptr_t addr, void *kbuf, size_t size, int write);

#endif