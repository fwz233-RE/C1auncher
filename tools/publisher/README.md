# C1-Slim 普通应用发布工具

下载入口：https://github.com/fwz233-RE/C1ancher/releases/latest

下载 `C1Slim-Publisher-20260907.zip` 和同名 `.sha256` 校验文件，核对后解压并保留整个 `C1-Open-Publisher` 目录。工具在电脑上运行，将已编译的普通应用打包并上传到应用仓库。

## 文件

- `c1publish.exe`：Windows x64。
- `c1publish-linux-amd64`：Linux x64。
- `c1publish-linux-arm64`：Linux ARM64。
- `server.url` 与 `repository.ed25519.pub`：公开地址与仓库验签公钥。按工具自身目录读取，不要删除或替换成来源不明的公钥。
- `README.md`、`DISTRIBUTION-NOTE.txt`、`THIRD_PARTY_NOTICES.txt`、`SHA256SUMS`：说明和校验文件。

本包没有原生 macOS、Windows ARM64 或 32 位电脑版本。上述电脑工具也不能作为 C1 的 MIPS 设备应用运行或上传。

## Windows PowerShell 快速使用

进入解压目录，先查看帮助并选择自己的应用 ID：

    .\c1publish.exe -help
    .\c1publish.exe -open -id my-example -next-version

查询不预留版本。将确定的版本号写进自己的应用，交叉编译为设备用的 MIPS 静态 ELF，然后上传。以下假设实际编译版本为 `0.1.0`：

    .\c1publish.exe -open -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -binary .\my-example

如果有资源，准备专用 `payload` 目录，程序位于 `payload/bin/my-example`，资源放在同一个目录树，再执行：

    .\c1publish.exe -open -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -entry bin/my-example -payload .\payload

`-binary` 和 `-payload` 二选一。不要把工作区、源码目录、个人数据或含凭据的目录作为 payload。完整教程：https://github.com/fwz233-RE/C1ancher/blob/main/docs/publishing.md

## Linux

    chmod +x c1publish-linux-amd64
    sha256sum -c SHA256SUMS
    ./c1publish-linux-amd64 -help

使用 `./c1publish-linux-amd64` 替换 Windows 命令中的程序名，路径使用 Linux 写法。Linux ARM64 使用 `c1publish-linux-arm64`。ZIP 解压后如丢失执行权限，先执行 chmod。

## 运行要求和风险

应用入口必须为 Linux、ELF32、MIPS 小端、o32、MIPS32/MIPS32r2、双精度硬浮点、静态链接。工具只负责打包上传，不代替交叉编译。版本改变后重新编译；相同版本不同内容会被拒绝。

发布不会自动安装到所有设备，用户需在 APP 列表刷新后下载安装。没有实机测试时请如实说明。

`-open` 是匿名发布，任何人都能为开放应用提交更高版本，首发者不独占 ID。已有受保护应用和核心组件不能匿名覆盖。`-open` 与 `-token-file` 互斥；开放发布不需要令牌。

默认地址为 `http://www.fwz233.com`，特定错误时固定回退到 `http://123.56.214.77`。HTTP 不加密且不能防止上传内容被篡改，因此需要显式使用 `-allow-insecure-http`；下载索引的公钥验签不消除上传风险。签名不代表安全审核，设备应用没有沙箱隔离，请只安装可信软件。

## 分发范围

此目录保存发布工具的使用说明和打包脚本，客户端下载见 Release 附件。分发说明见 `DISTRIBUTION-NOTE.txt`，Go 运行时等第三方声明见 `THIRD_PARTY_NOTICES.txt`。
