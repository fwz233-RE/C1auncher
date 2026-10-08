//go:build !linux || !mipsle

package c1device

// 模拟器的键盘映射：物理键（scancode 数值）→ c1device.Event。
//
// 本文件是 SDL-free 的公共部分，只做"数值 scancode → rune"的换算；
// 真正依赖 SDL 类型、把 scancode 翻译成 Event 的版本在 keymap_window.go
// （仅窗口/默认构建编译）。这样 -tags headless 构建完全不需要 SDL3。
//
// 这里**不复用**包内的 mapKey()：它是面向真机 evdev 硬件键矩阵的私有函数，
// 且覆盖不全（没有 Esc / Space / Tab，缺大量字母）。模拟器直接构造 Event。
import "time"

const (
	// 长按后首次重复的等待时间，以及之后的重复间隔。
	// 取值参照常见实体键盘手感，让"长按翻页"和真机接近。
	repeatDelay    = 450 * time.Millisecond
	repeatInterval = 90 * time.Millisecond
)

// repeatableKey 判定哪些键支持长按自动重复。
// 真机由 evdev 的 value==2 提供；这里用软件节奏复刻。
// 只有导航与音量键重复——打字类按键重复会干扰输入。
func repeatableKey(k Key) bool {
	switch k {
	case KeyUp, KeyDown, KeyLeft, KeyRight, KeyVolumeUp, KeyVolumeDown:
		return true
	}
	return false
}

// scancodeRune 把字母与数字行映射成 rune。用物理键的数值而非字符事件，
// 保证不同键盘布局下行为一致。只做 SDL-free 的数值换算，不引用任何 SDL 类型。
//
// SDL scancode 取值（跨版本稳定）：A=4…Z=29，1=30…9=38，0=39。
func scancodeRune(code uint32) (rune, bool) {
	const (
		scancodeA = 4
		scancodeZ = 29
		scancode1 = 30
		scancode9 = 38
		scancode0 = 39
	)
	switch {
	case code >= scancodeA && code <= scancodeZ:
		return rune('a' + int(code-scancodeA)), true
	case code >= scancode1 && code <= scancode9:
		return rune('1' + int(code-scancode1)), true
	case code == scancode0:
		return '0', true
	}
	return 0, false
}
