#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

rwildcard = $(foreach d, $(wildcard $1*), \
            $(filter $(subst *, %, $2), $d) \
            $(call rwildcard, $d/, $2))

define unique =
  $(eval seen :=)
  $(foreach _,$1,$(if $(filter $_,${seen}),,$(eval seen += $_)))
  ${seen}
endef

TARGET		:= Plex3DS
BUILD		:= build
SOURCES		:= source
DATA		:= data
INCLUDES	:= include source external/include
ROMFS		:= romfs

APP_TITLE		:= Plex3DS
APP_DESCRIPTION	:= Plex Media Client for Nintendo 3DS
APP_AUTHOR		:= Jared & Antigravity
PRODUCT_CODE	:= CTR-PLEX
UNIQUE_ID		:= 0xEE73D

# Options for code generation (Optimized for ARM11 MPCORE / New 3DS)
ARCH	:= -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:= -Wall -Wextra -Wno-unused -O2 -mword-relocations \
		-fomit-frame-pointer -ffunction-sections -fdata-sections \
		$(ARCH)

CFLAGS	+= $(INCLUDE) -DARM11 -D__3DS__ -DCURL_STATICLIB

CXXFLAGS	:= $(CFLAGS) -fno-exceptions -std=gnu++17

ASFLAGS	:= $(ARCH)
LDFLAGS	:= -specs=3dsx.specs $(ARCH) -Wl,--gc-sections,--allow-multiple-definition,-Map,$(notdir $*.map)

LIBS	:= -lavformat -lavcodec -lswscale -lswresample -lavutil \
		   -lmpg123 -lFLAC -logg \
		   -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz \
		   -lcitro2d -lcitro3d -lctru -lm

LIBDIRS_DEVKITPRO := libctru portlibs/3ds

export LD := $(CXX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:= $(CURDIR)/$(TARGET)
export TOPDIR	:= $(CURDIR)

export VPATH	:= $(CURDIR)/$(SOURCES)

export DEPSDIR	:= $(CURDIR)/$(BUILD)

CFILES		:= $(patsubst $(SOURCES)/%, %, $(call rwildcard, $(SOURCES), *.c))
CPPFILES	:= $(patsubst $(SOURCES)/%, %, $(call rwildcard, $(SOURCES), *.cpp))
SFILES		:= $(patsubst $(SOURCES)/%, %, $(call rwildcard, $(SOURCES), *.s))

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OBJDIRS        := $(call unique, $(patsubst %, $(BUILD)/%, $(dir $(OFILES_SOURCES))))
export OFILES         := $(OFILES_SOURCES)

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
				   $(foreach dir,$(LIBDIRS_DEVKITPRO),-I$(DEVKITPRO)/$(dir)/include) \
				   -I$(CURDIR)/$(BUILD)

export LIBPATHS	:= -L$(CURDIR)/external/lib $(foreach dir,$(LIBDIRS_DEVKITPRO),-L$(DEVKITPRO)/$(dir)/lib)

export _3DSXDEPS	:= $(if $(NO_SMDH),,$(OUTPUT).smdh)

.PHONY: $(BUILD) clean all test

all: $(BUILD) $(OBJDIRS)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

PYTHON ?= $(shell which python 2>/dev/null || which python3 2>/dev/null || which py 2>/dev/null || echo "/c/Users/Jared/AppData/Local/Programs/Python/Python313/python.exe")

test:
	@$(PYTHON) tests/test_plex3ds_core.py

$(BUILD):
	@mkdir -p $@

$(OBJDIRS):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

else

.PHONY: all

all: $(OUTPUT).3dsx

$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)

$(OUTPUT).elf: $(OFILES)

-include $(DEPSDIR)/*.d

endif
