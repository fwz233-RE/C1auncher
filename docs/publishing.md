# 自助注册、打包并发布普通应用

下载 [发布器 1.1.0](https://github.com/fwz233-RE/C1ancher/releases/tag/publisher-v1.1.0) 中的 `C1Slim-Publisher-1.1.0.zip` 和同名 `.sha256`，校验后解压，保留整个工具目录。旧的 `C1Slim-Publisher-20260907.zip` 不支持自助注册，请换用新版。

## Windows：双击即可操作

双击 `c1publish.exe`，按中文菜单选择：

1. **注册作者并保存令牌**：填写未被占用的作者名。工具自动生成令牌并在服务器登记，无需维护者审批。按提示记录并私下备份令牌文件位置。
2. **使用已有令牌发布应用**：选择令牌文件，填写应用 ID、显示名和版本，选择提前编译好的设备程序，或包含程序及资源的专用 payload 目录，确认后上传。
3. **查询应用建议版本**：更新前查询版本，将相同版本写入应用并重新编译后发布。

程序负责注册、打包、上传，不负责编写或交叉编译应用。请上传 C1 的静态 MIPS ELF 程序，不是电脑上的发布器 EXE。默认 HTTP 服务会提示风险并要求确认；这不是管理员审批。

## 命令行注册

在解压目录运行，作者名和应用 ID 请换成自己的。令牌文件应放在自己可写的私密目录，切勿放进 payload：

    .\c1publish.exe -register -author "MyAuthor" -token-file .\my-author.token -allow-insecure-http

工具先将随机令牌保存，再发送注册。网络错误时使用**相同作者名和相同令牌文件重试**，不要删除文件或重新生成令牌。若作者名已被别人占用，换一个名字。作者名是自选名称，不代表已验证的真实身份。

已有维护者发放的令牌可以直接使用，不用重新注册。同一个作者可以发布多个新应用，无需逐个申请 ID。

## 编译前确定版本

    .\c1publish.exe -id my-example -next-version

查询不会占用 ID 或预留版本。把相同版本写入程序并按 [开发入门](app-development.md) 编译；同版本不同内容会被拒绝，改了程序应升版本并重新编译。

## 发布单文件应用

    .\c1publish.exe -token-file .\my-author.token -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -binary "C:\path\to\my-example"

首次成功发布一个未占用、未受保护的新 ID 后，它自动归属于你的作者身份。更新时保持相同 ID 和令牌，使用更高版本。其他作者和匿名上传不能更新你的应用。

## 发布带资源的应用

准备专用 `payload` 目录，程序位于 `payload/bin/my-example`，资源位于 `payload/assets/`：

    .\c1publish.exe -token-file .\my-author.token -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -entry bin/my-example -payload .\payload

`-binary` 和 `-payload` 二选一。文件路径不能含绝对路径、空格或 `..` 段，不支持符号链接。不要打包源码、凭据或个人数据。单文件最多 16 MiB，总内容最多 64 MiB，最多 1024 个文件，压缩包最多 32 MiB。

Linux x64 先运行 `chmod +x c1publish-linux-amd64`，再用 `./c1publish-linux-amd64` 替换上述程序名；Linux ARM64 使用 `c1publish-linux-arm64`。设备兼容性未确认时省略可选 `-mode`。

## 应用归属和常见错误

- 新作者无法接管已有受保护应用，也不能认领已发布的匿名应用。首次发布的 ID 以服务器成功提交为准，不是查询或本地打包时预留。
- 注册 HTTP 409：作者名或令牌已绑定另一个身份。不要冒用已有作者名。
- 发布 HTTP 403：应用属于其他作者、受保护或自助功能未启用；核对 ID。新版服务启用后，已有令牌也能发布自己的新 ID。
- HTTP 409：版本、内容或大小写 ID 冲突；内容有变化就升版本重新编译。
- HTTP 429：注册过于频繁，等待后使用原文件重试。
- HTTP 503：服务忙或容量限制；按提示有限重试，不要高频请求。
- 注册 HTTP 404/405 或 `self registration is disabled`：服务器未提供自助注册；生成本地令牌不代表注册成功。

只有工具成功退出并且服务器记录的 ID、版本与包校验值符合预期，才算发布完成。上传不会强制推送或自动安装到所有设备；设备用户在 APP 列表刷新后下载安装。

## 匿名发布兼容与风险

旧的显式 `-open` 仍受支持，不需要令牌，但作者为 `Anonymous (open)`，任何人都能发布其更高版本，不提供独占维护权。需要自己的作者名和更新权限时请选择注册后令牌发布。`-open` 与 `-token-file` 互斥。

令牌相当于作者的发布凭据，切勿公开、提交到 Git 或发送到不可信服务；妥善离线备份。当前没有令牌找回、恢复或自行换发功能，丢失后不能保证自助恢复原作者和应用。

默认公开 HTTP 不加密，令牌及上传内容可能被截获或篡改，必须显式接受 `-allow-insecure-http`；索引公钥验签不能消除此风险。签名表明服务端签发，不代表人工安全审核；设备应用没有沙箱隔离，只安装可信软件。注册和发布均建议使用维护者提供的 HTTPS 地址（如已部署）。核心组件仍走独立维护流程，不属于第三方发布入口。

源码与构建说明：[C1ancher-server/PUBLIC-README.md](../C1ancher-server/PUBLIC-README.md)。
