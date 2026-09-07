# C1ancher 2.0.0

面向 C1-Slim / MP-D261 的 Windows 安装器、普通应用和开发者发布工具集中在此版本下载。安装器与核心版本为 **2.0.0**，各应用保留自己的独立版本号。

## 下载与安装

- **C1SlimInstaller-2.0.0-lf-fix-20260907.zip**：Windows x64 完整安装器。整包解压后运行 `C1SlimInstaller.exe`，保持 EXE、`payload/` 和其余配套文件的目录关系。
- **C1SlimInstaller-2.0.0-lf-fix-20260907.zip.sha256**：安装器 ZIP 的 SHA-256 校验值。
- **普通应用 ZIP**：每个 ZIP 均包含原始应用包和适用许可，具体文件见下方应用列表。
- **C1Slim-Applications-Sources-20260907.zip**：六个应用的对应源码、固定版本依赖、构建与静态库重新链接材料。
- **C1Slim-Applications-Notices-20260907.zip**：应用第三方许可汇总；每个应用 ZIP 内也包含适用许可。
- **APPLICATIONS-README.zh-CN.md / APPLICATIONS-MANIFEST.json / APPLICATIONS-SHA256SUMS**：应用使用说明、版本与原包清单、附件校验值。
- **C1Slim-Publisher-20260907.zip**：电脑端应用发布工具，支持 Windows x64、Linux x64 和 Linux ARM64；整包解压后使用。
- **C1Slim-Publisher-20260907.zip.sha256**：发布工具 ZIP 的 SHA-256 校验值。包内另有逐文件 `SHA256SUMS`。
- **Source code**：本标签对应的设备端、普通应用、示例及构建说明。

已有 C1ancher 的设备，推荐在 APP 列表刷新后选择所需应用安装或更新。Release 中的普通应用归档供下载、校验及开发者使用，不能当作 Windows 安装器运行，也不是整机刷机镜像。`App/c1device` 是共享开发库，`examples/hello` 是源码示例，均不作为独立设备应用分发。

## 安装器注意事项

安装器采用已交付的 LF 换行修正版，配套签名核心为 **2.0.0，序列 11**，包状态为 **READY**。本次发布沿用完整已验证安装包，未重写程序或签名内容。

**安装会修改启动配置，并在检查新核心正常运行后删除原厂学习软件。请先备份重要数据，再阅读随包 `USER-GUIDE.md`。** 安装会配置 root ADB，并在通过硬件及配置检查后开启自动深度休眠；请充分了解相关权限、恢复和唤醒风险。电脑可能还需要安装匹配的 USB 驱动。

随包文档保留了此前安装器迭代的历史说明；其中旧版本号和“尚未构建”等历史段落不代表本附件的当前状态。当前状态以本节、`BUNDLE-STATUS.json` 和配套签名清单为准。

## 普通应用与开发

- Hello **1.5.1**：`C1Slim-App-hello-1.5.1.zip`。
- 阅读器 **0.1.20**：`C1Slim-App-book-reader-0.1.20.zip`。
- 音乐播放器 **0.3.4**：`C1Slim-App-music-player-0.3.4.zip`。
- 图片浏览器 **0.1.4**：`C1Slim-App-pic-0.1.4.zip`。
- Pinao 钢琴 **0.1.4**：`C1Slim-App-pinao-0.1.4.zip`。
- ChiChuGames **0.1.3**：`C1Slim-App-chichugames-0.1.3.zip`。

六个应用 ZIP 在已有发行包外补齐许可，内部原始 `tar.gz` 保持原有版本和字节。本次增加 GitHub 下载入口，不会自动推送、安装或覆盖设备上的应用。重新分发时请同时保留适用许可与对应源码材料。

设备端 `c1pkg` 接受应用 ID，目前没有直接安装本地 `tar.gz` 的参数；普通用户推荐通过 APP 列表安装，下载包用于留存、校验和开发。请勿通过手工覆盖包管理器状态目录绕过验签。

阅读、音乐、图片应用使用原版 MiSans Normal 字体，对应 ZIP 内附小米完整许可协议和版权说明。字体遵循专门协议，不能单独分发或按 GPL/OFL 字体处理。

开发新应用请阅读 [应用开发入门](https://github.com/fwz233-RE/C1ancher/blob/main/docs/app-development.md) 和 [打包与发布说明](https://github.com/fwz233-RE/C1ancher/blob/main/docs/publishing.md)。设备应用必须是兼容的 Linux/MIPS 小端静态程序，发布工具运行在电脑上。匿名开放发布不能覆盖受保护的官方应用。

发布工具本次只修订说明文件，三个客户端程序、公开地址和验签公钥均保持原有字节；ZIP 的校验值随文档修订更新，应使用本次下载的 `.sha256`。

## 核验范围

- 安装器：ZIP CRC、55 个文件的成员集合及逐文件 SHA-256、EXE 固定哈希、核心签名与 2.0.0/序列 11 元数据均已核验。
- 发布工具：九个文件的成员集合、CRC、SHA-256，以及三个程序的目标架构均已核验。
- 普通应用：核对已签名应用索引、归档 SHA-256、包成员和配套许可；具体结果见应用附件说明。
- 源码此前已通过核心主机测试与交叉构建、Go 测试/vet/交叉构建、游戏测试与交叉构建、安装器 **660 项离线测试**，详见 [源码验证记录](https://github.com/fwz233-RE/C1ancher/blob/main/docs/open-source-validation.md)。

本次 Release 整理没有执行新的实机安装、按键、音频、休眠或功耗测试。自动化检查和历史交付记录不等同于在所有设备上完成实机验收。
