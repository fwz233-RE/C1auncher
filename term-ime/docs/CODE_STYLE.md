# term-ime 代码规范

## 概述

本文档定义了 term-ime 项目的代码风格和最佳实践。

## 语言和标准

- 使用 C++17 标准
- 仅支持 Linux 平台
- 使用 CMake 构建系统

## 命名规范

### 类型命名

- **类名**: PascalCase（大驼峰）
  ```cpp
  class EventLoop { };
  class RimeIme { };
  ```

- **结构体**: PascalCase
  ```cpp
  struct LanguageConfig { };
  struct Candidate { };
  ```

- **枚举**: PascalCase，枚举值使用 PascalCase
  ```cpp
  enum class ImeState {
      Inactive,
      Composing,
      Selecting
  };
  ```

### 变量命名

- **局部变量**: snake_case（小写下划线）
  ```cpp
  int buffer_size = 1024;
  std::string input_text;
  ```

- **成员变量**: snake_case_（尾部下划线）
  ```cpp
  class App {
      Renderer renderer_;
      Pty pty_;
      bool initialized_ = false;
  };
  ```

- **常量**: kPascalCase
  ```cpp
  const int kMaxBufferSize = 4096;
  ```

### 函数命名

- **普通函数**: snake_case
  ```cpp
  void on_resize(int signum);
  bool initialize();
  ```

- **getter**: snake_case，无 get 前缀
  ```cpp
  int pty_fd() const;
  bool is_running() const;
  ```

- **setter**: set_snake_case
  ```cpp
  void set_mode(ImeMode mode);
  ```

### 文件命名

- 文件名使用 snake_case
- 头文件使用 `.hpp` 扩展名
- 源文件使用 `.cpp` 扩展名
- 每个类一对 `.hpp/.cpp` 文件

## 头文件规范

### 头文件保护

使用 `#pragma once`：

```cpp
#pragma once

// 内容...
```

### 包含顺序

1. 相关头文件（如 `foo.cpp` 包含 `foo.hpp`）
2. 项目头文件
3. 第三方库头文件
4. 系统头文件
5. C 标准库头文件

```cpp
#include "foo.hpp"           // 1. 相关头文件
#include "core/config.hpp"   // 2. 项目头文件
#include <spdlog/spdlog.h>   // 3. 第三方库
#include <uv.h>              // 4. 系统库
#include <string>            // 5. C++ 标准库
#include <cstring>           // 6. C 标准库
```

## 代码风格

### 大括号

使用 K&R 风格（开括号在同一行）：

```cpp
if (condition) {
    // ...
} else {
    // ...
}

void function() {
    // ...
}
```

### 缩进

- 使用 4 空格缩进
- 不使用 Tab

### 行长度

- 最大 120 字符
- 尽量保持 80 字符以内

### 空格

- 关键字后加空格：`if (`, `for (`, `while (`
- 运算符两侧加空格：`a + b`, `a = b`
- 逗号后加空格：`func(a, b, c)`

### 空行

- 函数之间空一行
- 逻辑块之间空一行
- 文件末尾空一行

## 注释规范

### 文件头注释

```cpp
// 文件简短描述
// 详细说明（可选）
```

### 类注释

```cpp
// 简短描述
class ClassName {
    // ...
};
```

### 函数注释

仅在复杂或非显而易见的函数上添加注释：

```cpp
// 初始化事件循环
// 返回: 成功返回 true
bool initialize();
```

### 行内注释

使用 `//`，放在代码上方或行尾：

```cpp
// 处理输入数据
process(data);

int result = compute();  // 计算结果
```

## 最佳实践

### 内存管理

- 使用智能指针（`std::unique_ptr`, `std::shared_ptr`）
- 避免裸指针 `new/delete`
- 使用 RAII 管理资源

```cpp
// 好
auto dict = std::make_unique<Dict>();

// 避免
Dict* dict = new Dict();
delete dict;
```

### 错误处理

- 使用返回值表示成功/失败
- 使用 `spdlog` 记录错误
- 避免异常（性能考虑）

```cpp
bool load(const std::string& path) {
    if (!fs::exists(path)) {
        spdlog::error("File not found: {}", path);
        return false;
    }
    return true;
}
```

### 日志

使用 `spdlog`：

```cpp
spdlog::debug("Debug message: {}", value);
spdlog::info("Info message");
spdlog::warn("Warning: {}", reason);
spdlog::error("Error: {}", error_msg);
```

### 异步编程

使用 libuv：

```cpp
// 定时器
loop.set_timer([]() {
    // 回调
}, 1000);

// 异步工作
loop.queue_work(
    []() { /* 后台工作 */ },
    []() { /* 完成回调 */ }
);
```

### 配置

使用 JSON 配置：

```cpp
AppConfig config = AppConfig::load(path);
config.save(path);
```

## 禁止事项

- 禁止使用 `using namespace std;`
- 禁止使用全局变量（除必要）
- 禁止使用 C 风格转换（使用 `static_cast` 等）
- 禁止忽略编译器警告
- 禁止提交未格式化的代码

## 格式化

使用 `clang-format`：

```bash
make format
```

## 代码审查清单

- [ ] 代码符合命名规范
- [ ] 无编译警告
- [ ] 无内存泄漏
- [ ] 错误处理完整
- [ ] 日志级别合适
- [ ] 注释清晰必要
- [ ] 无重复代码
- [ ] 函数职责单一
