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
             -lSceRegMgrForDriver_stub -lSceSysrootForDriver_stub -lSceSysrootForKernel_stub -lSceProcEventForDriver_stub -lgcc

UCFLAGS   := -Wl,-q -Wall -Wextra -Wno-unused-parameter -Wno-attribute-alias -O2 -nostdlib -fno-builtin -fshort-wchar -Iinclude
ULIBS     := -L$(STUBDIR) -ltaihen_stub -lpstv1080p_stub -lSceLibKernel_stub -lSceIofilemgr_stub \
             -lSceRegistryMgr_stub -lSceKernelModulemgr_stub -lgcc

# Configurator app (LiveArea VPK, vita2d). Needs the vdpm packages libvita2d,
# freetype, libpng, libjpeg-turbo, zlib in $(VITASDK) (`vdpm install libvita2d`).
MKSFOEX   := $(TOOLBIN)/vita-mksfoex
PACKVPK   := $(TOOLBIN)/vita-pack-vpk
CFGDIR    := configurator
CFG_TITLE_ID := PSTV10801
# vita2d is vendored in third_party/vita2d (libvita2d.a built from
# github.com/xerpi/libvita2d with THIS toolchain, i.e. soft-float like the
# SDK's libc; the vdpm package is hard-float and cannot be linked against this
# libc). Only the core/pgf members are pulled in, so no libpng/jpeg/freetype.
# It also needs the SceSharedFb functions (a library of the SceAppMgr module)
# and sceAppMgrGetBudgetInfo (SceDriverUser); the SDK snapshot's stubs for
# those two modules are stale, so current ones are regenerated from the NID
# database and linked ahead of the SDK copies.
SHAREDFB_DIR := $(BUILD)/stubs_appmgr
VITA2D_DIR := third_party/vita2d
CFG_CFLAGS := -Wl,-q -Wall -Wextra -Wno-unused-parameter -O2 -Iinclude -I$(VITA2D_DIR)
CFG_LIBS  := -L$(SHAREDFB_DIR) -L$(STUBDIR) -L$(VITA2D_DIR) -lpstv1080p_stub -lvita2d -lSceDisplay_stub -lSceGxm_stub \
             -lSceSysmodule_stub -lSceCtrl_stub -lSceTouch_stub -lScePgf_stub -lSceCommonDialog_stub -lSceIofilemgr_stub \
             -lSceLibKernel_stub -lSceProcessmgr_stub -lSceAppMgr_stub -lSceAppMgrUser_stub -lm -lc

# Regenerated user stubs for the two modules whose SDK copies are stale:
# SceAppMgr.yml (its SceSharedFb library) and SceDriverUser.yml (its
# SceAppMgrUser library, which the database maps to the "SceAppMgr" stub name,
# hence the rename to libSceAppMgrUser_stub.a so both archives can coexist).
$(SHAREDFB_DIR)/libSceAppMgr_stub.a: $(VITASDK)/share/vita-headers/db/360/SceAppMgr.yml $(VITASDK)/share/vita-headers/db/360/SceDriverUser.yml | $(BUILD)
	rm -rf $(SHAREDFB_DIR) && mkdir -p $(SHAREDFB_DIR)/a $(SHAREDFB_DIR)/b
	$(LIBSGEN) -yml=$(VITASDK)/share/vita-headers/db/360/SceAppMgr.yml -output=$(SHAREDFB_DIR)/a
	$(MAKE) -C $(SHAREDFB_DIR)/a >/dev/null
	$(LIBSGEN) -yml=$(VITASDK)/share/vita-headers/db/360/SceDriverUser.yml -output=$(SHAREDFB_DIR)/b
	$(MAKE) -C $(SHAREDFB_DIR)/b >/dev/null
	cp $(SHAREDFB_DIR)/a/libSceAppMgr_stub.a $(SHAREDFB_DIR)/libSceAppMgr_stub.a
	cp $(SHAREDFB_DIR)/b/libSceAppMgr_stub.a $(SHAREDFB_DIR)/libSceAppMgrUser_stub.a
	rm -rf $(SHAREDFB_DIR)/a $(SHAREDFB_DIR)/b

.PHONY: all kernel user stubs configurator clean
all: kernel user configurator

kernel: $(BUILD)/pstv1080p.skprx
user:   $(BUILD)/pstv1080p_settings.suprx
stubs:  $(STUBDIR)/libpstv1080p_stub.a
configurator: $(BUILD)/pstv1080p_configurator.vpk

$(BUILD)/cfg.elf: $(CFGDIR)/main.c include/pstv1080p.h $(STUBDIR)/libpstv1080p_stub.a $(SHAREDFB_DIR)/libSceAppMgr_stub.a | $(BUILD)
	$(CC) $(CFG_CFLAGS) $(CFGDIR)/main.c -o $@ $(CFG_LIBS)

$(BUILD)/cfg.velf: $(BUILD)/cfg.elf
	$(ELFCREATE) $< $@

$(BUILD)/cfg_eboot.bin: $(BUILD)/cfg.velf
	$(MAKEFSELF) -c $< $@

$(BUILD)/cfg_param.sfo: | $(BUILD)
	$(MKSFOEX) -s TITLE_ID=$(CFG_TITLE_ID) -d ATTRIBUTE2=0 "pstv1080p Configurator" $@

$(BUILD)/pstv1080p_configurator.vpk: $(BUILD)/cfg_eboot.bin $(BUILD)/cfg_param.sfo \
        $(CFGDIR)/sce_sys/icon0.png $(CFGDIR)/sce_sys/livearea/contents/bg.png \
        $(CFGDIR)/sce_sys/livearea/contents/startup.png $(CFGDIR)/sce_sys/livearea/contents/template.xml
	$(PACKVPK) -s $(BUILD)/cfg_param.sfo -b $(BUILD)/cfg_eboot.bin \
	    -a $(CFGDIR)/sce_sys/icon0.png=sce_sys/icon0.png \
	    -a $(CFGDIR)/sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
	    -a $(CFGDIR)/sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
	    -a $(CFGDIR)/sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
	    $@

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
