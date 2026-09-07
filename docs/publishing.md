# 打包并发布普通应用

## 下载工具

从本仓库的 [最新 Release](https://github.com/fwz233-RE/C1ancher/releases/latest) 下载 `C1Slim-Publisher-20260907.zip` 和对应 SHA-256 校验文件。解压后保留以下文件在同一目录：

- `c1publish.exe`：Windows x64。
- `c1publish-linux-amd64`：Linux x64。
- `c1publish-linux-arm64`：Linux ARM64。
- `server.url`：公开服务器地址。
- `repository.ed25519.pub`：仓库验签公钥。
- `README.md` 和包内许可证/校验文件。

工具是独立可执行程序。地址和公钥从程序所在目录读取，公钥只负责验证仓库签名，不是上传密码。请从维护者可信渠道取得整个工具包，不要临时下载并信任来源不明的公钥。

## 编译前确定版本

选择尚未占用、未受保护的应用 ID，在解压目录运行：

    .\c1publish.exe -open -id my-example -next-version

查询不会占用 ID 或预留版本。把相同版本写入应用再编译；同版本不同内容会被拒绝，改了程序就升版本并重新编译。

## 发布单文件应用

先按 [开发入门](app-development.md) 编译设备二进制。然后执行：

    .\c1publish.exe -open -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -binary "C:\path\to\my-example"

`-binary` 指向设备用的 MIPS ELF 文件，不是电脑上的 `c1publish.exe`。请使用自己的应用 ID，示例名称不代表可以覆盖已存在的受保护应用。

## 发布带资源的应用

准备一个仅包含运行所需普通文件的 `payload` 目录，入口为 `payload/bin/my-example`，资源放入 `payload/assets/`。执行：

    .\c1publish.exe -open -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -entry bin/my-example -payload "C:\path\to\payload"

`-binary` 和 `-payload` 二选一。工具自动完成打包和上传。

文件路径不能包含绝对路径、空格或 `..` 段，不能包含符号链接、凭据、源码或个人数据。单文件最多 16 MiB，总内容最多 64 MiB，最多 1024 个文件，压缩包最多 32 MiB。

Linux 上先执行 `chmod +x c1publish-linux-amd64`，再用 `./c1publish-linux-amd64` 替换上面的 Windows 程序名；ARM64 使用对应文件。路径改为本机 Linux 路径。

兼容旧设备时省略可选 `-mode`。终端应用可参考最小示例；直接绘制屏幕的应用应参考现有应用的运行方式，并验证所需核心版本。

## 发布成功之后

只有工具成功退出且服务器目录中的应用 ID、版本与包校验值符合预期，才算发布完成。设备用户在 APP 列表刷新后下载安装；上传不会强制推送或自动安装到所有设备。

- HTTP 403：应用 ID 受保护或权限不足。
- HTTP 409：版本或内容冲突；更改内容需升版本并重新编译。
- HTTP 503：服务忙；保持原内容有限重试，不要持续高频请求。

## 权限和风险

开放发布显式使用 `-open`，不需要上传令牌，作者显示为 `Anonymous (open)`。**任何人都可以为开放应用发布更高版本，首发者不拥有独占更新权。** 既有受保护应用仍需维护者授权令牌，核心组件继续走独立维护流程。`-open` 与 `-token-file` 不能同时使用。

当前工具使用公开 HTTP 服务，因此命令要求显式添加 `-allow-insecure-http`。HTTP 不加密，也不能防止上传内容被中途修改；下载索引的公钥验签不消除这个上传风险。签名只表明服务端签发，不代表代码经过人工安全审核；设备上的应用没有沙箱隔离，请只安装可信软件。

这是普通应用发布流程，不是安装器、启动器、守护程序或核心更新的第三方发布入口。
