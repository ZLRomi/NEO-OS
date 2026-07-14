# Tasks

## 阶段一：进程管理与调度（CPU 管理）
- [x] Task 1: 新建进程子系统骨架 `process.c/h`：定义 PCB 结构（pid、状态、上下文、页表目录、优先级、时间片、栈、名称）
  - [x] SubTask 1.1: 定义进程状态枚举（READY/RUNNING/BLOCKED/TERMINATED）与 PCB 结构
  - [x] SubTask 1.2: 实现就绪队列与阻塞队列管理
  - [x] SubTask 1.3: 实现 `process_create`、`process_exit`、`process_block`、`process_wake`
- [x] Task 2: 实现上下文切换 `switch_to`（保存/恢复通用寄存器、EIP、ESP、CR3）
  - [x] SubTask 2.1: 编写 `switch_to` 内联汇编保存当前上下文
  - [x] SubTask 2.2: 切换 CR3 指向新进程页目录
  - [x] SubTask 2.3: 恢复新进程上下文并 iret
- [x] Task 3: 实现抢占式轮转调度器 `sched.c/h`
  - [x] SubTask 3.1: 重配 PIT 为 100Hz，时钟 ISR 触发 `schedule()`
  - [x] SubTask 3.2: 实现 round-robin 调度（时间片耗尽切下一进程）
  - [x] SubTask 3.3: 实现优先级调度（高优先级优先抢占）
- [x] Task 4: 初始化首个内核空闲进程与 init 进程，在 kernel_main 中调用 `process_init`

## 阶段二：同步原语与 IPC
- [x] Task 5: 实现同步原语 `sync.c/h`
  - [x] SubTask 5.1: 自旋锁（cli+自旋+解锁）
  - [x] SubTask 5.2: 信号量（wait/post，阻塞队列）
  - [x] SubTask 5.3: 互斥锁（owner 字段）
- [x] Task 6: 实现死锁简单检测（资源分配图环检测，警告日志）
- [x] Task 7: 实现 IPC `ipc.c/h`
  - [x] SubTask 7.1: 管道（环形缓冲 + 阻塞读写）
  - [x] SubTask 7.2: 消息队列（定长消息链表）
  - [x] SubTask 7.3: 共享内存（共享物理页映射到双方页表）

## 阶段三：内存保护与虚拟内存
- [x] Task 8: 实现缺页中断处理 `#PF`（int 14）
  - [x] SubTask 8.1: 读取 CR2 获取缺页地址
  - [x] SubTask 8.2: 合法地址分配物理页并映射，重新执行
  - [x] SubTask 8.3: 非法地址终止进程并记录 klog
- [x] Task 9: 实现用户态/内核态页表隔离
  - [x] SubTask 9.1: 用户页表 user 位置位，内核页表 user 位清零
  - [x] SubTask 9.2: 进程拥有独立页目录（CR3 切换）
- [x] Task 10: 实现请求分页（用户进程按需映射页）

## 阶段四：系统调用接口
- [x] Task 11: 实现 int 0x80 系统调用门
  - [x] SubTask 11.1: 设置 IDT 0x80 门为 ring3 可调用（DPL=3）
  - [x] SubTask 11.2: 实现系统调用分发器（按 eax 分发）
- [x] Task 12: 实现基础系统调用 `syscall.c/h`
  - [x] SubTask 12.1: sys_write / sys_read
  - [x] SubTask 12.2: sys_exit / sys_yield
  - [x] SubTask 12.3: sys_fork（复制页表，trampoline 子进程返回0）/ sys_exec 待文件系统
  - [x] SubTask 12.4: sys_open / sys_close / sys_read / sys_write（对接 FAT32）
  - [x] SubTask 12.5: sys_alloc / sys_draw_frame / sys_play_audio（供应用使用）

## 阶段五：磁盘驱动与文件系统
- [x] Task 13: 实现 ATA/IDE PIO 磁盘驱动 `disk.c/h`
  - [x] SubTask 13.1: 检测 IDE 控制器
  - [x] SubTask 13.2: 实现 LBA28 `disk_read_sector` / `disk_write_sector`
- [x] Task 14: 实现 FAT32 文件系统 `fat32.c/h`
  - [x] SubTask 14.1: 解析 BPB，读取 FAT 表
  - [x] SubTask 14.2: 根目录遍历，路径解析
  - [x] SubTask 14.3: 文件打开/读（只读阶段）
  - [x] SubTask 14.4: 文件写/创建/删除（读写阶段）
  - [x] SubTask 14.5: 空闲簇位示图管理
- [x] Task 15: 在 kernel_main 挂载根文件系统，GSHELL 新增 `ls`、`cat`、`cd`、`mkdir`、`rm` 命令

## 阶段六：设备驱动框架
- [x] Task 16: 实现统一设备接口 `device.c/h`
  - [x] SubTask 16.1: 定义字符/块设备结构与设备表
  - [x] SubTask 16.2: 实现 `dev_open`/`dev_read`/`dev_write`/`dev_ioctl`
  - [x] SubTask 16.3: 注册键盘、鼠标、磁盘、帧缓冲为设备（/dev/tty0 等）
- [x] Task 17: 实现缓冲管理（块设备缓冲区，LRU 淘汰）

## 阶段七：用户认证与权限
- [x] Task 18: 实现用户账户管理 `auth.c/h`
  - [x] SubTask 18.1: 账户记录结构（用户名 + SHA256 密码哈希 + uid）
  - [x] SubTask 18.2: 登录验证函数
- [x] Task 19: 将现有锁屏登录面板对接账户系统（替代固定密码）
- [x] Task 20: 实现文件权限位检查（open/read/write 时校验 uid）

## 阶段八：系统监控与审计
- [x] Task 21: 实现内核环形日志 `klog.c/h`
  - [x] SubTask 21.1: 环形缓冲与 `klog_write`
  - [x] SubTask 21.2: 关键事件记录（进程创建/退出、缺页、错误、IPC）
- [x] Task 22: 实现任务管理器 GUI 应用
  - [x] SubTask 22.1: 进程列表展示（pid/名称/状态/优先级/CPU 占用/内存占用）
  - [x] SubTask 22.2: 结束任务功能
  - [x] SubTask 22.3: CPU/内存使用率图表
- [x] Task 23: GSHELL 新增 `ps`、`meminfo`、`top`、`klog` 命令
- [x] Task 24: 实现中断处理增强（统一中断分发器，保留现有键鼠 ISR）

## 阶段九：显卡驱动与高分辨率高刷新率
- [x] Task 25: 实现显卡驱动抽象层 `gfx_driver.c/h`
  - [x] SubTask 25.1: 定义显卡驱动接口（init/set_mode/get_fb/blit）
  - [x] SubTask 25.2: 实现 Bochs/QEMU VBE 驱动（基于现有 vbe_init）
  - [x] SubTask 25.3: 实现 VMware SVGA-II 驱动
  - [x] SubTask 25.4: 实现通用 VESA VBE 回退驱动
- [x] Task 26: 支持 1920×1080 32bpp 高分辨率与 60Hz+ 刷新率
  - [x] SubTask 26.1: 重构帧缓冲映射，支持更大虚拟区域
  - [x] SubTask 26.2: 动态分配 back_buffer 适配分辨率
- [x] Task 27: 实现运行时分辨率切换与显示设置面板
  - [x] SubTask 27.1: 显示设置 GUI（分辨率列表、切换）
  - [x] SubTask 27.2: 切换后重映射帧缓冲并刷新桌面

## 阶段九B：3D 加速显卡驱动
- [x] Task 28: 实现 3D 渲染抽象层 `gfx_3d.c/h`
  - [x] SubTask 28.1: 定义 3D API 接口（顶点/纹理/矩阵/绘制/清深度缓冲）
  - [x] SubTask 28.2: 实现矩阵运算（模型/视图/投影/相乘）
  - [x] SubTask 28.3: 实现顶点变换管线（变换+裁剪+透视除法+视口变换）
  - [x] SubTask 28.4: 实现深度缓冲（Z-buffer）分配与测试
- [x] Task 29: 实现软件 3D 光栅化器 `render_sw.c/h`
  - [x] SubTask 29.1: 三角形扫描转换（边函数/重心坐标）
  - [x] SubTask 29.2: 透视校正纹理映射
  - [x] SubTask 29.3: 颜色混合与深度测试写入
- [x] Task 30: 实现 VMware SVGA3D 硬件加速驱动 `gpu_svga3d.c/h`
  - [x] SubTask 30.1: SVGA3D 命令缓冲初始化与提交
  - [x] SubTask 30.2: 硬件表面/上下文管理
  - [x] SubTask 30.3: 将 3D API 命令转译为 SVGA3D 命令
- [x] Task 31: 实现 3D 驱动探测与回退（有硬件 3D 用硬件，否则软件）
  - [x] SubTask 31.1: 启动时探测 SVGA3D 能力位
  - [x] SubTask 31.2: 无 3D 硬件时回退到 render_sw
- [x] Task 32: 实现 3D 演示应用（旋转立方体，验证 3D 管线）
  - [x] SubTask 32.1: 定义立方体顶点与纹理
  - [x] SubTask 32.2: 动画循环旋转渲染

## 阶段十：浏览器增强（完整 CSS + 下载）
- [x] Task 33: 重写浏览器 CSS 引擎 `css.c/h`
  - [x] SubTask 33.1: CSS 解析器（选择器：标签/类/ID/后代）
  - [x] SubTask 33.2: 盒模型（margin/border/padding/content）布局算法
  - [x] SubTask 33.3: 颜色（十六进制/rgb/命名）、字体（大小/粗细/样式）、背景、边框
  - [x] SubTask 33.4: 块级/行内元素流式布局
- [x] Task 34: 浏览器渲染管线对接 CSS 引擎
  - [x] SubTask 34.1: 构建 DOM 节点树
  - [x] SubTask 34.2: 应用样式到节点
  - [x] SubTask 34.3: 按盒模型布局并渲染
- [x] Task 35: 实现下载功能 `download.c/h`
  - [x] SubTask 35.1: HTTP/HTTPS 下载到文件系统
  - [x] SubTask 35.2: 下载管理器 GUI（进度、取消、列表）
  - [x] SubTask 35.3: 浏览器集成（点击链接触发下载）

## 阶段十一：多媒体播放（MP3/MP4）
- [x] Task 36: 实现音频驱动 `audio.c/h`
  - [x] SubTask 36.1: Intel AC97 驱动（PCI 探测 + 缓冲输出）
  - [x] SubTask 36.2: 音频流接口（play/stop/pause）
- [x] Task 37: 实现 MP3 软解码器 `mp3.c/h`（验证管线第一步）
  - [x] SubTask 37.1: 帧头解析与同步
  - [x] SubTask 37.2: Huffman 解码与反量化
  - [x] SubTask 37.3: IMDCT 与 PCM 输出
- [x] Task 38: 实现 MP4 容器解析与视频软解 `mp4.c/h`（验证管线第一步）
  - [x] SubTask 38.1: MP4 容器解析（box 结构）
  - [x] SubTask 38.2: 视频帧软解（MPEG-4 Part 2，验证管线）
  - [x] SubTask 38.3: 帧缓冲渲染与音视频同步
- [x] Task 38B: 预留硬件加速解码接口 `video_decode_iface.h` / `audio_decode_iface.h`
  - [x] SubTask 38B.1: 定义统一解码接口（init/decode/flush/destroy）
  - [x] SubTask 38B.2: 软解实现该接口，播放器上层仅依赖接口
  - [x] SubTask 38B.3: 预留硬件解码后端注册点（未来接入无需改播放器）
- [x] Task 39: 实现播放器 GUI
  - [x] SubTask 39.1: 音频播放器 UI（播放/暂停/停止/进度）
  - [x] SubTask 39.2: 视频播放器 UI（播放/暂停/进度/全屏）
  - [x] SubTask 39.3: 文件关联（双击 .mp3/.mp4 启动播放器）

## 阶段十二：桌面美化与壁纸
- [x] Task 40: 实现图片解码 `bmp.c/h`、`jpeg.c/h`
  - [x] SubTask 40.1: BMP 解码（24/32 位）
  - [x] SubTask 40.2: JPEG 解码（基准 DCT）
- [x] Task 41: 实现壁纸自定义
  - [x] SubTask 41.1: 壁纸加载与渲染（铺满/居中/拉伸）
  - [x] SubTask 41.2: 显示设置中壁纸选择
  - [x] SubTask 41.3: 壁纸配置持久化到文件系统
- [x] Task 42: 窗口极致美化
  - [x] SubTask 42.1: 窗口投影阴影（半透明渐变，右下偏移）
  - [x] SubTask 42.2: 窗口圆角（接入 fb_draw_round_rect）
  - [x] SubTask 42.3: 标题栏垂直渐变
- [x] Task 43: 毛玻璃与半透明效果
  - [x] SubTask 43.1: 任务栏半透明 + 背景模糊
  - [x] SubTask 43.2: 开始菜单/右键菜单半透明效果
- [x] Task 44: 图标与高光美化（图标悬停高光、选中态）

## 阶段十三：流畅极致优化
- [x] Task 45: 完善双缓冲与脏矩形
  - [x] SubTask 45.1: back_buffer 作为唯一绘制目标
  - [x] SubTask 45.2: 脏矩形列表机制（仅重绘变化区域）
  - [x] SubTask 45.3: fb_blit 仅复制脏区域
- [x] Task 46: 实现缓动动画系统 `easing.c/h`
  - [x] SubTask 46.1: ease-out / ease-in-out / ease-in 缓动函数
  - [x] SubTask 46.2: 窗口最小化/恢复动画改用缓动
  - [x] SubTask 46.3: 开始菜单/登录动画改用缓动
- [x] Task 47: 输入响应优化
  - [x] SubTask 47.1: 鼠标移动节流（合并连续移动事件）
  - [x] SubTask 47.2: 动画期间不阻塞输入
- [x] Task 48: 热路径性能优化（字体渲染优化、填充优化、缓存）

## 阶段十四：第三方应用安装与跨平台
- [x] Task 49: 严格定义并冻结系统调用 ABI `syscall.h`
  - [x] SubTask 49.1: 定义系统调用编号与参数布局（eax/ebx/ecx/edx/esi/edi）
  - [x] SubTask 49.2: 文档化 ABI 稳定性约束（编号不再更改）
- [x] Task 49B: 定义 .neap 可执行文件格式与加载器
  - [x] SubTask 49B.1: 定义 .neap 格式（头部 + 代码段 + 数据段 + manifest）
  - [x] SubTask 49B.2: 实现 .neap 加载器（解析头部、加载段、创建进程）
- [x] Task 49C: 实现 NEO libc 用户态标准库 `userland/libc/`
  - [x] SubTask 49C.1: 系统调用封装（int 0x80 桩）
  - [x] SubTask 49C.2: POSIX 风格 API（open/read/write/close/exit/fork/exec）
  - [x] SubTask 49C.3: 内存管理（malloc/free）
  - [x] SubTask 49C.4: 字符串/格式化（strcpy/printf 等）
- [x] Task 49D: 提供 NEO 应用编译工具链说明（GCC -ffreestanding 静态链接，C 语言）
- [x] Task 50: 实现应用商店 `app_store.c/h` 与 GUI
  - [x] SubTask 50.1: 应用列表浏览（图标/名称/描述）
  - [x] SubTask 50.2: 安装/卸载功能
  - [x] SubTask 50.3: 已安装应用注册到开始菜单与桌面
  - [x] SubTask 50.4: 应用启动（加载 .neap，创建进程运行）
- [ ] Task 51: 实现硬件抽象层 `hal.c/h`
  - [ ] SubTask 51.1: 显示抽象（VBE/SVGA/VMSVGA）
  - [ ] SubTask 51.2: 音频抽象（AC97/HDA）
  - [ ] SubTask 51.3: 磁盘抽象（ATA/AHCI）
  - [ ] SubTask 51.4: 输入抽象（PS/2/USB）
  - [ ] SubTask 51.5: 运行时探测自动选择驱动
- [ ] Task 51B: 实现 USB 驱动栈 `usb.c/h`（脱离 QEMU 关键依赖）
  - [ ] SubTask 51B.1: USB 主机控制器驱动（UHCI/OHCI/EHCI/xHCI 至少一种）
  - [ ] SubTask 51B.2: USB HID 驱动（USB 键盘/鼠标）
  - [ ] SubTask 51B.3: USB Mass Storage 驱动（U 盘读写）
  - [ ] SubTask 51B.4: USB 设备热插拔检测与注册
- [ ] Task 52: 脱离 QEMU 支持
  - [ ] SubTask 52.1: 验证 GRUB 引导在真实硬件可用
  - [ ] SubTask 52.2: 确保通用 VESA VBE 回退在无专用驱动时工作
  - [ ] SubTask 52.3: ISO 可刻录到 U 盘启动（依赖 USB Mass Storage）

## 阶段十五：系统服务增强（网络/电源/用户/更新/调试/国际化）
- [ ] Task 53: 增强网络协议栈 `net.c/h`
  - [ ] SubTask 53.1: 完善 ARP/ICMP/UDP/TCP 协议
  - [ ] SubTask 53.2: 实现 DHCP 客户端（自动获取 IP，失败回退静态）
- [ ] Task 54: 实现 Socket API 系统调用 `socket.c/h`
  - [ ] SubTask 54.1: socket/connect/bind/listen/accept 系统调用
  - [ ] SubTask 54.2: send/recv/sendto/recvfrom 系统调用
- [x] Task 55: 实现电源管理 `acpi.c/h`、`power.c/h`
  - [x] SubTask 55.1: 读取 ACPI 表，获取硬件信息
  - [x] SubTask 55.2: 软关机/重启（ACPI 命令）
  - [x] SubTask 55.3: 空闲 HLT 降功耗（调度器空闲时 HLT）
  - [x] SubTask 55.4: 预留睡眠/休眠接口
- [x] Task 56: 实现用户管理增强 `userdb.c/h`
  - [x] SubTask 56.1: /etc/passwd 与 /etc/shadow 简化版读写
  - [x] SubTask 56.2: 权限提升（sudo/su 机制）
  - [x] SubTask 56.3: 预留多用户会话接口
- [ ] Task 57: 实现系统更新机制 `update.c/h`
  - [ ] SubTask 57.1: A/B 启动分区或内核热更新
  - [ ] SubTask 57.2: 包管理器（内核模块/驱动/系统库更新）
  - [ ] SubTask 57.3: 在线更新检查
- [ ] Task 58: 实现开发与调试基础设施 `kdebug.c/h`、`prof.c/h`
  - [ ] SubTask 58.1: 内核级 debugger（串口/网络远程调试）
  - [ ] SubTask 58.2: 性能剖析（CPU 采样/内存检测/I/O 热点）
  - [ ] SubTask 58.3: 崩溃转储（panic 时 dump 到磁盘）
- [ ] Task 59: 实现国际化与本地化框架 `i18n.c/h`、`ime.c/h`
  - [ ] SubTask 59.1: Unicode 全面支持（比较/大小写/排序）
  - [ ] SubTask 59.2: 多语言资源文件加载与切换
  - [ ] SubTask 59.3: 输入法框架（拼音输入法底层接口）
- [ ] Task 60: 实现动态链接器 `ld_neo.c/h`（演进目标，静态链接稳定后）
  - [ ] SubTask 60.1: ELF 动态符号解析与重定位
  - [ ] SubTask 60.2: 共享库内存映射
- [x] Task 61: 实现虚拟文件系统（VFS）`vfs.c/h`
  - [x] SubTask 61.1: 统一文件操作接口（open/read/write/readdir/stat）
  - [x] SubTask 61.2: 挂载点管理
  - [x] SubTask 61.3: FAT32 接入 VFS，预留 ext2/NTFS
- [x] Task 62: 实现磁盘调度算法 `disk_sched.c/h`（电梯算法 SCAN/C-SCAN、SSTF）
- [x] Task 63: 实现 SPOOLing 技术 `spool.c/h`（打印机虚拟化，独占设备共享）

## 阶段十六：UNIX/POSIX 子系统
- [ ] Task 64: 实现 VFS 核心数据结构 `vfs.c/h`
  - [ ] SubTask 64.1: struct file / struct inode / struct dentry 定义
  - [ ] SubTask 64.2: 每进程 fd 表，dup/dup2/close-on-exec
  - [ ] SubTask 64.3: mount/umount 挂载机制
  - [ ] SubTask 64.4: FAT32 与 devfs 接入 VFS
- [x] Task 65: 实现 procfs `procfs.c/h`
  - [x] SubTask 65.1: /proc 挂载与目录枚举
  - [x] SubTask 65.2: /proc/[pid]/status、/proc/[pid]/mem
  - [x] SubTask 65.3: /proc/meminfo、/proc/cpuinfo
  - [x] SubTask 65.4: ps/top 通过 /proc 获取进程列表
- [x] Task 66: 实现信号机制 `signal.c/h`
  - [x] SubTask 66.1: kill() 系统调用发送信号
  - [x] SubTask 66.2: signal/sigaction 注册处理函数
  - [x] SubTask 66.3: 内核返回用户态时检查并调用处理函数
  - [x] SubTask 66.4: SIGINT/SIGKILL/SIGTERM/SIGSEGV/SIGPIPE
  - [x] SubTask 66.5: 任务管理器"结束任务"发送 SIGKILL/SIGTERM
- [x] Task 67: 实现终端子系统 `tty.c/h`
  - [x] SubTask 67.1: 终端驱动（回显、行缓冲、Ctrl+C 转 SIGINT）
  - [x] SubTask 67.2: 伪终端 PTY（GUI 终端模拟器运行 shell）
  - [x] SubTask 67.3: 虚拟终端切换（Alt+F1~F7）
- [x] Task 68: 实现定时器与时钟服务 `rtc.c/h`、`timer.c/h`
  - [x] SubTask 68.1: RTC 驱动从 CMOS 读取/设置时间
  - [x] SubTask 68.2: sleep()/usleep() 系统调用
  - [x] SubTask 68.3: gettimeofday() 系统调用
  - [x] SubTask 68.4: 内核定时器列表与唤醒队列
- [x] Task 69: 完善管道与 shell 重定向 `pipe.c/h`
  - [x] SubTask 69.1: pipe() 匿名管道系统调用
  - [x] SubTask 69.2: 命名管道 FIFO（文件系统节点）
  - [x] SubTask 69.3: GSHELL I/O 重定向（> < >>）与管道连接（|）通过 dup2
- [x] Task 70: 实现事件通知 `event.c/h`
  - [x] SubTask 70.1: select()/poll() 系统调用
  - [x] SubTask 70.2: 预留 epoll/kqueue 接口（远期）
- [ ] Task 71: 实现 POSIX 兼容层 `posix.c/h`（远期愿景）
  - [ ] SubTask 71.1: POSIX API 子集
  - [ ] SubTask 71.2: Linux 系统调用号翻译层

## Task Dependencies
- Task 2 依赖 Task 1
- Task 3 依赖 Task 2
- Task 4 依赖 Task 3
- Task 7 依赖 Task 5
- Task 9 依赖 Task 8
- Task 10 依赖 Task 9
- Task 11 依赖 Task 9
- Task 12 依赖 Task 11
- Task 14 依赖 Task 13
- Task 15 依赖 Task 14
- Task 16 依赖 Task 13
- Task 19 依赖 Task 18
- Task 20 依赖 Task 14 与 Task 18
- Task 22 依赖 Task 21 与 Task 1
- Task 26 依赖 Task 25
- Task 27 依赖 Task 26
- Task 29 依赖 Task 28（软件光栅化）
- Task 30 依赖 Task 28 与 Task 25（SVGA3D 需 SVGA-II 基础）
- Task 31 依赖 Task 29 与 Task 30
- Task 32 依赖 Task 31
- Task 38B 依赖 Task 37 与 Task 38
- Task 39 依赖 Task 38B
- Task 34 依赖 Task 33
- Task 35 依赖 Task 14
- Task 37 依赖 Task 36
- Task 38 依赖 Task 36
- Task 41 依赖 Task 40
- Task 45 依赖 Task 25
- Task 49B 依赖 Task 49 与 Task 11
- Task 49C 依赖 Task 49
- Task 50 依赖 Task 14、Task 1 与 Task 49B
- Task 51B 依赖 Task 51
- Task 52 依赖 Task 51B（USB 是真实硬件输入与 U 盘启动前提）
- Task 53 依赖 Task 51
- Task 54 依赖 Task 53 与 Task 11
- Task 55 依赖 Task 51
- Task 56 依赖 Task 18 与 Task 14
- Task 57 依赖 Task 14 与 Task 53
- Task 58 依赖 Task 21 与 Task 14
- Task 59 依赖 Task 14
- Task 60 依赖 Task 49C 与 Task 49B
- Task 61 依赖 Task 14
- Task 62 依赖 Task 13
- Task 63 依赖 Task 16
- Task 64 依赖 Task 14 与 Task 16（VFS 核心整合 FAT32 与 devfs）
- Task 65 依赖 Task 64 与 Task 1
- Task 66 依赖 Task 1 与 Task 11
- Task 67 依赖 Task 66 与 Task 16
- Task 68 依赖 Task 3
- Task 69 依赖 Task 7 与 Task 64
- Task 70 依赖 Task 64 与 Task 54
- Task 71 依赖 Task 12 与 Task 60
- 阶段十二、十三（美化/流畅）与阶段一至八并行，无强依赖
