# ttcc — Tiny C to C51 Translator

**ttcc** 是一个将标准 **C11 源码**转换为 **Keil C51 方言源码**的源到源翻译器（source-to-source translator），并可自动调用 Keil C51 工具链生成 **8051 微控制器 HEX 文件**。

---

## 工作流水线

```
C11 源码 → 预处理 → 词法分析 → 语法分析 → AST
    → C11→C89 降级 Pass (8 个 Pass)
    → C51 代码生成 → Keil C51 兼容源码
    → [可选] C51.exe → BL51.exe → OH51.exe → .HEX
```

### AST 降级 Pass 列表

| Pass | 功能 | 说明 |
|------|------|------|
| 1. 声明提升 | `lower_hoist_decls` | 块内声明提升到块开头 (C89 风格) |
| 2. for-init 拆分 | `lower_split_for_init` | `for (int i; ...)` → `{int i; for (i; ...)}` |
| 3. 复合字面量展开 | `lower_expand_compound_literal` | `(T){...}` → 临时变量 |
| 4. 指定初始化器展平 | `lower_flatten_designated_init` | `.field=val` → 按声明顺序重排 |
| 5. _Generic 展开 | `lower_expand_generic` | 编译期确定类型并展开 |
| 6. _Static_assert 展开 | `lower_expand_static_assert` | 编译期求值 |
| 7. C11 属性清理 | `lower_strip_c11_attrs` | 移除 C11 特有属性 |
| 8. 匿名 struct/union 处理 | `lower_handle_anonymous` | 生成内部名称 |
| 9. VLA 检查 | `lower_check_vla` | 变长数组报错 |

### 类型映射

| C11 类型 | C51 类型 | 长度 |
|----------|----------|------|
| `_Bool` | `bit` / `unsigned char` | 1 bit / 1 B |
| `char` | `unsigned char` | 1 B |
| `short` | `int` | 2 B |
| `int` | `int` | 2 B |
| `long` | `long` | 4 B |
| `long long` | ❌ 不支持 | — |
| `float` / `double` | `float` | 4 B |
| 指针 (data/idata) | 1 B | — |
| 指针 (xdata/code) | 2 B | — |

---

## 使用方式

### 构建 ttcc

**使用 MinGW gcc:**
```bash
powershell -File build.ps1
```

**一键构建 + 生成演示 HEX:**
```bash
build_demo.bat
```

### 命令行用法

```bash
ttcc [选项] <输入文件.c>... | @列表文件.txt
```

| 选项 | 说明 |
|------|------|
| `-o <file>` | 输出 HEX 文件路径 (默认: `<输入>.HEX`) |
| `-I<dir>` | 添加头文件搜索路径 |
| `-D<name>[=<val>]` | 预定义宏 |
| `--target mcs51` | 目标架构: mcs51/c51/8051 (默认: host) |
| `--model small` | C51 内存模型: small/compact/large |
| `--no-build` | 只输出 C51 源码到 stdout，不生成 HEX |

### 示例

```bash
# 编译 demo 项目并生成 HEX
ttcc --target mcs51 --model small -Idemo -o demo\demo.HEX demo\main.c demo\timer.c demo\uart.c demo\gpio.c

# 仅查看 C51 翻译输出
ttcc --target mcs51 --no-build demo/main.c
```

### Keil 工具链调用

当使用 `--target mcs51` 时，ttcc 会自动调用嵌入的 Keil C51 工具链完成完整编译流水线：

1. **C51.exe** — 编译 `.c` → `.OBJ`
2. **A51.EXE** — 汇编 `STARTUP.A51` → `STARTUP.OBJ` (复位初始化)
3. **A51.EXE** — 汇编 `INIT.A51` → `INIT.OBJ` (全局变量运行时复制)
4. **BL51.EXE** — 链接 OBJ + C51 运行时库 → `.ABS`
5. **OH51.EXE** — 转换 `.ABS` → `.HEX`

---

## 测试

```bash
# 运行全部测试 (需 bash)
cd tests
make check

# L1: AST/C51 输出 diff 验证
make test-ast

# L2: Keil C51 编译验证 (需 WSL + C51.exe 或 wine)
make test-compile
```

| 层级 | 测试 | 数量 | 说明 |
|------|------|------|------|
| L1 | AST diff 验证 | 19 个用例 | 对比 C51 输出与期望结果 |
| L2 | 编译验证 | 223 个用例 | 调用实际 C51 编译器验证 |

---

## 环境要求

- **构建**: MinGW gcc (或任何 C99 编译器)
- **运行**: Windows (Keil 工具链为 Win32 二进制)

---

## 许可证

MIT
