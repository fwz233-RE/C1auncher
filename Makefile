CROSS_COMPILE ?= mipsel-linux-gnu-
VERSION := $(strip $(shell cat VERSION))
CC := $(CROSS_COMPILE)gcc
READELF := $(CROSS_COMPILE)readelf
STRIP := $(CROSS_COMPILE)strip
HOST_CC ?= cc

BUILD_DIR := build
TARGET := $(BUILD_DIR)/C1ancher
LAUNCHER_TARGET := $(BUILD_DIR)/C1ancher-launcher
PKG_TARGET := $(BUILD_DIR)/c1pkg
HOST_TEST := $(BUILD_DIR)/host-tests
HOST_LAUNCHER_TEST := $(BUILD_DIR)/host-launcher-tests
HOST_PKG_TARGET := $(BUILD_DIR)/host-c1pkg
ABI_REPORT := $(BUILD_DIR)/abi.txt
LAUNCHER_ABI_REPORT := $(BUILD_DIR)/launcher-abi.txt
PKG_ABI_REPORT := $(BUILD_DIR)/c1pkg-abi.txt

SOURCES := \
	src/main.c \
	src/core/status.c \
	src/core/record.c \
	src/core/power_policy.c \
	src/display/frame.c \
	src/ui/model.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/ui/wallpaper.c \
	src/ui/terminal_screen.c \
	src/services/wifi.c \
	src/services/terminal.c \
	src/hal/linux/power.c \
	src/hal/linux/system_state.c \
	src/hal/linux/display.c \
	src/hal/linux/ui_runtime.c \
	src/platform/stop.c

TSM_SOURCES := \
	third_party/libtsm/src/tsm/tsm-render.c \
	third_party/libtsm/src/tsm/tsm-screen.c \
	third_party/libtsm/src/tsm/tsm-selection.c \
	third_party/libtsm/src/tsm/tsm-unicode.c \
	third_party/libtsm/src/tsm/tsm-vte-charsets.c \
	third_party/libtsm/src/tsm/tsm-vte.c \
	third_party/libtsm/src/shared/shl-htable.c \
	third_party/libtsm/external/wcwidth/wcwidth.c

HOST_TEST_SOURCES := \
	tests/test_main.c \
	src/core/status.c \
	src/core/record.c \
	src/core/power_policy.c \
	src/display/frame.c \
	src/ui/model.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/ui/wallpaper.c \
	src/ui/terminal_screen.c \
	src/services/wifi.c \
	src/services/terminal.c \
	src/platform/stop.c

LAUNCHER_SOURCES := \
	src/launcher/main.c \
	src/launcher/policy.c

HOST_LAUNCHER_TEST_SOURCES := \
	tests/test_launcher.c \
	src/launcher/policy.c

PKG_SOURCES := \
	src/pkg/main.c \
	src/pkg/repo.c \
	src/pkg/store.c \
	src/pkg/tui.c \
	src/pkg/util.c

ED25519_VERIFY_SOURCES := \
	third_party/ed25519/fe.c \
	third_party/ed25519/ge.c \
	third_party/ed25519/sc.c \
	third_party/ed25519/sha512.c \
	third_party/ed25519/verify.c

TARGET_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(SOURCES:.c=.o))
TARGET_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(TSM_SOURCES:.c=.o))
HOST_TEST_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(HOST_TEST_SOURCES:.c=.o))
HOST_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(TSM_SOURCES:.c=.o))
PKG_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(PKG_SOURCES:.c=.o))
PKG_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(ED25519_VERIFY_SOURCES:.c=.o))
HOST_PKG_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(PKG_SOURCES:.c=.o))
HOST_PKG_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(ED25519_VERIFY_SOURCES:.c=.o))

TSM_INCLUDES := \
	-Ithird_party/libtsm/src/tsm \
	-Ithird_party/libtsm/src/shared \
	-Ithird_party/libtsm/external \
	-Ithird_party/libtsm/external/wcwidth
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -DC1_VERSION=\"$(VERSION)\" -Isrc $(TSM_INCLUDES)
PKG_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -DC1_VERSION=\"$(VERSION)\" -Isrc/pkg -Ithird_party/ed25519
COMMON_CFLAGS := -std=c11 -Os -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections -fstack-protector-strong
TSM_CFLAGS := -std=gnu99 -Os -Wall -Wextra -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-ffast-math -fno-strict-aliasing -ffunction-sections -fdata-sections
TARGET_CFLAGS := -march=mips32r2 -mabi=32 -mhard-float -mfp32
LDFLAGS := -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now

.PHONY: all clean verify host-test

all: $(TARGET) $(LAUNCHER_TARGET) $(PKG_TARGET) verify

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/target/third_party/libtsm/%.o: third_party/libtsm/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TSM_INCLUDES) $(TSM_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/third_party/libtsm/%.o: third_party/libtsm/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TSM_INCLUDES) $(TSM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/target/src/pkg/%.o: src/pkg/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/src/pkg/%.o: src/pkg/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD_DIR)/target/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -DED25519_NO_SEED -c $< -o $@

$(BUILD_DIR)/host/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) -DED25519_NO_SEED -c $< -o $@

$(BUILD_DIR)/target/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(TARGET): $(TARGET_OBJECTS) $(TARGET_TSM_OBJECTS) | $(BUILD_DIR)
	$(CC) $^ $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded $@

$(LAUNCHER_TARGET): $(LAUNCHER_SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(LAUNCHER_SOURCES) $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded $@

$(PKG_TARGET): $(PKG_OBJECTS) $(PKG_ED25519_OBJECTS) | $(BUILD_DIR)
	$(CC) $^ $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded $@

$(HOST_PKG_TARGET): $(HOST_PKG_OBJECTS) $(HOST_PKG_ED25519_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(HOST_TEST): $(HOST_TEST_OBJECTS) $(HOST_TSM_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(HOST_LAUNCHER_TEST): $(HOST_LAUNCHER_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(HOST_LAUNCHER_TEST_SOURCES) -o $@

host-test: $(HOST_TEST) $(HOST_LAUNCHER_TEST) $(HOST_PKG_TARGET)
	$(HOST_TEST)
	$(HOST_LAUNCHER_TEST)
	$(HOST_PKG_TARGET) --help >/dev/null

verify: $(TARGET) $(LAUNCHER_TARGET) $(PKG_TARGET)
	$(READELF) -h -l -A -d $(TARGET) > $(ABI_REPORT)
	$(READELF) -h -l -A -d $(LAUNCHER_TARGET) > $(LAUNCHER_ABI_REPORT)
	$(READELF) -h -l -A -d $(PKG_TARGET) > $(PKG_ABI_REPORT)

clean:
	rm -rf $(BUILD_DIR)