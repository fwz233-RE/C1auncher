---
sidebar_position: 3
---

# 配置

默认读取 `~/.config/term-ime/config.json`；设置 `XDG_CONFIG_HOME` 后读取该目录下的 `term-ime/config.json`。
也可使用 `term-ime /path/to/custom.json`。关闭设置面板时，会原路保存到实际加载的文件，而不是覆盖默认配置。

## 示例

```json
{
  "languages": [
    {"id": "zh-Hans", "name": "简体中文", "schema": "luna_pinyin_simp", "enabled": true}
  ],
  "active_language": "zh-Hans",
  "ui_language": "zh-CN",
  "max_candidates": 9,
  "fuzzy_pinyin": true,
  "show_mode_indicator": true,
  "rime_shared_data_dir": "",
  "rime_user_data_dir": "",
  "log_level": "warn",
  "log_file": ""
}
```

## 有效字段

- `shell`：可执行文件路径；明确配置（包括 `/bin/bash`）优先于环境变量。缺失或空字符串表示使用非空 `$SHELL`，否则回退 `/bin/bash`。保存设置时保留空值的自动选择语义。无效路径或执行权限错误会显示原因并返回非零退出码。
- `languages` / `active_language`：配置输入方案；目前随包提供简体拼音方案。
- `ui_language`：`zh-CN` 或 `en`。
- `max_candidates`：候选显示上限，限制在 1–9；旧字段 `page_size` 兼容读取。
- `fuzzy_pinyin`：模糊音，默认开启。省略该字段与没有配置文件时的行为一致。
- `show_mode_indicator`：是否显示模式标签；输入法不可用时仍显示 `EN!` 提示。
- `rime_shared_data_dir`：共享方案、词库与 OpenCC 数据目录。为空时优先查找可执行文件所属安装目录下的 `share/term-ime/rime-data`，再查找构建目录和常见安装位置。
- `rime_user_data_dir`：Rime 用户配置、编译产物与用户词库目录。为空时使用 `$XDG_DATA_HOME/term-ime` 或 `~/.local/share/term-ime`。
- `log_level`：`debug`、`info`、`warn`、`error` 或 `off`。
- `log_file`：追加日志的文件路径。空字符串表示不写文件日志；程序不会再强制打开默认调试日志。

`candidate_bar_position` 当前只支持 `bottom`；其他值降级为底栏。旧 `dict_path` / `extra_dicts` 不再有效，请在 Rime 方案中定义额外词库。

配置写入采用同目录临时文件和原子替换。单个字段类型错误会回退到该字段默认值，不应重置其他有效配置。Rime 初始化或方案选择失败时保持英文直通，状态栏显示 `EN!`。
