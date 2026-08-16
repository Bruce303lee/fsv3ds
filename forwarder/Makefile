#-------------------------------------------------------------------------------
# fsv3ds-launcher - CIA forwarder for fsv3ds. See source/main.c: its entire
# job is chainloading sdmc:/3ds/fsv3ds/fsv3ds.3dsx via Luma3DS's hb:ldr port,
# so it gets a Home Menu icon instead of requiring the Homebrew Launcher.
#
# Unlike fsv3ds's own root Makefile, this doesn't produce a .3dsx at all --
# makerom consumes the linked .elf directly (confirmed against a real
# working CIA build, github.com/FlagBrew/Checkpoint's 3ds/Makefile), so the
# build stops there and hands off to bannertool + makerom for the .cia.
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=<path to>/devkitpro; export DEVKITARM=$$DEVKITPRO/devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := fsv3ds-launcher
BUILD       := build
SOURCES     := source
INCLUDES    := source

APP_TITLE       := fsv3ds
APP_DESCRIPTION := Launches fsv3ds
APP_AUTHOR      := fsv3ds project

ICON        := assets/icon.png
BANNER_IMAGE := assets/banner.png
# bannertool's own docs call -a/--audio optional ("if with a CWAV"), but in
# practice (v1.2.3) omitting it entirely makes makebanner fail with a
# generic "Missing arguments" error regardless of -i/-o -- a silent WAV
# works around it. Confirmed on the actual bannertool binary before
# committing to this, not assumed from the docs.
BANNER_AUDIO := assets/silent.wav
RSF_PATH    := assets/app.rsf
ICON_FLAGS  := nosavebackups,visible

# makerom's own default if these are left blank (0xff3ff / CTR-P-CTAP) is
# the generic "unconfigured homebrew" convention -- picked our own instead
# so this doesn't collide with every other from-a-tutorial homebrew CIA.
UNIQUE_ID     := 0xFF3FE
PRODUCT_CODE  := CTR-P-FSV1

MAKEROM    := makerom
BANNERTOOL := bannertool

#-------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
           -fomit-frame-pointer -ffunction-sections \
           $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lctru -lm

LIBDIRS := $(CTRULIB)

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD   :=  $(CC)

export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES          =  $(OFILES_SOURCES)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     -I$(CURDIR)/$(BUILD) -I$(CTRULIB)/include

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all cia

#-------------------------------------------------------------------------------
all: $(BUILD)
	@$(BANNERTOOL) makebanner -i "$(BANNER_IMAGE)" -a "$(BANNER_AUDIO)" -o $(BUILD)/banner.bnr
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i "$(ICON)" -f "$(ICON_FLAGS)" -o $(BUILD)/icon.icn
	@$(MAKEROM) -f cia -o $(TARGET).cia -target t -exefslogo -elf "$(TARGET).elf" \
		-rsf "$(RSF_PATH)" -banner "$(BUILD)/banner.bnr" -icon "$(BUILD)/icon.icn" \
		-DAPP_PRODUCT_CODE="$(PRODUCT_CODE)" -DAPP_UNIQUE_ID="$(UNIQUE_ID)"
	@echo "built ... $(TARGET).cia"

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).cia

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES_SOURCES:.o=.d)

all: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#-------------------------------------------------------------------------------
