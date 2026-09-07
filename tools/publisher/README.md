# C1-Slim 自助发布器 1.1.0

下载：https://github.com/fwz233-RE/C1ancher/releases/tag/publisher-v1.1.0

新版文件名为 `C1Slim-Publisher-1.1.0.zip`，配套同名 `.sha256`。旧日期版 EXE 不支持自助注册。请保留完整解压目录，包含 Windows x64 EXE、Linux x64/ARM64 程序、server.url、仓库验签公钥、许可证和校验文件。

Windows 双击 `c1publish.exe`，根据中文菜单注册作者并保存令牌，再用已有令牌发布自己编译的应用。无需管理员审核或逐个授权新应用。作者名和应用 ID 不得冒用已有身份或受保护应用。

命令行也可使用：

    .\c1publish.exe -register -author "MyAuthor" -token-file .\my-author.token -allow-insecure-http
    .\c1publish.exe -id my-example -next-version
    .\c1publish.exe -token-file .\my-author.token -allow-insecure-http -id my-example -version 0.1.0 -name "My Example" -binary .\my-example

注册失败时保留令牌，用同一个作者和文件重试。首次成功发布未占用的新应用 ID 后，它自动归属于你；更新保持同一 ID 和令牌，升版本并重新编译即可。已有作者令牌也能发布自己的新 ID。

Linux 先 `chmod +x c1publish-linux-amd64`，然后替换上述程序名；ARM64 使用对应文件。运行 `-tool-version` 应显示 `C1-Slim Publisher 1.1.0`。

工具负责打包上传，不负责编写或编译应用。上传的是 Linux 静态 MIPS 小端、o32、双精度硬浮点设备 ELF，不是本发布器 EXE。需要资源时使用 `-payload` 和 `-entry`；完整教程在包内 `GUIDE.zh-CN.md`，也见 https://github.com/fwz233-RE/C1ancher/blob/main/docs/publishing.md 。

重要：令牌私下备份，不能放入上传目录或公开仓库。没有令牌丢失后的自助找回功能。默认 HTTP 会明文传输令牌，可能被截获或篡改，必须显式接受风险；公钥验签不能保护上传链路。服务器地址从程序旁边的 server.url 读取，官方域名失败时仅回退固定官方 IP。不要随意替换可信公钥。

兼容的 `-open` 是匿名发布，显示 `Anonymous (open)`，任何人都能更新其应用，不能与令牌同时使用。需要作者名及独占更新权时请使用注册后令牌发布。

发布不会自动安装到所有设备，签名不等于代码安全审核，设备应用没有沙箱隔离。只安装可信软件。此包不提供 macOS、Windows ARM64 或 32 位电脑原生版本。

发布器和共用服务器源码位于 `C1ancher-server/`，构建脚本为 `tools/publisher/build_self_service.py`，第一方许可见包内 LICENSE。第三方声明见 THIRD_PARTY_NOTICES.txt。
