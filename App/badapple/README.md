# Bad Apple — C1-Slim 单色动画播放器

当前版本 `0.1.1` 将完整预处理动画**直接内置在可执行文件中**，从应用管理器安装或更新后打开即可播放，无须复制素材、创建目录或联网下载。当前仍为无声播放。电脑在构建前预处理帧；设备不解析 MP4、不调用 FFmpeg、不解压 PNG，也不依赖额外字体。支持 C1-Slim / MP-D261 的 Linux、MIPS 小端、o32、硬浮点运行环境。

## 设计与边界

- 屏幕 296×152，1 bit/pixel，每帧 5624 字节。
- 独立原始帧，按播放时间直接定位，只读取当前一帧。内置素材以 Go `embed` 字符串存储于二进制只读数据中，不复制整段到 Go 堆，也不解包到设备文件系统；进程地址空间包含这段数据，实际驻留内存随操作系统分页而变化。不会补播积压帧。
- 默认每次提交完成后等待 **700ms**，用 `--interval 300ms` 等参数进行实验（允许 100ms..2s）。这是软件提交间隔，**不是已测面板帧率**；旧项目的约 700ms 观测值不代表已经验证的硬件极限。
- 动画保持原来的时间长度：显示较慢时丢过期帧，而不是把整首曲子的画面放慢。单次定时器避免写屏期间积压计时事件。
- 进入时全刷；播放中整帧去重并用 `fast_refresh_only=1` 快刷。暂停后按 R 可手动全刷；退出恢复进入前的 `fast_refresh_only` 值。写屏成功只代表驱动接受数据，不代表物理显示完成。
- 不修改驱动、波形或 `refresh_max`，不把 `refresh_max=30` 解释为“每分钟 30 次”或“每开机只能全刷 30 次”。旧文档存在相互矛盾的表述，需进一步实测。
- 独占显示锁兼容 `c1pkg` 传入的描述符；矩阵键盘独占读取，GPIO 键盘保持共享，电源键交给系统。
- Go 8 MiB 内存目标只是运行时提示，不是整个进程的硬内存上限。

## 操作

- OK / Enter / 空格 / P：暂停或继续。
- 左 / 右：后退或前进 5 秒。
- N：回到开头，保留当前暂停状态。
- H：帮助；打开帮助会暂停，关闭后再按 OK 继续。
- R：暂停时执行一次全刷以清理残影，播放中忽略。
- Q / 返回 / HOME：退出。
- 电源键：保留系统行为。

默认循环；`--loop=false` 播放一次后停在最后一帧。

## 1. 准备构建素材（开发者操作，安装用户无须执行）

从仓库根运行：

    py -3 -m pip install -r App/badapple/tools/requirements.txt

已有编号图片目录或 ZIP 时，无须下载或视频解析：

    py -3 App/badapple/tools/prepare.py --input D:\frames --output build/badapple-media/badapple.bap --source-fps 30 --fps 2

`--source-fps` 应与图片来源的实际帧率一致。`--fps 2` 是输出素材帧率，必须在 1..30 内且不大于源帧率。工具按时间抽取图片，最后一个时间间隔最多多出不足一帧的时长。文件名末尾必须有帧编号（如 `output_0001.png`），按编号排序，重复编号拒绝；编号的间隔不代表额外停留时间。ZIP 不会被解压到磁盘。

素材源：[Felixoofed/badapple-frames](https://github.com/Felixoofed/badapple-frames)，其中 `frames.zip` 已切帧。可手动下载，也提供独立的可选下载工具：

    py -3 App/badapple/tools/fetch_frames.py --output build/badapple-media/frames.zip
    py -3 App/badapple/tools/prepare.py --input build/badapple-media/frames.zip --output build/badapple-media/badapple.bap --source-fps 30 --fps 2

下载工具输出 SHA-256 供记录，不执行上游代码。上游未声明素材许可，媒体权利独立于播放器代码许可；发布者应确认相应使用权。`0.1.1` 发布构建将这些预处理画面嵌入程序，素材来源及许可状态见 `THIRD_PARTY_NOTICES.md`。构建不自动下载媒体，缺少内置素材时直接失败，避免再次发布只能显示缺失素材提示的版本。

画面处理：保持比例、白色补边、灰度阈值转黑白，默认阈值 128，可用 `--threshold` 调整。Bad Apple 的黑白剪影通常不需要抖动，避免引入无用像素变化。像素打包采用设备专用的每 8 行一组格式，而非通用逐行 1bpp。

没有素材时可生成原创几何图形用于测试（它不是 Bad Apple）：

    py -3 App/badapple/tools/prepare.py --demo --seconds 10 --fps 2 --output build/badapple-media/demo.bap

## 2. 主机验证与构建

    cd App\badapple
    .\build.ps1 -Version 0.1.1 -AssetPath ..\..\build\badapple-media\badapple.bap
    go test ./...
    go vet ./...
    py -3 -B -m unittest discover -s tools -v
    go run . --preview ..\..\build\badapple-media\sample-frame.png --preview-frame 120

默认设备二进制：`build/catalog-payload/badapple/badapple`（相对仓库根）。构建脚本会跑主机测试、主机和 MIPS 的 `go vet`、交叉编译，然后检查 ELF32、小端、MIPS 和无动态解释器/动态段。编译固定 `CGO_ENABLED=0 GOMIPS=hardfloat`。如需独立 ABI 复核，可在 WSL 使用 `readelf -h -l -A`。

`-AssetPath` 将素材准备到被 Git 忽略的 `embedded/badapple.bap` 构建输入位置，然后通过 `go:embed` 编入二进制；设备端不需要这个文件。后续构建可复用已准备的输入。当前内置动画为 439 帧、2fps、219.5 秒。首次从源码构建应先执行上述带 `-AssetPath` 的构建命令（或者先生成 `embedded/badapple.bap`），再直接运行 Go 命令。发布时使用全新 payload 目录，避免混入旧版本遗留文件。

## 3. 设备安装与运行

在设备应用管理器中刷新列表，安装或更新 **Bad Apple** 后直接打开。`0.1.1` 默认读取二进制内置动画，不要求 `/storage/mtp/BadApple` 或 `assets` 目录，也不读取旧版遗留的默认外部素材。

开发测试仍可显式覆盖：命令行 `--file PATH` 优先，其次是环境变量 `C1_BADAPPLE_FILE`，两者均未设置时播放内置动画。显式指定的文件无效时显示错误页，按返回/Q/HOME 可退出；去掉覆盖参数即可恢复内置动画。

可在已授权连接的设备上做临时测试（仓库根执行；安装/推送不由构建脚本自动执行）：

    adb push build/catalog-payload/badapple/badapple /tmp/badapple
    adb shell chmod 755 /tmp/badapple
    adb shell /tmp/badapple --duration 20s

帧率实验，例如：

    /tmp/badapple --interval 500ms --duration 20s

想实验更高提交频率，应先生成更高素材帧率（例如 `--fps 10`），否则同一素材帧重复提交会被跳过。建议短时间比较 1000/700/500/300ms，再通过拍摄实屏判断丢帧、残影与真实显示帧率；单独测量 `write()` 耗时不足以得出物理帧率。

正式普通应用发布参照 `docs/publishing.md`：应用 ID 可用 `badapple`（若已被占用则选择自己的 ID），显示名 `Bad Apple`，入口 `badapple`。本次实现不自动上传仓库、安装设备或修改系统配置。

## 文件格式 C1BA0001

固定 32 字节小端头部，Python 格式 `<8sHHIIIII`：

- magic：8 字节 ASCII `C1BA0001`。
- width/height：各 uint16，必须为 296/152。
- fps numerator/denominator：各 uint32。
- frame count：uint32。
- frame bytes：uint32，必须为 5624。
- reserved：uint32，必须为 0。

随后是 `frame count × 5624` 字节独立帧，无差分、无压缩、无音频。黑=1、白=0，字节偏移 `(y/8)*296+x`，位掩码 `0x80>>(y%8)`。播放器校验头部和完整文件大小，拒绝尾随/截断数据，读取期间的短帧也会终止播放。播放器支持 1..30fps、最多 108000 帧；Python 工具的输入项限制更严格（100000）。

选择原始独立帧是为了在很低的设备显示帧率下随时直接跳转；XOR/RLE 差分在丢帧时仍需恢复中间状态，因此第一版不采用。每分钟的素材在 2fps 时约 659 KiB，在 30fps 时约 9.65 MiB；这是磁盘大小，不是运行时内存需求。

## 来源与验证范围

实现参考了本仓库 Pinao 的设备接口及前述公开项目的“离线单色帧”思路，未复制无许可证的上游播放器或波形代码。自研代码遵循根目录许可证；Go 系统调用依赖为 `golang.org/x/sys`，其许可见第三方说明。

自动测试覆盖文件校验、逐帧读取、时间定位、暂停/继续/跳转、按键、退出清理、转换采样、位序、ZIP 安全及主机预览。主机预览是软件帧，不是真机照片。真实屏幕效果、设备电源键/休眠恢复和长时间残影仍需在设备上验收；当前不提供音频，也不承诺高物理帧率。
