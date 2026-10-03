//go:build !linux || !mipsle

package c1device

import (
	"testing"

	"github.com/jupiterrider/purego-sdl3/sdl"
)

// 两套键位表必须为"双端共有的控制键"给出相同的 Event。
//
// 这不是重复测试：mapKey（evdev，真机硬件矩阵）与 mapSDLScancode（SDL，PC
// 键盘）是两个独立实现，各自都有测试却从不相交。双端通用架构的核心承诺是
// "同一物理按键在两端产生同一 Event"——一旦某端漏了一个控制键，UI 行为就会
// 在模拟器上看正常、在真机上失效。
func TestHostAndDeviceAgreeOnControlKeys(t *testing.T) {
	// 双端都应支持的键：一边写 evdev code，一边写等价 SDL scancode。
	pairs := []struct {
		name     string
		evdev    uint16
		scancode sdl.Scancode
	}{
		{"Up", 103, sdl.ScancodeUp},
		{"Down", 108, sdl.ScancodeDown},
		{"Left", 105, sdl.ScancodeLeft},
		{"Right", 106, sdl.ScancodeRight},
		{"OK", 28, sdl.ScancodeReturn},
		{"Back", 102, sdl.ScancodeEscape},
		{"Pause", 25, sdl.ScancodeSpace},
		{"VolumeDown", 114, sdl.ScancodeMinus},
		{"VolumeUp", 115, sdl.ScancodeEquals},
	}

	for _, p := range pairs {
		t.Run(p.name, func(t *testing.T) {
			deviceEvent, deviceOK := mapKey(p.evdev)
			if !deviceOK {
				t.Fatalf("mapKey(%d) 未识别 %s，真机上该键无效", p.evdev, p.name)
			}
			hostEvent, hostOK := mapSDLScancode(p.scancode)
			if !hostOK {
				t.Fatalf("mapSDLScancode(%v) 未识别 %s，模拟器上该键无效", p.scancode, p.name)
			}
			if hostEvent != deviceEvent {
				t.Errorf("%s 双端不一致: device mapKey(%d)=%+v, host mapSDLScancode(%v)=%+v",
					p.name, p.evdev, deviceEvent, p.scancode, hostEvent)
			}
		})
	}
}

// Backspace 在两端都必须产生 '\b'，否则编辑行为会分叉。
func TestHostAndDeviceAgreeOnBackspace(t *testing.T) {
	hostEvent, ok := mapSDLScancode(sdl.ScancodeBackspace)
	if !ok || hostEvent.Key != KeyRune || hostEvent.Rune != '\b' {
		t.Fatalf("host Backspace = %+v,%v; want KeyRune '\\b'", hostEvent, ok)
	}
	// evdev 14 = KEY_BACKSPACE (111是第二个 backspace 键位)
	for _, code := range []uint16{14, 111} {
		ev, ok := mapKey(code)
		if !ok || ev.Key != KeyRune || ev.Rune != '\b' {
			t.Errorf("mapKey(%d) = %+v,%v; want KeyRune '\\b'", code, ev, ok)
		}
	}
}

// 真机的字符集刻意比模拟器小：只有 qwertyuio/z/v/. 和数字。
// 这不是 bug（真机没有全键盘），但必须显式记录，否则后人可能误以为
// 两端应当一致而去"修正"其中一边。
func TestDeviceCharsetIsSmallerThanHost(t *testing.T) {
	// 真机 mapKey 支持的字母，按 evdev code 16..24 映射到 "qwertyuio"。
	const deviceLetters = "qwertyuio"
	for code := uint16(16); code < 16+uint16(len(deviceLetters)); code++ {
		r := rune(deviceLetters[code-16])
		got, ok := mapKey(code)
		if !ok || got.Key != KeyRune || got.Rune != r {
			t.Errorf("mapKey(%d) = %+v,%v; want KeyRune %q", code, got, ok, r)
		}
		//同一个字母在模拟器上也必须可得。
		hgot, hok := scancodeRune(uint32(sdl.ScancodeA + sdl.Scancode(r-'a')))
		if !hok || hgot != r {
			t.Errorf("模拟器应支持字母 %q，得到 %q,%v", r, hgot, hok)
		}
	}

	// 模拟器支持、真机不支持的字母——这是有意差异，把它记录下来。
	deviceSet := map[rune]bool{}
	for _, d := range deviceLetters {
		deviceSet[rune(d)] = true
	}
	hostOnly := 0
	for r := rune('a'); r <= 'z'; r++ {
		if _, ok := scancodeRune(uint32(sdl.ScancodeA + sdl.Scancode(r-'a'))); ok && !deviceSet[r] {
			hostOnly++
		}
	}
	if hostOnly == 0 {
		t.Error("预期模拟器的字母集大于真机；若真机已支持全部字母，本测试应更新")
	}
}
