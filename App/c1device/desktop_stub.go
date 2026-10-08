// 与 platform_host.go 的 tag 对称：只有真机（linux/mipsle）才有真实实现，
// 模拟器（任意平台，含 linux/amd64）一律空实现——否则在 Linux 上跑模拟器时
// 会误杀父进程。
//go:build !linux || !mipsle

package c1device

func ReturnToDesktop() {}
