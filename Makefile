#-------------------------------------------------------------------------------
# fsv3ds - homebrew 3DS port of fsv (https://github.com/mcuelenaere/fsv)
# Standard devkitARM 3DS homebrew Makefile.
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=<path to>/devkitpro; export DEVKITARM=$$DEVKITPRO/devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#-------------------------------------------------------------------------------
# TARGET   : the output name
# BUILD    : directory where object files & intermediate files will be placed
# SOURCES  : list of directories containing source code
# INCLUDES : list of directories containing header files
#-------------------------------------------------------------------------------
TARGET      := fsv3ds
BUILD       := build
SOURCES     := source source/compat
INCLUDES    := source

APP_TITLE       := fsv3ds
APP_DESCRIPTION := 3D filesystem visualizer (fsv port)
APP_AUTHOR      := fsv3ds project

#-------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
           -fomit-frame-pointer -ffunction-sections \
           $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# -lpng16 before -lz (libpng depends on zlib); libjpeg-turbo has no such
# ordering requirement.
LIBS    := -lcitro2d -lcitro3d -lctru -lpng16 -ljpeg -lz -lm

#-------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#-------------------------------------------------------------------------------
PORTLIBS := $(DEVKITPRO)/portlibs/3ds
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#-------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add
# additional rules for different file extensions
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES   :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))

export LD   :=  $(if $(strip $(CPPFILES)),$(CXX),$(CC))

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(PICAFILES:.v.pica=.shbin.o)
export OFILES          =  $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES          := $(PICAFILES:.v.pica=_shbin.h)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     -I$(CURDIR)/$(BUILD) -I$(CTRULIB)/include -I$(PORTLIBS)/include

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES_SOURCES:.o=.d)

all: $(OUTPUT).3dsx

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OFILES_SOURCES): $(HFILES)
$(OUTPUT).elf: $(OFILES)

#-------------------------------------------------------------------------------
.PRECIOUS: %.shbin
%.shbin.o %_shbin.h: %.shbin
#-------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
#-------------------------------------------------------------------------------
