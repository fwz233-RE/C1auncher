# 最小 C1-Slim 终端应用

这个示例只使用标准输入输出，不直接操作屏幕、声音、网络或系统配置。程序显示版本，等待 Enter 后退出；`--version` 可非交互查询版本。源码按 GPL-3.0-only 发布，完整许可见 [核心 LICENSE](../../C1ancher/LICENSE)。

## Linux / WSL 构建

安装 `make`、`gcc-mipsel-linux-gnu` 和 `binutils-mipsel-linux-gnu` 后，在本目录运行：

    make VERSION=0.1.0

产物为 `build/c1-example`，是设备使用的静态 MIPS ELF，不是电脑程序。修改版本后先执行 `make clean` 再构建，避免复用旧产物。

## 电脑上先测试

有本机 C 编译器的 Linux / WSL 环境可以运行：

    cc -std=c11 -Wall -Wextra -Werror main.c -o /tmp/c1-example-host
    /tmp/c1-example-host --version
    printf '\n' | /tmp/c1-example-host

这个本机测试只验证终端逻辑，不代替设备兼容性和实机验证。

## 在自己的设备上验证

仅在你有权限使用、已支持 ADB 和 C1ancher 的设备上操作：

    adb push build/c1-example /tmp/c1-example
    adb shell chmod +x /tmp/c1-example
    adb shell /tmp/c1-example --version

交互效果可在设备的 C1ancher 终端运行 `/tmp/c1-example` 检查。测试前确认 `adb devices` 选择的是正确设备。示例不需要重启、修改启动脚本或替换系统核心。

## 打包与发布

先选择自己的应用 ID，并查询建议版本，再编译对应版本。从 [开发者工具 Release](https://github.com/fwz233-RE/C1ancher/releases/tag/developer-kit-20260907) 下载发布工具，按 [发布说明](../../docs/publishing.md) 使用 `-binary` 上传 `build/c1-example`。
