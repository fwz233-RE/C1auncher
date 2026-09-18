---
sidebar_position: 1
---

# 快速开始

## 从源码构建

输入法现作为 C1auncher 仓库内的 `term-ime/` 普通文件夹维护，依赖源码已经包含在 `deps/` 中，无需 Git 子模块。

本次源码导入没有发布新的预编译输入法安装包。请在 Linux 或 WSL 中构建；旧项目的一键安装脚本不用于安装本仓库版本。

```bash
sudo apt-get install -y build-essential cmake pkg-config
git clone https://github.com/fwz233-RE/C1auncher.git
cd C1auncher/term-ime
make build
make test
./build/term-ime
```

## 安装到用户目录（可选）

```bash
cmake --install build --prefix "$HOME/.local"
~/.local/bin/term-ime
```

安装时保留 `share/term-ime/rime-data`，其中包含中文输入必需的方案、词库和 OpenCC 数据。设备端 MIPS 构建与验收仍需单独完成。

:::tip 字体推荐
推荐在终端使用等宽字体（如 [Maple Mono](https://github.com/subframe7536/maple-font)、Sarasa Mono、JetBrains Mono），以获得最佳的中文与候选词对齐效果。
:::

## 配置（可选）

配置文件位于 `~/.config/term-ime/config.json`。可在这里开关语言、切换界面语言。

```json
{
  "languages": [
    {"id": "zh-Hans", "name": "简体中文", "enabled": true}
  ],
  "active_language": "zh-Hans",
  "ui_language": "zh-CN"
}
```

:::caution
需在真实 TTY 或支持 alternate screen 的终端中运行。
:::
