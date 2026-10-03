//go:build (!linux || !mipsle) && !headless

package c1device

// 窗口变体：链接 SDL3，把帧渲染到屏幕并还原墨水屏观感。
// 仅在默认（非 headless）host 构建下编译；无头变体见 platform_headless.go。
//
// pump() 是唯一接触 SDL 的 goroutine：SDL 的建窗、渲染、事件泵都在这里，
// 且第一条语句是 runtime.LockOSThread()，保证全部 SDL 调用留在同一 OS 线程。
import (
	"fmt"
	"runtime"
	"time"
	"unsafe"

	"github.com/jupiterrider/purego-sdl3/sdl"
)

// pump 是窗口变体的事件/渲染循环，也是唯一接触 SDL 的 goroutine。
func (p *hostPlatform) pump() {
	runtime.LockOSThread()

	defer close(p.done)
	// 关闭 out 之前已投递的事件仍可被应用读到（Go 带缓冲 channel 的语义），
	// 所以"关窗 = 按返回键"能让应用走正常退出路径而不是报错。
	defer close(p.out)

	defer sdl.Quit()
	if !sdl.Init(sdl.InitVideo) {
		return
	}

	scale := p.opts.scale

	var window *sdl.Window
	var renderer *sdl.Renderer
	if !sdl.CreateWindowAndRenderer(simWindowTitle(scale),
		DisplayWidth*scale, DisplayHeight*scale, 0, &window, &renderer) {
		return
	}
	defer sdl.DestroyWindow(window)
	defer sdl.DestroyRenderer(renderer)

	texture := sdl.CreateTexture(renderer, sdl.PixelFormatRGBA8888,
		sdl.TextureAccessStreaming, DisplayWidth, DisplayHeight)
	if texture == nil {
		return
	}
	defer sdl.DestroyTexture(texture)
	// 最近邻：放大后保持 1bpp 的硬边，不被插值糊掉
	sdl.SetTextureScaleMode(texture, sdl.ScaleModeNearest)

	dst := sdl.FRect{
		W: float32(DisplayWidth * scale),
		H: float32(DisplayHeight * scale),
	}

	// RGBA 像素缓冲：alpha 恒定，灰度每次由帧解码写入
	pix := make([]byte, DisplayWidth*DisplayHeight*4)
	for i := 0; i < DisplayWidth*DisplayHeight; i++ {
		pix[i*4+3] = 0xFF
	}
	gray := make([]uint8, DisplayWidth*DisplayHeight)
	for i := range gray {
		gray[i] = 0xFF // 起始白屏
	}

	// 当前按住的键 → 下一次产生重复事件的时间（仅记录可重复的键）
	held := map[sdl.Scancode]time.Time{}

	// 显示状态
	var (
		cur         Frame  // 待显示/正在显示的内容
		haveContent bool   // 是否已经收到过一帧
		ghostFrom   []bool // 上一次内容的黑点掩码，用于算残影
		flashing    bool   // 是否正在走全刷闪烁
		flashStart  time.Time
		postFlash   bool // 闪烁刚结束，本帧应以"无残影"的干净画面呈现
	)

	for {
		// 1) 抽干输入
		var event sdl.Event
		for sdl.PollEvent(&event) {
			switch event.Type() {
			case sdl.EventQuit:
				// 关窗口等价于按返回键
				p.emit(Event{Key: KeyBack})
				return
			case sdl.EventKeyDown:
				key := event.Key()
				// Ctrl 组合键是模拟器自身的控制键，不转发给应用
				if key.Mod&sdl.KeymodCtrl != 0 {
					if s, ok := simScaleKey(key.Scancode, scale); ok && s != scale {
						scale = s
						sdl.SetWindowSize(window, DisplayWidth*scale, DisplayHeight*scale)
						sdl.SetWindowTitle(window, simWindowTitle(scale)) // 标题里的倍数要跟着变
						dst.W = float32(DisplayWidth * scale)
						dst.H = float32(DisplayHeight * scale)
					}
					continue
				}
				if key.Repeat {
					continue // 重复由下面按软件节奏产生，忽略 OS 的重复事件
				}
				if ev, ok := mapSDLScancode(key.Scancode); ok {
					p.emit(ev)
					if repeatableKey(ev.Key) {
						if _, already := held[key.Scancode]; !already {
							held[key.Scancode] = time.Now().Add(repeatDelay)
						}
					}
				}
			case sdl.EventKeyUp:
				delete(held, event.Key().Scancode)
			case sdl.EventWindowFocusLost:
				// 失焦后收不到 keyup，必须清空，否则会一直重复下去
				for sc := range held {
					delete(held, sc)
				}
			}
		}

		// 1b) 软件自动重复：长按导航/音量键时持续产生 Repeat=true 的事件
		if len(held) > 0 {
			now := time.Now()
			for sc, next := range held {
				if now.Before(next) {
					continue
				}
				if ev, ok := mapSDLScancode(sc); ok && repeatableKey(ev.Key) {
					p.emit(Event{Key: ev.Key, Repeat: true})
				}
				held[sc] = now.Add(repeatInterval)
			}
		}

		// 2) 取最新的绘制请求（cap=1，最新胜）
		select {
		case req := <-p.draw:
			cur = req.frame
			haveContent = true
			if req.full {
				if p.opts.timing {
					flashing = true
					flashStart = time.Now()
				} else {
					postFlash = true // 不做动画，但语义上仍是"全刷后画面干净"
				}
			}
		default:
		}

		// 3) 计算本帧该显示什么
		switch {
		case flashing:
			elapsed := float64(time.Since(flashStart).Milliseconds()) / float64(fullRefreshMs)
			switch {
			case elapsed < flashWhite1End:
				paintFlat(pix, 0xFF)
			case elapsed < flashBlackEnd:
				paintFlat(pix, 0x00)
			case elapsed < flashWhite2End:
				paintFlat(pix, 0xFF)
			default:
				flashing = false
				postFlash = true
				ghostFrom = paintContent(pix, gray, cur, ghostFrom, p.opts.ghost, false)
			}
		case haveContent && postFlash:
			postFlash = false
			ghostFrom = paintContent(pix, gray, cur, ghostFrom, p.opts.ghost, false)
		case haveContent:
			ghostFrom = paintContent(pix, gray, cur, ghostFrom, p.opts.ghost, true)
		}

		// 4) 上屏
		if haveContent || flashing {
			sdl.UpdateTexture(texture, nil, unsafe.Pointer(&pix[0]), DisplayWidth*4)
		}
		sdl.SetRenderDrawColor(renderer, 0, 0, 0, 0xFF)
		sdl.RenderClear(renderer)
		sdl.RenderTexture(renderer, texture, nil, &dst)
		sdl.RenderPresent(renderer)

		select {
		case <-p.quit:
			return
		default:
		}

		time.Sleep(8 * time.Millisecond) // 约 120fps，够跟手又不空转
	}
}

// simWindowTitle 窗口标题：始终显示当前放大倍数与常用按键提示，
// 免得忘掉 Ctrl+- 这类模拟器自身的快捷键（完整说明见 README.md）。
func simWindowTitle(scale int32) string {
	return fmt.Sprintf("C1-Slim Simulator %dx | Esc退出 | Ctrl+-缩小 Ctrl+=放大 Ctrl+0原尺寸 | 用法见 README.md", scale)
}

// simScaleKey 把 Ctrl 组合键映射成新的放大倍数。
//
//	Ctrl+= 放大   Ctrl+- 缩小   Ctrl+0 回到 1:1
func simScaleKey(sc sdl.Scancode, cur int32) (int32, bool) {
	switch sc {
	case sdl.ScancodeEquals:
		return min(cur+1, simMaxScale), true
	case sdl.ScancodeMinus:
		return max(cur-1, simMinScale), true
	case sdl.Scancode0:
		return 1, true
	}
	return cur, false
}

// emit 非阻塞投递：队列满时丢弃最旧的一条，避免 SDL 线程被卡住。
func (p *hostPlatform) emit(ev Event) {
	select {
	case p.out <- ev:
	default:
		select {
		case <-p.out:
		default:
		}
		select {
		case p.out <- ev:
		default:
		}
	}
}
