INCLUDE_CJSON=1
include ../common/config.mk

CFILES := $(CFILES) \
	../common/utils/udp.c \
	../common/utils/retroarch_cmd.c

TARGET = guidemon
CFLAGS := $(CFLAGS) -Os -ffunction-sections -fdata-sections
LDFLAGS := $(LDFLAGS) -Wl,--gc-sections

include ../common/commands.mk
include ../common/recipes.mk
