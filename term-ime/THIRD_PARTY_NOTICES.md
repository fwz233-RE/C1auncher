# term-ime 来源与第三方许可

## 组件范围与来源

本目录从 [adam-ikari/term-ime](https://github.com/adam-ikari/term-ime) 的本地工作副本导入，基准提交为 `1d8294700d73bf9ff928b33159964c99ace0e0a5`，同时保留导入前已有的输入处理、终端、配置及回归测试修改。

自 2026-09-18 起，它作为 [fwz233-RE/C1auncher](https://github.com/fwz233-RE/C1auncher/tree/main/term-ime) 内的普通文件夹维护。Git 远程关联和子模块元数据已移除；来源链接用于署名与追溯，不是需要初始化的子模块。

原项目 README 声明采用 MIT License；导入版本没有单独的根 LICENSE 或明确的根版权年份。本次保留该声明，不虚构作者、年份或替原作者重新授权。主仓库的 GPL 默认许可不替换该声明，也不替换本目录第三方代码、词库和文档各自的许可。各文件头与随附许可证优先于本索引。

## 内置依赖与许可文件

以下路径相对于本目录。依赖以普通源码目录保存，不需要 `git submodule update`。

- FTXUI：MIT，见 [`deps/ftxui/LICENSE`](deps/ftxui/LICENSE)。
- spdlog：MIT，见 [`deps/spdlog/LICENSE`](deps/spdlog/LICENSE)；内置 fmt 见 [`fmt.license.rst`](deps/spdlog/include/spdlog/fmt/bundled/fmt.license.rst)。其 `.gitattributes` 在导入时改为 `* -text`，避免主仓库换行规则改写原始源码或 PNG。
- nlohmann/json：MIT，见 [`deps/json/LICENSE.MIT`](deps/json/LICENSE.MIT)；附加材料许可保留在同目录 `LICENSES/`。
- GoogleTest：BSD-3-Clause，见 [`deps/googletest/LICENSE`](deps/googletest/LICENSE)。
- Boost.Ext SML：Boost Software License 1.0，见 [`deps/sml/LICENSE.md`](deps/sml/LICENSE.md)。文档附带的其他组件仍遵循各自声明。
- utf8proc：MIT 与 Unicode 数据许可，完整声明见 [`deps/utf8proc/LICENSE.md`](deps/utf8proc/LICENSE.md)。
- librime：BSD-3-Clause，见 [`deps/librime/LICENSE`](deps/librime/LICENSE)。Darts-clone 见 [`COPYING.darts-clone`](deps/librime/include/COPYING.darts-clone)。本目录使用已移除系统 Boost 依赖的上游分支。
- libuv：MIT，见 [`deps/libuv/LICENSE`](deps/libuv/LICENSE)；额外源码声明见 [`LICENSE-extra`](deps/libuv/LICENSE-extra)，文档许可见 [`LICENSE-docs`](deps/libuv/LICENSE-docs)。
- glog：BSD-3-Clause，见 [`deps/librime/deps/glog/COPYING`](deps/librime/deps/glog/COPYING)。
- LevelDB：BSD-3-Clause，见 [`deps/librime/deps/leveldb/LICENSE`](deps/librime/deps/leveldb/LICENSE)。内置 Benchmark 为 Apache-2.0，GoogleTest 为 BSD-3-Clause，分别见其 `third_party/` 目录内的 LICENSE。
- yaml-cpp：MIT，见 [`deps/librime/deps/yaml-cpp/LICENSE`](deps/librime/deps/yaml-cpp/LICENSE)；测试框架许可保留在 `test/googletest-1.13.0/LICENSE`。
- marisa-trie：BSD-2-Clause 或 LGPL-2.1-or-later 双许可；本次按 BSD 分支分发，完整声明见 [`COPYING.md`](deps/librime/deps/marisa-trie/COPYING.md)。
- OpenCC：Apache-2.0，见 [`LICENSE`](deps/librime/deps/opencc/LICENSE) 和 [`AUTHORS`](deps/librime/deps/opencc/AUTHORS)。其内嵌 marisa、GoogleTest、Benchmark、pybind11 等保留各自许可，不统一改为 Apache。
- OpenCC 内嵌 RapidJSON 1.1.0：MIT 与部分 BSD 代码；补入对应版本的 [`license.txt`](deps/librime/deps/opencc/deps/rapidjson-1.1.0/license.txt)。该全文还描述上游 JSON_checker 的独立许可，本目录实际仅内置 RapidJSON 头文件，不含 `bin/jsonchecker/`。
- OpenCC 内嵌 TCLAP 1.2.5：MIT，保留源码版权头；补充上游现行完整许可 [`licenses/tclap-COPYING.txt`](licenses/tclap-COPYING.txt)，来源为 [mirror/tclap 的 COPYING](https://github.com/mirror/tclap/blob/master/COPYING)。不宣称该补充文件是 1.2.5 标签的逐字快照。
- OpenCC 内嵌 Darts-clone：BSD-3-Clause，源码头署名保留；完整条款可见同项目内的 [`COPYING.darts-clone`](deps/librime/include/COPYING.darts-clone)。

## Rime 方案与数据

- `data/rime-data/luna_pinyin*.yaml` 及拼音配置来自 Rime 朙月拼音体系。词典版本字段为 `2024.02.10`，原文件内的作者与 CC-CEDICT、Android、新酷音、OpenCC、萌典等来源致谢完整保留。[rime-luna-pinyin](https://github.com/rime/rime-luna-pinyin) 的上游 LICENSE 为 LGPL v3，作者材料见 [`rime-luna-pinyin-AUTHORS.txt`](licenses/rime-luna-pinyin-AUTHORS.txt)。
- `data/rime-data/essay.txt` 是 Rime 八股文词频数据，对应 [rime-essay](https://github.com/rime/rime-essay)。上游 LICENSE 为 LGPL v3，作者材料见 [`rime-essay-AUTHORS.txt`](licenses/rime-essay-AUTHORS.txt)。未核定本地词频数据对应的独立上游提交，不将其描述为最新上游的逐字副本。
- `default.yaml` 与 librime 示例中的 `symbols.yaml` 对应 [rime-prelude](https://github.com/rime/rime-prelude)，上游采用 LGPL v3；作者材料见 [`rime-prelude-AUTHORS.txt`](licenses/rime-prelude-AUTHORS.txt)。
- LGPL v3 全文随本目录提供于 [`licenses/LGPL-3.0.txt`](licenses/LGPL-3.0.txt)，其引用的 GPL v3 全文已在仓库 [`../C1ancher/LICENSE`](../C1ancher/LICENSE) 中提供。单独再分发本组件或数据时须一并携带这两份完整文本。
- 导入前已有的 `luna_pinyin_simp_fuzzy.schema.yaml`、`t2s_full.json` 和 `variants*.txt` 保留了模糊音与异体字转换扩展；截至 2026-09-18 的本地修改随源码保存，本次未修改词库内容。额外异体字表的独立原始来源和生成版本未记录，不将它们误称为已核对版本的 OpenCC 原版文件。
- `data/rime-data/opencc/TSCharacters.ocd2` 与 `TSPhrases.ocd2` 是 OpenCC 转换数据，保留 Apache-2.0 许可及 OpenCC 作者声明。对应文本字典在 `deps/librime/deps/opencc/data/dictionary/`；没有宣称这两个二进制一定由当前内置提交生成。
- `deps/librime/data/minimal/` 的示例词库也保留原文件声明。特别是 `cangjie5.dict.yaml` 版本 `0.17` 的文件头明确标为 GPL，并保留《五倉世紀》与惜緣、佛振的署名；不得将其归入 librime 引擎的 BSD 许可。

本节补充的上游许可与作者材料于 2026-09-18 获取，仅用于补充已有来源的分发说明，不更改原始数据的授权。

## 源码版本记录

以下是导入时各原子模块的提交。目录已展开为普通文件，保留记录以便后续升级与对照：

- `deps/ftxui` — https://github.com/arthursonzogni/ftxui — `5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6`
- `deps/spdlog` — https://github.com/gabime/spdlog — `8e5613379f5140fefb0b60412fbf1f5406e7c7f8`
- `deps/json` — https://github.com/nlohmann/json — `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`
- `deps/googletest` — https://github.com/google/googletest — `f8d7d77c06936315286eb55f8de22cd23c188571`
- `deps/sml` — https://github.com/boost-ext/sml — `2e228bbe440cd8a654186a2caeb3f27e0b4ecee6`
- `deps/utf8proc` — https://github.com/JuliaStrings/utf8proc — `26dbf597ffa8167ef5edcc7f2da905500a5aa3c2`
- `deps/librime` — https://github.com/adam-ikari/librime — `1d7c2618bbaaa28f1987d57a0d26f33921f391be`
- `deps/libuv` — https://github.com/libuv/libuv — `74c1dcb8455d735d3ffbb1515ba78661621d5cf5`
- `deps/librime/deps/glog` — https://github.com/google/glog — `7b134a5c82c0c0b5698bb6bf7a835b230c5638e4`
- `deps/librime/deps/googletest` — https://github.com/google/googletest — `f8d7d77c06936315286eb55f8de22cd23c188571`
- `deps/librime/deps/leveldb` — https://github.com/google/leveldb — `99b3c03b3284f5886f9ef9a4ef703d57373e61be`
- `deps/librime/deps/leveldb/third_party/benchmark` — https://github.com/google/benchmark — `bf585a2789e30585b4e3ce6baf11ef2750b54677`
- `deps/librime/deps/leveldb/third_party/googletest` — https://github.com/google/googletest — `c27acebba3b3c7d94209e0467b0a801db4af73ed`
- `deps/librime/deps/marisa-trie` — https://github.com/s-yata/marisa-trie — `3e87d53b78e15f2f43783d5e376561a8c9722051`
- `deps/librime/deps/opencc` — https://github.com/BYVoid/OpenCC — `556ed22496d650bd0b13b6c163be9814637970ae`
- `deps/librime/deps/yaml-cpp` — https://github.com/jbeder/yaml-cpp — `2f86d13775d119edbb69af52e5f566fd65c6953b`

## 此次未分发

- 构建目录、缓存、运行日志及旧独立 Git 元数据。
- 原独立仓库的 GitHub Actions 自动发布配置及旧版下载脚本；本次仅导入源码，不发布新的二进制或网站。
- 本地辅助工具的状态与记录。
- `website/static/fonts/` 中的两份 Maple Mono NF CN 字体：本地没有完整版本来源和随附许可，暂不上传；网站现有样式使用系统等宽字体回退。

未来发布二进制、独立源码包或输入法数据包时，应随包保留实际分发组件的全部许可、作者声明及所需对应源码；本说明不能替代各项许可证正文。
