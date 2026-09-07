# Go Hello 设备应用

这个普通应用展示 C1-Slim 的屏幕显示、按键输入、退出信号和返回主页处理，可作为 Go 图形应用的简短参考。更小的标准输入输出示例见 [`examples/hello`](../../examples/hello/README.md)。

Windows PowerShell 安装 Go 1.26.4 或兼容更新版本，并准备 WSL 的 `file` / `readelf` 后，在本目录运行：

    go test ./...
    go vet ./...
    .\build.ps1 -Version 0.1.0

默认产物位于仓库根的 `build/catalog-payload/hello/hello`。版本号通过构建脚本写入程序；`--version` 可查询。构建脚本会检查设备 ELF 架构和静态链接。

将修改版作为自己的应用发布时，使用新的应用 ID，并按 [统一发布说明](../../docs/publishing.md) 用 `-binary` 上传产物；现有官方 `hello` ID 不可匿名覆盖。实机验证应覆盖显示、按键和正常退出，本机测试不代替设备验证。

自研代码许可见仓库根 [LICENSE](../../LICENSE)。
