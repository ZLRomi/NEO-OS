# Tasks

## Task 1: 增强 panic 处理器
- [x] SubTask 1.1: 定义 `panic_context_t` 结构，保存寄存器、CR2、错误码、EIP、ESP 等现场信息
- [x] SubTask 1.2: 实现 `panic_dump_registers(const panic_context_t *ctx)`，按固定格式输出到串口
- [x] SubTask 1.3: 实现 `panic_stack_trace(uint32_t ebp, uint32_t eip)`，基于 EBP 链输出至少 8 层返回地址
- [x] SubTask 1.4: 修改 `panic()` 入口，接收 panic 消息并组合输出寄存器、栈回溯、最近 32 条 klog
- [x] SubTask 1.5: 为未处理异常（#PF/#GP 等）提供 `panic_from_exception(...)` 入口，自动填充 CR2/错误码

## Task 2: klog 分级与过滤
- [x] SubTask 2.1: 在 `klog.h` 定义 `KLOG_DEBUG/INFO/WARN/ERROR/FATAL` 级别常量
- [x] SubTask 2.2: 修改 `klog_write()` 签名，增加 `level` 参数；低于 `g_klog_level` 的日志被丢弃
- [x] SubTask 2.3: 提供 `klog_set_level(int level)` 与 `klog_get_level()` 运行时接口
- [x] SubTask 2.4: 更新现有 `klog_write` 调用点，按严重程度赋予合理级别（可批量替换默认 INFO）
- [x] SubTask 2.5: 在 klog 环形缓冲中保存级别字段，dump 时带 `[DEBUG]` / `[ERROR]` 等前缀

## Task 3: 串口统一调试通道
- [x] SubTask 3.1: 初始化 COM1（0x3F8）：设置波特率 115200 8N1，检测是否存在（写读 0xAE 测试）
- [x] SubTask 3.2: 实现 `serial_putc()` / `serial_puts()` / `serial_printf(const char *fmt, ...)`
- [x] SubTask 3.3: 将 klog 输出重定向到串口：在 `klog_write()` 中调用 `serial_puts()`
- [x] SubTask 3.4: 确保 panic 输出也通过串口完整打印
- [x] SubTask 3.5: 更新 Makefile / QEMU 启动参数，默认携带 `-serial stdio`

## Task 4: 内核断言
- [x] SubTask 4.1: 在 `klog.h` 或新建 `assert.h` 中定义 `kernel_assert(cond)` 宏
- [x] SubTask 4.2: 宏展开为条件判断，失败时调用 `panic("ASSERTION FAILED: %s at %s:%d", #cond, __FILE__, __LINE__)`
- [x] SubTask 4.3: 在关键初始化路径和大内存分配调用点添加断言（如堆初始化、帧缓冲映射、驱动探测结果）
- [x] SubTask 4.4: 验证断言失败时 panic 消息包含文件名与行号

## Task 5: 崩溃摘要持久化
- [x] SubTask 5.1: 在 panic 流程末尾调用 `panic_write_crash_log(const char *summary)`
- [x] SubTask 5.2: 检测文件系统是否已挂载（通过一个全局标志或 VFS 状态函数）
- [x] SubTask 5.3: 创建目录 `/var/crash`（不存在时）并写入 `last_panic.log`
- [x] SubTask 5.4: 文件内容包含：时间戳、panic 消息、寄存器现场文本、最近 32 条 klog
- [x] SubTask 5.5: 写盘失败时不影响串口输出，也不导致二次 panic

## Task 6: 启动自测与集成
- [x] SubTask 6.1: 在 `kernel_main` 中添加 `boot_selftest()` 调用点
- [x] SubTask 6.2: 验证堆初始化成功，输出 `KLOG_INFO`
- [x] SubTask 6.3: 验证帧缓冲 / GPU 驱动初始化成功，失败时 `KLOG_ERROR` + `kernel_assert`
- [x] SubTask 6.4: 验证关键数据结构大小与对齐（如 PCB、页表项）
- [x] SubTask 6.5: 确保自测失败时进入 panic 而不是继续启动

## Task 7: 编译验证与 QEMU 测试
- [ ] SubTask 7.1: 运行 `wsl make -C /mnt/f/NEO-OS` 确保无新增警告
- [ ] SubTask 7.2: 运行 `wsl make -C /mnt/f/NEO-OS run` 验证串口输出正常
- [ ] SubTask 7.3: 人为触发一次 panic（如在 kernel_main 临时调用 `panic("test panic")`），验证输出包含寄存器与栈回溯
- [ ] SubTask 7.4: 验证 klog 分级：临时插入 DEBUG 日志，默认不输出，设置 level=0 后输出
- [ ] SubTask 7.5: 验证崩溃摘要文件：在文件系统挂载后触发 panic，检查 `/var/crash/last_panic.log`

# Task Dependencies
- Task 1 依赖 Task 3（panic 输出需串口通道先可用）
- Task 2 与 Task 3 可并行
- Task 4 依赖 Task 1（断言失败进入 panic）
- Task 5 依赖 Task 1 与文件系统已挂载
- Task 6 依赖 Task 2、Task 3、Task 4
- Task 7 依赖所有前置任务
