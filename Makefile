.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := LiteFoil
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs
ICON        := romfs/icon/icon.jpg
APP_TITLE   := LiteFoil
APP_AUTHOR  := Ahoy
APP_VERSION := 22.5.2
ENABLE_EXIT_LOGS ?= 0
ENABLE_RUNTIME_LOGS ?= 0
DOWNLOAD_TCP_MAXSEG ?= 1412
DOWNLOAD_TCP_NODELAY ?= 0
DOWNLOAD_USER_AGENT ?= Mozilla/5.0 (NS) LiteFoil/2.0
VERSION_HEADER := $(BUILD)/AppVersion.hpp

DEFINES += -DAPP_VERSION=\"$(APP_VERSION)\"

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS += `curl-config --cflags`
CFLAGS += $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS := -lpu -lSDL2_ttf -lSDL2_gfx -lSDL2_image -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
        -lwebp -lpng -ljpeg `sdl2-config --libs` -lfreetype `$(PREFIX)pkg-config --cflags freetype2` \
        -lharfbuzz -lmbedtls -lmbedx509 -lmbedcrypto -lsodium -lzstd -lminizip -lz -lbz2 `curl-config --libs` -lnx

LIBDIRS := $(PORTLIBS) $(LIBNX) $(CURDIR)/third_party/Plutonium/Plutonium

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
icons := $(wildcard *.jpg)
ifneq (,$(findstring $(TARGET).jpg,$(icons)))
export APP_ICON := $(TOPDIR)/$(TARGET).jpg
else
ifneq (,$(findstring icon.jpg,$(icons)))
export APP_ICON := $(TOPDIR)/icon.jpg
endif
endif
else
export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(ROMFS),)
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean plutonium_lib forwarder_nsp version_header $(BUILD)

all: version_header plutonium_lib forwarder_nsp $(BUILD)

version_header:
	@mkdir -p $(BUILD)
	@printf '#pragma once\n\n#define LITEFOIL_APP_VERSION "%s"\n#define LITEFOIL_ENABLE_EXIT_LOGS %s\n#define LITEFOIL_ENABLE_RUNTIME_LOGS %s\n#define LITEFOIL_DOWNLOAD_TCP_MAXSEG %s\n#define LITEFOIL_DOWNLOAD_TCP_NODELAY %s\n#define LITEFOIL_DOWNLOAD_USER_AGENT "%s"\n' "$(APP_VERSION)" "$(ENABLE_EXIT_LOGS)" "$(ENABLE_RUNTIME_LOGS)" "$(DOWNLOAD_TCP_MAXSEG)" "$(DOWNLOAD_TCP_NODELAY)" "$(DOWNLOAD_USER_AGENT)" > $(VERSION_HEADER).tmp
	@if ! cmp -s $(VERSION_HEADER).tmp $(VERSION_HEADER); then mv $(VERSION_HEADER).tmp $(VERSION_HEADER); else rm $(VERSION_HEADER).tmp; fi

plutonium_lib:
	@$(MAKE) --no-print-directory -C $(CURDIR)/third_party/Plutonium/Plutonium -f Makefile

forwarder_nsp:
	@$(MAKE) --no-print-directory -C $(CURDIR)/forwarder -f Makefile

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
	@$(MAKE) --no-print-directory -C $(CURDIR)/forwarder -f Makefile clean

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro: $(OUTPUT).elf
endif

$(OUTPUT).elf: $(OFILES)
$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
