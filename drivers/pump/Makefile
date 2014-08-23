obj-m := pump.o

pump-objs := pump_proc.o pump_drv.o

all:
	make -C /usr/src/kernel M=$(PWD) modules

clean:
	make -C /usr/src/kernel M=$(PWD) clean