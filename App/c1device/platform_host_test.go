//go:build !linux || !mipsle

package c1device

import (
	"image"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/jupiterrider/purego-sdl3/sdl"
)

// 窗口标题必须带上当前倍数（缩放后要同步更新）与快捷键提示。
func TestSimWindowTitle(t *testing.T) {
	title := simWindowTitle(3)
	if !strings.Contains(title, "3x") {
		t.Fatalf("标题应显示当前倍数, got %q", title)
	}
	if !strings.Contains(title, "README.md") {
		t.Fatalf("标题应提示查看 README.md, got %q", title)
	}
	if !strings.Contains(title, "Ctrl+-") {
		t.Fatalf("标题应提示缩小快捷键, got %q", title)
	}
	if simWindowTitle(3) == simWindowTitle(4) {
		t.Fatal("倍数变化时标题必须变化")
	}
}

func TestSimScaleKey(t *testing.T) {
	cases := []struct {
		name string
		from int32
		sc   sdl.Scancode
		want int32
	}{
		{"zoom in", 4, sdl.ScancodeEquals, 5},
		{"zoom out", 4, sdl.ScancodeMinus, 3},
		{"clamp at max", 8, sdl.ScancodeEquals, 8},
		{"clamp at min", 1, sdl.ScancodeMinus, 1},
		{"reset to 1:1", 6, sdl.Scancode0, 1},
	}
	for _, c := range cases {
		got, ok := simScaleKey(c.sc, c.from)
		if !ok {
			t.Fatalf("%s: not recognized as a scale key", c.name)
		}
		if got != c.want {
			t.Fatalf("%s: scale = %d, want %d", c.name, got, c.want)
		}
	}
}

// 缩放键必须只认 Ctrl 组合（由 pump 判定），普通方向键不能被吞掉。
func TestSimScaleKeyIgnoresOtherKeys(t *testing.T) {
	for _, sc := range []sdl.Scancode{sdl.ScancodeUp, sdl.ScancodeDown, sdl.ScancodeReturn, sdl.ScancodeEscape} {
		if _, ok := simScaleKey(sc, 4); ok {
			t.Fatalf("scancode %v 不应被当作缩放键（会吞掉应用输入）", sc)
		}
	}
}

// 残影：上一次是黑、这一次变白的像素应留一层灰，而不是纯白。
func TestPaintContentGhost(t *testing.T) {
	pix := make([]byte, DisplayWidth*DisplayHeight*4)
	gray := make([]uint8, DisplayWidth*DisplayHeight)

	var first Frame
	setPixel(&first, 0, 0, true)
	ghostFrom := paintContent(pix, gray, first, nil, 192, true)
	if pix[0] != 0x00 {
		t.Fatalf("新画的黑点应为 0x00, got %#x", pix[0])
	}

	var second Frame // 全白：刚才那个黑点现在变白
	ghostFrom = paintContent(pix, gray, second, ghostFrom, 192, true)
	if pix[0] != 192 {
		t.Fatalf("变白的旧黑点应留残影 192, got %#x", pix[0])
	}
	if ghostFrom[0] {
		t.Fatal("全白帧后 ghostFrom[0] 应为 false")
	}
}

// 全刷会清除残影——这正是应用"每 12 次全刷一次"的意义。
func TestFullRefreshClearsGhost(t *testing.T) {
	pix := make([]byte, DisplayWidth*DisplayHeight*4)
	gray := make([]uint8, DisplayWidth*DisplayHeight)

	var first Frame
	setPixel(&first, 0, 0, true)
	ghostFrom := paintContent(pix, gray, first, nil, 192, true)

	var second Frame
	ghostFrom = paintContent(pix, gray, second, ghostFrom, 192, false) // applyGhost=false
	if pix[0] != 0xFF {
		t.Fatalf("全刷后应无残影(纯白), got %#x", pix[0])
	}
	if ghostFrom[0] {
		t.Fatal("全刷后 ghostFrom 应反映当前帧（全白）")
	}
}

// ghost=255 等于关闭残影。
func TestGhostLevel255Disables(t *testing.T) {
	pix := make([]byte, DisplayWidth*DisplayHeight*4)
	gray := make([]uint8, DisplayWidth*DisplayHeight)

	var first Frame
	setPixel(&first, 5, 5, true)
	ghostFrom := paintContent(pix, gray, first, nil, 255, true)

	var second Frame
	idx := (5*DisplayWidth + 5) * 4
	paintContent(pix, gray, second, ghostFrom, 255, true)
	if pix[idx] != 0xFF {
		t.Fatalf("ghost=255 时不应有可见残影, got %#x", pix[idx])
	}
}

func TestPaintFlat(t *testing.T) {
	pix := make([]byte, DisplayWidth*DisplayHeight*4)
	paintFlat(pix, 0x00)
	for i := 0; i < DisplayWidth*DisplayHeight; i++ {
		if pix[i*4] != 0x00 {
			t.Fatalf("像素 %d 未填黑", i)
		}
	}
	paintFlat(pix, 0xFF)
	for i := 0; i < DisplayWidth*DisplayHeight; i++ {
		if pix[i*4] != 0xFF {
			t.Fatalf("像素 %d 未填白", i)
		}
	}
}

// 导出的 PNG 必须能被第三方解码器读回，且尺寸/像素都对。
func TestWriteGrayPNG(t *testing.T) {
	// 故意放在不存在的子目录里，顺便验证会自动建目录
	path := filepath.Join(t.TempDir(), "shots", "frame.png")

	gray := make([]uint8, DisplayWidth*DisplayHeight)
	for i := range gray {
		gray[i] = 0xFF
	}
	gray[0] = 0x00 // 左上角一个黑点

	if err := writeGrayPNG(path, gray); err != nil {
		t.Fatalf("writeGrayPNG: %v", err)
	}

	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("导出文件不存在: %v", err)
	}
	defer f.Close()

	img, err := png.Decode(f)
	if err != nil {
		t.Fatalf("导出的不是合法 PNG: %v", err)
	}
	if img.Bounds().Dx() != DisplayWidth || img.Bounds().Dy() != DisplayHeight {
		t.Fatalf("尺寸 = %dx%d, want %dx%d", img.Bounds().Dx(), img.Bounds().Dy(), DisplayWidth, DisplayHeight)
	}
	g, ok := img.(*image.Gray)
	if !ok {
		t.Fatalf("应为灰度图, got %T", img)
	}
	if g.GrayAt(0, 0).Y != 0x00 {
		t.Fatalf("(0,0) 应为黑, got %#x", g.GrayAt(0, 0).Y)
	}
	if g.GrayAt(1, 0).Y != 0xFF {
		t.Fatalf("(1,0) 应为白, got %#x", g.GrayAt(1, 0).Y)
	}
}

// 端到端：无头模式收到一帧后导出，并且不会卡住（无头路径不碰 SDL，可直接测）。
func TestRunHeadlessWritesFrameAndReturns(t *testing.T) {
	path := filepath.Join(t.TempDir(), "out.png")

	p := &hostPlatform{
		out:  make(chan Event, 4),
		draw: make(chan drawReq, 1),
		quit: make(chan struct{}),
		done: make(chan struct{}),
		opts: simOptions{headless: true, dump: path},
	}

	var frame Frame
	setPixel(&frame, 10, 10, true)
	p.draw <- drawReq{frame: frame, full: true}

	done := make(chan struct{})
	go func() { p.runHeadless(); close(done) }()

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("runHeadless 未返回（会挂住应用）")
	}

	if _, err := os.Stat(path); err != nil {
		t.Fatalf("未导出文件: %v", err)
	}
}

func TestSeqFramePath(t *testing.T) {
	cases := []struct {
		base string
		n    int
		want string
	}{
		{"out/demo.png", 3, "out/demo_0003.png"},
		{"demo.png", 16, "demo_0016.png"},
		{"frame", 2, "frame_0002.png"}, // 无扩展名时补 .png
	}
	for _, c := range cases {
		if got := seqFramePath(c.base, c.n); got != c.want {
			t.Fatalf("seqFramePath(%q, %d) = %q, want %q", c.base, c.n, got, c.want)
		}
	}
}

// 帧序列必须导满 frames 帧就停，且文件名带 4 位序号。
// 模拟一个"收到按键就重绘"的应用：runHeadless 注入按键 → 我们回一帧。
func TestRunHeadlessExportsSequenceAndStops(t *testing.T) {
	dir := t.TempDir()
	base := filepath.Join(dir, "demo.png")
	frames := 4

	p := &hostPlatform{
		out:  make(chan Event, 16),
		draw: make(chan drawReq, 1),
		quit: make(chan struct{}),
		done: make(chan struct{}),
		opts: simOptions{
			headless:   true,
			dump:       base,
			frames:     frames,
			frameDelay: 0,
			timeout:    10 * time.Second,
		},
	}

	var frame Frame
	setPixel(&frame, 10, 10, true)
	p.draw <- drawReq{frame: frame, full: true}

	// 假应用：每收到一个按键就回一帧
	go func() {
		for range p.out {
			p.draw <- drawReq{frame: frame, full: false}
		}
	}()

	done := make(chan struct{})
	go func() { p.runHeadless(); close(done) }()

	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("多帧导出未按帧数上限收工（会挂住）")
	}

	for i := 1; i <= frames; i++ {
		path := seqFramePath(base, i)
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("缺少第 %d 帧 %s: %v", i, filepath.Base(path), err)
		}
	}
	if _, err := os.Stat(seqFramePath(base, frames+1)); err == nil {
		t.Fatalf("导出了第 %d 帧，超过上限 %d", frames+1, frames)
	}
}

// 应用不再重绘时，靠总超时收工，绝不挂住。
func TestRunHeadlessTimeoutStops(t *testing.T) {
	p := &hostPlatform{
		out:  make(chan Event, 16),
		draw: make(chan drawReq, 1),
		quit: make(chan struct{}),
		done: make(chan struct{}),
		opts: simOptions{
			headless:   true,
			dump:       filepath.Join(t.TempDir(), "demo.png"),
			frames:     16,
			frameDelay: 0,
			timeout:    200 * time.Millisecond,
		},
	}

	var frame Frame
	setPixel(&frame, 10, 10, true)
	p.draw <- drawReq{frame: frame, full: true}
	// 之后不再回帧，模拟应用卡住不再重绘

	done := make(chan struct{})
	go func() { p.runHeadless(); close(done) }()

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("应用不重绘时未按总超时收工（会挂住）")
	}
}

func TestReadSimOptionsFrameSequence(t *testing.T) {
	t.Setenv("C1SIM_FRAMES", "16")
	if got := readSimOptions().frames; got != 16 {
		t.Fatalf("frames = %d, want 16", got)
	}
	t.Setenv("C1SIM_FRAMES", "1000")
	if got := readSimOptions().frames; got != simMaxFrames {
		t.Fatalf("frames 应被限制在 %d, got %d", simMaxFrames, got)
	}
	t.Setenv("C1SIM_FRAMES", "0")
	if got := readSimOptions().frames; got != 1 {
		t.Fatalf("frames 至少为 1, got %d", got)
	}

	t.Setenv("C1SIM_FRAME_DELAY", "50")
	if got := readSimOptions().frameDelay; got != 50*time.Millisecond {
		t.Fatalf("frameDelay = %v", got)
	}
	t.Setenv("C1SIM_FRAME_DELAY", "")
	if got := readSimOptions().frameDelay; got != simDefaultFrameDelay {
		t.Fatalf("未设置时应回落到 %v, got %v", simDefaultFrameDelay, got)
	}

	t.Setenv("C1SIM_TIMEOUT", "3")
	if got := readSimOptions().timeout; got != 3*time.Second {
		t.Fatalf("timeout = %v", got)
	}
}

// 帧序列脚本里的每个键都必须让应用重绘，否则 Draw 会去重，
// 序列就会停在那个画面上白等到超时。
//
// 这层防不住"别的应用不处理某个键"的情况（c1device 不能反查 apps/demo），
// 所以脚本只取 Demo 一定会改变状态的键，并且应用换实现时要复核这个列表。
func TestSimScriptKeysAllChangeRender(t *testing.T) {
	// Demo 不处理 Left/Right/Unknown；Back 会让应用直接退出。
	inert := map[Key]bool{
		KeyLeft:    true,
		KeyRight:   true,
		KeyUnknown: true,
		KeyBack:    true,
	}
	for i, k := range simScriptKeys {
		if inert[k] {
			t.Fatalf("simScriptKeys[%d] = %v 不改变渲染，序列会停滞", i, k)
		}
		if k == KeyUnknown {
			t.Fatalf("simScriptKeys[%d] 是 KeyUnknown", i)
		}
	}
	for i := 1; i < len(simScriptKeys); i++ {
		if simScriptKeys[i] == simScriptKeys[i-1] {
			t.Fatalf("simScriptKeys[%d] 与前一个相同（%v），相邻重复没有意义", i, simScriptKeys[i])
		}
	}
}

func TestReadSimOptionsHeadless(t *testing.T) {
	t.Setenv("C1SIM_HEADLESS", "1")
	if !readSimOptions().headless {
		t.Fatal("C1SIM_HEADLESS=1 应开启无头模式")
	}
	t.Setenv("C1SIM_DUMP", "out/demo.png")
	if got := readSimOptions().dump; got != "out/demo.png" {
		t.Fatalf("dump = %q, want out/demo.png", got)
	}
	t.Setenv("C1SIM_DUMP", "")
	if got := readSimOptions().dump; got != simDefaultDump {
		t.Fatalf("未设置时应回落到 %q, got %q", simDefaultDump, got)
	}
}

func TestReadSimOptionsTimingAndGhost(t *testing.T) {
	t.Setenv("C1SIM_TIMING", "0")
	if readSimOptions().timing {
		t.Fatal("C1SIM_TIMING=0 应关闭时序模拟")
	}
	t.Setenv("C1SIM_TIMING", "1")
	if !readSimOptions().timing {
		t.Fatal("C1SIM_TIMING=1 应开启时序模拟")
	}
	t.Setenv("C1SIM_GHOST", "999")
	if got := readSimOptions().ghost; got != 255 {
		t.Fatalf("ghost 应钳到 255, got %d", got)
	}
}

func TestReadSimOptionsClampsScale(t *testing.T) {
	t.Setenv("C1SIM_SCALE", "99")
	if got := readSimOptions().scale; got != simMaxScale {
		t.Fatalf("C1SIM_SCALE=99 should clamp to %d, got %d", simMaxScale, got)
	}
	t.Setenv("C1SIM_SCALE", "0")
	if got := readSimOptions().scale; got != simMinScale {
		t.Fatalf("C1SIM_SCALE=0 should clamp to %d, got %d", simMinScale, got)
	}
	t.Setenv("C1SIM_SCALE", "abc") // non-numeric falls back to default
	if got := readSimOptions().scale; got != simDefaultScale {
		t.Fatalf("invalid value should fall back to %d, got %d", simDefaultScale, got)
	}
}
