# C1ancher：C1-Slim 设备端与应用开发

这里集中维护 C1-Slim / MP-D261 的设备端核心和普通应用源码，目的是让开发者能阅读实现、编译自己的程序，并通过发布工具把应用上传到现有软件仓库。

## 下载与安装

[最新 Release](https://github.com/fwz233-RE/C1ancher/releases/latest) 集中提供 **2.0.0 Windows 安装包**、普通应用发行包及开发者发布工具。各应用保留自己的版本号，下载说明见 [2.0.0 发行说明](docs/release-v2.0.0.md)。

安装器应整包解压，保留 EXE 和配套 `payload/`；安装会修改设备启动配置，并在确认新核心正常运行后删除原厂学习软件，操作前请阅读随包用户指南并自行备份。已有 C1ancher 的设备可通过应用管理器下载安装普通应用。

## 从这里开始

1. 阅读 [设备应用开发入门](docs/app-development.md)。
2. 从 [最小 C 应用](examples/hello/README.md) 开始，或参考下面的完整应用。
3. 从 [发布器 1.1.0](https://github.com/fwz233-RE/C1ancher/releases/tag/publisher-v1.1.0) 下载 `C1Slim-Publisher-1.1.0.zip`，解压后保留整个目录，双击 EXE 可自助注册作者和发布应用。
4. 按 [打包与发布说明](docs/publishing.md) 上传应用。用户在设备 APP 列表刷新后自行下载安装。

## 源码目录

- [`C1ancher/`](C1ancher/README.md)：主界面、启动器、应用包管理、保活守护、核心更新、安装器和测试。
- [`App/hello/`](App/hello/README.md)：Go 屏幕与按键示例；[`App/c1device/`](App/c1device/README.md) 提供共用设备接口。
- [`App/book-reader/`](App/book-reader/README.md)：阅读器。
- [`App/music-player/`](App/music-player/README.md)：音乐播放器。
- [`App/pic/`](App/pic/README.md)：图片浏览器。
- [`Pinao/`](Pinao/README.md)：钢琴应用；保留原目录拼写。
- [`ChiChuGames/`](ChiChuGames/README.md)：游戏应用。
- [`examples/hello/`](examples/hello/README.md)：不依赖其他项目的终端应用示例。
- [`C1ancher-server/`](C1ancher-server/PUBLIC-README.md)：自助注册发布器及共用应用仓库服务源码。
- [`tools/publisher/`](tools/publisher/README.md)：发布工具下载、完整性校验和使用说明。

所有内容沿用同一个 GitHub 仓库。旧版本提交和标签保留；核心源码现在位于 `C1ancher/`，旧文档中的仓库根构建命令需先进入该目录。

## 目标设备与构建

设备屏幕为 296×152 黑白电子纸，使用实体键盘。设备程序的目标为 Linux、MIPS 小端、ELF32、o32、MIPS32/MIPS32r2、双精度硬浮点、静态链接。电脑上的 Windows EXE、APK 或 x86 Linux 程序不能作为设备应用上传。

核心组件使用 C 和 MIPS 交叉工具链；普通应用也有 Go 示例。具体依赖和命令见 [开发入门](docs/app-development.md) 以及各应用 README。

发行包及验证范围见 [2.0.0 发行说明](docs/release-v2.0.0.md)，源码自动化验证见 [验证记录](docs/open-source-validation.md)。用户空间安装包与普通应用包都不是整机分区镜像。

## 使用与安全
- 发布工具作为 Release 附件分发；程序、公开服务器地址和验签公钥一起提供。
- 系统备份、原厂固件、个人书籍/音乐/图片、运行日志、令牌和私钥不属于源码分发范围。
- 自助注册后，首次成功发布的新应用归作者独占维护；显式 `-open` 仍为匿名发布，不提供独占权。已有受保护应用和系统核心不能被他人覆盖。
- HTTP 上传不加密，签名不代表恶意代码审核，设备应用目前没有沙箱隔离。详细限制见 [发布说明](docs/publishing.md)。

## 许可证

设备端和普通应用自研源码沿用现有 [GNU GPL v3](C1ancher/LICENSE)，覆盖范围见根 [LICENSE](LICENSE)。各组件有独立许可时遵循其声明；第三方库、字体、图片和其他素材不因放入本仓库而变更许可证。汇总见 [第三方与组件许可说明](THIRD_PARTY_NOTICES.md)。
