#ifndef ICE_DEVICE_H
#define ICE_DEVICE_H

#include "ice_ioctl.h"

long ice_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

#endif