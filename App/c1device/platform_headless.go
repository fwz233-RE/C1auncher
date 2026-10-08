//go:build (!linux || !mipsle) && headless

package c1device

// 无头变体：完全不链接 SDL3，只能把帧导出成 PNG（CI / 自动化 / 无显卡环境）。
// 与窗口变体互斥——二选一由 build tag 决定，所以本文件不需要也不允许 import SDL。
//
// 因为不触碰任何 SDL，这个变体：
//   - 不需要安装 SDL3 运行时（也不需要 cgo）；
//   - 可以跑在纯 Linux / CI 容器里做"渲染确定性回归"；
//   - go.mod 里的 purego-sdl3 依赖对它而言是死依赖，可忽略（默认构建仍需要）。
import "runtime"

// pump 是纯无头循环：锁定 OS 线程、关闭通道约定不变，直接把帧交给 runHeadless 导出。
func (p *hostPlatform) pump() {
	runtime.LockOSThread()

	defer close(p.done)
	// 关闭 out 之前已投递的事件仍可被应用读到（Go 带缓冲 channel 的语义），
	// 所以"导出完 = 按返回键"能让应用走正常退出路径而不是报错。
	defer close(p.out)

	p.runHeadless()
}
