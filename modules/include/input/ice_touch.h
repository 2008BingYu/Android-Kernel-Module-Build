#ifndef ICE_TOUCH_H
#define ICE_TOUCH_H

#include "ice_ioctl.h"

int  ice_touch_init(void);
void ice_touch_exit(void);

int ice_touch_event(const TOUCH_IO *t);

int ice_touch_config(TOUCH_IO *t);

#endif