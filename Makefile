ccflags-y += -Og
CFLAGS_rcar_cluster_drv.o    += -Og

qos-y := rcar_cluster_drv.o
obj-m := rcar_cluster_drv.o

ccflags-y += -I$(KERNEL_SRC)/include
RCAR_CLUSTER_MODULE =

all:
	make -C $(KERNEL_SRC) M=$(shell pwd) modules

clean:
	make -C $(KERNEL_SRC) M=$(shell pwd) clean

install:
	$(CP) ./r_taurus_cluster_protocol.h $(KERNEL_SRC)/include/$(RCAR_CLUSTER_MODULE)

