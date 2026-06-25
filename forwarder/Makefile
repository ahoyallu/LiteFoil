.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment.")
endif

FWDIR       := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOTDIR     := $(abspath $(FWDIR)/..)

TARGET      := forwarder
TITLE_ID    := 0100cafe00000000
APP_TITLE   := LiteFoil
APP_AUTHOR  := Ahoy
APP_VERSION := $(shell grep "^APP_VERSION" $(ROOTDIR)/Makefile | cut -d= -f2 | tr -d ' ')
ICON        := $(ROOTDIR)/romfs/icon/icon.jpg
OUT_NSP     := $(ROOTDIR)/romfs/forwarder/LiteFoil.nsp

PROD_KEYS ?= $(if $(wildcard $(ROOTDIR)/third_party/keys.dat),$(ROOTDIR)/third_party/keys.dat,\
             $(if $(wildcard $(HOME)/.switch/prod.keys),$(HOME)/.switch/prod.keys,))

ifeq ($(strip $(PROD_KEYS)),)
$(warning "keys.dat not found — forwarder NSP will NOT be built.")
NO_KEYS := 1
endif

ifeq ($(OS),Windows_NT)
HACBREWPACK ?= $(ROOTDIR)/third_party/hacbrewpack.exe
else
HACBREWPACK ?= /usr/local/bin/hacbrewpack
endif

include $(DEVKITPRO)/devkitA64/base_rules

LIBNX   := $(DEVKITPRO)/libnx

ARCH    := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
ASARCH  := -march=armv8-a
CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -DVERSION=\"$(APP_VERSION)\"
ASFLAGS := -g $(ASARCH)
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH)

LIBS    := -lnx
INCLUDE := -I$(LIBNX)/include
LIBPATH := -L$(LIBNX)/lib

TOOLS   := $(DEVKITPRO)/tools/bin
LOGO_SRC := $(FWDIR)assets

.PHONY: all clean

ifdef NO_KEYS
all:
	@echo "  SKIP forwarder NSP (no keys found)"
else
all: $(OUT_NSP)
endif

build/main.o: source/main.c
	@echo "  CC   $<"
	@mkdir -p build
	@$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

build/trampoline.o: source/trampoline.s
	@echo "  AS   $<"
	@mkdir -p build
	@$(AS) $(ASFLAGS) -c $< -o $@

build/$(TARGET).elf: build/main.o build/trampoline.o
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) $^ $(LIBPATH) $(LIBS) -o $@

exefs/main: build/$(TARGET).elf
	@echo "  NSO  exefs/main"
	@mkdir -p exefs
	@$(TOOLS)/elf2nso $< $@

exefs/main.npdm: $(FWDIR)config.json
	@echo "  NPDM exefs/main.npdm"
	@mkdir -p exefs
	@$(TOOLS)/npdmtool $< $@

control/control.nacp:
	@echo "  NACP control/control.nacp"
	@mkdir -p control
	@$(TOOLS)/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

control/icon_AmericanEnglish.dat: $(ICON)
	@echo "  ICON control/icon_AmericanEnglish.dat"
	@mkdir -p control
	@cp $< $@

logo/NintendoLogo.png: $(LOGO_SRC)/logo.png
	@mkdir -p logo
	@cp $< $@

logo/StartupMovie.gif: $(LOGO_SRC)/logo.gif
	@mkdir -p logo
	@cp $< $@

$(OUT_NSP): exefs/main exefs/main.npdm control/control.nacp control/icon_AmericanEnglish.dat logo/NintendoLogo.png logo/StartupMovie.gif
	@echo "  NSP  $(TITLE_ID).nsp"
	@mkdir -p $(ROOTDIR)/romfs/forwarder
	@"$(HACBREWPACK)" -k "$(PROD_KEYS)" --titleid $(TITLE_ID) --titlename "$(APP_TITLE)" 2>&1
	@mv hacbrewpack_nsp/$(TITLE_ID).nsp $@
	@rm -rf hacbrewpack_nsp
	@echo "  --> $(OUT_NSP)"

clean:
	@echo "  CLEAN forwarder"
	@rm -rf build exefs control logo hacbrewpack_nsp
