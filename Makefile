#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

# Include the current devkitPro Nintendo DS/DSi build rules.
include $(DEVKITARM)/ds_rules

#---------------------------------------------------------------------------------
# Project configuration
#---------------------------------------------------------------------------------
TARGET      := Readmanna
BUILD       := build
SOURCES     := source
INCLUDES    := include


ifeq ($(BUILD_STAGE),)
export GAME_ICON := $(CURDIR)/icon.bmp
endif


# Banner metadata. GAME_ICON may be overridden with a 32x32 BMP/GRF path.
GAME_TITLE     := Readmanna
GAME_SUBTITLE1 := Absorb Knowledge
GAME_SUBTITLE2 := like a god

# DSi-exclusive title type. ndstool treats any high title ID other than
# 00030000 as DSi-exclusive; 00030004 is the DSiWare title namespace.
DSI_TITLE_ID_HIGH := 00030004

#---------------------------------------------------------------------------------
# ARM9 code generation -- release/performance build for DSi
#---------------------------------------------------------------------------------
# DSi ARM9 is still ARM946E-S / ARMv5TE.  Compile in ARM state rather than Thumb
# to favor execution speed in the image decode/scale/copy hot paths.
ARCH := -march=armv5te -mtune=arm946e-s -marm

# Aggressive but semantics-preserving optimization:
#   -O3               high optimization level
#   -flto              whole-program link-time optimization for project objects
#   -fomit-frame-pointer
#                      frees a register where GCC can do so
#   -ffunction/data-sections
#                      lets the Calico linker script discard unused sections
#   -fno-unwind-*      no unwind tables are needed because C++ exceptions are off
#
# Deliberately NOT using -Ofast/-ffast-math: this project does not need unsafe
# floating-point semantics to obtain the useful ARM9 optimizations.
OPTFLAGS := -O3 -flto -fomit-frame-pointer \
            -ffunction-sections -fdata-sections \
            -fno-unwind-tables -fno-asynchronous-unwind-tables

WARNFLAGS := -Wall -Wextra

CFLAGS   := $(OPTFLAGS) $(WARNFLAGS) $(ARCH) $(INCLUDE) -DARM9 -DNDEBUG
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := $(ARCH)

# -flto must also be present during the final link.
# ds_rules replaces ds_arm9.specs with Calico's current ds9.specs, which already
# enables --gc-sections and the DS/DSi linker layout.
LDFLAGS := -specs=ds_arm9.specs $(ARCH) -flto \
           -Wl,-Map,$(notdir $*.map)

# Let ds_rules add the matching Calico ARM9 support library automatically.
LIBS    := -lfat -lnds9
LIBDIRS := $(LIBNDS)

#---------------------------------------------------------------------------------
# Two-stage recursive build
#---------------------------------------------------------------------------------
ifeq ($(BUILD_STAGE),)

# Keep repository paths relative inside the recursive build. Besides making the
# commands easier to read, this prevents shell metacharacters in an extracted
# directory name (for example "download(1)") from breaking compiler commands.
ifneq ($(filter $(BUILD)/%,$(TARGET)),)
export OUTPUT := $(notdir $(TARGET))
else
export OUTPUT := ../$(TARGET)
endif

export VPATH   := $(foreach dir,$(SOURCES),../$(dir))
export DEPSDIR := .

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD     := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I../$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I.

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d "$@" ] || mkdir -p "$@"
	@$(MAKE) --no-print-directory -C "$(BUILD)" -f "../Makefile" \
		BUILD_STAGE=1 NITRO_FILES=

clean:
	@echo clean ...
	@rm -fr "$(BUILD)" "$(TARGET).elf" "$(TARGET).nds" "$(TARGET).map" "$(TARGET).lst"

#---------------------------------------------------------------------------------
else

DEPENDS := $(OFILES:.o=.d)

# Build an actual DSi-exclusive SRL instead of the DSi-enhanced image produced by
# the stock ds_rules recipe.  The current ndstool creates the extended DSi header
# from the ARM9/ARM7 ELFs; -u 00030004 causes unitcode 3 (DSi-exclusive).
#
# The project's TWL_BSS objects ensure that the ARM9 ELF contains TWL sections,
# and Calico's ds7_maine.elf supplies the matching ARM7/ARM7i runtime.
$(OUTPUT).nds: $(OUTPUT).elf
	@echo "building DSi-exclusive ... $(notdir $@)"
	@ndstool -c "$@" \
		-9 "$<" \
		-7 "$(CALICO)/bin/ds7_maine.elf" \
		-b "$(GAME_ICON)" "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" \
		-u $(DSI_TITLE_ID_HIGH)
	@echo "built ... $(notdir $@)"

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
