#ifndef ICE_MEM_H
#define ICE_MEM_H

#include "ice_ioctl.h"

int ice_read_mem(COPY_MEMORY *cm);
int ice_write_mem(COPY_MEMORY *cm);

int ice_module_base(MODULE_BASE *mb);

#endif