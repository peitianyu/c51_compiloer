# ttcc — Tiny C to C51 Translator

**ttcc** 是一个源到源翻译器，将标准 **C11** 代码转换为 **Keil C51** 方言，并可自动调用 Keil 工具链生成 **8051 HEX** 文件。

> 📌 **本工具仅供学习研究使用，禁止用于任何商业用途。**

---

## 快速开始

### 构建

使用 MinGW GCC：

```bash
powershell -File build.ps1
```

或一键构建并生成演示 HEX：

```bash
build_demo.bat
```

### 基本用法

```bash
ttcc [选项] <输入文件.c>... | @<列表文件>
```

| 选项 | 说明 |
|------|------|
| `-o <file>` | 输出 HEX 路径（默认：`<输入>.HEX`） |
| `-I<dir>` | 添加头文件搜索路径 |
| `-D<name>[=<val>]` | 预定义宏 |
| `--target mcs51` | 目标架构：`mcs51`/`c51`/`8051`（默认：`host`） |
| `--model small` | C51 内存模型：`small`/`compact`/`large` |
| `--no-build` | 只输出 C51 源码到 stdout，不生成 HEX |
| `@<file>` | 从文件读取参数（支持 `#` 注释，可混用命令行） |

> **注意**：PowerShell 中 `@` 需转义：`` .\ttcc.exe `@list.txt ``

### 示例

```bash
# 编译并生成 HEX
ttcc --target mcs51 --model small -Idemo -o demo\demo.HEX demo\main.c demo\timer.c

# 仅查看翻译后的 C51 源码
ttcc --target mcs51 --no-build demo/main.c

# 使用参数文件
ttcc --% @list.txt 
```

---

## 工作流水线

```
C11 源码 → 预处理 → 词法/语法分析 → AST
    → 9 个降级 Pass → C51 代码生成
    → [可选] C51.exe → BL51.exe → OH51.exe → .HEX
```

### AST 降级 Pass

| # | Pass | 作用 |
|---|------|------|
| 1 | 声明提升 | 块内声明移到块开头（C89 兼容） |
| 2 | for-init 拆分 | `for (int i;..)` → `{int i; for (i;..)}` |
| 3 | 复合字面量展开 | `(T){...}` → 临时变量 |
| 4 | 指定初始化器展平 | `.field=val` → 按声明顺序重排 |
| 5 | `_Generic` 展开 | 编译期类型选择并展开 |
| 6 | `_Static_assert` 展开 | 编译期求值 |
| 7 | C11 属性清理 | 移除 C11 特有属性 |
| 8 | 匿名 struct/union | 生成内部名称 |
| 9 | VLA 检查 | 变长数组报错（不支持） |

### 类型映射

| C11 类型 | C51 类型 | 大小 |
|----------|----------|------|
| `_Bool` | `bit` / `unsigned char` | 1 bit / 1 B |
| `char` | `unsigned char` | 1 B |
| `short` | `int` | 2 B |
| `int` | `int` | 2 B |
| `long` | `long` | 4 B |
| `long long` | ❌ 不支持 | — |
| `float`/`double` | `float` | 4 B |
| 指针 (data/idata) | 1 B | — |
| 指针 (xdata/code) | 2 B | — |

---

## 环境要求

- **构建**：C99 兼容编译器（MinGW GCC 推荐）
- **运行**：Windows（Keil 工具链为 Win32 二进制）

---

## 使用条款

- ✅ 允许个人学习、研究、非商业用途。
- ❌ 禁止用于任何商业项目、产品、服务或盈利活动。
- 📄 如需商业使用，请联系作者获取授权。

---

如有问题，欢迎提 Issue 或 PR（仅限学习交流）。