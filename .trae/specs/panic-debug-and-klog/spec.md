# 错误处理与调试基础设施增强 Spec

## Why
当前 NEO-OS 在遇到致命错误、内存分配失败或驱动初始化异常时，缺乏清晰的现场记录和输出通道，定位问题往往需要反复猜测。需要建立统一的 panic 处理、klog 分级、串口调试和断言机制，使启动失败和运行时崩溃都能被快速诊断。

## What Changes
- 增强 `panic()` 处理器：输出寄存器现场、栈回溯、最近 klog 摘要、错误码与错误地址。
- 为 `klog` 增加级别（DEBUG / INFO / WARN / ERROR / FATAL）和运行时过滤开关。
- 统一串口调试输出通道，使 klog 可通过 COM1 输出，QEMU 与真实硬件都能捕获。
- 新增 `kernel_assert()` 宏，断言失败时进入 panic 并保留文件名与行号。
- panic 时尝试将崩溃摘要写入 `/var/crash/last_panic.log`，供重启后查看。
- 启动阶段增加关键自测：堆初始化结果、帧缓冲映射状态、关键数据结构合法性检查。

## Impact
- Affected specs: `os-core-evolution`（Task 58 调试与剖析基础设施）
- Affected code:
  - `kernel/panic.c` 或 `kernel/kernel.c`（panic 入口）
  - `kernel/klog.c/h`（分级与过滤）
  - `kernel/serial.c/h` 或新建 `kernel/debug.c/h`（串口输出通道）
  - `kernel/heap.c`（大内存分配错误码与断言）
  - `kernel/fat32.c` 或 `kernel/vfs.c`（崩溃摘要写盘）
  - `kernel/kernel_main` 初始化顺序与启动自测

## ADDED Requirements

### Requirement: 增强 panic 现场输出
系统 SHALL 在调用 `panic()` 时，通过串口输出以下信息：
- 触发 panic 的消息字符串。
- EIP、ESP、EFLAGS、CR0/CR2/CR3、EAX/EBX/ECX/EDX/ESI/EDI/EBP/ESP 等寄存器现场。
- 触发时的错误码与 CR2 地址（如为 #PF 等异常触发）。
- 最近 32 条 klog 摘要。
- 栈帧回溯（至少 8 层，基于 EBP 链）。

#### Scenario: 正常 panic
- **WHEN** 内核检测到不可恢复错误并调用 `panic("...")`
- **THEN** 串口立即输出上述现场信息，系统停止响应

#### Scenario: 异常直接触发 panic
- **WHEN** 未处理异常（如 #PF/#GP）进入 panic
- **THEN** 输出包含 CR2/错误码的异常上下文，而不仅是 panic 消息

### Requirement: klog 分级与过滤
系统 SHALL 为 klog 提供日志级别：
- `KLOG_DEBUG` = 0
- `KLOG_INFO` = 1
- `KLOG_WARN` = 2
- `KLOG_ERROR` = 3
- `KLOG_FATAL` = 4

运行时默认过滤级别为 `KLOG_INFO`；低于当前级别的日志不被输出。

#### Scenario: 调试时开启详细日志
- **WHEN** 通过 GRUB 命令行或运行时命令设置 `klog_level=0`
- **THEN** DEBUG 级别日志也能输出到串口

### Requirement: 串口统一调试通道
系统 SHALL 提供 `serial_puts()` / `serial_printf()`，并将 klog 输出重定向到 COM1（I/O 0x3F8）。QEMU 启动时可通过 `-serial stdio` 在宿主机终端查看；真实硬件可通过串口线捕获。

#### Scenario: QEMU 调试
- **WHEN** 使用 `wsl make -C /mnt/f/NEO-OS run` 或对应 QEMU 命令启动
- **THEN** 宿主机终端实时显示 klog 与 panic 输出

### Requirement: 内核断言
系统 SHALL 提供 `kernel_assert(cond)` 宏，断言失败时调用 `panic()`，消息包含文件名、行号和表达式。

#### Scenario: 检测非法状态
- **WHEN** 代码执行到 `kernel_assert(ptr != NULL)` 且 ptr 为 NULL
- **THEN** 触发 panic，输出 `ASSERTION FAILED: ptr != NULL at file.c:123`

### Requirement: 崩溃摘要持久化
系统 SHALL 在 panic 时尝试将摘要写入 `/var/crash/last_panic.log`；若文件系统未就绪或写盘失败，则跳过并继续串口输出。

#### Scenario: 文件系统已就绪
- **WHEN** panic 发生在文件系统挂载之后
- **THEN** `/var/crash/last_panic.log` 包含时间戳、panic 消息、寄存器现场和最近 klog

## MODIFIED Requirements

### Requirement: 大内存分配错误处理
`kmalloc()` / `kzalloc()` 在大于可用内存或需要跨页连续分配失败时，不再静默返回 NULL 后让调用者崩溃，而是记录 `KLOG_ERROR` 并返回 NULL；调用者使用 `kernel_assert` 或显式检查。

## REMOVED Requirements
无。
