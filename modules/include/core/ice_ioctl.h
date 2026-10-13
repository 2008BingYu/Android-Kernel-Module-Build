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
    uintptr_t base;
} MODULE_BASE;

typedef struct _ICE_FILE_CTX {
    bool authorized;
} ICE_FILE_CTX;

#endif