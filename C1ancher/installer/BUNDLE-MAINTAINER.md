# Windows GUI 外置 payload：维护者组装说明

`installer/build-bundle.py` 是离线组装工具。它只复制允许清单中的文件、验证 Ed25519 签名并计算 SHA-256，不编译 GUI、不签名、不生成生产密钥、不执行任何输入程序、不调用 ADB、不连接设备或服务器。

## 2026-09-07 必须开启休眠的安装成功门槛

当前授权目标为 **2.0.0**。本次仅实现并离线测试安装器源码；未构建正式 EXE、组装发布目录、分配序列、签名或部署。下文的 1.3.x 产物记录均为历史，不是本次授权目标，也不得据此复用旧核心交付 2.0.0。

`enable-suspend` 与 `verify-suspend` 均接收本次已验证 payload 的 release manifest SHA-256 和 `c1pkg` SHA-256。助手只执行可信 `current` 指向的物理 generation 中的 `c1pkg`，检查签名、哈希、严格 `confirmed` 状态及相同版本/序列/security epoch；`idle` 也不能通过新策略门槛。即使旧核心仍有有效签名，只要与本次 payload 不同，就不会执行启用命令。准备与 enrollment 本身继续保留旧偏好；覆盖禁用设置是用户在 GUI 明确确认后的独立步骤。硬件条件不满足时保留或创建禁用标记并失败，不调用任何核心电源命令。

此次改动同时涉及 EXE 安装流程和外置助手，必须重新编译 EXE 并组装，不能复用旧 EXE。安装器在配套核心验签、启动和连续运行镜像检查后调用 `enable-suspend` 和 `verify-suspend`，两步通过才进入后续配置和原厂清理。全部步骤结束（包括所选重启验收）后再次调用 `verify-suspend`，再清理暂存目录和记录成功。每个 helper 命令必须返回退出码 0 和唯一对应成功标记；失败保留暂存目录，不记录成功。成功记录写入 `automatic_suspend=enabled` 与 `automatic_suspend_verified=true`。

准备阶段仍保留旧禁用标记，但成功安装的最终策略明确覆盖旧禁用偏好。不受支持硬件无法通过开启/验收门槛。该门槛验证持久化开关而非实际睡眠周期，本次修订只做离线测试，不再次操作设备。下面直到“输入与公开配置”为止均为此前交付/探索历史，其中“保留所有 disable 标记”“复用旧 GUI”“尚未构建”等说法不适用于本次强制开启版本。

## 此前交付记录（仅供追溯）

以下“源码尚未交付”和旧版 1.3.4 路径是本次组装前的历史检查记录。当前新核心已固定为 **1.3.7／序列 9**，源码提交 `e90d73c`，manifest SHA-256 `78f45cb6467607f4edb360ef45afebe7810baacc98546a8ecfd2816a2df378f6`。完整签名输入位于 `D:/c1slim/build/manual-suspend-20260907/enrollment-9-1.3.7`；新 GUI 位于同父目录 `installer-gui`；输出目标为 `D:/c1slim/C1ancher/build/C1SlimInstaller-1.3.7-deepsleep-20260907`。原信任公钥保持不变，不发布 OTA 渠道。

生产 USB 处理已通过一次真实休眠、短按唤醒及手动 USB 拔插，未配置整套 USB 重启备用步骤。当前设备签名更新确认成功、自动休眠开关已启用。完整启动器自动休眠的追加实机测试未获得成功计数增长，用户要求停止后不再继续，也不计为通过。完整主机、生命周期、MIPS 构建和 130 项安装相关测试通过；该离线结果不替代长期续航或多台首次安装验收。

## 默认深度休眠的交付门禁（历史源码策略记录）

新的 `payload/device-setup.sh` 会在精确匹配 C1-Slim 硬件条件时让首次安装默认启用深度休眠。产品接受唤醒后由用户手动拔插 USB 数据线恢复连接；**USB/ADB 无需全自动恢复，也不要求 Wi-Fi 连接事件触发 USB 修复或整套 USB 重启兜底**。验收应验证实际 `mem` 休眠、物理电源键唤醒，以及需要时手动拔插后 USB/ADB 可恢复，并将这项操作告知用户。安装、升级或文件传输尚在进行时仍应保持供电和连接，不能把手动重连说明理解成允许中途拔线。

- 本次只修改源码默认值、测试与说明，没有构建新 EXE、签名核心、组装安装包或更新设备。现有 `1.3.4 / sequence 6` enrollment 是旧的签名产物，不能把当前源码运行时修改说成已包含在该核心中。仅重建 EXE 或刷新外层 SHA256SUMS 不能更新签名核心。
- 新策略交付前须固定已批准的核心源码、四组件哈希、签名 manifest SHA-256 和完整 enrollment，完成上述简化要求的实机验收，再组装到新目录。旧核心没有本次运行时源码修改；在没有针对其实际字节完成审查与验收前，不将新助手与旧 enrollment 作为本次新功能交付。不要在并行构建中复用或自行分配发布序列。
- `build-bundle.py` 的 READY 仅表示结构、签名和运输校验通过，不表示已验证休眠、唤醒或手动 USB 重连。目前没有签名的深度休眠能力字段；未硬编码尚未批准的版本/序列门槛，也未扩展签名协议。硬件识别、私有夹具和旧 `suspend-probe-passed` 文件均不代替实机验收。
- 保持所有旧 disable 标记；`C1SETUP 1\n` 与空文件都不能证明用户未显式选择禁用。不受支持硬件在首次安装、升级或重试时均保持禁用。受支持设备通过旧 PowerShell 入口显式启用时使用 `-EnableAutoSuspend`，显式禁用使用 `-DisableAutoSuspend`；省略两者即保留已有偏好。不把迁移用户偏好夹带进核心更新或 enrollment。

## 2026-09-07 已有产物与下一次组装

已核对的现有 EXE 包为 `D:/c1slim/C1ancher/build/C1SlimInstaller-1.3.4-processfix-20260906/C1SlimInstaller.exe`，另有 `D:/c1slim/C1ancher/build/C1SlimInstaller-1.3.4-adbhotspot-uifit-20260906/C1SlimInstaller.exe`。两者的原包 SHA256SUMS 和 EXE 的 `--validate-payload` 只读校验通过；这次没有修改这些目录或 ZIP。`processfix` 包的 `payload/device-setup.sh` 仍在首次安装创建禁用标记，不包含新的默认启用策略。

`processfix` 的签名核心仍为 **1.3.4／序列 6**，release manifest SHA-256 为 `ee9b6702d82e22e8d6984c739f4acf23d67da72e6e15c0e80d763e6021f8c6d5`，bootstrap SHA-256 为 `afb3e983c3d2592f44b7068986b16f0d8d8cc9e0962c365a890160ee2400cbff`，公开核心密钥 SHA-256 为 `914f57f69ea17d2be486aa6e8b29e88dd7b54984533443948a8eaafcae725e68`。源码 `VERSION` 同为 1.3.4，版本文字相同不代表二进制相同；当前工作树含未交付的运行时修改。

默认值位于外置 helper，单改它不需要重新编译 GUI；如还需带入 GUI 源码修改，先在 PowerShell 执行 `& D:/c1slim/C1ancher/installer/build-installer.ps1 -OutputDirectory D:/c1slim/C1ancher/build/installer-gui-deepsuspend-20260907`。该目标必须尚不存在。然后按下列命令组装；这是待执行步骤，本次没有运行构建或组装。

```powershell
$python = 'C:/Users/123/AppData/Local/Programs/Python/Python312/python.exe'
# 由维护者提供已审查、验收并完整签名的新 enrollment；这里不是现成产物路径。
$enrollment = 'D:/releases/reviewed-deepsuspend-enrollment'
& $python -B D:/c1slim/C1ancher/installer/build-bundle.py build `
  --enrollment-dir $enrollment `
  --expected-core-key-sha256 914f57f69ea17d2be486aa6e8b29e88dd7b54984533443948a8eaafcae725e68 `
  --adb-platform-tools-dir D:/c1slim/C1ancher/build/platform-tools-latest-20260906-final `
  --publisher-dir D:/c1slim/C1ancher-server/build/publisher-bundle-latest-20260906-1743 `
  --installer-exe D:/c1slim/C1ancher/build/installer-gui-deepsuspend-20260907/C1SlimInstaller.exe `
  --go-license D:/c1slim/C1ancher/build/C1SlimInstaller-1.3.4-processfix-20260906/GO-LICENSE.txt `
  --dotnet-notices D:/c1slim/C1ancher/build/C1SlimInstaller-1.3.4-processfix-20260906/DOTNET-THIRD-PARTY-NOTICES.txt `
  --output-dir D:/c1slim/C1ancher/build/C1SlimInstaller-deepsuspend-20260907
```

若复用已审核 GUI，将 `--installer-exe` 改为上面的 `processfix` EXE 路径，省略 GUI 构建步骤；若升级编译依赖，同时提供对应版本的许可文件。组装后运行 `wsl -d Ubuntu-22.04 -- python3 -B /mnt/d/c1slim/C1ancher/installer/check-device-scripts.py --payload /mnt/d/c1slim/C1ancher/build/C1SlimInstaller-deepsuspend-20260907/payload`，并以新 EXE 的 `--validate-payload` 校验新包。

当前交付障碍是尚未固定并验收此次运行时源码对应的完整签名核心，而不是缺少 Wi-Fi 触发修复或自动 USB 重启兜底。新核心需维护者通过 `scripts/build-core-release.ps1` 与 `scripts/build-core-enrollment.ps1` 的既有发布流程提供，明确源码快照、发布版本/序列和同一可信签名密钥；本次不分配序列、不访问私钥、不签名、不发布。`refresh-manifest` 只重算未签名的外层清单，既不能复制新的 helper，也不能给旧核心补上源码修改；本次交付必须使用新目录，保留已发布产物。

## 输入与公开配置

使用 Python 3.12 和 `cryptography`。签名验证依赖缺失时停止，没有跳过签名的选项。维护者可以在自己的 Python 环境预先安装该依赖；工具不会自动安装、下载或寻找环境中的私钥。

四个主要输入：

- `--enrollment-dir`：`scripts/build-core-enrollment.ps1` 已生成并经维护者确认来源的完整 **13 文件签名目录**。禁止传入单个 release、散装二进制或测试夹具。省略此参数只生成 `NOT_READY` 模板，进程返回 **2**。显式指定不存在、不完整或签名无效的目录属于错误，返回 **1**，不会悄悄降级成模板。
- `--adb-platform-tools-dir`（别名 `--adb-dir`）：可信的 Windows platform-tools 目录，只复制 `adb.exe`、`AdbWinApi.dll`、`AdbWinUsbApi.dll`，不运行它们。
- `--publisher-dir`：可信发布工具目录，只复制 `c1publish.exe`、`c1publish-linux-amd64`、`c1publish-linux-arm64`、`README.md`、`repository.ed25519.pub`、`server.url`。即使该目录还有 token、私钥、源代码、虚拟环境或其他构建产物，也不会读取或复制那些额外文件。交付前仍应人工确认这六个允许文件本身是公开发布产物，而不是被错误命名的敏感文件。
- `--output-dir`：**尚不存在的新目录**。已有目录（包括空目录）一律拒绝。重新组装请换新目录名，不提供覆盖或清空旧输出选项。

可选 `--installer-exe` 接收已经构建好的 GUI EXE，按固定名字 `C1SlimInstaller.exe` 复制到输出根目录；不提供时只组装外置资源，可以随后将正确的 GUI EXE 放入同级目录，再刷新清单。

交付根目录还必需原样复制工程 `installer/USER-GUIDE.md`、`installer/THIRD-PARTY-NOTICES.txt`，以及 ADB 输入目录的 `NOTICE.txt`（输出名 `PLATFORM-TOOLS-NOTICE.txt`）。生成的 `README.txt` 指向用户指南。这些文件均纳入根目录允许清单及顶层 SHA256SUMS。

可选 `--go-license` 和 `--dotnet-notices` 分别明确接收 Go `LICENSE`、.NET `ThirdPartyNotices.txt` 的来源路径，按原字节输出为 `GO-LICENSE.txt`、`DOTNET-THIRD-PARTY-NOTICES.txt`，并纳入根允许清单与顶层 SHA256SUMS。未传参数时不寻找主机安装目录、不复制该文件；显式传入不存在的路径则失败。维护者可分别传入 `'C:/Program Files/Go/LICENSE'` 和 `'C:/Program Files/dotnet/ThirdPartyNotices.txt'`。READY 包刷新保留并校验清单中的这些公开交付文件。

公开资源来源固定，不扫描其他候选目录：

- 应用公钥来自工程 `config/app-repo/repository.ed25519.pub`，原样复制，必须为 32 字节。`developer/repository.ed25519.pub` 必须与它一致。
- 默认应用地址为 `http://www.fwz233.com/c1/v2`，核心地址为 `http://www.fwz233.com/c1/core/v1/stable`，与现有 `scripts/build-repository-profile.ps1` 的新官方 **HTTP** 默认值一致。组装时检查该公开配置脚本的默认值，发现变化就停止，而不是悄悄改变端点。发布工具的 `server.url` 必须为 `http://www.fwz233.com`（允许末尾 `/` 与常规行结束符）。不接受凭据、查询参数或其他端点。
- `profile/device-repository-config.sh` 与 `profile/c1-update-check.sh` 来自现有脚本，按已有 profile 构建逻辑规范化 CRLF 为 LF。
- USB 原始脚本来自工作区根目录 `firmware-analysis/system-rootfs/etc/init.d/S90usb`，固定 SHA-256 为 `c2b278b283e9bf851461d9e8f6edfd207cec3b120585f0e091777d163562e965`。`S90usb.open` 严格沿用 `install-open-adb.ps1` 的唯一变换：把恰好一处 `\t#/etc/init.d/usb/adb\t$1` 替换为 `\t/etc/init.d/usb/adb\t$1`，其他字节不变。
- USB 辅助脚本、`device-setup.sh`、两个 profile 脚本以及 Neofetch 的 wrapper/upstream/config 这七个未签名 shell 文本按固定名单在组装时将 CRLF 转为 LF，拒绝 BOM、残留 CR、NUL、无效 UTF-8，不强制添加末尾换行。源文件不改写。其他非脚本附件仍原字节复制。
- 所有三个 enrollment 脚本和两份 S90usb 仅校验 shell 文本格式，不转换字节；完整签名 enrollment 和固定原厂哈希保持原样。不能通过刷新哈希掩盖脚本格式错误。
- accessories 的五个 neofetch 文件来自 `third_party/neofetch`；壁纸来自工作区根目录 `Pic/wallpaper.raw`，必须恰好 5624 字节。默认布局为 `<workspace>/C1ancher` 与 `<workspace>/Pic`，这里的 workspace 不是名为 `workspace` 的子文件夹。

工具拒绝允许路径上的软链接、Windows junction/reparse point、硬链接、特殊文件、空文件及超出限制的文件。输入 publisher/platform-tools 不做递归复制；enrollment 必须精确符合目录和文件契约。

## 示例

在工程根目录运行（路径仅为示例，换成已审核的公开输入）：

```powershell
$python = 'C:/Users/123/AppData/Local/Programs/Python/Python312/python.exe'
& $python -B installer/build-bundle.py build `
  --enrollment-dir D:/releases/reviewed-core-enrollment `
  --adb-platform-tools-dir D:/public-tools/platform-tools `
  --publisher-dir D:/releases/reviewed-c1publish `
  --installer-exe D:/gui-release/C1SlimInstaller.exe `
  --output-dir D:/releases/c1-installer-new
```

若已经通过独立可信渠道确认核心公钥的 SHA-256，可加 `--expected-core-key-sha256`。该选项只接受公钥摘要，不接收私钥路径。**包内自带公钥和有效自签名不等于独立确认官方来源**；初次组装信任来自维护者确认的 enrollment 输入。工具保持输入核心公钥原样，并在 `BUNDLE-STATUS.json` 记录其摘要；没有偷偷创建、替换或“修复”信任根的机制。

工具重新验证：完整 13 文件集合、核心 raw/PEM 公钥一致性、bootstrap Ed25519 签名、bootstrap 的全部文件哈希绑定和已知原厂 daemon 基线、release Ed25519 签名、release 字段及四个核心二进制的名称/角色/模式/大小/哈希、静态 MIPS32r2 ELF32 o32 hard-float double 程序头/ABI 标记，以及恢复验证器能力标记。完整 release 构建审查仍由已有 `build-core-enrollment.ps1` 调用的 `validate-core-release.sh` 完成；此工具不重新编译或执行目标二进制，也不能从文件内容证明是谁运行过构建脚本。

## 输出契约与清单层级

```text
<output>/
  C1SlimInstaller.exe                 # --installer-exe 提供，或后续放入
  BUNDLE-STATUS.json                 # READY/NOT_READY 与公开版本/公钥摘要
  README.txt
  USER-GUIDE.md
  THIRD-PARTY-NOTICES.txt
  PLATFORM-TOOLS-NOTICE.txt
  GO-LICENSE.txt                     # 仅 --go-license 显式提供时
  DOTNET-THIRD-PARTY-NOTICES.txt       # 仅 --dotnet-notices 显式提供时
  SHA256SUMS                         # 整个交付目录的文件清单
  payload/                           # GUI EXE 同级的外置必需目录
    SHA256SUMS                       # 全部 payload 文件，排除本清单自身
    device-setup.sh
    enrollment/                      # 完整且不可改写的原始 13 文件
      app-daemon-bootstrap.sh
      device-core-enroll.sh
      enroll.sh
      core.ed25519.pub
      core.ed25519.pem
      bootstrap.v1
      bootstrap.v1.sig
      release/
        manifest.v1
        manifest.v1.sig
        artifacts/{C1ancher,c1pkg,C1ancher-launcher,c1updater}
    enrollment.SHA256SUMS             # 相对 enrollment/ 的 13 文件摘要
    enrollment-release.SHA256SUMS     # 相对 enrollment/release/ 的摘要
    enrollment-artifacts.SHA256SUMS   # 相对 enrollment/release/artifacts/ 的摘要
    profile/{五个 profile 文件,SHA256SUMS}
    accessories/{六个 accessories 文件,SHA256SUMS}
    usb/{S90usb.original,S90usb.open,device-open-adb.sh,SHA256SUMS}
    developer/{六个 publisher 文件,SHA256SUMS}
    tools/{三个 Windows ADB 文件,SHA256SUMS}
```

**enrollment 内绝不能增加 `SHA256SUMS` 或说明文件。** 现有设备端 `device-core-enroll.sh` 要求目录内恰好 13 文件、3 个目录。为保留此契约，三个 enrollment 分层 SHA256SUMS 都放在 `payload/` 根层，签名子树不增加任何文件。GUI 上传 enrollment 时应只上传 `enrollment/` 下原始文件，不把同级分层清单塞入其中。

所有 SHA256SUMS 使用小写十六进制、两个空格、正斜杠相对路径、排序后的文件名、无 BOM 的 ASCII 和 LF。子层清单先生成；父清单包括子层清单本身。顶层交付清单还覆盖 EXE（若已提供）、公开状态及说明。**SHA256SUMS 是未签名的运输校验，不是授权签名或独立信任根。** 维护者仍须通过可信渠道分发整个安装包。

缺 enrollment 的模板有 `payload/NOT_READY.txt`，没有签名 enrollment，也没有伪造签名或空占位核心文件；`BUNDLE-STATUS.json` 标记 `NOT_READY`。不得把这种模板描述为可安装包。模板需在具备真实已签名 enrollment 后重新组装到另一个新目录，`refresh-manifest` 不会把模板升级成 READY。

## 换核心版本与 refresh-manifest

关闭 GUI，在受控维护目录中把 `payload/enrollment` **整体替换为同一已批准核心公钥签出的新完整 enrollment 目录**。保留 `BUNDLE-STATUS.json`，不要手改其中的旧公钥摘要；它使正常维护流程检测到意外信任根切换。新包仍应由已有生产签名流程生成，不能只复制 `build/C1ancher`、`c1pkg`、launcher 或 updater。

```powershell
& $python -B installer/build-bundle.py refresh-manifest `
  --output-dir D:/releases/c1-installer-new
```

该命令在所有签名、签名文件哈希绑定、允许清单、公开信任配置等验证完成后，才重算外层 SHA256SUMS 和公开版本状态。它 **绝不改写 enrollment 内任何一个字节**，也不重新签署 `release/manifest.v1` 或 `bootstrap.v1`。缺失/旧的外层 SHA256SUMS 可以重建；签名文件缺失、额外文件、私钥/token 文件或不同核心公钥会直接拒绝。

- 只覆盖某个核心二进制：报 `Signed release component mismatch`；updater 还可能先触发 `Bootstrap binding mismatch`，不会用重算外层 SHA256SUMS 掩盖问题。
- 只更新并签署 release，却没有重新生成 bootstrap：报 `Bootstrap binding mismatch`，因为 bootstrap 绑定 release 清单及签名。
- 整体换成同一公钥签出的完整合法 enrollment：更新外层清单和公开版本信息，**无需重新构建 EXE**。
- 更换核心公钥：报 `Core trust key mismatch`。信任根轮换必须走独立审查流程，本命令不支持。`BUNDLE-STATUS.json` 本身不是防恶意改包的签名，不能替代可信分发或独立公钥确认。

正常内容校验失败不会改写任何输出；刷新时逐文件原子替换清单，最后更新顶层清单。断电/进程中断后可能留下旧父清单或临时文件，此时验证应失败；先人工确认并清理该次刷新遗留的临时文件，再重新刷新，不能跳过检查继续发布。刷新期间请勿运行 GUI 或并发修改资源。

## 设备脚本发布门禁

组装后须在 WSL/Linux 运行 `python3 installer/check-device-scripts.py --payload <新包>/payload`，检查实际交付的 12 个 shell 脚本/配置字节和 `sh -n`/`bash -n` 语法。它只单独执行 USB 辅助脚本严格限定的前两行（shebang 和 `set -eu`），完整脚本从不在主机执行。另运行 `python3 tests/test_installer_usb.py`，在重写路径的私有临时根目录中验证真实 USB 脚本的安装、校验、重复运行与失败回滚；测试替换 mount/process/sync 行为，不运行真实设备命令。

新版 EXE 在设备命令之前验证 shell 文本格式；上传完后，还会在设备配置前解析全部八个安装脚本。哈希正确只说明字节完整，不能代替可执行格式检查。无末尾 LF 的合法脚本允许原样保留，尤其禁止给签名 enrollment 补换行或重新签名来规避检查。

## 核心上传权限回归

Windows ADB 可能生成目录 `777`、文件 `666`，这会被设备安全读取器拒绝，即使内容哈希和签名都正确。EXE 对本次随机暂存目录的固定三目录/13文件先检查类型与链接，再设置 root:root 目录 `700`、文件 `600`，检查实际元数据并重新核对13个SHA-256，之后才允许设备配置。不得通过放宽 `secure_file.c` 或改写签名包处理该错误。

构建 C# 离线测试后，运行 `wsl -d Ubuntu-22.04 -u root -- python3 -B /mnt/d/c1slim/C1ancher/tests/test_installer_enrollment_metadata.py`。它从测试 CLI 导出生产 EXE 使用的同一个权限命令，仅将当前暂存根替换为私有临时目录；真实 root 身份仅用于该临时目录的属主测试，不访问设备。测试从生产源码编译宿主安全读取器/Ed25519验证器，以真实公开签名 enrollment 的临时副本重现 `666` 拒绝，再验证权限修正后原签名通过，并拒绝篡改、符号链接、硬链接和特殊文件。生产签名输入只读，MIPS程序从不执行，无 ADB 或部署。

## 离线测试与当前生产就绪状态

```powershell
& $python -B -m unittest discover -s tests -p test_installer_bundle.py -v
```

测试输出全部位于自动清理的临时目录。测试在内存中生成 **仅测试使用** 的 Ed25519 密钥并执行真实签名/验证，私钥不落盘；合成的核心、ADB、publisher、GUI 字节明确为不可执行夹具，从不执行、发布或用作生产输入。无假签名通过、无生产密钥生成、无 ADB、无网络和设备操作。测试涵盖完整组装、NOT_READY、重复输出拒绝、精确允许清单、签名篡改、仅覆盖二进制、仅重签 release、整体替换 enrollment、信任根变更拒绝、分层清单再生、USB 原样变换、HTTP 默认值、壁纸尺寸及链接拒绝。

2026-09-06 最终复查使用工作区根目录 `build/installer-enrollment-1.3.4-20260906` 的完整生产签名 enrollment，核心为线上 stable **1.3.4、序列 6**。完整 READY 包位于 `C1ancher/build/C1SlimInstaller-1.3.4-latest-20260906`，交付 ZIP 为该目录同级同名 `.zip`，外部校验文件为 `.zip.SHA256SUMS`。当次记录中，线上 stable 六个文件通过 GET/HEAD 与本地签名发布逐字节核对，彼时核心源码与该发布的冻结源码一致；这不是对 2026-09-07 当前工作树或线上状态的重新确认。旧版签名目录保留作历史与回退依据，不原地混入新文件。

安装器从当前源码构建，使用 .NET 8 系列最新稳定补丁 **8.0.30**、BouncyCastle.Cryptography **2.7.0**；ADB 使用 Google 官方稳定版 **37.0.1**。上述依赖版本已再次查询官方元数据核实，未迁移 .NET 主版本或覆盖主机 Android SDK。neofetch 保持上游最终正式版 **7.1.0** 与项目设备适配。发布工具使用 `C1ancher-server/build/publisher-bundle-latest-20260906-1743` 的完整六文件版本，与已上线的 `publisher-bundle-live-20260906` 及其 ZIP 一致，包含官方开放发布功能。

2026-09-06 后续权限修复版位于 `C1ancher/build/C1SlimInstaller-1.3.4-metadatafix-20260906`。按上述固定暂存目录权限策略解决真实设备上传文件 `666` 被安全读取器拒绝的问题，权限命令压缩为1319字节并验证完整ADB请求小于4KiB。515项C#（含实际EXE）、8项真实权限/原签名集成测试、41项组装、31项删除辅助脚本、7项USB辅助脚本及两种DPI测试全部通过。完整包39项来源对比、12个设备shell文件检查、55个ZIP成员读回通过；EXE验证真实新包成功且仍拒绝旧CRLF包。核心1.3.4/序列6全部签名字节保持不变。未替操作员执行真实设备安装。

2026-09-06 后续脚本修复版位于 `C1ancher/build/C1SlimInstaller-1.3.4-shellfix-20260906`。旧直接删除包的 USB helper 含 242 处 CRLF，已在隔离运行的两行前缀中复现 `set: Illegal option`。新包仅规范化未签名 shell 文本，完整签名核心逐字节保持一致。338 项 C# 回归（含新 EXE）、41 项组装测试、31 项删除辅助脚本测试、7 项真实 USB helper 私有根目录测试及 96/192 DPI 界面测试通过；新交付包的 12 个设备 shell 文件字节/语法全部通过，39 项来源对比和 55 项 ZIP 文件读回校验通过。这些检查没有向真实设备安装；用户点击的设备安装仍需最终验收。

2026-09-06 后续直接删除版位于 `C1ancher/build/C1SlimInstaller-1.3.4-directremove-20260906`。本版直接取消原厂学习软件备份步骤和备份/恢复命令，不增加选项；签名核心及实际运行主页验证成功后才删除固定 `/usr/bin/d261` 目录，历史备份、普通应用和用户数据保持不动。核心继续使用相同签名的 1.3.4 / 序列 6。139 项 C# 离线测试（含新 EXE 校验）、35 项组装测试、31 项设备辅助脚本模拟测试及 96/192 DPI 界面测试通过。旧包保留，不覆盖。本次测试和打包不执行真实设备安装，首次安装验收仍由操作员主动发起。
