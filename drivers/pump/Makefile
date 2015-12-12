KERNEL_SRC_DIR=$(HOME)/work/Zynq-Linux/Linux-Digilent-Dev/

obj-m := pump.o

pump-objs := pump_proc.o pump_drv.o

all:
	make -C $(KERNEL_SRC_DIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC_DIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=$(PWD) clean
