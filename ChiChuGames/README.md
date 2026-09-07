# ChiChuGames

C1-Slim / MP-D261 黑白电子纸设备的游戏合集，使用 C 编写。源码版本见 `Makefile`，游戏列表见 [game-catalog.md](docs/game-catalog.md)。本目录作为总仓库中的普通源码目录公开，不需要初始化子模块。

## 构建与测试

Ubuntu / WSL 安装 `build-essential`、`gcc-mipsel-linux-gnu` 和 `binutils-mipsel-linux-gnu` 后，在本目录运行：

    make host-test
    make target verify

设备产物为 `build/C1ancher-chichugames`。目标为静态 MIPS32r2、小端、o32、硬浮点程序。源码中的字体和音效数组已经生成，普通构建不需要下载原始资源，也不需要执行资源生成器。

`make dump` 生成主机侧 PBM 预览，预览不等于实机屏幕验收。涉及实体按键、电子纸刷新、音频和退出资源释放的行为需要设备验证。

## 结构

- `src/games/`：游戏逻辑。
- `src/gfx/`：绘图和字体。
- `src/platform/`：屏幕、按键、音频等设备适配。
- `tests/`：主机逻辑和输入生命周期测试。
- `tools/genfont.py`、`tools/gen_sfx.py`：资源生成方法，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。音效再生成需自行取得原始资源及 Python 的 NumPy、SoundFile。

## 发布

下载本仓库的 [发布工具](https://github.com/fwz233-RE/C1ancher/releases/tag/developer-kit-20260907)，按 [统一发布说明](../docs/publishing.md) 打包上传。官方游戏 ID 受保护，自己的修改版应选择新 ID，并在编译前设置一致的应用版本。

旧的停止系统核心启动脚本和旧仓库打包流程不属于公开开发入口，也不需要用于普通应用发布。请通过正常的应用管理流程启动游戏，避免停止核心守护进程。

自研代码许可见仓库根 [LICENSE](../LICENSE)，第三方字体和音效见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
