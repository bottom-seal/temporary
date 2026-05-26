RISCV_GNU ?= riscv64-unknown-elf
CC       = $(RISCV_GNU)-gcc
LD       = $(RISCV_GNU)-ld
OBJCOPY  = $(RISCV_GNU)-objcopy

CFLAGS   = -mcmodel=medany -ffreestanding -nostdlib -g -Wall -Iinclude -march=rv64imac_zicsr_zifencei -mabi=lp64
LDFLAGS  = -T boot/link.ld

QEMU     ?= qemu-system-riscv64
TARGET   = kernel

SRCS = \
	boot/start.S \
	src/sbi.c \
	src/main.c \
	src/uart.c \
	src/dt_parse.c \
	src/initrd_parse.c \
	src/str.c \
	src/shell.c \
	src/page_alloc.c \
	src/page_alloc_test.c \
	src/exec.c \
	src/framebuffer.c \
	src/trap.c \
	src/plic.c \
	src/timer.c \
	src/task.c \
	src/thread.c \
	src/syscall.c \
	src/signal.c \
	src/vm.c \
	src/mmap.c

OBJS = $(SRCS:.c=.o)
OBJS := $(OBJS:.S=.o)

BUILD    = build

ITS = $(BUILD)/$(TARGET).its
ELF = $(BUILD)/$(TARGET).elf
BIN = $(BUILD)/$(TARGET).bin
FIT = $(BUILD)/$(TARGET).fit

build: clean $(FIT)

$(ELF): $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(FIT): $(BIN) $(ITS)
	mkimage -f $(ITS) $@

boot/%.o: boot/%.S
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	$(QEMU) -M virt -m 8G -kernel $(BIN) -display none -serial stdio

clean:
	rm -rf $(ELF) $(BIN) $(FIT) $(OBJS)
