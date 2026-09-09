SHELL := /bin/sh
.DEFAULT_GOAL := host
BUILD := build
VERSION ?= 0.1.3
CROSS_COMPILE ?= mipsel-linux-gnu-
HOST_CC ?= gcc
PYTHON ?= python3
INCLUDES := -Isrc -I../ChiChuGames/src -I../C1ancher/src
COMMON := -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wpedantic -Werror
HOST_FLAGS := $(COMMON) -O2 -DRT_HOST -DCHICHU_HOST
TARGET_FLAGS := $(COMMON) -Os -march=mips32r2 -mabi=32 -mhard-float -mfp32 \
	-fstack-protector-strong -ffunction-sections -fdata-sections
TARGET_LINK := -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now
GFX := ../ChiChuGames/src/gfx/canvas.c ../ChiChuGames/src/gfx/font.c ../ChiChuGames/src/gfx/font_data.c
SESSION := src/session.c ../C1ancher/src/platform/app_lease.c
SOURCES := src/main.c src/ui.c src/terrain.c src/game.c src/save.c src/runtime.c src/text.c $(GFX) $(SESSION)
UI_TEST_SOURCES := src/ui.c src/terrain.c src/text.c src/game.c src/save.c src/runtime.c $(GFX) $(SESSION)
HEADERS := src/cjk.h $(wildcard src/*.h) ../ChiChuGames/src/config.h ../ChiChuGames/src/gfx/canvas.h ../ChiChuGames/src/gfx/font.h
LOGIC_TESTS := $(filter-out tests/test_runtime.c,$(wildcard tests/test_game*.c tests/test_save*.c tests/test_logic.c))
LOGIC_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(LOGIC_TESTS))

host: $(BUILD)/inkwars-host
target: $(BUILD)/inkwars

src/cjk.h: tools/font_subset.py $(filter src/%.c,$(SOURCES))
	$(PYTHON) tools/font_subset.py

$(BUILD):
	mkdir -p $@

$(BUILD)/inkwars-host: $(SOURCES) $(HEADERS) Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/inkwars: $(SOURCES) $(HEADERS) Makefile | $(BUILD)
	$(CROSS_COMPILE)gcc $(TARGET_FLAGS) $(INCLUDES) $(SOURCES) $(TARGET_LINK) -o $@
	$(CROSS_COMPILE)strip --strip-unneeded $@

$(BUILD)/test_runtime: tests/test_runtime.c src/runtime.c src/runtime.h Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/test_%: tests/test_%.c src/game.c src/save.c src/game.h Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) $< src/game.c src/save.c -o $@

$(BUILD)/test_ui: tests/test_ui.c $(UI_TEST_SOURCES) $(HEADERS) Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) $< $(UI_TEST_SOURCES) -o $@

$(BUILD)/test_game-mips: tests/test_game.c src/game.c src/save.c src/game.h Makefile | $(BUILD)
	$(CROSS_COMPILE)gcc $(TARGET_FLAGS) $(INCLUDES) $< src/game.c src/save.c $(TARGET_LINK) -o $@

$(BUILD)/test_ui-mips: tests/test_ui.c $(UI_TEST_SOURCES) $(HEADERS) Makefile | $(BUILD)
	$(CROSS_COMPILE)gcc $(TARGET_FLAGS) $(INCLUDES) $< $(UI_TEST_SOURCES) $(TARGET_LINK) -o $@

$(BUILD)/test_device_runtime-mips: tests/test_device_runtime.c src/runtime.c src/runtime.h $(SESSION) Makefile | $(BUILD)
	$(CROSS_COMPILE)gcc $(TARGET_FLAGS) $(INCLUDES) -DC1_APP_LEASE_PATH='"/tmp/inkwars-device-runtime/hardware.lock"' $< $(SESSION) $(TARGET_LINK) -o $@

$(BUILD)/test_terrain-mips: tests/test_terrain.c src/terrain.c src/terrain.h $(GFX) Makefile | $(BUILD)
	$(CROSS_COMPILE)gcc $(TARGET_FLAGS) $(INCLUDES) $< src/terrain.c $(GFX) $(TARGET_LINK) -o $@

mips-test: $(BUILD)/test_game-mips $(BUILD)/test_ui-mips $(BUILD)/test_device_runtime-mips $(BUILD)/test_terrain-mips

$(BUILD)/test_session: tests/test_session.c $(SESSION) src/session.h Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) -DC1_APP_LEASE_PATH='"/tmp/inkwars-session-tests/hardware.lock"' $< $(SESSION) -lutil -o $@

$(BUILD)/test_device_runtime: tests/test_device_runtime.c src/runtime.c src/runtime.h $(SESSION) Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) -DC1_APP_LEASE_PATH='"/tmp/inkwars-device-runtime/hardware.lock"' $< $(SESSION) -o $@

$(BUILD)/test_terrain: tests/test_terrain.c src/terrain.c src/terrain.h $(GFX) Makefile | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(INCLUDES) $< src/terrain.c $(GFX) -o $@

test: $(BUILD)/test_runtime $(BUILD)/test_session $(BUILD)/test_device_runtime $(BUILD)/test_terrain $(LOGIC_BINS) $(BUILD)/test_ui
	./$(BUILD)/test_runtime
	./$(BUILD)/test_session
	./$(BUILD)/test_device_runtime
	./$(BUILD)/test_terrain
	@set -e; for t in $(LOGIC_BINS); do ./$$t; done
	./$(BUILD)/test_ui

previews: test
	$(PYTHON) tools/previews.py
	$(PYTHON) tools/terrain_preview.py

verify: $(BUILD)/inkwars
	$(PYTHON) tools/package.py --verify-only --binary $(BUILD)/inkwars --readelf $(CROSS_COMPILE)readelf

package: target verify
	$(PYTHON) tools/package.py --binary $(BUILD)/inkwars --version $(VERSION) --readelf $(CROSS_COMPILE)readelf
	$(PYTHON) tools/source_package.py

clean:
	rm -rf $(BUILD)

.PHONY: host target test mips-test previews verify package clean
