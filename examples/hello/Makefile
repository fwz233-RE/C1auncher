CROSS_COMPILE ?= mipsel-linux-gnu-
CC := $(CROSS_COMPILE)gcc
READELF := $(CROSS_COMPILE)readelf
VERSION ?= 0.1.0
BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/c1-example
CFLAGS := -std=c11 -Os -Wall -Wextra -Werror -mips32r2 -mabi=32 -mhard-float -mfp32

.PHONY: all verify clean
all: $(TARGET) verify

$(TARGET): main.c Makefile
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DAPP_VERSION='"$(VERSION)"' -static -o $@ main.c

verify: $(TARGET)
	$(READELF) -h -A $(TARGET)
	! $(READELF) -l $(TARGET) | grep -q INTERP
	! $(READELF) -d $(TARGET) | grep -q NEEDED

clean:
	rm -rf $(BUILD_DIR)
