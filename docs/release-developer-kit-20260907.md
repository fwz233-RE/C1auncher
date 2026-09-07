# 开发者工具与源码入口：2026-09-07

设备端和普通应用源码集中维护在 `fwz233-RE/C1ancher` 仓库，提供开发、编译和上传普通应用的入门资料。

## 下载

- **C1Slim-Publisher-20260907.zip**：Windows x64、Linux x64、Linux ARM64 发布客户端，以及配套地址、公钥、说明和许可文件。
- **C1Slim-Publisher-20260907.zip.sha256**：整个 ZIP 的 SHA-256；解压目录内另有逐文件 `SHA256SUMS`。
- **Source code**：本次标签对应的设备端、普通应用和示例源码。最新文档请看仓库首页。

需要安装器和普通应用发行包，请前往 [最新 Release](https://github.com/fwz233-RE/C1ancher/releases/latest)。

## 开始开发

阅读 [应用开发入门](https://github.com/fwz233-RE/C1ancher/blob/main/docs/app-development.md)，参考标准 C 最小示例、Go Hello、阅读器、播放器、图片、钢琴或游戏源码。

下载并解压发布工具，保留整个 `C1-Open-Publisher` 目录，在其中执行：

    .\c1publish.exe -help
    .\c1publish.exe -open -id my-example -next-version

把确定的版本编入自己的设备应用，交叉编译后按 [打包与发布说明](https://github.com/fwz233-RE/C1ancher/blob/main/docs/publishing.md) 上传。

## 工具包说明

客户端二进制沿用已有 2026-09-06 分发版本。2026-09-07 的说明修订精简了文字，程序、地址和公钥保持不变；工具 ZIP 和对应校验文件一并更新，请以当前下载的 `.sha256` 为准。

ZIP 已检查成员集合、程序架构、CRC 和 SHA-256。此前完成的源码验证见 [验证记录](https://github.com/fwz233-RE/C1ancher/blob/main/docs/open-source-validation.md)。

## 使用提示

- 发布工具运行在电脑上；上传应用须为 Linux/MIPS 小端、o32、硬浮点静态 ELF32。
- `-open` 是匿名发布，开放应用 ID 不归首发者独占；受保护应用和核心组件不能匿名覆盖。
- HTTP 上传需要显式 `-allow-insecure-http`。签名不代表代码审核，设备应用没有沙箱隔离。
- 设备端和普通应用自研源码遵循仓库的 GPL v3 声明；各组件和第三方内容遵循配套许可文件。
