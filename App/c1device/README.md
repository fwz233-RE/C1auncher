# c1device 共享设备模块

供同级 `book-reader`、`music-player` 和 `pic` Go 应用通过 `replace c1device => ../c1device` 使用。请保留 `App/` 的相对目录结构。

这个模块集中放置设备屏幕、按键、文本等共用适配；具体接口以本目录 Go 源码为准。在本目录运行 `go test ./...` 和 `go vet ./...`，完整应用构建与设备验证仍在各应用目录进行。

开发入门见 [仓库文档](../../docs/app-development.md)。自研代码许可见根 [LICENSE](../../LICENSE)，Go 模块依赖按其原有许可证使用。
