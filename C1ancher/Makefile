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
UPDATER_TARGET := $(BUILD_DIR)/c1updater
HOST_TEST := $(BUILD_DIR)/host-tests
HOST_DESKTOP_TEST := $(BUILD_DIR)/host-desktop-tests
HOST_LAUNCHER_TEST := $(BUILD_DIR)/host-launcher-tests
HOST_LAUNCHER_CLEANUP_TEST := $(BUILD_DIR)/host-launcher-cleanup-tests
HOST_PKG_TEST := $(BUILD_DIR)/host-pkg-tests
HOST_PKG_STORAGE_TEST := $(BUILD_DIR)/host-pkg-storage-tests
HOST_PKG_REPO_TEST := $(BUILD_DIR)/host-pkg-repo-tests
HOST_PKG_TRANSACTION_TEST := $(BUILD_DIR)/host-pkg-transaction-tests
HOST_PKG_TUI_TEST := $(BUILD_DIR)/host-pkg-tui-tests
HOST_PKG_CONFIG_TEST := $(BUILD_DIR)/host-pkg-config-tests
HOST_PLATFORM_UPDATE_TEST := $(BUILD_DIR)/host-platform-update-tests
HOST_TERMINAL_LIFECYCLE_TEST := $(BUILD_DIR)/host-terminal-lifecycle-tests
HOST_UI_LIFECYCLE_TEST := $(BUILD_DIR)/host-ui-lifecycle-tests
HOST_UPDATE_TEST := $(BUILD_DIR)/host-update-tests
HOST_UPDATER_TARGET := $(BUILD_DIR)/host-c1updater
HOST_PKG_TARGET := $(BUILD_DIR)/host-c1pkg
ABI_REPORT := $(BUILD_DIR)/abi.txt
LAUNCHER_ABI_REPORT := $(BUILD_DIR)/launcher-abi.txt
PKG_ABI_REPORT := $(BUILD_DIR)/c1pkg-abi.txt
UPDATER_ABI_REPORT := $(BUILD_DIR)/c1updater-abi.txt

SOURCES := \
	src/main.c \
	src/core/status.c \
	src/core/record.c \
	src/core/power_policy.c \
	src/display/frame.c \
	src/ui/model.c \
	src/ui/preferences.c \
	src/services/desktop_data.c \
	src/services/desktop_jobs.c \
	src/ui/input_method.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/pkg/text.c \
	src/ui/wallpaper.c \
	src/platform/media_directories.c \
	src/ui/terminal_screen.c \
	src/services/wifi.c \
	src/services/terminal.c \
	src/services/battery.c \
	src/services/time_sync.c \
	src/services/input_service.c \
	src/platform/liveness.c \
	src/platform/shutdown.c \
	src/hal/linux/power.c \
	src/hal/linux/usb_power.c \
	src/hal/linux/poweroff.c \
	src/hal/linux/system_state.c \
	src/hal/linux/display.c \
	src/hal/linux/led.c \
	src/hal/linux/ui_runtime.c \
	src/platform/app_lease.c \
	src/platform/power_config.c \
	src/platform/stop.c \
	src/platform/update_request.c \
	src/platform/update_health.c \
	src/update/state.c \
	src/security/secure_file.c

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
	src/ui/preferences.c \
	src/services/desktop_data.c \
	src/services/desktop_jobs.c \
	src/ui/input_method.c \
	src/ui/canvas.c \
	src/ui/render.c \
	src/pkg/text.c \
	src/ui/wallpaper.c \
	src/platform/media_directories.c \
	src/ui/terminal_screen.c \
	src/services/wifi.c \
	src/services/terminal.c \
	src/services/battery.c \
	src/services/time_sync.c \
	src/services/input_service.c \
	src/platform/liveness.c \
	src/platform/shutdown.c \
	src/hal/linux/led.c \
	src/platform/stop.c \
	src/platform/update_request.c \
	src/platform/update_health.c \
	src/update/state.c \
	src/security/secure_file.c

LAUNCHER_SOURCES := \
	src/launcher/main.c \
	src/launcher/cleanup.c \
	src/update/state.c \
	src/security/secure_file.c \
	src/launcher/policy.c \
	src/platform/liveness.c \
	src/platform/shutdown.c

HOST_LAUNCHER_TEST_SOURCES := \
	tests/test_launcher.c \
	src/launcher/policy.c

HOST_LAUNCHER_CLEANUP_TEST_SOURCES := \
	tests/test_launcher_cleanup.c \
	src/launcher/cleanup.c \
	src/update/state.c \
	src/security/secure_file.c

PKG_SOURCES := \
	src/pkg/main.c \
	src/pkg/desktop.c \
	src/pkg/desktop_fetch.c \
	src/services/desktop_data.c \
	src/ui/preferences.c \
	src/pkg/repo.c \
    src/pkg/store.c \
	src/pkg/metrics.c \
	src/security/sha256.c \
	src/security/secure_file.c \
	src/pkg/launch_mode.c \
	src/pkg/storage_layout.c \
	src/pkg/tui.c \
	src/pkg/gui.c \
	src/ui/input_method.c \
	src/pkg/text.c \
	src/pkg/tui_model.c \
	src/pkg/util.c \
	src/platform/app_lease.c \
	src/platform/power_config.c

HOST_PKG_TEST_SOURCES := \
	tests/test_pkg.c \
	src/pkg/launch_mode.c \
	src/pkg/tui_model.c \
	src/pkg/util.c \
	src/platform/app_lease.c

HOST_PKG_STORAGE_TEST_SOURCES := \
	tests/test_pkg_storage.c \
	src/pkg/storage_layout.c \
	src/pkg/util.c

UPDATE_SOURCES := \
	src/update/main.c \
	src/update/check.c \
	src/update/protocol.c \
	src/update/state.c \
	src/update/io.c \
	src/update/log.c \
	src/update/repository.c \
	src/update/transaction.c \
	src/update/boot.c \
	src/update/slot.c \
	src/update/supervise.c \
	src/update/supervise_policy.c \
	src/platform/shutdown.c \
	src/launcher/policy.c \
	src/security/secure_file.c \
	src/platform/update_request.c \
	src/security/sha256.c \
	src/security/trusted_ed25519.c

HOST_UPDATE_TEST_SOURCES := \
	tests/test_update.c \
	src/update/check.c \
	src/update/protocol.c \
	src/update/state.c \
	src/update/io.c \
	src/update/log.c \
	src/update/repository.c \
	src/update/transaction.c \
	src/update/boot.c \
	src/update/slot.c \
	src/update/supervise.c \
	src/update/supervise_policy.c \
	src/platform/shutdown.c \
	src/launcher/policy.c \
	src/security/secure_file.c \
	src/platform/update_request.c \
	src/security/sha256.c \
	src/security/trusted_ed25519.c

ED25519_VERIFY_SOURCES := \
	third_party/ed25519/fe.c \
	third_party/ed25519/ge.c \
	third_party/ed25519/sc.c \
	third_party/ed25519/sha512.c \
	third_party/ed25519/verify.c

HOST_UPDATE_SIGN_SOURCES := \
	third_party/ed25519/keypair.c \
	third_party/ed25519/sign.c

IME_DIR := ../term-ime/integration
TARGET_OBJECTS := $(BUILD_DIR)/target/ime-client.o $(addprefix $(BUILD_DIR)/target/,$(SOURCES:.c=.o))
TARGET_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(TSM_SOURCES:.c=.o))
HOST_TEST_OBJECTS := $(BUILD_DIR)/host/ime-client.o $(addprefix $(BUILD_DIR)/host/,$(HOST_TEST_SOURCES:.c=.o))
HOST_TSM_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(TSM_SOURCES:.c=.o))
PKG_OBJECTS := $(BUILD_DIR)/target/ime-client.o $(addprefix $(BUILD_DIR)/target/,$(PKG_SOURCES:.c=.o))
PKG_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(ED25519_VERIFY_SOURCES:.c=.o))
HOST_PKG_OBJECTS := $(BUILD_DIR)/host/ime-client.o $(addprefix $(BUILD_DIR)/host/,$(PKG_SOURCES:.c=.o))
HOST_PKG_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(ED25519_VERIFY_SOURCES:.c=.o))
UPDATE_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(UPDATE_SOURCES:.c=.o))
HOST_UPDATE_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(UPDATE_SOURCES:.c=.o))
UPDATE_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/target/,$(ED25519_VERIFY_SOURCES:.c=.o))
HOST_UPDATE_TEST_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(HOST_UPDATE_TEST_SOURCES:.c=.o))
HOST_UPDATE_ED25519_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(ED25519_VERIFY_SOURCES:.c=.o))
HOST_UPDATE_SIGN_OBJECTS := $(addprefix $(BUILD_DIR)/host/,$(HOST_UPDATE_SIGN_SOURCES:.c=.o))

TSM_INCLUDES := \
	-Ithird_party/libtsm/src/tsm \
	-Ithird_party/libtsm/src/shared \
	-Ithird_party/libtsm/external \
	-Ithird_party/libtsm/external/wcwidth
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -DC1_VERSION=\"$(VERSION)\" -Isrc -I$(IME_DIR) $(TSM_INCLUDES)
PKG_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -DC1_VERSION=\"$(VERSION)\" -Isrc -Isrc/pkg -I$(IME_DIR) -Ithird_party/ed25519
UPDATE_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -DC1_VERSION=\"$(VERSION)\" -Isrc -Ithird_party/ed25519
COMMON_CFLAGS := -std=c11 -Os -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections -fstack-protector-strong
TSM_CFLAGS := -std=gnu99 -Os -Wall -Wextra -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-ffast-math -fno-strict-aliasing -ffunction-sections -fdata-sections
TARGET_CFLAGS := -march=mips32r2 -mabi=32 -mhard-float -mfp32
LDFLAGS := -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now

.PHONY: all clean verify host-test host-update-tests

all: $(TARGET) $(LAUNCHER_TARGET) $(PKG_TARGET) $(UPDATER_TARGET) verify $(BUILD_DIR)/c1pkg-font-LICENSE.txt $(BUILD_DIR)/c1pkg-font-README.md

$(BUILD_DIR)/c1pkg-font-LICENSE.txt: third_party/pkg_font/LICENSE.txt | $(BUILD_DIR)
	cp $< $@

$(BUILD_DIR)/c1pkg-font-README.md: third_party/pkg_font/README.md | $(BUILD_DIR)
	cp $< $@

# Header edits must rebuild every consumer; otherwise changing package struct
# layout can silently link incompatible old/new objects during local builds.
PROJECT_HEADERS := VERSION $(wildcard src/*.h src/*/*.h src/*/*/*.h $(IME_DIR)/*.h)
$(sort $(TARGET_OBJECTS) $(HOST_TEST_OBJECTS) $(PKG_OBJECTS) $(HOST_PKG_OBJECTS) $(UPDATE_OBJECTS) $(HOST_UPDATE_OBJECTS) $(HOST_UPDATE_TEST_OBJECTS)): $(PROJECT_HEADERS)
$(LAUNCHER_TARGET) $(HOST_LAUNCHER_TEST) $(HOST_LAUNCHER_CLEANUP_TEST) $(HOST_PKG_TEST) $(HOST_PKG_STORAGE_TEST) $(HOST_PKG_REPO_TEST) $(HOST_PKG_TRANSACTION_TEST) $(HOST_PKG_TUI_TEST) $(HOST_PKG_CONFIG_TEST) $(HOST_PLATFORM_UPDATE_TEST): $(PROJECT_HEADERS)

$(BUILD_DIR)/host-input-service-tests: tests/test_input_service.c src/services/input_service.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_input_service.c src/services/input_service.c -o $@

.PHONY: input-service-test
input-service-test: $(BUILD_DIR)/host-input-service-tests
	$(BUILD_DIR)/host-input-service-tests

host-test: input-service-test

.PHONY: desktop-bundle-test
desktop-bundle-test:
	python3 tests/test_desktop_bundle.py

host-test: desktop-bundle-test

.PHONY: pkg-local-test
pkg-local-test:
	python3 tests/test_pkg_local.py

host-test: pkg-local-test

$(BUILD_DIR)/host-input-method-tests: tests/test_input_method.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.h $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_input_method.c src/ui/input_method.c -o $@

$(BUILD_DIR)/host-desktop-input-runtime-tests: tests/test_desktop_input_runtime.c src/hal/linux/ui_runtime.c src/ui/input_method.c src/ui/model.c src/ui/preferences.c src/core/power_policy.c $(IME_DIR)/c1_ime_client.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_desktop_input_runtime.c src/ui/input_method.c src/ui/model.c src/ui/preferences.c src/core/power_policy.c $(IME_DIR)/c1_ime_client.c -Wl,--gc-sections -o $@

.PHONY: input-method-test
input-method-test: $(BUILD_DIR)/host-input-method-tests $(BUILD_DIR)/host-desktop-input-runtime-tests $(BUILD_DIR)/host-input-render-tests
	$(BUILD_DIR)/host-input-method-tests
	$(BUILD_DIR)/host-desktop-input-runtime-tests
	$(BUILD_DIR)/host-input-render-tests

host-test: input-method-test

$(BUILD_DIR)/host-desktop-service-tests: tests/test_desktop_services.c src/core/power_policy.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_desktop_services.c src/core/power_policy.c -o $@

$(BUILD_DIR)/host-battery-tests: tests/test_battery.c src/services/battery.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_battery.c src/services/battery.c -o $@

$(BUILD_DIR)/host-battery-runtime-tests: tests/test_battery_runtime.c src/hal/linux/ui_runtime.c src/services/battery.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_battery_runtime.c src/services/battery.c -Wl,--gc-sections -o $@

.PHONY: battery-test neofetch-test
battery-test: $(BUILD_DIR)/host-battery-tests $(BUILD_DIR)/host-battery-runtime-tests
	$(BUILD_DIR)/host-battery-tests
	$(BUILD_DIR)/host-battery-runtime-tests

neofetch-test:
	python3 tests/test_neofetch.py

host-test: battery-test neofetch-test

.PHONY: desktop-service-test
desktop-service-test: $(BUILD_DIR)/host-desktop-service-tests
	$(BUILD_DIR)/host-desktop-service-tests

$(BUILD_DIR)/host-time-sync-tests: tests/test_time_sync.c src/services/time_sync.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_time_sync.c -o $@

.PHONY: time-sync-test
time-sync-test: $(BUILD_DIR)/host-time-sync-tests
	$(BUILD_DIR)/host-time-sync-tests

host-test: time-sync-test

$(BUILD_DIR)/host-desktop-tests: tests/test_desktop.c src/ui/preferences.c src/ui/model.c src/core/status.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_desktop.c src/ui/preferences.c src/ui/model.c src/core/status.c -o $@

.PHONY: desktop-test
desktop-test: $(BUILD_DIR)/host-desktop-tests
	$(BUILD_DIR)/host-desktop-tests

$(BUILD_DIR)/host-wallpaper-tests: tests/test_wallpaper.c src/ui/wallpaper.c src/platform/media_directories.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_wallpaper.c src/ui/wallpaper.c src/platform/media_directories.c -o $@

.PHONY: wallpaper-test
wallpaper-test: $(BUILD_DIR)/host-wallpaper-tests
	$(BUILD_DIR)/host-wallpaper-tests

host-test: desktop-test wallpaper-test
host-test: desktop-service-test

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

$(BUILD_DIR)/target/src/update/%.o: src/update/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UPDATE_CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/target/src/security/%.o: src/security/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UPDATE_CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/src/update/%.o: src/update/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(UPDATE_CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/src/security/%.o: src/security/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(UPDATE_CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/tests/test_update.o: tests/test_update.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(UPDATE_CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD_DIR)/target/ime-client.o: $(IME_DIR)/c1_ime_client.c $(IME_DIR)/c1_ime_client.h $(IME_DIR)/protocol.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/ime-client.o: $(IME_DIR)/c1_ime_client.c $(IME_DIR)/c1_ime_client.h $(IME_DIR)/protocol.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) -c $< -o $@

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

$(UPDATER_TARGET): $(UPDATE_OBJECTS) $(UPDATE_ED25519_OBJECTS) | $(BUILD_DIR)
	$(CC) $^ $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded $@

$(HOST_PKG_TARGET): $(HOST_PKG_OBJECTS) $(HOST_PKG_ED25519_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(HOST_TEST): $(HOST_TEST_OBJECTS) $(HOST_TSM_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(BUILD_DIR)/wifi-preview: tests/render_wifi_preview.c $(filter-out $(BUILD_DIR)/host/tests/test_main.o,$(HOST_TEST_OBJECTS)) $(HOST_TSM_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $^ -o $@

.PHONY: wifi-preview
wifi-preview: $(BUILD_DIR)/wifi-preview
	$(BUILD_DIR)/wifi-preview $(BUILD_DIR)

$(HOST_LAUNCHER_TEST): $(HOST_LAUNCHER_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(HOST_LAUNCHER_TEST_SOURCES) -o $@

$(HOST_LAUNCHER_CLEANUP_TEST): $(HOST_LAUNCHER_CLEANUP_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(HOST_LAUNCHER_CLEANUP_TEST_SOURCES) -o $@

$(HOST_PKG_TEST): $(HOST_PKG_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) $(HOST_PKG_TEST_SOURCES) -o $@

$(HOST_PKG_STORAGE_TEST): $(HOST_PKG_STORAGE_TEST_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) $(HOST_PKG_STORAGE_TEST_SOURCES) -o $@

$(HOST_PKG_REPO_TEST): tests/test_pkg_repo.c src/pkg/repo.c src/pkg/text.c src/pkg/util.c $(ED25519_VERIFY_SOURCES) $(HOST_UPDATE_SIGN_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_repo.c src/pkg/text.c src/pkg/util.c $(ED25519_VERIFY_SOURCES) $(HOST_UPDATE_SIGN_SOURCES) -o $@

$(HOST_PKG_TRANSACTION_TEST): tests/test_pkg_metrics_store.c tests/test_pkg_transaction.c src/pkg/store.c src/pkg/metrics.h src/pkg/util.c src/pkg/tui_model.c src/pkg/launch_mode.c src/platform/app_lease.c | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_metrics_store.c src/pkg/util.c src/pkg/tui_model.c src/pkg/launch_mode.c src/platform/app_lease.c -o $@

$(HOST_PKG_CONFIG_TEST): tests/test_pkg_config.c src/pkg/main.c | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_config.c -o $@

$(HOST_PKG_TUI_TEST): tests/test_pkg_tui.c src/pkg/tui.c src/pkg/util.c src/pkg/tui_model.c | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_tui.c src/pkg/util.c src/pkg/tui_model.c -o $@

$(BUILD_DIR)/host-pkg-gui-tests: tests/test_pkg_gui.c src/pkg/gui.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_gui.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c -o $@

$(BUILD_DIR)/host-pkg-ime-tests: tests/test_pkg_ime.c tests/test_pkg_gui.c src/pkg/gui.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_ime.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c -o $@

$(BUILD_DIR)/host-desktop-key-bridge-tests: tests/test_pkg_gui.c tests/desktop_key_bridge.c src/hal/linux/ui_runtime.c src/pkg/gui.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c src/ui/model.c src/ui/terminal_screen.c src/services/terminal.c $(IME_DIR)/c1_ime_client.c $(HOST_TSM_OBJECTS) $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) -D_GNU_SOURCE -DC1_TEST_PHYSICAL_KEYS tests/test_pkg_gui.c tests/desktop_key_bridge.c src/pkg/metrics.c src/security/sha256.c src/security/secure_file.c src/pkg/desktop.c src/services/desktop_data.c src/ui/preferences.c src/pkg/text.c src/pkg/tui_model.c src/pkg/util.c src/platform/app_lease.c src/ui/input_method.c src/ui/model.c src/ui/terminal_screen.c src/services/terminal.c $(IME_DIR)/c1_ime_client.c $(HOST_TSM_OBJECTS) -Wl,--gc-sections -o $@

.PHONY: desktop-key-bridge-test
desktop-key-bridge-test: $(BUILD_DIR)/host-desktop-key-bridge-tests
	$(BUILD_DIR)/host-desktop-key-bridge-tests

host-test: desktop-key-bridge-test

.PHONY: pkg-cooldown-test
pkg-cooldown-test:
	python3 tests/test_pkg_cooldown.py

host-test: pkg-cooldown-test

.PHONY: pkg-metrics-test pkg-device-metrics-test
pkg-metrics-test:
	python3 tests/test_pkg_metrics.py

pkg-device-metrics-test:
	python3 tests/test_pkg_device_metrics.py

host-test: pkg-metrics-test pkg-device-metrics-test

.PHONY: pkg-ime-test
pkg-ime-test: $(BUILD_DIR)/host-pkg-ime-tests
	$(BUILD_DIR)/host-pkg-ime-tests

host-test: pkg-ime-test

.PHONY: pkg-gui-test
$(BUILD_DIR)/host-pkg-text-tests: tests/test_pkg_text.c src/pkg/text.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) -D_DEFAULT_SOURCE $(COMMON_CFLAGS) tests/test_pkg_text.c src/pkg/text.c -o $@

$(BUILD_DIR)/host-pkg-metadata-utf8-tests: tests/test_pkg_metadata_utf8.c src/pkg/repo.c src/pkg/text.c src/pkg/util.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(PKG_CPPFLAGS) $(COMMON_CFLAGS) tests/test_pkg_metadata_utf8.c src/pkg/repo.c src/pkg/text.c src/pkg/util.c -Wl,--gc-sections -o $@

pkg-gui-test: $(BUILD_DIR)/host-pkg-gui-tests $(BUILD_DIR)/host-pkg-text-tests $(BUILD_DIR)/host-pkg-metadata-utf8-tests
	$(BUILD_DIR)/host-pkg-gui-tests $(BUILD_DIR)/pkg-gui-preview.pbm
	$(BUILD_DIR)/host-pkg-text-tests
	$(BUILD_DIR)/host-pkg-metadata-utf8-tests

host-test: pkg-gui-test

$(HOST_PLATFORM_UPDATE_TEST): tests/test_platform_update.c src/platform/app_lease.c src/platform/update_health.c | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_platform_update.c src/platform/app_lease.c src/platform/update_health.c -o $@

$(HOST_UPDATE_TEST): $(HOST_UPDATE_TEST_OBJECTS) $(HOST_UPDATE_ED25519_OBJECTS) $(HOST_UPDATE_SIGN_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

$(HOST_UPDATER_TARGET): $(HOST_UPDATE_OBJECTS) $(HOST_UPDATE_ED25519_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $^ -o $@

host-update-tests: $(HOST_UPDATE_TEST)

host-test: $(HOST_TEST) $(HOST_LAUNCHER_TEST) $(HOST_LAUNCHER_CLEANUP_TEST) $(HOST_PKG_TEST) $(HOST_PKG_STORAGE_TEST) $(HOST_PKG_REPO_TEST) $(HOST_PKG_TRANSACTION_TEST) $(HOST_PKG_TUI_TEST) $(HOST_PKG_CONFIG_TEST) $(HOST_PLATFORM_UPDATE_TEST) $(HOST_UPDATE_TEST) $(HOST_UPDATER_TARGET) $(HOST_PKG_TARGET)
	$(HOST_TEST)
	$(HOST_LAUNCHER_TEST)
	$(HOST_LAUNCHER_CLEANUP_TEST)
	$(HOST_PKG_TEST)
	$(HOST_PKG_STORAGE_TEST)
	$(HOST_PKG_REPO_TEST)
	$(HOST_PKG_TRANSACTION_TEST)
	$(HOST_PKG_TUI_TEST)
	$(HOST_PKG_CONFIG_TEST)
	$(HOST_PLATFORM_UPDATE_TEST)
	$(HOST_UPDATE_TEST)
	$(HOST_UPDATER_TARGET) --self-test
	$(HOST_PKG_TARGET) --help > /dev/null

$(HOST_TERMINAL_LIFECYCLE_TEST): tests/test_terminal_lifecycle.c src/services/terminal.c src/platform/liveness.c src/platform/app_lease.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_terminal_lifecycle.c src/services/terminal.c src/platform/liveness.c src/platform/app_lease.c -o $@

$(HOST_UI_LIFECYCLE_TEST): tests/test_ui_lifecycle.c src/hal/linux/ui_runtime.c src/platform/app_lease.c src/ui/model.c src/ui/preferences.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_ui_lifecycle.c src/ui/model.c src/ui/preferences.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c -Wl,--gc-sections -o $@

# Wi-Fi service simulations never access real interfaces or credentials.
include tests/wifi-tests.mk
host-test: wifi-service-test

.PHONY: lifecycle-test
lifecycle-test: host-test $(HOST_TERMINAL_LIFECYCLE_TEST) $(HOST_UI_LIFECYCLE_TEST)
	$(HOST_TERMINAL_LIFECYCLE_TEST)
	$(HOST_UI_LIFECYCLE_TEST)
	python3 tests/test_launcher_process.py
	python3 tests/test_bootstrap.py
	C1_UPDATE_TEST_BUILD_DIR=$(abspath $(BUILD_DIR)) python3 tests/test_bootstrap_recovery.py
	C1_UPDATE_TEST_BUILD_DIR=$(abspath $(BUILD_DIR)) python3 tests/test_runtime_log.py
	C1_UPDATE_TEST_BUILD_DIR=$(abspath $(BUILD_DIR)) python3 tests/test_update_repository.py
	python3 tests/test_pkg_http.py --build-dir "$(BUILD_DIR)" --cc "$(HOST_CC)"

verify: $(TARGET) $(LAUNCHER_TARGET) $(PKG_TARGET) $(UPDATER_TARGET)
	$(READELF) -h -l -A -d $(TARGET) > $(ABI_REPORT)
	$(READELF) -h -l -A -d $(LAUNCHER_TARGET) > $(LAUNCHER_ABI_REPORT)
	$(READELF) -h -l -A -d $(PKG_TARGET) > $(PKG_ABI_REPORT)
	$(READELF) -h -l -A -d $(UPDATER_TARGET) > $(UPDATER_ABI_REPORT)

.PHONY: shutdown-supervision-test
shutdown-supervision-test:
	python3 tests/test_shutdown_supervision.py

host-test: shutdown-supervision-test launcher-partition-cleanup-test

.PHONY: launcher-partition-cleanup-test
launcher-partition-cleanup-test:
	python3 tests/test_launcher_partition_cleanup.py

.PHONY: power-key-test
$(BUILD_DIR)/host-power-key-tests: tests/test_power_key.c src/core/power_policy.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_power_key.c src/core/power_policy.c -o $@

$(BUILD_DIR)/host-poweroff-tests: tests/test_poweroff.c src/hal/linux/poweroff.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_poweroff.c src/hal/linux/poweroff.c -o $@

$(BUILD_DIR)/host-power-runtime-tests: tests/test_power_runtime.c src/hal/linux/ui_runtime.c src/core/power_policy.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_power_runtime.c src/core/power_policy.c -Wl,--gc-sections -o $@

$(BUILD_DIR)/host-shutdown-runtime-tests: tests/test_shutdown_runtime.c src/hal/linux/ui_runtime.c src/core/power_policy.c src/core/record.c src/core/status.c src/platform/stop.c src/ui/model.c src/ui/preferences.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_shutdown_runtime.c src/core/power_policy.c src/core/record.c src/core/status.c src/platform/stop.c src/ui/model.c src/ui/preferences.c src/ui/input_method.c $(IME_DIR)/c1_ime_client.c -Wl,--gc-sections -o $@

power-key-test: $(BUILD_DIR)/host-power-key-tests $(BUILD_DIR)/host-poweroff-tests $(BUILD_DIR)/host-power-runtime-tests $(BUILD_DIR)/host-shutdown-runtime-tests
	$(BUILD_DIR)/host-power-key-tests
	$(BUILD_DIR)/host-poweroff-tests
	$(BUILD_DIR)/host-power-runtime-tests
	$(BUILD_DIR)/host-shutdown-runtime-tests

host-test: power-key-test

# USB suspend tests inject I/O into the production helper; no device is accessed.
.PHONY: usb-power-test
$(BUILD_DIR)/host-usb-power-tests: tests/test_usb_power.c src/hal/linux/usb_power.c src/hal/linux/power.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_usb_power.c src/hal/linux/usb_power.c -Wl,--gc-sections -o $@

usb-power-test: $(BUILD_DIR)/host-usb-power-tests
	$(BUILD_DIR)/host-usb-power-tests

host-test: usb-power-test

.PHONY: child-process-test
$(BUILD_DIR)/host-child-process-tests: tests/test_child_processes.c src/platform/liveness.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_child_processes.c src/platform/liveness.c -Wl,--gc-sections -o $@

child-process-test: $(BUILD_DIR)/host-child-process-tests
	$(BUILD_DIR)/host-child-process-tests

host-test: child-process-test

$(BUILD_DIR)/host-input-render-tests: tests/test_input_render.c $(filter-out $(BUILD_DIR)/host/tests/test_main.o,$(HOST_TEST_OBJECTS)) $(HOST_TSM_OBJECTS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $^ -Wl,--gc-sections -o $@

$(BUILD_DIR)/host-desktop-jobs-tests: tests/test_desktop_jobs.c src/services/desktop_jobs.c $(PROJECT_HEADERS) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_desktop_jobs.c src/services/desktop_jobs.c -Wl,--wrap=read,--wrap=waitpid,--wrap=kill,--wrap=close,--wrap=execv,--wrap=opendir -o $@

.PHONY: desktop-feed-test desktop-jobs-test update-check-test
desktop-feed-test:
	python3 tests/test_desktop_feed.py --build-dir $(BUILD_DIR)/desktop-feed

desktop-jobs-test: $(BUILD_DIR)/host-desktop-jobs-tests
	$(BUILD_DIR)/host-desktop-jobs-tests

update-check-test:
	C1_UPDATE_TEST_BUILD_DIR=$(BUILD_DIR)/update-check python3 tests/test_update_check.py

host-test: desktop-feed-test desktop-jobs-test update-check-test

clean:
	rm -rf $(BUILD_DIR)
