obj-m := IceDriver.o

IceDriver-y := \
    src/core/ice_main.o \
    src/core/ice_ioctl.o \
    src/core/ice_sym.o \
    src/mem/ice_mem.o \
    src/mem/ice_module.o \
    src/input/ice_touch.o

ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/include/core
ccflags-y += -I$(src)/include/mem
ccflags-y += -I$(src)/include/input