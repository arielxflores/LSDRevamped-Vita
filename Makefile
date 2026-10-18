# PS Vita build (VitaSDK + vitaGL); see docs/building.md
VITASDK ?= /usr/local/vitasdk
PREFIX  := $(VITASDK)/bin/arm-vita-eabi
CC      := $(PREFIX)-gcc
TITLE   := LSD Revamped
TITLEID := LSDR00001
VERSION := 01.00
BUILD   := build/vita

SRC := src/main.c src/util.c src/dream.c src/collide.c src/render.c src/text.c src/game.c src/dreamsys.c src/entities.c src/scene.c src/luaapi.c src/greyman.c src/audio.c src/synth.c src/skyfx.c src/video.c src/lz4.c src/ui.c src/title.c src/settings.c src/controls.c src/vorbis.c src/pak.c src/script.c src/shot.c src/platform_vita.c
LUA_SRC := $(wildcard src/lua/*.c)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC)) $(patsubst src/lua/%.c,$(BUILD)/lua/%.o,$(LUA_SRC))

CFLAGS := -O2 -g -Wl,-q -Wall -Wno-unused-parameter -mcpu=cortex-a9 -mfpu=neon -ffast-math -Isrc
LIBS := -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub \
	-lSceAppMgr_stub -lSceAppUtil_stub -lSceCommonDialog_stub -lSceGxm_stub -lSceDisplay_stub \
	-lSceSysmodule_stub -lSceKernelDmacMgr_stub -lSceCtrl_stub -lSceTouch_stub -lScePower_stub -lSceAudio_stub -lSceLibKernel_stub \
	-lz -lstdc++ -lm -lc

DATA :=
VPK  := $(BUILD)/LSDRevamped-Vita.vpk

# LiveArea artwork (bubble, background, gate) generated from art/src by tools/livearea.py
ART_DIR := $(BUILD)/sce_sys_art
ART_SRC := art/src/icon.jpg art/src/background.png art/src/startup.png

all: $(VPK)

.DELETE_ON_ERROR:

# the synth follows LSDR's float maths exactly: no fast-math there
$(BUILD)/synth.o: CFLAGS += -fno-fast-math -ffp-contract=off

$(BUILD)/%.o: src/%.c src/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/lua/%.o: src/lua/%.c src/lua/*.h
	@mkdir -p $(BUILD)/lua
	$(CC) -O2 -mcpu=cortex-a9 -mfpu=neon -w -c -o $@ $<

# headroom after the code segment for vita-elf-create's SCE module data (without it the link
# fails whenever the code happens to end just short of a 64 KiB boundary)
$(BUILD)/lsdr.elf: $(OBJ)
	$(CC) $(CFLAGS) -Wl,--defsym,__sce_headroom=0x4000 -o $@ $^ $(LIBS)

$(BUILD)/lsdr.velf: $(BUILD)/lsdr.elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(BUILD)/eboot.bin: $(BUILD)/lsdr.velf
	$(VITASDK)/bin/vita-make-fself -c -s $< $@

$(BUILD)/param.sfo:
	@mkdir -p $(BUILD)
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLEID) -s APP_VER=$(VERSION) -d ATTRIBUTE2=12 "$(TITLE)" $@

$(ART_DIR)/sce_sys/icon0.png: $(ART_SRC) tools/livearea.py sce_sys/livearea/contents/template.xml
	python3 tools/livearea.py --src art/src --out $(ART_DIR)

$(VPK): $(BUILD)/eboot.bin $(BUILD)/param.sfo $(DATA) $(ART_DIR)/sce_sys/icon0.png
	rm -f $@
	$(VITASDK)/bin/vita-pack-vpk -s $(BUILD)/param.sfo -b $(BUILD)/eboot.bin \
		-a $(ART_DIR)/sce_sys/icon0.png=sce_sys/icon0.png \
		-a $(ART_DIR)/sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
		-a $(ART_DIR)/sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
		-a $(ART_DIR)/sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
		$(foreach f,$(DATA),-a $(f)=$(f)) \
		$(foreach f,$(EXTRA),-a $(f)=$(f)) \
		$@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
