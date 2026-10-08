//go:build (!linux || !mipsle) && !headless

package c1device

// 窗口变体的键盘映射：SDL 物理键 → c1device.Event。
// 仅默认（非 headless）host 构建编译；无头变体不需要键盘输入。
import "github.com/jupiterrider/purego-sdl3/sdl"

// mapSDLScancode 把 SDL 扫描码翻译成模拟器事件。
func mapSDLScancode(sc sdl.Scancode) (Event, bool) {
	switch sc {
	// 导航 / 操作
	case sdl.ScancodeUp:
		return Event{Key: KeyUp}, true
	case sdl.ScancodeDown:
		return Event{Key: KeyDown}, true
	case sdl.ScancodeLeft:
		return Event{Key: KeyLeft}, true
	case sdl.ScancodeRight:
		return Event{Key: KeyRight}, true
	case sdl.ScancodeReturn, sdl.ScancodeKpEnter:
		return Event{Key: KeyOK}, true
	case sdl.ScancodeEscape:
		return Event{Key: KeyBack}, true
	case sdl.ScancodeSpace:
		return Event{Key: KeyPause}, true

	// 音量（真机为机身侧键）
	case sdl.ScancodeMinus, sdl.ScancodeLeftBracket:
		return Event{Key: KeyVolumeDown}, true
	case sdl.ScancodeEquals, sdl.ScancodeRightBracket:
		return Event{Key: KeyVolumeUp}, true

	// 编辑键
	case sdl.ScancodeBackspace:
		return Event{Key: KeyRune, Rune: '\b'}, true
	case sdl.ScancodeTab:
		return Event{Key: KeyRune, Rune: '\t'}, true
	}

	if r, ok := scancodeRune(uint32(sc)); ok {
		return Event{Key: KeyRune, Rune: r}, true
	}
	return Event{}, false
}
