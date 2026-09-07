# ChiChuGames — C1-Slim 墨水屏游戏合集
# host-test: cc 逻辑单测; target: mipsel 交叉构建; dump: PBM 画面审查
# verify: ABI 核验; pack: 本地 c1pkg 发布包

CROSS_COMPILE ?= mipsel-linux-gnu-
VERSION := 0.1.3
BUILD := build

CC := $(CROSS_COMPILE)gcc
READELF := $(CROSS_COMPILE)readelf
STRIP := $(CROSS_COMPILE)strip
HOST_CC ?= cc

INCLUDES := -Isrc -Isrc/gfx
TARGET_CFLAGS := -std=c11 -Os -g -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections -fstack-protector-strong \
	-march=mips32r2 -mabi=32 -mhard-float -mfp32 \
	-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-DCCG_VERSION=\"$(VERSION)\"
TARGET_LDFLAGS := -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now
HOST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
	-DCHICHU_HOST -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-DCCG_VERSION=\"$(VERSION)\"

SRCS := \
	src/main.c \
	src/app_runtime.c \
	src/platform/display.c \
	src/platform/input.c \
	src/platform/time.c \
	src/platform/audio.c \
	src/platform/sfx_data.c \
	src/platform/led.c \
	src/gfx/canvas.c \
	src/gfx/font.c \
	src/gfx/font_data.c \
	src/gfx/pattern.c \
	src/rng.c \
	src/ui/menu.c \
	src/ui/help.c \
	src/ui/ui_common.c \
	src/games/games_table.c \
	src/games/snake.c \
	src/games/tetris.c \
	src/games/g2048.c \
	src/games/mines.c \
	src/games/gomoku.c \
	src/games/memory.c \
	src/games/sokoban.c \
	src/games/hanoi.c \
	src/games/sudoku.c \
	src/games/reversi.c \
	src/games/fifteen.c \
	src/games/lightsout.c \
	src/games/wordle.c \
	src/games/game24.c \
	src/games/klotski.c \
	src/games/maze.c \
	src/games/peg.c \
	src/games/connect4.c \
	src/games/life.c \
	src/games/mastermind.c \
	src/games/hangman.c \
	src/games/nonogram.c \
	src/games/bullscows.c \
	src/games/dotsbox.c \
	src/games/blackjack.c \
	src/games/pyramid.c \
	src/games/golf.c \
	src/games/klondike.c \
	src/games/freecell.c \
	src/games/whack.c \
	src/games/kakuro.c \
	src/games/nim.c \
	src/games/battleship.c \
	src/games/yahtzee.c \
	src/games/hex.c \
	src/games/hexmines.c \
	src/games/checkers.c \
	src/games/isola.c \
	src/games/chess.c \
	src/games/darkchess.c \
	src/games/binarypuzzle.c \
	src/games/futoshiki.c \
	src/games/skyscrapers.c \
	src/games/numberlink.c \
	src/games/anagrams.c \
	src/games/questions.c \
	src/games/killersudoku.c \
	src/games/nurikabe.c \
	src/games/pentomino.c \
	src/games/matchstick.c \
	src/games/simon.c \
	src/games/spider.c \
	src/games/pokerdraw.c \
	src/games/towerdefense.c \
	src/games/ccheckers.c \
	src/games/backgammon.c \
	src/games/boggle.c \
	src/games/wordchain.c \
	src/games/mathtrain.c \
	src/games/primerush.c \
	src/games/binmorse.c \
	src/games/tictacdice.c \
	src/games/slitherlink.c \
	src/games/hashi.c \
	src/games/starbattle.c \
	src/games/tents.c \
	src/games/kenken.c \
	src/games/jigsaw.c \
	src/games/life2.c \
	src/games/memdigits.c \
	src/games/geo.c \
	src/games/flagquiz.c \
	src/games/elements.c \
	src/games/timestrain.c \
	src/games/clockquiz.c \
	src/games/riddle.c \
	src/games/dicequest.c \
	src/games/liarsdice.c \
	src/games/miner.c \
	src/games/rails.c \
	src/games/citybuilder.c \
	src/games/beefarm.c \
	src/games/mushgarden.c \
	src/games/quake.c \
	src/games/antcolony.c \
	src/games/mahjongmatch.c \
	src/games/stubs.c

OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
HOST_OBJS := $(patsubst src/%.c,$(BUILD)/hostobj/%.o,$(SRCS))

TARGET := $(BUILD)/C1ancher-chichugames
HOST_TEST := $(BUILD)/host-tests
DUMP := $(BUILD)/dump_pbm
ABI_REPORT := $(BUILD)/abi.txt
SIZE_REPORT := $(BUILD)/size.txt
GOLDEN := $(BUILD)/golden

all: target host-test verify

# ---------------- 目标(设备) ----------------
target: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(TARGET_CFLAGS) $(OBJS) -o $@ $(TARGET_LDFLAGS)
	$(STRIP) --strip-unneeded $@

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TARGET_CFLAGS) $(INCLUDES) -c $< -o $@

# ---------------- host 测试 ----------------
BATCH1_TESTS := $(wildcard tests/batch1/*.c)

$(BUILD)/host-input-lifecycle: tests/test_input_lifecycle.c src/platform/input.c src/platform/input.h
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) tests/test_input_lifecycle.c -o $@

host-test: $(HOST_TEST) $(BATCH1_TESTS) $(BUILD)/host-input-lifecycle
	./$(HOST_TEST)
	./$(BUILD)/host-input-lifecycle
	@set -e; for t in $(BATCH1_TESTS); do \
		if [ "$$t" = "tests/batch1/test_menu_cats.c" ]; then \
			$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $$t \
				$(BUILD)/hostobj/platform/display.o $(BUILD)/hostobj/platform/input.o \
				$(BUILD)/hostobj/platform/time.o $(BUILD)/hostobj/app_runtime.o $(BUILD)/hostobj/platform/audio.o \
				$(BUILD)/hostobj/platform/sfx_data.o $(BUILD)/hostobj/platform/led.o \
				$(BUILD)/hostobj/gfx/canvas.o \
				$(BUILD)/hostobj/gfx/font.o $(BUILD)/hostobj/gfx/font_data.o \
				$(BUILD)/hostobj/gfx/pattern.o $(BUILD)/hostobj/rng.o \
				$(BUILD)/hostobj/ui/ui_common.o \
				$(wildcard $(BUILD)/hostobj/games/*.o) \
				-o $(BUILD)/batch_test || exit 1; \
		elif [ "$$t" = "tests/batch1/test_audio_led.c" ]; then \
			$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $$t \
				$(BUILD)/hostobj/platform/display.o $(BUILD)/hostobj/platform/input.o \
				$(BUILD)/hostobj/platform/time.o $(BUILD)/hostobj/app_runtime.o $(BUILD)/hostobj/platform/sfx_data.o \
				$(BUILD)/hostobj/gfx/canvas.o \
				$(BUILD)/hostobj/gfx/font.o $(BUILD)/hostobj/gfx/font_data.o \
				$(BUILD)/hostobj/gfx/pattern.o $(BUILD)/hostobj/rng.o \
				$(BUILD)/hostobj/ui/ui_common.o -o $(BUILD)/batch_test || exit 1; \
		else \
			$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $$t \
				$(BUILD)/hostobj/platform/display.o $(BUILD)/hostobj/platform/input.o \
				$(BUILD)/hostobj/platform/time.o $(BUILD)/hostobj/app_runtime.o $(BUILD)/hostobj/platform/audio.o \
				$(BUILD)/hostobj/platform/sfx_data.o $(BUILD)/hostobj/platform/led.o \
				$(BUILD)/hostobj/gfx/canvas.o \
				$(BUILD)/hostobj/gfx/font.o $(BUILD)/hostobj/gfx/font_data.o \
				$(BUILD)/hostobj/gfx/pattern.o $(BUILD)/hostobj/rng.o \
				$(BUILD)/hostobj/ui/ui_common.o -o $(BUILD)/batch_test || exit 1; \
		fi; \
		./$(BUILD)/batch_test || exit 1; \
		echo "PASS: $$t"; \
	done

$(HOST_TEST): tests/test_logic.c $(HOST_OBJS) src/gfx/font_data.c $(SRCS)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) tests/test_logic.c \
		$(BUILD)/hostobj/platform/display.o $(BUILD)/hostobj/platform/input.o \
		$(BUILD)/hostobj/platform/time.o $(BUILD)/hostobj/app_runtime.o $(BUILD)/hostobj/platform/audio.o \
		$(BUILD)/hostobj/platform/sfx_data.o $(BUILD)/hostobj/platform/led.o \
		$(BUILD)/hostobj/gfx/canvas.o \
		$(BUILD)/hostobj/gfx/font.o $(BUILD)/hostobj/gfx/font_data.o \
		$(BUILD)/hostobj/gfx/pattern.o $(BUILD)/hostobj/rng.o \
		$(BUILD)/hostobj/ui/ui_common.o -o $@

$(BUILD)/hostobj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) -c $< -o $@

# ---------------- PBM 画面审查 ----------------
dump: $(DUMP)
	@mkdir -p $(GOLDEN)
	./$(DUMP)

DUMP_OBJS := $(filter-out $(BUILD)/hostobj/main.o,$(HOST_OBJS))
$(DUMP): tests/dump_pbm.c $(DUMP_OBJS) src/gfx/font_data.c
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) tests/dump_pbm.c $(DUMP_OBJS) -o $@

# ---------------- 验证 ----------------
verify: $(TARGET)
	$(READELF) -h $(TARGET) > $(ABI_REPORT)
	$(READELF) -l $(TARGET) >> $(ABI_REPORT)
	$(CROSS_COMPILE)size $(TARGET) | tee $(SIZE_REPORT)
	@echo "--- ABI 核验 ---"
	@! grep -q 'INTERP' $(ABI_REPORT) && echo "OK: 无动态解释器(纯静态)"
	@grep -q 'MIPS R3000' $(ABI_REPORT) && echo "OK: MIPS32 架构"
	@grep -q 'mips32r2' $(ABI_REPORT) && echo "OK: mips32r2"
	@echo "--- 尺寸 ---"
	@cat $(SIZE_REPORT)

# ---------------- 音效接入完整性检查 ----------------
acheck:
	sh tools/check_audio.sh

# ---------------- 清理 ----------------
clean:
	rm -rf $(BUILD)

.PHONY: all target host-test dump verify clean acheck
