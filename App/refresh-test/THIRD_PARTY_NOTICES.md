# Refresh Test 第三方许可说明

- 本应用自研源码沿用本仓库 GNU GPL v3；完整许可文本位于 `C1ancher/LICENSE`，构建时复制为 `licenses/GPL-v3.txt`。
- 设备二进制静态包含 Go 运行时（Go Authors，BSD 3-Clause）和 `golang.org/x/sys` v0.47.0（Go Authors，BSD 3-Clause）。构建脚本从实际使用的 Go 工具链及模块目录复制完整许可为 `licenses/Go-LICENSE.txt` 和 `licenses/x-sys-LICENSE.txt`。
- 设备显示独占锁、输入事件读取和返回桌面的实现复用本仓库 `App/badapple` 的 GPL v3 代码；本应用不包含 Bad Apple 视频、音频或预处理素材。
- 测试画面由程序生成，不依赖外部媒体素材或字体文件。
- 本应用仅使用现有驱动的写屏及刷新控制接口，不包含第三方自定义电子纸波形表，也不修改驱动波形。
