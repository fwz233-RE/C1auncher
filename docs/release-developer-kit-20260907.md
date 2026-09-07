# 开发者工具与源码入口：2026-09-07

本次将设备端和普通应用源码集中到现有 `fwz233-RE/C1ancher` 仓库，提供开发、编译和上传普通应用的入口。

## 下载什么

- **C1Slim-Publisher-20260907.zip**：Windows x64、Linux x64、Linux ARM64 发布客户端，以及公开服务器地址、验签公钥、使用说明和第三方许可。
- **C1Slim-Publisher-20260907.zip.sha256**：整个 ZIP 的 SHA-256。解压目录内另有逐文件 `SHA256SUMS`。
- GitHub 自动生成的 **Source code**：对应本次标签的设备端、普通应用、示例和文档源码，不包含服务端实现或生产数据。

工具 ZIP 的 SHA-256：

`84143316d8e1c67509148008402161e02bbdd2beba898ed7026dae0a04ee7832`

## 开始开发

先看 [应用开发入门](https://github.com/fwz233-RE/C1ancher/blob/main/docs/app-development.md)，然后参考标准 C 最小示例、Go Hello、阅读器、播放器、图片、钢琴或游戏源码。普通应用不需要搭建服务器。

下载并解压工具包，保留整个 `C1-Open-Publisher` 目录，在其中执行：

    .\c1publish.exe -help
    .\c1publish.exe -open -id my-example -next-version

把确定的版本编入自己的设备应用，交叉编译后按 [打包与发布说明](https://github.com/fwz233-RE/C1ancher/blob/main/docs/publishing.md) 上传。工具自动完成打包和提交，不需要 SSH、服务器密码或签名私钥。

## 范围与验证

本次是源码汇总和开发者发布工具分发，**不是新整机固件或新的核心安装器发行版**。发布客户端二进制沿用已有 2026-09-06 分发版本，本次补充公开说明和第三方许可，并重新形成经过成员、架构、CRC 和 SHA-256 检查的工具包。工具包没有服务器源码、私钥或上传令牌文件。

已执行核心主机测试与交叉构建、普通 Go 应用测试/vet/交叉构建、游戏主机测试及交叉构建、安装器 660 项离线回归，以及最小示例编译。完整范围和限制见 [验证记录](https://github.com/fwz233-RE/C1ancher/blob/main/docs/open-source-validation.md)。本次没有操作真实设备，没有发布新应用到生产应用服务器。

## 使用限制

- 发布工具运行在电脑上；上传的应用必须是 Linux/MIPS 小端、o32、硬浮点静态 ELF32。
- `-open` 是匿名发布，开放应用 ID 不归首发者独占；受保护应用和核心组件不能匿名覆盖。
- HTTP 上传需要显式 `-allow-insecure-http`，上传内容不受传输加密保护。公钥验签不代表代码审核，设备应用没有沙箱隔离。
- 设备端和普通应用自研源码沿用仓库声明的 GPL v3；第三方内容保持原许可。独立发布客户端按包内分发说明提供，本次不公开其实现或服务端源码。
