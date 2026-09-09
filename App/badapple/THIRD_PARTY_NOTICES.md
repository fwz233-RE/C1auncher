# Bad Apple 第三方与素材说明

- 自研播放器与预处理工具遵循仓库根目录所声明的 GNU GPL v3；完整文本位于 `C1ancher/LICENSE`。
- 设备二进制静态包含 Go 运行时（Go Authors，BSD 3-Clause）和 `golang.org/x/sys`（Go Authors，BSD 3-Clause）。构建脚本将两者安装目录中的完整 LICENSE 复制到 payload 的 `licenses/`。
- 电脑预处理依赖 Pillow，完整许可随安装包提供：https://github.com/python-pillow/Pillow/blob/main/LICENSE 。Pillow 不打包进设备程序。
- 微型文字字形及几何测试动画在本项目中直接定义，不引入外部字体文件。
- [Felixoofed/badapple-frames](https://github.com/Felixoofed/badapple-frames) 提供视频截图资源，上游未声明明确的媒体再分发许可。原曲、编曲、演唱和动画等权利不因出现在 GitHub 上而变成开源软件许可。`0.1.1` 按发布者要求在二进制中内置预处理动画（439 帧、296×152 单色、2fps、219.5 秒，无音频），不将媒体声明为 GPL 或其他开源许可；公开可下载不等于取得再分发授权，发布者应确认相应媒体使用权。
- 内置画面的源归档为该仓库 `main/frames.zip`，SHA-256：`de4cdf173f7c384eac7e6a98e3a77fa7836b4b5c1543ebbdd5c2ea7dc8792681`。转换使用本项目 `tools/prepare.py`，参数为 `--source-fps 30 --fps 2`，默认阈值 128、白色补边。构建输入仍由 Git 忽略，构建脚本不自动下载。
- 没有复制 `fast-t5-47-epaper`、Waveshare Bad Apple 等未明确授权的播放器源码，也没有采用其他面板的自定义波形表。
