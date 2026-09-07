#!/bin/sh
# 发布前音效接入完整性检查(make acheck)
# 标准:
#  1. 每款游戏至少 4 处 audio_ 调用(<5 只提示)
#  2. 每款必须有胜负音(audio_win/audio_lose), 沙盒免检
#  3. ui 层必须有导航音(menu/ui_common 需 tick+select/move; 帮助页无光标, 豁免 tick)
set -e
FAIL=0
SANDBOX='life life2 mushgarden'
for f in src/games/*.c; do
	case "$f" in *games_table.c|*stubs.c) continue;; esac
	base=$(basename "$f" .c)
	n=$(grep -c 'audio_' "$f")
	[ "$n" -lt 4 ] && { echo "FAIL: $f audio=$n (<4)"; FAIL=1; }
	[ "$n" -lt 5 ] && echo "note: $f audio=$n"
	if ! grep -q 'audio_win\|audio_lose' "$f"; then
		case " $SANDBOX " in *" $base "*) ;; *) echo "FAIL: $f missing win/lose"; FAIL=1;; esac
	fi
done
grep -q 'audio_tick' src/ui/menu.c || { echo "FAIL: menu.c missing tick"; FAIL=1; }
grep -q 'audio_select\|audio_move' src/ui/menu.c || { echo "FAIL: menu.c missing select/move"; FAIL=1; }
grep -q 'audio_tick' src/ui/ui_common.c || { echo "FAIL: ui_common.c missing tick"; FAIL=1; }
grep -q 'audio_select\|audio_move' src/ui/ui_common.c || { echo "FAIL: ui_common.c missing select/move"; FAIL=1; }
grep -q 'audio_select\|audio_move' src/ui/help.c || { echo "FAIL: help.c missing select/move"; FAIL=1; }
[ $FAIL -eq 0 ] || { echo "AUDIO CHECK FAILED"; exit 1; }
echo "OK: audio coverage complete"
