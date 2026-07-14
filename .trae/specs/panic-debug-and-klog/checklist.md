# Checklist

## Panic 现场输出
- [ ] `panic_context_t` 能保存 EIP、ESP、EFLAGS、CR0/CR2/CR3、通用寄存器
- [ ] `panic()` 输出 panic 消息、寄存器现场、最近 32 条 klog
- [ ] 栈回溯基于 EBP 链输出至少 8 层返回地址
- [ ] 未处理异常（#PF/#GP）进入 panic 时输出 CR2 与错误码
- [ ] 所有 panic 输出同时出现在串口终端

## klog 分级
- [ ] `KLOG_DEBUG/INFO/WARN/ERROR/FATAL` 常量已定义
- [ ] `klog_write` 支持传入级别参数
- [ ] 运行时 `g_klog_level` 默认过滤 `INFO` 以下日志
- [ ] `klog_set_level` 可在运行时调整过滤级别
- [ ] klog dump 时带级别前缀（如 `[ERROR]`）

## 串口调试通道
- [ ] COM1 初始化成功（115200 8N1）
- [ ] `serial_putc` / `serial_puts` / `serial_printf` 工作正常
- [ ] klog 默认重定向到串口
- [ ] panic 信息完整输出到串口
- [ ] QEMU 启动参数默认携带 `-serial stdio`

## 内核断言
- [ ] `kernel_assert(cond)` 宏可用
- [ ] 断言失败消息包含表达式、文件名、行号
- [ ] 断言失败进入 panic 而不是继续执行
- [ ] 关键初始化路径已添加断言

## 崩溃摘要持久化
- [ ] panic 时检测文件系统是否已挂载
- [ ] 文件系统就绪时创建 `/var/crash/last_panic.log`
- [ ] 崩溃文件包含时间戳、panic 消息、寄存器、最近 klog
- [ ] 写盘失败不导致二次 panic，也不阻塞串口输出

## 启动自测
- [ ] `kernel_main` 调用 `boot_selftest()`
- [ ] 堆初始化失败进入 panic
- [ ] 帧缓冲 / GPU 初始化失败进入 panic 或输出 ERROR
- [ ] 关键数据结构大小与对齐检查通过

## 编译与运行
- [ ] `wsl make -C /mnt/f/NEO-OS` 无新增警告
- [ ] QEMU 启动后串口可见 klog 输出
- [ ] 人为触发 panic 时串口输出包含寄存器和栈回溯
- [ ] DEBUG 日志默认不输出，调整 level 后输出
- [ ] 文件系统挂载后 panic，`/var/crash/last_panic.log` 存在且内容完整
