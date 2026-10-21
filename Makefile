KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# Build with the same compiler as the kernel: CachyOS kernels are built with clang
ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config),)
  KMAKE_FLAGS += LLVM=1
endif

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KMAKE_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KMAKE_FLAGS) clean

.PHONY: all clean
