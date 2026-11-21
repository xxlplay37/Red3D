#---------------------------------------------------------------------------------
# Reddit3DS — Makefile devkitARM
#   make        -> Reddit3DS.3dsx
#   make cia    -> Reddit3DS.cia   (nécessite makerom dans le PATH)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM n'est pas défini. Installe devkitPro (3ds-dev) : https://devkitpro.org/wiki/Getting_Started")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
TARGET          := Reddit3DS
BUILD           := build
SOURCES         := source
INCLUDES        := source
ROMFS           := romfs

APP_TITLE       := Reddit3DS
APP_DESCRIPTION := Client Reddit pour 3DS
APP_AUTHOR      := Homebrew
ICON            := assets/icon.png

# CIA (bannière pré-générée : plus besoin de bannertool)
BANNER          := assets/banner.bnr
RSF             := app.rsf
PRODUCT_CODE    := CTR-H-RDIT
UNIQUE_ID       := 0xFB3DD

#---------------------------------------------------------------------------------
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -mword-relocations -ffunction-sections -std=gnu11 $(ARCH)
CFLAGS   += $(INCLUDE) -D__3DS__
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,-z,noexecstack

LIBS     := -lavformat -lavcodec -lavutil -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lpng16 -ljpeg -lz -lcitro2d -lcitro3d -lctru -lm
# FFmpeg minimal (H.264 + AAC + MP4) compilé par build.sh
FFMPEG   ?= $(HOME)/.cache/reddit3ds/ffmpeg
LIBDIRS  := $(FFMPEG) $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON   := $(TOPDIR)/$(ICON)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all cia clean

all: $(BUILD) $(ROMFS)/cacert.pem
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

# Bundle de certificats racine (Mozilla) pour la vérification TLS
$(ROMFS)/cacert.pem:
	@mkdir -p $(ROMFS)
	@echo "Téléchargement de cacert.pem..."
	@curl -fsSL -o $@ https://curl.se/ca/cacert.pem

cia: all
	@command -v makerom >/dev/null || { echo "makerom introuvable (voir README)"; exit 1; }
	@makerom -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf $(RSF) \
		-icon $(TARGET).smdh -banner $(BANNER) -exefslogo -target t \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID="$(UNIQUE_ID)" -DAPP_ROMFS="$(ROMFS)"
	@echo "OK -> $(TARGET).cia"

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
