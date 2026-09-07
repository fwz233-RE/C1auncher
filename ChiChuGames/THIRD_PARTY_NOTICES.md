# ChiChuGames 第三方说明

## Adafruit_GFX 经典位图字体

`tools/glcdfont.c` 来自 [Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library)。2026-09-07 核对时，本地文件与上游版本 `ac6d7c3869a693d406f77b9bfcd486b0673169f0` 中的文件具有相同 Git blob ID：`535da3a396867b55e88dcee7c806ec83f43bd9f3`。

字体输入通过 `tools/genfont.py` 转置为 `src/gfx/font_data.c` 中的字形数据；相邻头文件 `font_gen.h` 描述其格式。上游使用 BSD 许可证，不以“公域字体”作为本仓库的授权依据。完整条款及版权见 [`licenses/Adafruit-GFX-BSD.txt`](licenses/Adafruit-GFX-BSD.txt)，重新分发源码或包含该字体的二进制时保留此说明和许可。

## Kenney 音效

`src/platform/sfx_data.c` 和 `tools/gen_sfx.py` 记录使用 Kenney 的 CC0 音效；生成器的输入目录由以下两个资源包合并：

- [Interface Sounds](https://kenney.nl/assets/interface-sounds)。
- [UI Audio](https://kenney.nl/assets/ui-audio)。

生成器选取 `drop_002.ogg`、`drop_001.ogg`、`toggle_001.ogg`、`pluck_001.ogg`、`error_001.ogg`、`maximize_001.ogg` 和 `minimize_001.ogg`，转换为 48 kHz 单声道 int16 数组，并进行重采样、限长和幅度调整。原始压缩包不在本仓库中；已生成的 C 数组是编译输入。

许可参考：[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)。从上游重新下载资源时保留其随包许可并核对所需文件；不能把 CC0 声明扩展到其他来源的素材。

本项目自研代码按仓库根 [LICENSE](../LICENSE) 分发。上述第三方内容保持原许可。
