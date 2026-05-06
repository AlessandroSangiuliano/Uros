# Minimal i386 multiboot kernel — QEMU TCG PT-write reproducer.

CC      ?= gcc
LD      ?= ld
QEMU    ?= qemu-system-i386

CFLAGS  := -m32 -ffreestanding -fno-pic -fno-pie -fno-builtin \
           -fno-stack-protector -nostdlib -O0 \
           -Wall -Wextra -mno-sse -mno-mmx -mno-sse2

ASFLAGS := -m32

LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

QEMU_BASE := -m 64M -kernel repro.elf -nographic -no-reboot \
             -monitor none -serial mon:stdio

OBJS := boot.o main.o

all: repro.elf

repro.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

boot.o: boot.S
	$(CC) $(ASFLAGS) -c -o $@ $<

main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

run-tcg: repro.elf
	$(QEMU) $(QEMU_BASE) -accel tcg -cpu qemu32

# Match the configuration used by the upstream kernel investigation
# (osfmk-mklinux issue #192).
run-tcg-icelake: repro.elf
	$(QEMU) $(QEMU_BASE) -accel tcg -cpu Icelake-Server,+sha-ni

run-kvm: repro.elf
	$(QEMU) $(QEMU_BASE) -enable-kvm -cpu host

clean:
	rm -f *.o repro.elf

.PHONY: all clean run-tcg run-kvm
