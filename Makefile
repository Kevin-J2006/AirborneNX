#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

export LD := aarch64-none-elf-g++

TARGET      := AirborneNX
BUILD       := build
SOURCES     := source source/reimpl source/utils lib/falso_jni lib/so_util
DATA        := data
INCLUDES    := include source source/utils source/reimpl lib lib/falso_jni lib/so_util

APP_TITLE   := AirborneNX
APP_AUTHOR  := ItsDroidy06
APP_VERSION := 1.0.1
APP_ICON    := $(TOPDIR)/icon.jpg

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -DFALSOJNI_DEBUGLEVEL=3 \
               -DAIRBORNE_VERSION=\"$(APP_VERSION)\" \
               -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-function -Wno-unused-variable

# make DEBUG=1 builds with verbose logging and GL call checking.
ifeq ($(DEBUG),1)
CFLAGS      += -DAIRBORNE_DEBUG=1
endif
# make PROFILE=1 logs where the time of every slow frame went.
ifeq ($(PROFILE),1)
CFLAGS      += -DAIRBORNE_PROFILE=1
endif

CFLAGS      += $(INCLUDE)

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS     := -g $(ARCH)
LDFLAGS     =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS        := -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lSDL2 -lstdc++ -lnx -lz -lm

LIBDIRS     := $(PORTLIBS) $(LIBNX)

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

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS  := --nacp=$(CURDIR)/$(TARGET).nacp --icon=$(APP_ICON)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).pfs0 $(TARGET).nss $(TARGET).nso $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

endif
