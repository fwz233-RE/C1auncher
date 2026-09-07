# 设备应用开发入门

本仓库帮助你开发在 C1-Slim / MP-D261 上运行的普通应用。开发和发布普通应用不要求编译启动器、安装器或服务器。

## 1. 先选一个参考项目

- 第一个程序：[`examples/hello`](../examples/hello/README.md)，标准 C 终端应用，不需要图形库。
- 图形、文字与按键：[`App/book-reader`](../App/book-reader/README.md)、[`App/pic`](../App/pic/README.md)。
- 共用设备接口：`App/c1device/`，供同级 Go 应用通过本地模块引用。
- 音乐播放：[`App/music-player`](../App/music-player/README.md)。
- 实时音频和按键：[`Pinao`](../Pinao/README.md)。
- 游戏实现：[`ChiChuGames`](../ChiChuGames/README.md)。

保留 `App/` 内各目录的相对位置；其 Go 模块通过 `replace c1device => ../c1device` 引用共用代码。不要只复制某个应用目录后遗漏共用模块。

## 2. 准备构建环境

### C / C++

在 Ubuntu 或 Windows 的 WSL Ubuntu 中安装：

    sudo apt-get update
    sudo apt-get install build-essential gcc-mipsel-linux-gnu g++-mipsel-linux-gnu binutils-mipsel-linux-gnu python3 openssl

编译最小示例，在仓库根运行：

    make -C examples/hello VERSION=0.1.0

产物为 `examples/hello/build/c1-example`。示例使用静态链接及 MIPS32r2、o32、硬浮点选项，构建末尾会输出 ELF 信息并检查动态依赖。

### Go

建议安装 **Go 1.26.4 或更新的兼容版本**。当前应用模块分别声明了 Go 1.26.0 或 1.26.4；初次构建需要下载 `go.mod` / `go.sum` 中固定的依赖。

以图片应用为例，在 Windows PowerShell 中运行：

    cd App\pic
    go test ./...
    go vet ./...
    $env:GOOS = 'linux'
    $env:GOARCH = 'mipsle'
    $env:GOMIPS = 'hardfloat'
    $env:CGO_ENABLED = '0'
    go build -trimpath -o build\pic .

交叉编译环境变量仅用于设备产物；之后应关闭该终端，或恢复这些变量，再运行本机测试。设备版程序不能直接在 Windows 上执行。

完整应用可能还需要字体或其他资源、版本注入及额外检查。阅读器、音乐播放器和图片应用的构建脚本应显式指定字体，例如在仓库根执行：

    .\App\pic\build.ps1 -Version 0.1.0 -FontPath 'C:\fonts\your-licensed-font.ttf'

该字体必须与应用渲染器兼容，并允许你的使用及再分发方式。本仓库不附带本地 MiSans 文件；打包时应把所选字体要求的许可一起放入资源目录，不能用图像解码库的许可替代字体许可。发布版本应优先使用各应用已有构建脚本和 README，而不是把上述通用命令当作完整的应用打包步骤。

## 3. 遵守设备运行约定

- 目标为 Linux、ELF32、MIPS 小端、o32、MIPS32/MIPS32r2、双精度硬浮点、静态链接。
- 屏幕为 296×152 黑白电子纸。图形应用应按变化刷新，避免持续全屏刷新和忙等待。
- 按键、屏幕和音频设备的使用方式参考现有实现；不要把桌面系统的设备路径和行为直接套用到掌机。
- 退出时释放文件、音频和输入设备等资源，避免残留子进程。启动、保活与回收机制可参考核心目录下的 `src/launcher/`、`src/platform/app_lease.*`、`src/update/supervise.*` 和 `scripts/app-daemon-bootstrap.sh`。
- 阅读、音乐、图片的用户媒体目录分别为 `/storage/mtp/Book`、`/storage/mtp/Music` 和 `/storage/mtp/Pic`；这些个人媒体不随源码分发。
- 应用包只携带运行必需资源。可写用户数据不要当成发行资源打包，具体保存位置按应用已有约定处理。
- 普通应用不要修改系统启动流程、核心签名配置或其他应用的数据。

## 4. 在发布前验证

先跑应用的本机测试，再交叉编译，核对 ELF 架构、静态链接和运行资源。然后在自己的设备上验证打开、交互、退出和保存数据；涉及音频、休眠、屏幕刷新的行为需要实机验证。

最小示例 README 给出了只上传临时文件的 ADB 测试方法，不需要重启或替换系统核心。没有设备时可以分享源码，但不要声称通过实机验收。

## 5. 上传到现有应用仓库

下载本仓库 Release 的开发者发布工具，按 [打包与发布](publishing.md) 操作。选择自己的应用 ID，查询版本，把版本写入程序后编译，再以单文件或 `payload` 目录上传。无需服务端源码和维护者凭据。

## 需要研究或修改核心时

Linux / WSL 在仓库根执行：

    make -C C1ancher -j2 host-test all

Windows PowerShell 可执行：

    .\C1ancher\scripts\build.ps1

核心的四个设备产物位于 `C1ancher/build/`：主程序、启动器、`c1pkg` 和 `c1updater`。源码构建不需要生产私钥；官方签名安装包和核心发布则需要维护者的独立输入，不能用普通应用上传接口替代。安装器的维护者脚本可能引用未公开的本地发布资料，这些不是普通应用开发依赖。
