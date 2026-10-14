#ifndef ICE_TOUCH_H
#define ICE_TOUCH_H

#include "ice_ioctl.h"

int  ice_touch_init(void);
void ice_touch_exit(void);

int ice_touch_event(const TOUCH_IO *t);

/*
 * t->x_max: 输入, 期望虚拟槽位数(1..5); 0 表示仅查询/确保初始化。
 * 成功后写回: t->x = 屏最大X, t->y = 屏最大Y, t->y_max = 实际虚拟槽位数。
 */
int ice_touch_config(TOUCH_IO *t);

#endif