CROSS_COMPILE ?= mipsel-linux-gnu-
CC := $(CROSS_COMPILE)gcc
READELF := $(CROSS_COMPILE)readelf
STRIP := $(CROSS_COMPILE)strip
HOST_CC ?= cc

BUILD_DIR := build
TARGET := $(BUILD_DIR)/C1ancher
LAUNCHER_TARGET := $(BUILD_DIR)/C1ancher-launcher
HOST_TEST := $(BUILD_DIR)/host-tests
HOST_LAUNCHER_TEST := $(BUILD_DIR)/host-launcher-tests
ABI_REPORT := $(BUILD_DIR)/abi.txt
LAUNCHER_ABI_REPORT := $(BUILD_DIR)/launcher-abi.txt

SOURCES := \
	src/main.c \
	src/core/status.c \
	src/core/record.c \
	src/display/frame.c \
	src/ui/model.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/ui/wallpaper.c \
	src/ui/terminal_screen.c \
	src/services/wifi.c \
	src/services/ssh.c \
	src/services/terminal.c \
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
	src/display/frame.c \
	src/ui/model.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/ui/wallpaper.c \
	src/ui/terminal_screen.c \
	src/services/terminal.c

LAUNCHER_SOURCES := \
	src/launcher/main.c \
	src/launcher/policy.c

HOST_LAUNCHER_TEST_SOURCES := \
	tests/test_launcher.c \
	src/launcher/policy.c

TARGET_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(SOURCES:.c=.o))
TARGET_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(TSM_SOURCES:.c=.o))
HOST_TEST_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(HOST_TEST_SOURCES:.c=.o))
HOST_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(TSM_SOURCES:.c=.o))

TSM_INCLUDES := \
	-Ithird_party/libtsm/src/tsm \
	-Ithird_party/libtsm/src/shared \
	-Ithird_party/libtsm/external \
	-Ithird_party/libtsm/external/wcwidth
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Isrc $(TSM_INCLUDES)
COMMON_CFLAGS := -std=c11 -Os -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections -fstack-protector-strong
TSM_CFLAGS := -std=gnu99 -Os -Wall -Wextra -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-ffast-math -fno-strict-aliasing -ffunction-sections -fdata-sections
TARGET_CFLAGS := -march=mips32r2 -mabi=32 -mhard-float -mfp32
LDFLAGS := -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now

.PHONY: all clean verify host-test

all: $(TARGET) $(LAUNCHER_TARGET) verify

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/target/third_party/libtsm/%.o: third_party/libtsm/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TSM_INCLUDES) $(TSM_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/third_party/libtsm/%.o: third_party/libtsm/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TSM_INCLUDES) $(TSM_CFLAGS) -c $< -o $@

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

$(HOST_TEST): $(HOST_TEST_OBJECTS) $(HOST_TSM_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(HOST_LAUNCHER_TEST): $(HOST_LAUNCHER_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(HOST_LAUNCHER_TEST_SOURCES) -o $@

host-test: $(HOST_TEST) $(HOST_LAUNCHER_TEST)
	$(HOST_TEST)
	$(HOST_LAUNCHER_TEST)

verify: $(TARGET) $(LAUNCHER_TARGET)
	$(READELF) -h -l -A -d $(TARGET) > $(ABI_REPORT)
	$(READELF) -h -l -A -d $(LAUNCHER_TARGET) > $(LAUNCHER_ABI_REPORT)

clean:
	rm -rf $(BUILD_DIR)