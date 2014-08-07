
DTC=dtc
BOOTGEN=bootgen

devicetree.dtb : devicetree.dts
	$(DTC) -I dts -O dtb -o $@ $<

BOOT.bin : u-boot.bif ../project/project.sdk/fsbl/Debug/fsbl.elf  ../project/project.runs/impl_1/design_1_wrapper.bit u-boot.elf
	$(BOOTGEN) -image u-boot.bif -w on -o BOOT.bin

