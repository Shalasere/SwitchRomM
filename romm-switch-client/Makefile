#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# Verbose build output (set VERBOSE=1 when calling make to see full commands)
ifeq ($(strip $(VERBOSE)),1)
Q :=
else
Q := @
endif

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# Project settings
#---------------------------------------------------------------------------------
TARGET      := romm-switch-client
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs

APP_TITLE   := RomM Switch Client
APP_AUTHOR  := Shalasere
APP_VERSION := 0.2.8
ICON        := assets/icon.jpg

CURL_CFLAGS := $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS   := $(shell curl-config --libs 2>/dev/null)
ifeq ($(strip $(CURL_LIBS)),)
$(error "switch-curl is required. Install it in devkitPro MSYS2: pacman -S switch-curl")
endif

#---------------------------------------------------------------------------------
# Options for code generation (align with libnx template)
#---------------------------------------------------------------------------------
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS      := -g -Wall -O2 -ffunction-sections \
               $(ARCH) $(DEFINES) $(CURL_CFLAGS)

CFLAGS      += $(INCLUDE) -D__SWITCH__ -DROMM_APP_VERSION=\"$(APP_VERSION)\"

CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions

ASFLAGS     := -g $(ARCH)

LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS        := -lSDL2 -lEGL -lglapi -ldrm_nouveau -lnx $(CURL_LIBS)

LIBDIRS     := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# No need to edit below unless adding extra file extensions/rules
# (inherited from libnx template)
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT    := $(CURDIR)/$(TARGET)
export TOPDIR    := $(CURDIR)

export VPATH     := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR   := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD   := $(CC)
else
export LD   := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE    := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)

export LIBPATHS   := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
jsons := $(wildcard $(TARGET).json)
ifneq (,$(jsons))
  export APP_JSON := $(TOPDIR)/$(TARGET).json
endif
else
export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

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

ifneq ($(APP_TITLEID),)
export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	$(Q)[ -d $@ ] || mkdir -p $@
	$(Q)$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

.PHONY: run
run: $(OUTPUT).nro
	$(Q)nxlink -s $(OUTPUT).nro

clean:
	$(Q)echo clean ...
ifeq ($(strip $(APP_JSON)),)
	$(Q)rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	$(Q)rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

# If build flags/version in the top-level Makefile change (e.g. APP_VERSION),
# force object/NACP rebuilds. Without this, Make may reuse old .o files and the
# app can report an older ROMM_APP_VERSION even after bumping APP_VERSION.
$(OFILES) $(OUTPUT).nacp : $(TOPDIR)/Makefile

ifeq ($(strip $(APP_JSON)),)
all : $(OUTPUT).nro
ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro : $(OUTPUT).elf
endif
else
all : $(OUTPUT).nsp
$(OUTPUT).nsp : $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso : $(OUTPUT).elf
endif

$(OUTPUT).elf : $(OFILES)
$(OFILES_SRC) : $(HFILES_BIN)

# Binary data rule (extend if you add other extensions)
%.bin.o %_bin.h : %.bin
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
