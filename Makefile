#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#---------------------------------------------------------------------------------
TARGET      :=  TorrentShopNX
BUILD       :=  build
SOURCES     :=  source source/ui source/catalog source/rss source/torrent source/download source/installer source/net source/utils source/datasource source/buffer source/config
DATA        :=
INCLUDES    :=  include _external/borealis/library/include _external/borealis/library/include/borealis/extern _external/borealis/library/include/borealis/extern/nanovg _external/borealis/library/lib/extern/fmt/include _external/borealis/library/lib/extern/tweeny/include _external/borealis/library/lib/extern/yoga
ROMFS       :=  resources

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Base CFLAGS
CFLAGS  :=  -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) -DYG_ENABLE_EVENTS -DBRLS_RESOURCES=\"romfs:/\" -DHAVE_LIBNX -DSWITCH -DSTBI_NO_THREAD_LOCALS -DBOOST_ASIO_DISABLE_CONCEPTS

#---------------------------------------------------------------------------------
# External libraries logic
#---------------------------------------------------------------------------------
USE_LIBTORRENT ?= 1
LIBTORRENT_SRCDIR ?= $(TOPDIR)/_external/libtorrent-1.2.17
BOOST_INCLUDEDIR ?= $(TOPDIR)/_external/boost_1_83_0
EXTRA_LIBPATHS :=

ifeq ($(USE_LIBTORRENT),1)
	SOURCES += $(LIBTORRENT_SRCDIR)/src
	SOURCES += $(LIBTORRENT_SRCDIR)/src/kademlia
	SOURCES += $(LIBTORRENT_SRCDIR)/ed25519/src
	CFLAGS += -DTSNX_USE_LIBTORRENT=1 -DTORRENT_BUILDING_LIBRARY=1 \
	          -DBOOST_ERROR_CODE_HEADER_ONLY -DBOOST_SYSTEM_NO_DEPRECATED \
	          -DBOOST_SYSTEM_NO_LIB -DBOOST_CHRONO_HEADER_ONLY \
	          -DBOOST_DATE_TIME_NO_LIB -DBOOST_ALL_NO_LIB
	LIBDIRS += $(LIBTORRENT_SRCDIR) $(BOOST_INCLUDEDIR)
	# Extra flags for libtorrent
	CFLAGS  += -DTSNX_USE_LIBTORRENT -DBOOST_ASIO_ENABLE_CANCELIO \
	           -include $(TOPDIR)/include/switch_posix_compat.h \
	           -I$(LIBTORRENT_SRCDIR)/include -I$(BOOST_INCLUDEDIR)
endif

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS += $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Define INCLUDE and LIBPATHS - AFTER LIBDIRS is fully populated
#---------------------------------------------------------------------------------
export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(TOPDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) $(EXTRA_LIBPATHS)

#---------------------------------------------------------------------------------
# Final flags formulation
#---------------------------------------------------------------------------------
BOREALIS_BUILD_DIR := $(TOPDIR)/_external/borealis/build/library
BOREALIS_LIBS := $(BOREALIS_BUILD_DIR)/libborealis.a \
                 $(BOREALIS_BUILD_DIR)/lib/extern/yoga/yoga/libyogacore.a \
                 $(BOREALIS_BUILD_DIR)/lib/extern/fmt/libfmt.a \
                 $(BOREALIS_BUILD_DIR)/libtinyxml2.a

CFLAGS      +=  $(INCLUDE) -D__SWITCH__
CXXFLAGS    :=  $(CFLAGS) -std=gnu++20 -fexceptions
ASFLAGS     :=  -g $(ARCH)
LDFLAGS     :=  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--wrap=socketExit
LIBS        :=  $(BOREALIS_LIBS) -lglfw3 -lEGL -lglapi -ldrm_nouveau -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lzstd -lnx -lm

#---------------------------------------------------------------------------------
# Pass-through/recursive rules
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD    :=  $(CC)
else
    export LD    :=  $(CXX)
endif

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

ifneq ($(strip $(ROMFS)),)
    export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(foreach dir,$(SOURCES),[ -d $(BUILD)/$(dir) ] || mkdir -p $(BUILD)/$(dir);)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile \
		USE_LIBTORRENT=$(USE_LIBTORRENT) \
		LIBTORRENT_SRCDIR=$(LIBTORRENT_SRCDIR) \
		BOOST_INCLUDEDIR=$(BOOST_INCLUDEDIR) \
		TOPDIR=$(TOPDIR)

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all : $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf
	@echo building romfs ...
	@$(DEVKITPRO)/tools/bin/build_romfs $(TOPDIR)/$(ROMFS) $(CURDIR)/romfs.bin
	@$(DEVKITPRO)/tools/bin/elf2nro $< $@ --romfs=$(CURDIR)/romfs.bin
	@echo built ... $(notdir $@)

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
