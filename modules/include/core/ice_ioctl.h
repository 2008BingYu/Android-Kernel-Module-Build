#ifndef ICE_IOCTL_H
#define ICE_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define OP_INIT_KEY     0x800
#define OP_GET_PID      0x801
#define OP_MODULE_BASE  0x802
#define OP_READ_MEM     0x803
#define OP_WRITE_MEM    0x804
#define OP_GET_POINTER  0x805
#define OP_TOUCH        0x806

/* OP_TOUCH 子命令: 一个入口通吃, mode 字段区分 */
#define TOUCH_MODE_EVENT    0   /* 下发触摸事件(劫持真实触摸屏上报) */
#define TOUCH_MODE_CONFIG   1   /* x_max=期望虚拟槽位数(1..5,0=查询);
                                   成功写回 x=屏最大X, y=屏最大Y,
                                   y_max=实际虚拟槽位数 */

#define TOUCH_ACTION_DOWN   0   /* 按下, slot/x/y 有效 */
#define TOUCH_ACTION_MOVE   1   /* 移动, slot/x/y 有效 */
#define TOUCH_ACTION_UP     2   /* 抬起, slot 有效 */
#define TOUCH_ACTION_CANCEL 3   /* 全部触点抬起 */

typedef struct _COPY_MEMORY {
    pid_t     pid;
    uintptr_t addr;
    void     *buffer;
    size_t    size;
} COPY_MEMORY;

typedef struct _GET_PID {
    char     *name;
    pid_t     pid;
} GET_PID;

typedef struct _GET_POINTER {
    pid_t      pid;
    uintptr_t  addr;
    uintptr_t  value;
} GET_POINTER;

typedef struct _MODULE_BASE {
    pid_t     pid;
    char     *name;
    char      perm[8];
    int       index;
    uintptr_t base;
} MODULE_BASE;

typedef struct _TOUCH_IO {
    int       mode;      /* TOUCH_MODE_EVENT / TOUCH_MODE_CONFIG */
    int       action;    /* TOUCH_ACTION_DOWN/MOVE/UP/CANCEL */
    int       slot;      /* 触点槽位 0..9 */
    int       x;
    int       y;
    int       x_max;     /* mode=CONFIG 时有效: 屏幕宽 */
    int       y_max;     /* mode=CONFIG 时有效: 屏幕高 */
} TOUCH_IO;

typedef struct _ICE_FILE_CTX {
    bool authorized;
} ICE_FILE_CTX;

#endif