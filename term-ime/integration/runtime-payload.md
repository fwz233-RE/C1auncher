# MIPS 输入法运行载荷准备（离线、未签名）

`integration/prepare-runtime.py` 将现有 MIPS 构建产物转换为只含运行资源的 `payload/`，供独立的应用打包步骤使用。它不访问设备、不调用 ADB、不签名、不下载、不发布、不执行 Git 提交或推送。运行包入口为 `bin/c1-ime-service`。

## 准备命令

要求 Ubuntu 22.04 / WSL、已安装的 MIPS 交叉工具链、QEMU 与本地完整依赖。当前许可收集路径匹配 Ubuntu 22.04 的 GCC 10 MIPS 工具链；更换发行版/工具链时应先更新并核对对应版权文件，缺失时失败，不静默省略。

在 Windows PowerShell 执行（输出目录必须尚不存在）：

```powershell
wsl -d Ubuntu-22.04 -- python3 /mnt/d/c1slim/term-ime/integration/prepare-runtime.py --build-dir /mnt/d/c1slim/.c1-ime-mips-build --output /mnt/d/c1slim/.c1-ime-runtime --jobs 2
```

重复准备请选择新的输出目录；脚本不删除或覆盖已有目录。`--build-dir` 与 `--output` 不能互相包含。准备失败时保留输出和失败报告用于排查；只有 `reports/report.json` 的 `success: true` 才代表技术准备通过。

准备器会：

1. 调用 `build-mips.py` **增量构建当前源码**，复用目标构建树但重新检查 ELF、静态库成员和 pthread 实现；不会接受未经构建更新的旧 service。
2. 检查源数据没有 `build/` 或任何 Rime `.bin`，在 Linux `/tmp` 下创建全新私有用户目录，仅使用 `qemu-mipsel -cpu 24Kf` 执行真正的 MIPS 服务，以默认维护模式生成词典。QEMU 是执行环境，不是宿主 Rime 编译器。
3. 只复制白名单中的词典/配置/OpenCC 文件和 `c1-ime-service`。不复制用户学习库、user.yaml、installation.yaml、锁文件、原始大词典、essay.txt、fuzzy/未使用方案、SDK/demo/test 二进制或 `.a`。
4. 将实际输出副本放到 Linux 文件系统，另建全新 user 目录，以 `--prebuilt-only` 验证 READY、“nihao → 你好”、“zhongguo → 中国”和五候选翻页。
5. 确认共享资源 SHA-256 不变，用户目录无词典 `.bin`，逐项移除五个预编译文件时服务迅速失败而非重新部署。成功后写出整包 SHA-256、目标数据来源哈希、源码控制文件哈希、ELF、耗时、仿真进程内存及日志。

QEMU 进程的 `/proc/<pid>/status` 可观测 `VmRSS`/`VmHWM`；这些数值包含模拟器/JIT 开销，不是 guest-only 内存或设备内存。20 ms 采样的峰值也可能错过短暂峰值。脚本没有施加所谓“50 MiB 实机限制”，不据此宣称真实设备已经满足预算。

## 输出布局

- `payload/bin/c1-ime-service`：真实全静态 MIPS ELF，未 strip，与本次目标构建/仿真使用的文件哈希一致。
- `payload/share/rime-data/default.yaml`、`luna_pinyin_simp.schema.yaml`：经过目标部署得到的小配置，供当前 Rime 配置加载器解析。
- `payload/share/rime-data/build/default.yaml`、`luna_pinyin_simp.schema.yaml`。
- `payload/share/rime-data/build/luna_pinyin_simp.prism.bin`、`luna_pinyin.table.bin`、`luna_pinyin.reverse.bin`：本次 MIPS/QEMU 生成的目标资源，禁止换成 x86_64/宿主版本。
- `payload/share/rime-data/opencc/`：`t2s_full.json`、`TSCharacters.ocd2`、`TSPhrases.ocd2`、`variants.txt`、`variants_ext.txt`、`variants_jp.txt`。
- `payload/NOTICE.txt`、`payload/licenses/`：已知版权、许可全文、作者与未完成义务说明。
- `SHA256SUMS`：相对于输出根目录的 payload 全部文件哈希。
- `reports/report.json`、`reports/elf-report.txt`、`reports/deploy/`、`reports/prebuilt/`：证据，不属于 payload。

父流程只将 `payload/` 作为载荷输入，设置 `entry=bin/c1-ime-service`；报告不能混入应用资源。许可目录和 NOTICE 必须随载荷保留。本脚本没有调用 `C1ancher/scripts/build-app-package.ps1`，也没有修改 `C1ancher/`。

## 必须传递的服务参数

```sh
/absolute/install-root/bin/c1-ime-service \
  --prebuilt-only \
  --socket /absolute/private-runtime/socket \
  --shared-data /absolute/install-root/share/rime-data \
  --user-data /absolute/dedicated-private-user
```

- 参数路径必须为绝对路径。`shared-data` 是可信、完整的安装资源目录，user-data 必须独立于原 term-ime 数据目录，并在应用更新时保留用户正常学习数据。
- socket 父目录与 user-data 必须为服务用户拥有的 0700 目录；服务可创建缺失的 user-data，但 socket 父目录由调用方准备。不要在 `/mnt/d` 上验证 Unix 权限。
- 服务前台运行，调用方管理生命周期。创建 socket 或连接成功不代表可输入，必须等待 STATUS 响应中的 READY。
- **约 50 MiB 预算的设备必须使用 `--prebuilt-only`**。错误退出时保留英文降级，不得移除此参数后重试。
- 缺失/空的必需资源导致退出码 1；参数非法为 2。失败清理服务自身创建的 socket，已有其他 socket 不被删除。
- 模式要求受信任的固定 `luna_pinyin_simp` 载荷，不是通用任意 Rime 方案加载接口。文件完整性由打包校验保证；它不替代签名验证，也不承诺识别所有损坏或恶意词典。

`RimeIme::initialize(bool prebuilt_only = false)` 保留原调用者的默认部署行为。传 true 时不调用 `start_maintenance` 或 `deploy_schema`。当前定制 librime 的方案加载器仍可解析小 YAML；这不构建词典。fresh-user 实测中甚至无需写 `user/build/`，只有受控配置和正常学习数据库。

默认模式的补充部署检查同时识别 `user/build/` 与 `shared/build/` 下的 prism，避免有目标预编译资源时仅因 user 缺失而额外部署。

## 自动回归

```powershell
# 纯 Python 白名单、symlink、ELF ABI/静态/栈保护测试
wsl -d Ubuntu-22.04 -- python3 /mnt/d/c1slim/term-ime/integration/tests/test_prepare_runtime.py -v

# 对实际准备产物复验：两个独立 fresh-user、十二种缺失资源、无重编译
wsl -d Ubuntu-22.04 -- python3 /mnt/d/c1slim/term-ime/integration/tests/test_prebuilt_runtime.py --payload /mnt/d/c1slim/.c1-ime-runtime/payload --report-dir /mnt/d/c1slim/.c1-ime-runtime/retest

# 原有默认维护模式，以及 MIPS SDK / C 示例回归
wsl -d Ubuntu-22.04 -- python3 /mnt/d/c1slim/term-ime/integration/tests/test_mips.py --package /mnt/d/c1slim/.c1-ime-mips-build/package --report-dir /mnt/d/c1slim/.c1-ime-runtime/default-regression
```

`test_prepare_runtime.py` 同时注册为 CTest `c1-ime-runtime-preparation`，可在 host/cross 构建树运行。真实 QEMU 载荷测试是显式命令，不依赖已安装系统服务。测试不得使用 `python -O`，协议校验包含断言。

## 2026-09-19 实测

本机 Ubuntu 22.04 / GCC 10.3 / QEMU 6.2.0、24Kf：

- service SHA-256：`fa60737b232d9ac885e6b762cb85ec4950b73637fb1d30535155071c18ec900e`。
- ELF32、little-endian、o32、MIPS32r2、hard-float、CPR1=32；无 INTERP/NEEDED/动态节，GNU_STACK=RW，存在 GNU_RELRO。ELF 头的通用 Machine 字段显示“MIPS R3000”，具体 ISA 由 Flags/ABI attributes 的 MIPS32r2 确认，不能只看 Machine 文本。
- 载荷总文件字节数 13,610,561（约 12.98 MiB）。这是文件大小总和，不是压缩包大小或内存用量。
- 本次目标词典部署至 READY：14.304 秒；预编译 fresh-user READY：0.209 秒。独立重复测试为 0.326 / 0.237 秒，均提交“你好”“中国”，无 user `.bin`。
- 预编译首次 READY 的 QEMU 进程 VmRSS 19,268 KiB，输入后 23,300 KiB，采样峰值 23,836 KiB；目标部署阶段采样峰值 143,824 KiB，READY 时 VmHWM 144,956 KiB。均包含仿真开销，不能直接推算实机。
- 6 项准备器单元测试通过；12 项缺失资源检查均退出 1、无词典重建；原默认模式的 MIPS SDK/示例/输入/翻页/密码绕过/SIGTERM 清理回归通过，冷部署 READY 为 16.993 秒。所有真实服务测试使用同一 service 哈希。
- 仍观察到 WSL `/mnt/d` 构建时间偏差提示；目标源文件实际重编、链接、ABI/归档检查和 QEMU 执行通过。无新增服务编译警告。

## 许可与验收边界

本地已存在的依赖许可证、词典来源说明、Rime LGPLv3/GPLv3、glibc LGPLv2.1、GCC 运行库版权/例外、X11 常量声明均保留。marisa 采用其 BSD 双许可分支。NOTICE 明确列出缺失的上游根 MIT 版权细节、额外异体字来源、OpenCC 数据原始版本等仍需核定事项。

**准备成功仅表示技术载荷和本地仿真通过，不代表已完成分发许可审核。** 静态 glibc 的 LGPL 对应源码/重链接对象义务、词典及词频衍生数据的 LGPL 对应源码义务、适用的安装信息义务和其他许可条件，仍需分发方在发布前完成。本任务按要求没有生成完整源码归档，也没有上传任何源码。

真实设备 CPU/FPU、内核兼容、内存峰值、启动/退出、电源与应用更新生命周期仍须由用户自行刷机后验收。
