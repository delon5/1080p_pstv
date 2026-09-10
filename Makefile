# pstv1080p — plain Makefile (no cmake needed). Requires VITASDK (with the taihen package installed: `vdpm taihen`).
ifndef VITASDK
$(error VITASDK is not set. Example: export VITASDK=/usr/local/vitasdk)
endif
export PATH := $(VITASDK)/bin:$(PATH)

# Tools are referenced by absolute path: GNU Make 3.81 (macOS default) ignores a Makefile-level
# `export PATH` when locating recipe commands, so a bare `arm-vita-eabi-gcc` fails there.
# The stub `makefile` written by vita-libs-gen-2 already prefixes $(VITASDK)/bin/ itself.
# Note: vita-libs-gen-2 takes `-yml=<file> -output=<dir>` options (not positional arguments as
# DESIGN.md first wrote it); it accepts the yml vita-elf-export emits unmodified.
TOOLBIN   := $(VITASDK)/bin
CC        := $(TOOLBIN)/arm-vita-eabi-gcc
ELFCREATE := $(TOOLBIN)/vita-elf-create
MAKEFSELF := $(TOOLBIN)/vita-make-fself
ELFEXPORT := $(TOOLBIN)/vita-elf-export
LIBSGEN   := $(TOOLBIN)/vita-libs-gen-2
BUILD     := build
STUBDIR   := $(BUILD)/stubs

KCFLAGS   := -Wl,-q -Wall -Wextra -Wno-unused-parameter -Wno-attribute-alias -O2 -nostdlib -fno-builtin -D__VITA_KERNEL__ -Iinclude
KLIBS     := -ltaihenForKernel_stub -ltaihenModuleUtils_stub -lSceSysclibForDriver_stub -lSceSysmemForDriver_stub \
             -lSceIofilemgrForDriver_stub -lSceThreadmgrForDriver_stub -lSceDisplayForDriver_stub \
             -lSceRegMgrForDriver_stub -lSceSysrootForDriver_stub -lSceSysrootForKernel_stub -lgcc

UCFLAGS   := -Wl,-q -Wall -Wextra -Wno-unused-parameter -Wno-attribute-alias -O2 -nostdlib -fno-builtin -fshort-wchar -Iinclude
ULIBS     := -L$(STUBDIR) -ltaihen_stub -lpstv1080p_stub -lSceLibKernel_stub -lSceIofilemgr_stub \
             -lSceRegistryMgr_stub -lSceAVConfig_stub_weak -lgcc

.PHONY: all kernel user stubs clean
all: kernel user

kernel: $(BUILD)/pstv1080p.skprx
user:   $(BUILD)/pstv1080p_settings.suprx
stubs:  $(STUBDIR)/libpstv1080p_stub.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/kernel.elf: kernel/main.c include/pstv1080p.h | $(BUILD)
	$(CC) $(KCFLAGS) kernel/main.c -o $@ $(KLIBS)

$(BUILD)/kernel.velf: $(BUILD)/kernel.elf kernel/pstv1080p.yml
	$(ELFCREATE) -e kernel/pstv1080p.yml $< $@

$(BUILD)/pstv1080p.skprx: $(BUILD)/kernel.velf
	$(MAKEFSELF) -c $< $@

$(STUBDIR)/libpstv1080p_stub.a: $(BUILD)/kernel.elf kernel/pstv1080p.yml
	$(ELFEXPORT) k $(BUILD)/kernel.elf kernel/pstv1080p.yml $(BUILD)/pstv1080p_imports.yml
	rm -rf $(STUBDIR) && mkdir -p $(STUBDIR)
	$(LIBSGEN) -yml=$(BUILD)/pstv1080p_imports.yml -output=$(STUBDIR)
	$(MAKE) -C $(STUBDIR)

$(BUILD)/user.elf: user/main.c include/pstv1080p.h $(STUBDIR)/libpstv1080p_stub.a | $(BUILD)
	$(CC) $(UCFLAGS) user/main.c -o $@ $(ULIBS)

$(BUILD)/user.velf: $(BUILD)/user.elf user/pstv1080p_settings.yml
	$(ELFCREATE) -e user/pstv1080p_settings.yml $< $@

$(BUILD)/pstv1080p_settings.suprx: $(BUILD)/user.velf
	$(MAKEFSELF) -c $< $@

clean:
	rm -rf $(BUILD)
