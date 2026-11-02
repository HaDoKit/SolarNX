#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
# Single source of truth for the version: ../VERSION (also read by SolarNX.csproj and installer.iss).
# Set before switch_rules, which falls back to 1.0.0 when APP_VERSION is empty.
APP_VERSION	:=	$(strip $(shell cat "$(TOPDIR)/../VERSION"))
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#---------------------------------------------------------------------------------
# DEBUG=1 (or `make debug`) builds a separate -debug.nro with -Og and -DDEBUG, isolated in its own
# build_debug/ object directory so debug/release object files never mix on an incremental build.
# With DEBUG defined, common/logger's solarnxLoggerInit() calls nxlinkStdio() so solarnxLog()
# output streams back to the PC live when launched via `nxlink -s` through hbmenu's netloader -
# see common/logger/logger.c.
DEBUG ?= 0

ifeq ($(DEBUG),1)
	TARGET	:=	SolarNX-Switch-debug
	BUILD	:=	build_debug
else
	TARGET	:=	SolarNX-Switch
	BUILD	:=	build
endif

SOURCES		:=	. audio input network video menu common/logger common/fontcache common/appstate common/lang common/rs library settings transport
DATA		:=	data
INCLUDES	:=	.
# App-selection menu icons (menu.c) - see romfs/icons/.
ROMFS		:=	romfs

APP_TITLE	:=	SolarNX
APP_AUTHOR	:=	SolarNX

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -ffunction-sections \
			$(ARCH) $(DEFINES) -DSOLARNX_VERSION=\"$(APP_VERSION)\" \
			`$(PREFIX)pkg-config --cflags sdl2 SDL2_ttf SDL2_image SDL2_gfx libavformat libavcodec libavutil libswresample libswscale opusfile`

ifeq ($(DEBUG),1)
	CFLAGS	+=	-Og -DDEBUG
else
	CFLAGS	+=	-O2
endif

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	`$(PREFIX)pkg-config --libs sdl2 SDL2_ttf SDL2_image SDL2_gfx libavformat libavcodec libavutil libswresample libswscale opusfile` \
			-lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

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

# On demand only (not part of `all`): a raw exefs PFS0 .nsp (elf2nso + npdmtool + build_pfs0,
# all three from switch_rules' own built-in %.nso/%.npdm/%.nsp pattern rules below) for
# Atmosphere's atmosphere/contents/<TitleID>/ content-override mechanism. Not a retail-style,
# Tinfoil-installable NSP with its own Album icon (that needs NCA packing via hacBrewPack, not set
# up here) - see exheader.json's own comment for the caveats on the permission set.
export APP_JSON := $(TOPDIR)/exheader.json

.PHONY: $(BUILD) clean all debug

#---------------------------------------------------------------------------------
all: $(BUILD)

debug:
	@$(MAKE) --no-print-directory DEBUG=1

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr build build_debug \
		SolarNX-Switch.nro SolarNX-Switch.nacp SolarNX-Switch.elf SolarNX-Switch.nso SolarNX-Switch.npdm SolarNX-Switch.nsp \
		SolarNX-Switch-debug.nro SolarNX-Switch-debug.nacp SolarNX-Switch-debug.elf SolarNX-Switch-debug.nso SolarNX-Switch-debug.npdm SolarNX-Switch-debug.nsp

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# .nro only. The raw exefs .nsp that switch_rules can also build (APP_JSON/exheader.json below) is
# NOT an installable package - no content meta - so DBI rejects it with "Content meta not found";
# confirmed live, it got mistaken for the HOME-menu tile. The tile is SolarNX-Forwarder.nsp from
# tools/build-forwarder.ps1. `make SolarNX-Switch.nsp` still builds the exefs image on demand.
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
