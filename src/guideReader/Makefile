INCLUDE_CJSON=1
include ../common/config.mk

CFILES := $(CFILES) \
	../common/utils/udp.c \
	../common/utils/retroarch_cmd.c

TARGET = guideReader
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_ttf -lSDL_image -lSDL_rotozoom -lpng

include ../common/commands.mk
include ../common/recipes.mk
