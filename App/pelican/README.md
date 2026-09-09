# 骑自行车的鹈鹕 / Pelican

C1-Slim / MP-D261 独立小应用：进入即循环播放原创黑白鹈鹕骑车动画，返回 / HOME / Q 退出。没有菜单、联网、音频、字体文件或视频素材下载。

## 画面

296×152、1bit。长嘴、喉囊、翅膀、车架、转动车轮、踩踏动作与移动路面由程序绘制；12 个循环帧在启动时生成并打包，播放时直接读取缓存。不是从网上下载的梗图或视频。

- 首帧请求全刷，后续通过共用 `c1device` 的快刷路径写完整 5624 字节帧。
- 默认每次写屏调用完成后等待 750ms，再提交下一帧；这不是面板物理帧率测量。
- 只有一次正在执行的写屏，不积压动画帧；写屏任务独立于输入与信号处理。退出时等待已提交的写屏结束后关闭设备。
- 沿用 `c1device` 当前键位映射；其把电源键也映射为返回键，当前应用也会随之退出，未修改共用库。
- 沿用现有应用启动方式，应从 C1ancher 应用管理器启动，以避免桌面与应用竞争屏幕。
- 没有修改驱动波形或硬件参数。连续显示的残影、有效帧率、休眠唤醒与真机退出体验仍需实机验证。

## 电脑预览

进入此目录，执行 `go run . --preview preview.gif` 生成三倍大小、无限循环的 GIF。预览和设备使用同一套画面生成逻辑，但电脑预览不模拟电子纸响应、丢帧和残影。`--preview` 与 `--version` 不打开设备屏幕。

可使用 `go run . --preview preview.gif --interval 300ms` 调整预览节奏；设备也接受 `--interval` 参数，范围 100ms 至 5s，更高提交频率不代表面板刷新更快。

## 构建与测试

Windows PowerShell：`powershell -ExecutionPolicy Bypass -File .\build.ps1 -Version 0.1.0`。

脚本运行主机单元测试和 `go vet`，然后交叉编译 Linux / MIPS 小端、硬浮点、CGO 关闭的程序。默认产物：`build/catalog-payload/pelican/pelican`（相对工作区根目录）。开发依赖 Go 1.26 与相邻 `App/c1device`，设备无需安装 Go。

发布时使用普通应用标识 `pelican`、显示名 `Pelican on a Bicycle`、入口 `pelican`，payload 目录为上述产物所在目录。打包发布流程见根目录 `docs/publishing.md`；当前实现未自动上传或安装到设备。

测试覆盖帧尺寸、纯黑白像素、周期一致性、帧差异、设备位序、GIF 帧数和循环设置、版本查询、参数校验、首帧全刷与后续快刷、退出和写屏错误传播。

## 许可

本目录原创代码及程序生成的鹈鹕图形按 GPL-3.0 发布，许可证正文见 `../../C1ancher/LICENSE`。依赖 `golang.org/x/image`（含 basicfont 位图字体）、`golang.org/x/sys`、`golang.org/x/text` 沿用各自许可，分发时保留其许可声明。
