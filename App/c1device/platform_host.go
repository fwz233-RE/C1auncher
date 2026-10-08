//go:build !linux || !mipsle

package c1device

// PC 模拟器后端：把 5624 字节的 1bpp 帧渲染/导出，并还原墨水屏观感。
//
// 本文件是 host 侧（PC）平台的公共骨架，不依赖 SDL，窗口与无头两种变体共用。
// 两种变体通过 build tag 选择：
//
//   - 默认（`!headless`）：链接 SDL3 的窗口引擎，见 platform_window.go。
//   - `-tags headless`：完全不链接 SDL3，只能把帧导出成 PNG（CI / 自动化友好），
//     见 platform_headless.go。无头变体因此不需要 SDL3 运行时，也不引入 cgo。
//
// 设计要点
//
//  1. 与真机一致的语义：帧去重、首帧强制全刷（见 Draw）。
//  2. 线程模型：SDL 的建窗、渲染、事件泵必须在同一个 OS 线程。
//     因此窗口变体的 pump() 第一条语句是 runtime.LockOSThread()，且**所有** SDL 调用
//     都只发生在这个 goroutine 里；Draw() 只往 channel 投递帧，绝不碰 SDL。
//  3. 双端分派：host 文件带 !linux || !mipsle，交叉编译到 linux/mipsle 时不参与编译，
//     因此真机产物不含任何 SDL 依赖（也不需要 cgo）。
//
// 模拟器开关（环境变量，详见 README）：
//
//	C1SIM_SCALE	窗口放大倍数，1..8，默认 4
//	C1SIM_TIMING	1（默认）模拟刷新时序：全刷走"白→黑→白"闪烁，约 700ms
//			0 立即显示，便于截图与自动化
//	C1SIM_GHOST	残影灰度 0..255，越大越淡，255 等于关闭，默认 192
//	C1SIM_HEADLESS	1 无头模式：不开窗口，把帧导出成 PNG 后让应用正常退出
//	C1SIM_DUMP	无头模式的输出路径，默认 frame.png
//			C1SIM_FRAMES>1 时作为文件名前缀，产出 <前缀>_0001.png 等序列
//	C1SIM_FRAMES	无头模式导出的帧数，1（默认）只导首帧到 C1SIM_DUMP；
//			2..64 导出带序号的帧序列，并自动注入合成按键驱动画面变化
//	C1SIM_FRAME_DELAY	帧序列两帧的间隔毫秒数，默认 120，避免无节流狂写磁盘
//	C1SIM_TIMEOUT	无头模式的总超时，默认 10s，到点强制退出，绝不挂住
//
// 注意：C1SIM_HEADLESS 环境变量只在默认（窗口）变体里做"运行时分流"；
// 用 `-tags headless` 构建时整个程序都是无头的，不需要也不链接 SDL3。
import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	simDefaultScale = 4
	simMinScale     = 1
	simMaxScale     = 8

	simDefaultGhost = 192
)

// 刷新时序取自 theBillLee/c1-slim 的实测：写入约 150ms，全刷约 700ms。
const (
	fullRefreshMs  = 700
	partialWriteMs = 150
)

// 全刷闪烁的阶段划分（占 fullRefreshMs 的比例）：白 → 黑 → 白 → 内容
const (
	flashWhite1End = 0.30
	flashBlackEnd  = 0.55
	flashWhite2End = 0.85
)

// simDefaultDump 是无头模式默认的输出文件名（相对当前工作目录）。
const simDefaultDump = "frame.png"

// 无头模式的导出参数默认值与上界。
const (
	simDefaultFrames = 1
	simMaxFrames     = 64

	simDefaultFrameDelay = 120 * time.Millisecond
	simMinFrameDelay     = 0
	simMaxFrameDelay     = 5 * time.Second

	simDefaultTimeout = 10 * time.Second
)

// simScriptKeys 是帧序列模式下自动注入的按键序列，用来驱动应用重绘。
//
// 每个键都必须改变渲染结果：Draw 对相同内容会去重，若某个键不产生新内容，
// 序列就会停在那个画面上白等到超时。脚本在 frames 用尽后循环，因此它是
// "周期"而非"覆盖全部交互"——应用是有限状态机时，绕一圈后会回到相同画面，
// 这属于预期（重复帧本身也是有效的回归基线）。
var simScriptKeys = []Key{
	KeyDown, KeyOK, KeyUp, KeyPause,
	KeyVolumeUp, KeyVolumeDown, KeyOK, KeyDown,
}

// simOptions 是模拟器的运行参数，全部来自环境变量，在 OpenPlatform 时读取一次。
type simOptions struct {
	scale      int32
	timing     bool
	ghost      uint8
	headless   bool
	dump       string
	frames     int
	frameDelay time.Duration
	timeout    time.Duration
}

func envInt(name string, def int) int {
	if v := os.Getenv(name); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func readSimOptions() simOptions {
	scale := int32(envInt("C1SIM_SCALE", simDefaultScale))
	if scale < simMinScale {
		scale = simMinScale
	}
	if scale > simMaxScale {
		scale = simMaxScale
	}

	ghost := envInt("C1SIM_GHOST", simDefaultGhost)
	if ghost < 0 {
		ghost = 0
	}
	if ghost > 255 {
		ghost = 255
	}

	dump := os.Getenv("C1SIM_DUMP")
	if dump == "" {
		dump = simDefaultDump
	}

	frames := envInt("C1SIM_FRAMES", simDefaultFrames)
	if frames < 1 {
		frames = 1
	}
	if frames > simMaxFrames {
		frames = simMaxFrames
	}

	delay := time.Duration(envInt("C1SIM_FRAME_DELAY", int(simDefaultFrameDelay/time.Millisecond))) * time.Millisecond
	if delay < simMinFrameDelay {
		delay = simMinFrameDelay
	}
	if delay > simMaxFrameDelay {
		delay = simMaxFrameDelay
	}

	timeout := time.Duration(envInt("C1SIM_TIMEOUT", int(simDefaultTimeout/time.Second))) * time.Second
	if timeout <= 0 {
		timeout = simDefaultTimeout
	}

	return simOptions{
		scale:      scale,
		timing:     envInt("C1SIM_TIMING", 1) != 0,
		ghost:      uint8(ghost),
		headless:   envInt("C1SIM_HEADLESS", 0) != 0,
		dump:       dump,
		frames:     frames,
		frameDelay: delay,
		timeout:    timeout,
	}
}

// writeGrayPNG 把每像素一字节的灰度缓冲写成 PNG（0x00 黑、0xFF 白）。
func writeGrayPNG(path string, gray []uint8) error {
	if dir := filepath.Dir(path); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	img := &image.Gray{
		Pix:    gray,
		Stride: DisplayWidth,
		Rect:   image.Rect(0, 0, DisplayWidth, DisplayHeight),
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// seqFramePath 把 C1SIM_DUMP 当成前缀，生成带序号的帧文件名：
// out/demo.png + 第 3 帧 → out/demo_0003.png。序号固定 4 位，天然按序排列。
func seqFramePath(base string, n int) string {
	ext := filepath.Ext(base)
	if ext == "" {
		ext = ".png"
	}
	return fmt.Sprintf("%s_%04d%s", strings.TrimSuffix(base, ext), n, ext)
}

// runHeadless 无头模式：不初始化 SDL，把帧导出成 PNG 后收工。
// 返回后 pump 的 defer 会关闭事件通道，应用据此正常退出（而不是报错）。
//
// 四道刹车保证一定停得下来：帧数上限、总超时、帧间隔、单帧等待上限。
func (p *hostPlatform) runHeadless() {
	gray := make([]uint8, DisplayWidth*DisplayHeight)
	for i := range gray {
		gray[i] = 0xFF
	}

	total := p.opts.frames
	if total < 1 {
		total = 1
	}
	exported := 0

	// writeFrame 导出一帧并计数；返回 false 表示写盘失败，应立即收工（不重试）。
	writeFrame := func(frame Frame) bool {
		// 无头导出的是"干净"内容：不做闪烁动画，也不叠残影
		DecodeGray(frame, gray)
		path := p.opts.dump
		if total > 1 {
			path = seqFramePath(p.opts.dump, exported+1)
		}
		if err := writeGrayPNG(path, gray); err != nil {
			fmt.Fprintf(os.Stderr, "headless: 导出失败: %v\n", err)
			return false
		}
		exported++
		fmt.Printf("headless: wrote %s (%dx%d) %d/%d\n",
			path, DisplayWidth, DisplayHeight, exported, total)
		return true
	}

	timeout := p.opts.timeout
	if timeout <= 0 {
		timeout = simDefaultTimeout // 零值 simOptions 也要有兜底，绝不立刻超时
	}
	deadline := time.After(timeout)
	keySeq := 0

	for {
		select {
		case req := <-p.draw:
			if !writeFrame(req.frame) {
				return
			}
			if exported >= total {
				return // 帧数够了，正常收工
			}

			// 还想要更多帧：注入下一个按键，让应用重绘出新内容。
			key := simScriptKeys[keySeq%len(simScriptKeys)]
			keySeq++
			select {
			case p.out <- Event{Key: key}:
			case <-p.quit:
				return
			}

			if p.opts.frameDelay > 0 {
				timer := time.NewTimer(p.opts.frameDelay)
				select {
				case <-timer.C:
				case <-p.quit:
					timer.Stop()
					return
				}
			}
		case <-p.quit:
			return
		case <-deadline:
			fmt.Fprintf(os.Stderr, "headless: 超时收工，已导出 %d/%d 帧\n", exported, total)
			return
		}
	}
}

type drawReq struct {
	frame Frame
	full  bool
}

type hostPlatform struct {
	out  chan Event
	draw chan drawReq
	quit chan struct{}
	done chan struct{}

	opts simOptions

	last  Frame // 仅由 Draw（应用 goroutine）访问
	ready bool
	close sync.Once
}

// OpenPlatform 与真机实现同名，靠 build tag 做文件级分派。
func OpenPlatform() (Platform, error) {
	p := &hostPlatform{
		out:  make(chan Event, 32),
		draw: make(chan drawReq, 1),
		quit: make(chan struct{}),
		done: make(chan struct{}),
		opts: readSimOptions(),
	}
	go p.pump()
	return p, nil
}

// paintContent 把帧画进 RGBA 缓冲，并按上一次内容叠加残影。
//
// 残影规则：上一次是黑、这一次变白的像素，留一层灰而不是纯白。
// 全刷会清掉残影（applyGhost=false），这正是应用"每 12 次做一次全刷防残影"的意义所在。
// 返回更新后的"上一次黑点掩码"，供下一次计算残影。
//
// 该函数是 SDL-free 的（只写字节），窗口变体与无头变体都可直接复用；
// 这里放在公共骨架里，避免在变体文件间重复。
func paintContent(pix, gray []uint8, frame Frame, ghostFrom []bool, ghost uint8, applyGhost bool) []bool {
	DecodeGray(frame, gray)

	n := DisplayWidth * DisplayHeight
	if ghostFrom == nil {
		ghostFrom = make([]bool, n)
	}

	for i := 0; i < n; i++ {
		v := gray[i]
		if applyGhost && v == 0xFF && ghostFrom[i] {
			v = ghost
		}
		pix[i*4], pix[i*4+1], pix[i*4+2] = v, v, v
		ghostFrom[i] = gray[i] == 0x00
	}
	return ghostFrom
}

// paintFlat 把整屏填成同一灰度（全刷闪烁用）。
func paintFlat(pix []byte, v uint8) {
	for i := 0; i < len(pix)/4; i++ {
		pix[i*4], pix[i*4+1], pix[i*4+2] = v, v, v
	}
}

// Draw 与真机保持同样语义：相同帧去重、首帧强制全刷。
func (p *hostPlatform) Draw(next Frame, full bool) error {
	if p.ready && next == p.last {
		return nil
	}
	full = full || !p.ready
	p.last, p.ready = next, true

	select {
	case <-p.draw:
	default:
	}
	select {
	case p.draw <- drawReq{frame: next, full: full}:
	case <-p.quit:
	}
	return nil
}

func (p *hostPlatform) Events() <-chan Event { return p.out }

func (p *hostPlatform) Close() error {
	p.close.Do(func() {
		close(p.quit)
		<-p.done
	})
	return nil
}
