OS = 32
QA = 32

# Chris Limson 2022-05-12

ifeq ("$(OS)", "32")
XFLAGS = -m32
LDFLAGS = -m elf_i386 -nodefaultlibs
endif

ifeq ("$(OS)", "64")
XFLAGS = -m64 -D x64 -mcmodel=kernel -mtls-direct-seg-refs -mno-red-zone
LDFLAGS = -m elf_x86_64 -nodefaultlibs
endif

ifeq ("$(QA)", "32")
QEMU = qemu-system-i386
endif

ifeq ("$(QA)", "64")
QEMU = qemu-system-x86_64
endif

OBJS = \
	bio.o\
	console.o\
	exec.o\
	file.o\
	fs.o\
	ide.o\
	ioapic.o\
	kalloc.o\
	kbd.o\
	lapic.o\
	log.o\
	main.o\
	mp.o\
	picirq.o\
	pipe.o\
	proc.o\
	spinlock.o\
	string.o\
	swtch$(OS).o\
	syscall.o\
	sysfile.o\
	sysproc.o\
	timer.o\
	trapasm$(OS).o\
	trap.o\
	uart.o\
	vectors.o\
	vm.o

CC = gcc
LD = ld
OBJCOPY = objcopy
CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -Wall -fno-omit-frame-pointer
CFLAGS += -fno-common -nostdlib -gdwarf-2 -O0 $(XFLAGS)
CFLAGS += -fno-stack-protector
ASFLAGS = -fno-pic -gdwarf-2 -Wa,-divide $(XFLAGS)

xv6.img: bootblock kernel
	dd if=/dev/zero of=xv6.img count=10000
	dd if=bootblock of=xv6.img conv=notrunc
	dd if=kernel of=xv6.img seek=1 conv=notrunc

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

bootblock: bootasm.S bootmain.c
	$(CC) -fno-builtin -fno-pic -m32 -nostdinc -O -o bootmain.o -c bootmain.c
	$(CC) -fno-builtin -fno-pic -m32 -nostdinc -o bootasm.o -c bootasm.S
	$(LD) -m elf_i386 -nodefaultlibs -N -e start -Ttext 0x7C00 -o bootblock.o bootasm.o bootmain.o
	$(OBJCOPY) -S -O binary -j .text bootblock.o bootblock
	./sign.pl bootblock

entryother: entryother.S
	$(CC) $(CFLAGS) -fno-pic -nostdinc -I. -o entryother.o -c entryother.S
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o bootblockother.o entryother.o
	$(OBJCOPY) -S -O binary -j .text bootblockother.o entryother

initcode: initcode$(OS).S
	$(CC) $(CFLAGS) -nostdinc -I. -o initcode.o -c initcode$(OS).S
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o initcode.out initcode.o
	$(OBJCOPY) -S -O binary initcode.out initcode

kernel: $(OBJS) entry$(OS).o entryother initcode kernel$(OS).ld
	$(LD) $(LDFLAGS) -T kernel$(OS).ld -o kernel entry$(OS).o $(OBJS) -b binary initcode entryother

vectors.S: vectors$(OS).pl
	./vectors$(OS).pl > vectors.S

ULIB = ulib.o usys.o printf.o umalloc.o

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^

mkfs: mkfs.c fs.h
	gcc -Werror -Wall -o mkfs mkfs.c

UPROGS=\
	_cat\
	_echo\
	_grep\
	_init\
	_kill\
	_ln\
	_ls\
	_mkdir\
	_rm\
	_sh\
	_wc\
	_zombie

fs.img: mkfs README $(UPROGS)
	./mkfs fs.img README $(UPROGS)

clean: 
	rm -f _* *.o *.d xv6.img fs.img \
	initcode initcode.out bootblock entryother kernel mkfs vectors.S

qemu: fs.img xv6.img
	$(QEMU) -nographic -smp 2 -m 512 \
	-drive file=xv6.img,index=0,media=disk,format=raw \
	-drive file=fs.img,index=1,media=disk,format=raw
