# 组件与第三方许可说明

自研设备端、普通应用和示例的许可范围见根 [LICENSE](LICENSE)，完整 GNU GPL v3 正文保留在 [C1ancher/LICENSE](C1ancher/LICENSE)。第三方代码和素材保持原许可，不统一改成 GPL。

## 核心与安装器

- 核心的 libtsm、CCAN 哈希表、wcwidth、Ed25519 和 Neofetch 见 [核心第三方说明](C1ancher/THIRD_PARTY_NOTICES.md)，各自完整许可保留在 `C1ancher/third_party/`。
- 包管理器的 C1 Package Bitmap 是 GNU Unifont 16.0.04 子集，按 SIL Open Font License 1.1 选项分发。见 [字体说明](C1ancher/third_party/pkg_font/README.md) 和 [完整上游声明](C1ancher/third_party/pkg_font/LICENSE.txt)。其字节和换行保持上游状态。
- Windows 安装器涉及 .NET、Bouncy Castle 等，见 [安装器第三方说明](C1ancher/installer/THIRD-PARTY-NOTICES.txt)。源码公开不代表本次另行分发了其完整安装器依赖包。

## 普通应用

- 图片应用的低内存图像解码改编代码保留 [BSD LICENSE](App/pic/internal/lowmem/LICENSE) 及 [说明](App/pic/internal/lowmem/README.md)。
- Go 应用的模块依赖由各自 `go.mod` / `go.sum` 固定，构建时下载。分发自己构建的二进制时还应保留 Go 运行时及所使用依赖的许可；仅分发源码不包含这些模块的完整副本。
- 阅读、音乐、图片应用可能需要字体资源。本仓库没有分发个人电脑上的 MiSans 字体；构建时使用 `-FontPath` 显式指定有合法使用和再分发许可的兼容字体，应用包需附带该字体要求的许可。图片解码库的 BSD LICENSE 不能代替字体许可。
- ChiChuGames 的 Adafruit 字体与 Kenney 音效见 [游戏第三方说明](ChiChuGames/THIRD_PARTY_NOTICES.md)。
- Pinao 的音色和鼓声由程序合成，不附带用户录音或商业音频采样。

## 发布工具附件

`c1publish` 作为独立客户端二进制在 GitHub Release 分发，服务端实现和该客户端源码不包含在本次设备端开源范围内。见 [分发说明](tools/publisher/DISTRIBUTION-NOTE.txt) 与 [Go 第三方许可](tools/publisher/THIRD_PARTY_NOTICES.txt)。根目录源码许可证不自动覆盖这些独立二进制。

## 本次未分发

系统备份、原厂固件、个人媒体、运行日志、凭据和私钥均不属于此源码仓库或开发者工具包。开发者自行增加素材或依赖时，需要另外核对来源和适用许可。
