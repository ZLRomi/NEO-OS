# NEO-OS 完整操作系统演进 Spec

## Why
NEO-OS 当前是一个具备 GUI、网络栈、中文字体渲染的单体内核，但缺少真正操作系统的核心能力：没有进程与调度、没有文件系统、没有系统调用与用户态保护、没有同步原语与 IPC、没有虚拟内存与缺页处理。同时现有能力不足：浏览器 CSS 支持残缺、无下载功能、无多媒体播放、无壁纸自定义、无任务管理器、无应用安装机制、分辨率刷新率受限、仅能在 QEMU 上运行、无显卡驱动、UI 美化与流畅度不足。

本规范将 NEO-OS 演进为一个真正的、可脱离 QEMU 在真实硬件上运行的完整操作系统，具备完整的 OS 内核能力、现代浏览器、多媒体播放、桌面美化、应用生态与跨平台硬件抽象层。

## What Changes
- 新增进程管理子系统：PCB、创建/撤销/挂起/恢复、抢占式轮转与优先级调度
- 新增同步原语：自旋锁、信号量、互斥锁、死锁检测
- 新增进程间通信（IPC）：管道、消息队列、共享内存
- 增强内存管理：缺页中断处理、请求分页、内核态/用户态内存保护、ring3 用户态
- 新增 ATA/IDE 磁盘驱动与 FAT32 文件系统（读写）
- 新增设备驱动框架：统一设备接口、缓冲管理、设备分配表
- 新增系统调用接口（int 0x80）：文件、进程、内存、设备 API
- 新增用户认证与文件权限（rwx）
- 新增内核日志与系统监控（含任务管理器 GUI 应用）
- 新增显卡驱动层：VBE 增强 + 真实显卡驱动（Bochs/QEMU、VMware SVGA、VirtualBox、常见物理显卡），含 3D 加速支持
- 新增 3D 加速子系统：GPU 命令缓冲、3D 管线（顶点变换/裁剪/透视投影/光栅化/纹理映射/深度缓冲）、OpenGL 风格 API、硬件加速回退到软件渲染
- 提高分辨率（最高 1920×1080+）与刷新率（60Hz+）
- 浏览器增强：完整 CSS 引擎（选择器、盒模型、布局、颜色、字体）、文件下载功能
- 新增多媒体播放：MP3 解码播放（AC97/HDA 音频驱动）、MP4 解码播放（MPEG4/H.264 软解 + 帧缓冲渲染）
- 新增桌面美化：壁纸自定义（BMP/JPEG 加载）、极致美化（阴影、圆角、毛玻璃、动画）、流畅极致（双缓冲、脏矩形、缓动、节流）
- 新增第三方应用安装机制：NEO 应用包格式（.neap）、应用商店、安装/卸载/运行
- 新增硬件抽象层（HAL）：隔离平台差异，支持 QEMU、VMware、VirtualBox、真实 PC
- 脱离 QEMU：支持从 U 盘/硬盘启动，支持真实硬件

## Impact
- Affected code:
  - `kernel/kernel.c`（新增进程初始化、文件系统挂载、系统调用注册、应用商店初始化）
  - `kernel/idt.c`（缺页中断 #PF、系统调用 int 0x80、调度时钟、音频/显卡中断）
  - `kernel/memory.c`（缺页处理、用户态页表、高分辨率帧缓冲映射）
  - `kernel/gui*.c`（双缓冲、缓动动画、美化渲染、壁纸、任务管理器、应用商店 UI）
  - `kernel/gui_apps.c`（浏览器 CSS 引擎重写、下载管理、多媒体播放器）
  - 新增内核模块：`process.c/h`、`sched.c/h`、`sync.c/h`、`ipc.c/h`、`fs.c/h`、`disk.c/h`、`fat32.c/h`、`syscall.c/h`、`device.c/h`、`auth.c/h`、`klog.c/h`、`gfx_driver.c/h`、`gfx_3d.c/h`、`gpu_svga3d.c/h`、`render_sw.c/h`、`audio.c/h`、`mp3.c/h`、`mp4.c/h`、`jpeg.c/h`、`bmp.c/h`、`app_store.c/h`、`hal.c/h`
- Affected specs: 进程调度、内存管理、文件系统、设备管理、用户接口、安全审计、图形显示、浏览器、多媒体、桌面美化、应用生态、跨平台

## ADDED Requirements

### Requirement: 进程控制块与生命周期
系统 SHALL 维护进程控制块（PCB），记录 pid、状态、页表、寄存器上下文、优先级、时间片。系统 SHALL 支持进程的创建、撤销、挂起、恢复。

#### Scenario: 创建进程
- **WHEN** 内核调用 `process_create(entry, priority)`
- **THEN** 分配 PCB、初始化栈与页表，加入就绪队列，返回 pid

#### Scenario: 挂起与恢复
- **WHEN** 进程被挂起
- **THEN** 状态置为 BLOCKED，移出就绪队列，保留上下文
- **WHEN** 进程被恢复
- **THEN** 状态置为 READY，重新加入就绪队列

### Requirement: 抢占式调度
系统 SHALL 实现基于 PIT 时钟中断的抢占式调度，默认采用轮转调度，支持优先级抢占。时间片默认 10ms（PIT 重配置为 100Hz）。

#### Scenario: 时间片耗尽
- **WHEN** 当前进程时间片耗尽且就绪队列非空
- **THEN** 保存上下文，切换到下一就绪进程，重置时间片

#### Scenario: 优先级抢占
- **WHEN** 高优先级进程进入就绪队列
- **THEN** 在下一时钟中断抢占当前低优先级进程

### Requirement: 同步原语
系统 SHALL 提供自旋锁（关中断+自旋）、信号量（计数）、互斥锁。信号量 SHALL 支持阻塞与唤醒。

#### Scenario: 互斥访问
- **WHEN** 进程获取已被占用的互斥锁
- **THEN** 进程阻塞并加入等待队列，释放 CPU
- **WHEN** 锁被释放
- **THEN** 唤醒等待队列首进程

### Requirement: 进程间通信
系统 SHALL 提供管道（半双工字节流）、消息队列（定长消息）、共享内存（页映射）三种 IPC 机制。

#### Scenario: 管道通信
- **WHEN** 进程向管道写入数据
- **THEN** 数据存入管道缓冲，读端可读取
- **WHEN** 缓冲满时写端阻塞，缓冲空时读端阻塞

### Requirement: 虚拟内存与缺页处理
系统 SHALL 处理缺页中断（#PF, int 14），实现请求分页。系统 SHALL 支持内存保护，用户态进程无法访问内核页。

#### Scenario: 请求分页
- **WHEN** 进程访问未映射的合法虚拟地址
- **THEN** 缺页处理分配物理页，建立映射，重新执行指令
- **WHEN** 访问非法地址
- **THEN** 终止进程并记录错误

### Requirement: 系统调用接口
系统 SHALL 通过 int 0x80 提供系统调用，eax 为调用号。系统 SHALL 至少提供：sys_write、sys_read、sys_exit、sys_fork、sys_exec、sys_open、sys_close、sys_yield、sys_alloc、sys_play_audio、sys_draw_frame。

#### Scenario: 用户程序请求内核服务
- **WHEN** 用户态程序执行 int 0x80，eax=4（write）
- **THEN** 切换到 ring0，读取参数，写入文件/设备，返回写入字节数

### Requirement: ATA 磁盘驱动
系统 SHALL 通过 ATA/IDE PIO（LBA28/48）读写硬盘扇区，提供 `disk_read_sector`、`disk_write_sector`。

#### Scenario: 读取扇区
- **WHEN** 调用 `disk_read_sector(lba, buf)`
- **THEN** 通过 PIO 读取 512 字节到 buf，返回成功状态

### Requirement: FAT32 文件系统
系统 SHALL 实现 FAT32 读写，支持根目录遍历、文件打开/读/写/创建/删除、目录创建。系统 SHALL 用位示图管理空闲簇。

#### Scenario: 读取文件
- **WHEN** 调用 `fs_open("/path/file")`
- **THEN** 解析路径，定位文件目录项，返回文件描述符
- **WHEN** 调用 `fs_read(fd, buf, n)`
- **THEN** 按 FAT 链读取簇数据，返回读取字节数

#### Scenario: 创建文件
- **WHEN** 调用 `fs_create("/path/new.txt")`
- **THEN** 分配空闲簇，写入目录项，返回文件描述符

### Requirement: 虚拟文件系统（VFS）与多文件系统支持
系统 SHALL 实现虚拟文件系统抽象层（VFS），定义统一文件操作接口（open/read/write/readdir/stat）。系统 SHALL 通过 VFS 支持多种文件系统挂载，初期支持 FAT32，预留 ext2/NTFS 接口。系统 SHALL 支持挂载点（如 /、/mnt/usb）。

#### Scenario: 挂载文件系统
- **WHEN** 调用 `vfs_mount(dev, "/mnt/usb", fstype)`
- **THEN** 在挂载点注册文件系统，后续路径访问走该文件系统驱动

#### Scenario: 跨文件系统访问
- **WHEN** 访问 `/mnt/usb/file`
- **THEN** VFS 路由到 USB 设备的文件系统驱动

### Requirement: 磁盘调度算法
系统 SHALL 实现磁盘调度算法优化磁头移动，减少寻道延迟。系统 SHALL 支持电梯算法（SCAN/C-SCAN）与最短寻道时间优先（SSTF），默认使用电梯算法。

#### Scenario: 磁盘请求合并
- **WHEN** 多个磁盘 I/O 请求排队
- **THEN** 按电梯算法排序请求，磁头沿一个方向移动处理，减少往返寻道

### Requirement: 设备驱动框架
系统 SHALL 提供统一设备接口（open/read/write/ioctl/close），区分字符设备与块设备。系统 SHALL 维护设备表，支持设备名访问（/dev/hda、/dev/tty0、/dev/audio、/dev/fb0）。

#### Scenario: 访问设备
- **WHEN** 调用 `dev_open("/dev/tty0")`
- **THEN** 查设备表返回设备描述符，后续 read/write 调用驱动函数

### Requirement: 用户认证与权限
系统 SHALL 支持用户账户（用户名+密码哈希），登录后获得 uid。文件 SHALL 支持权限位（owner/group/other 的 r/w/x）。

#### Scenario: 登录验证
- **WHEN** 用户在登录面板输入用户名密码
- **THEN** 与账户记录比对密码哈希（SHA256），匹配则进入桌面，失败则提示

#### Scenario: 权限检查
- **WHEN** 进程访问文件
- **THEN** 检查进程 uid 与文件权限位，无权限则返回 EACCES

### Requirement: 内核日志与任务管理器
系统 SHALL 维护内核环形日志（klog），记录关键事件。系统 SHALL 提供图形化任务管理器应用，展示进程列表、CPU/内存占用、可结束进程。

#### Scenario: 查看进程
- **WHEN** 用户打开任务管理器
- **THEN** 列出所有进程的 pid、名称、状态、优先级、CPU 占用、内存占用
- **WHEN** 用户点击"结束任务"
- **THEN** 终止选定进程并刷新列表

### Requirement: 显卡驱动与高分辨率高刷新率
系统 SHALL 实现显卡驱动抽象层，支持多种显卡：Bochs/QEMU VBE、VMware SVGA-II、VirtualBox VMSVGA、通用 VESA VBE。系统 SHALL 支持最高 1920×1080 32bpp，刷新率 60Hz+。系统 SHALL 支持运行时切换分辨率。

#### Scenario: 探测显卡
- **WHEN** 内核启动
- **THEN** 探测 PCI 显卡设备，加载对应驱动，初始化最优分辨率
- **WHEN** 无专用驱动
- **THEN** 回退到通用 VESA VBE 模式

#### Scenario: 切换分辨率
- **WHEN** 用户在显示设置中选择新分辨率
- **THEN** 调用驱动设置新模式，重映射帧缓冲，刷新桌面

### Requirement: 3D 加速显卡驱动
系统 SHALL 实现 3D 加速驱动，支持硬件 3D 渲染。系统 SHALL 支持 VMware SVGA-II 3D（SVGA3D）命令缓冲接口进行硬件加速渲染。系统 SHALL 在无硬件 3D 加速时回退到软件 3D 渲染（CPU 光栅化）。系统 SHALL 提供 GPU 命令缓冲机制，批量提交绘制命令到 GPU。

#### Scenario: 硬件 3D 加速可用
- **WHEN** 系统检测到支持 3D 的显卡（如 VMware SVGA3D）
- **THEN** 初始化 3D 上下文与命令缓冲，启用硬件加速渲染
- **WHEN** 应用提交 3D 绘制命令
- **THEN** 命令通过命令缓冲提交到 GPU 硬件执行

#### Scenario: 软件 3D 回退
- **WHEN** 显卡不支持 3D 加速
- **THEN** 回退到软件 3D 渲染器（CPU 光栅化），3D 应用仍可运行但帧率较低

### Requirement: 3D 渲染管线
系统 SHALL 实现完整 3D 渲染管线，包含：顶点变换（模型/视图/投影矩阵）、裁剪、透视除法、视口变换、光栅化（三角形扫描）、纹理映射（含透视校正）、深度缓冲（Z-buffer）、颜色混合。系统 SHALL 提供 OpenGL 风格的 3D API（顶点/纹理/矩阵/绘制命令）。

#### Scenario: 渲染 3D 场景
- **WHEN** 应用调用 3D API 提交三角形与纹理
- **THEN** 管线执行顶点变换、光栅化、纹理映射、深度测试，输出到帧缓冲
- **WHEN** 存在遮挡
- **THEN** 深度缓冲正确处理前后遮挡关系

#### Scenario: 纹理映射
- **WHEN** 三角形带有纹理坐标
- **THEN** 透视校正纹理映射正确采样纹理像素

### Requirement: 浏览器完整 CSS 引擎
系统 SHALL 实现完整 CSS 解析与渲染引擎，支持：选择器（标签/类/ID/后代）、盒模型（margin/border/padding/content）、布局（块级/行内/流式）、颜色（十六进制/rgb/命名）、字体（大小/粗细/样式）、背景、边框、定位。

#### Scenario: CSS 渲染
- **WHEN** 浏览器加载含 CSS 的 HTML
- **THEN** 解析 <style> 与 style 属性，应用样式到对应元素，按盒模型布局渲染
- **WHEN** 元素有 `color: #ff0000`
- **THEN** 文本渲染为红色
- **WHEN** 元素有 `background-color`、`border`、`padding`
- **THEN** 正确绘制背景色、边框、内边距

### Requirement: 浏览器下载功能
系统 SHALL 实现文件下载，支持 HTTP/HTTPS 下载文件到磁盘。系统 SHALL 提供下载管理器 UI，显示进度、可取消。

#### Scenario: 下载文件
- **WHEN** 用户点击下载链接或输入下载 URL
- **THEN** 通过 HTTP/HTTPS 获取文件，写入文件系统，显示下载进度
- **WHEN** 下载完成
- **THEN** 通知用户并保存到指定路径

### Requirement: 多媒体播放 MP3
系统 SHALL 实现 MP3 音频软解作为验证解码管线的第一步。系统 SHALL 实现音频驱动（Intel AC97 或 Intel HDA）。系统 SHALL 提供播放器 UI（播放/暂停/停止/进度）。系统 SHALL 在解码路径上预留硬件加速解码接口（`audio_decode_iface`），以便未来接入硬件解码器。

> 专利与算力说明：MP3 软解存在软件专利风险，CPU 软解仅作为初期验证管线使用，未来应迁移至硬件解码或免专利格式（如 Vorbis/Opus）。

#### Scenario: 播放 MP3
- **WHEN** 用户在文件管理器打开 .mp3 文件
- **THEN** 启动播放器，软解 MP3 帧，通过音频驱动输出声音，UI 显示进度
- **WHEN** 用户点击暂停
- **THEN** 暂停解码与输出

### Requirement: 多媒体播放 MP4
系统 SHALL 实现 MP4 容器解析与视频软解（MPEG-4 Part 2 软解）作为验证解码管线的第一步。系统 SHALL 在解码路径上预留硬件加速解码接口（`video_decode_iface`），以便未来接入硬件解码器。系统 SHALL 提供视频播放器 UI（播放/暂停/进度/全屏）。

> 专利与算力说明：H.264 软解存在软件专利风险且对 CPU 算力要求极高，720p 以上可能无法流畅播放。初期软解仅用于验证管线正确性，未来应迁移至硬件解码或免专利格式（如 VP8/VP9/AV1）。

#### Scenario: 播放 MP4（软解验证）
- **WHEN** 用户打开 .mp4 文件
- **THEN** 解析 MP4 容器，软件解码视频帧，按帧率渲染到屏幕，同步播放音频
- **WHEN** 用户点击全屏
- **THEN** 视频铺满屏幕

#### Scenario: 硬件解码接口预留
- **WHEN** 未来集成硬件解码器
- **THEN** 通过预留的 video_decode_iface 接口接入，无需改动播放器上层逻辑

### Requirement: 壁纸自定义
系统 SHALL 支持自定义桌面壁纸，支持 BMP 与 JPEG 格式。系统 SHALL 在显示设置中提供壁纸选择，壁纸保存在文件系统。

#### Scenario: 设置壁纸
- **WHEN** 用户在显示设置中选择壁纸图片
- **THEN** 加载图片解码，铺满或居中或拉伸显示到桌面，保存选择
- **WHEN** 系统重启
- **THEN** 从配置加载上次选择的壁纸

### Requirement: 桌面极致美化
系统 SHALL 实现极致美化：窗口投影阴影（半透明渐变）、圆角窗口、毛玻璃/半透明效果（任务栏/菜单）、标题栏垂直渐变、图标高光、平滑过渡动画、统一现代设计语言。系统 SHALL 使用 SVG 图标（已有）保持矢量清晰。

#### Scenario: 窗口阴影
- **WHEN** 窗口渲染
- **THEN** 在窗口右下方绘制半透明阴影（向右下偏移，alpha 渐变）

#### Scenario: 毛玻璃效果
- **WHEN** 任务栏/开始菜单渲染
- **THEN** 半透明背景，可透出桌面内容模糊化效果

### Requirement: 流畅极致
系统 SHALL 使用双缓冲消除撕裂。系统 SHALL 使用脏矩形列表仅重绘变化区域。系统 SHALL 使用缓动函数（ease-out/ease-in-out）实现窗口/菜单动画。系统 SHALL 对鼠标移动与重绘进行节流。系统 SHALL 优化热路径（字体渲染、填充）减少开销。

#### Scenario: 无撕裂渲染
- **WHEN** 画面更新
- **THEN** 先绘制到 back_buffer，再一次性 blit，肉眼无撕裂

#### Scenario: 平滑动画
- **WHEN** 窗口最小化
- **THEN** 使用 ease-out 缓动从原位置缩放至任务栏，动画期间不阻塞输入

### Requirement: 第三方应用安装
系统 SHALL 定义 NEO 应用包格式（.neap），包含可执行代码 + 元数据（名称/图标/版本/权限）。系统 SHALL 提供应用商店 GUI，可浏览/下载/安装/卸载/启动应用。系统 SHALL 将已安装应用注册到开始菜单与桌面。

> ABI 稳定性约束：.neap 包要能运行，需要稳定的用户态应用程序二进制接口（ABI）。系统 SHALL 严格定义系统调用接口（一旦确定不再轻易更改）。系统 SHALL 预先提供一套编译好的用户态标准库（libc）。初期 SHALL 只支持 C 语言静态链接包，简化 ABI 兼容问题（动态链接与多语言支持留待未来）。

#### Scenario: 安装应用
- **WHEN** 用户在应用商店点击安装
- **THEN** 下载 .neap 包，校验签名，解压到 /apps 目录，注册到开始菜单
- **WHEN** 用户在开始菜单点击应用
- **THEN** 加载应用可执行文件（C 静态链接 ELF/NEO 格式），创建进程运行

#### Scenario: 卸载应用
- **WHEN** 用户在应用商店点击卸载
- **THEN** 终止运行中的进程，删除文件，移除注册

### Requirement: 用户态 ABI 与标准库
系统 SHALL 严格定义并冻结系统调用编号与参数布局（ABI）。系统 SHALL 提供 NEO libc 用户态标准库（C 静态链接），封装系统调用并提供 POSIX 风格 API（open/read/write/close/exit/fork/exec/malloc）。系统 SHALL 提供 NEO 应用编译工具链说明（基于 GCC -ffreestanding 静态链接）。系统 SHALL 定义 .neap 可执行文件格式（头部 + 代码段 + 数据段 + manifest）。

#### Scenario: 编译第三方应用
- **WHEN** 开发者使用 NEO libc 与工具链编译应用
- **THEN** 生成 C 静态链接的 .neap 可执行文件，可在 NEO-OS 上运行
- **WHEN** 系统升级
- **THEN** 已编译的 .neap 包仍能运行（ABI 向后兼容）

### Requirement: 硬件抽象层（HAL）
系统 SHALL 实现硬件抽象层，隔离平台差异：显示（VBE/SVGA）、音频（AC97/HDA）、磁盘（ATA/AHCI）、输入（PS/2/USB）。系统 SHALL 通过运行时探测自动选择驱动。

#### Scenario: 平台适配
- **WHEN** 系统在不同平台（QEMU/VMware/真实PC）启动
- **THEN** HAL 探测硬件，加载对应驱动，系统正常运行

### Requirement: 脱离 QEMU 运行
系统 SHALL 支持从真实硬件启动（通过 GRUB 引导，ISO 或硬盘安装）。系统 SHALL 不依赖 QEMU 特有硬件（提供通用驱动回退）。

#### Scenario: 真实硬件启动
- **WHEN** 将 NEO-OS ISO 刻录到 U 盘并在真实 PC 启动
- **THEN** GRUB 引导内核，系统探测硬件，进入桌面

### Requirement: 完整网络协议栈
系统 SHALL 实现完整 TCP/IP 协议栈：IP、ICMP、ARP、TCP、UDP。系统 SHALL 提供 Socket API 作为系统调用暴露给用户态，支持流式（TCP）与数据报（UDP）套接字。系统 SHALL 实现 DHCP 客户端自动获取 IP。系统 SHALL 实现 DNS 解析（复用现有 net_dns_resolve）。

#### Scenario: DHCP 获取 IP
- **WHEN** 系统启动且网卡初始化
- **THEN** 发送 DHCP DISCOVER，接收 OFFER/ACK，配置本机 IP/网关/DNS
- **WHEN** DHCP 失败
- **THEN** 回退到静态配置（10.0.2.15）

#### Scenario: Socket 网络通信
- **WHEN** 应用调用 socket(AF_INET, SOCK_STREAM) 并 connect
- **THEN** 系统建立 TCP 连接，应用可 send/recv 数据
- **WHEN** 应用调用 socket(AF_INET, SOCK_DGRAM) 并 sendto
- **THEN** 系统通过 UDP 发送数据报

### Requirement: 电源管理
系统 SHALL 支持 ACPI 电源管理：读取 ACPI 表、控制电源状态。系统 SHALL 实现软关机与重启（通过 ACPI 或 APM，非直接断电）。系统 SHALL 实现空闲降功耗（HLT 指令，CPU 空闲时进入低功耗而非忙等）。系统 SHALL 预留睡眠/休眠接口（挂起到内存/磁盘，长期目标）。

#### Scenario: 软关机
- **WHEN** 用户点击关机
- **THEN** 通过 ACPI 发送关机命令，系统安全退出并断电

#### Scenario: 空闲降功耗
- **WHEN** 系统无就绪进程
- **THEN** 执行 HLT 进入低功耗，下次中断唤醒

### Requirement: USB 驱动栈
系统 SHALL 实现 USB 主机控制器驱动（UHCI/OHCI/EHCI/xHCI，至少支持一种）。系统 SHALL 实现 USB HID 驱动支持 USB 键盘/鼠标。系统 SHALL 实现 USB Mass Storage 驱动支持 U 盘读写。

#### Scenario: USB 键盘鼠标
- **WHEN** 真实 PC 使用 USB 键盘鼠标
- **THEN** USB HID 驱动接收输入，转换为系统输入事件

#### Scenario: U 盘读写
- **WHEN** 插入 U 盘
- **THEN** USB Mass Storage 驱动识别设备，注册为块设备，可挂载访问

### Requirement: 动态链接与共享库（演进目标）
系统 SHALL 在静态链接 .neap 基础上演进支持动态链接：实现动态链接器（加载 ELF 解析动态符号、完成重定位）、共享库机制（多应用共享同一份代码内存映射）。初期保持 C 静态链接，动态链接作为后续演进目标。

#### Scenario: 动态链接应用
- **WHEN** 加载动态链接的 .neap
- **THEN** 动态链接器解析依赖共享库，重定位符号，加载运行
- **WHEN** 多个应用依赖同一 libc.so
- **THEN** 内存中仅映射一份共享代码页

### Requirement: 用户管理增强
系统 SHALL 实现用户/组管理数据库（/etc/passwd 与 /etc/shadow 简化版，存储用户名/UID/GID/密码哈希）。系统 SHALL 实现权限提升机制（类似 sudo/su，普通用户临时获取管理员权限）。系统 SHALL 预留多用户会话接口（多用户登录，Ctrl+Alt+F1-F7 切换，长期目标）。

#### Scenario: 用户数据库
- **WHEN** 用户登录
- **THEN** 读取 /etc/passwd 与 /etc/shadow 比对凭据

#### Scenario: 权限提升
- **WHEN** 普通用户执行需要管理员权限的操作
- **THEN** 提示输入管理员密码，验证后临时提权执行

### Requirement: 系统更新机制
系统 SHALL 实现系统更新：内核与系统包更新。系统 SHALL 支持 A/B 启动分区或内核热更新，更新失败可回滚。系统 SHALL 实现包管理器（更新内核模块、驱动、系统库）。系统 SHALL 实现在线更新检查（连接服务器检查新版本）。

#### Scenario: 系统更新
- **WHEN** 用户在系统设置检查更新
- **THEN** 连接更新服务器，比对版本，下载并安装更新
- **WHEN** 更新失败
- **THEN** 回滚到上一可用版本（A/B 分区）

### Requirement: 开发与调试基础设施
系统 SHALL 提供内核调试基础设施：内核级 debugger（可通过串口或网络远程调试）、性能剖析工具（CPU 采样、内存检测、I/O 热点）、崩溃转储（panic 时将内存 dump 到磁盘供事后分析）。

#### Scenario: 内核崩溃转储
- **WHEN** 内核发生 panic
- **THEN** 将内存与寄存器状态 dump 到磁盘文件，重启系统

#### Scenario: 性能剖析
- **WHEN** 开发者启动性能剖析
- **THEN** 采样 CPU 占用，输出热点函数报告

### Requirement: 国际化与本地化框架
系统 SHALL 实现 Unicode 全面支持（字符串比较、大小写转换、排序）。系统 SHALL 实现多语言界面框架（桌面与应用文本从资源文件加载，支持语言切换）。系统 SHALL 实现输入法框架（中文拼音/五笔输入法底层接口）。

#### Scenario: 多语言切换
- **WHEN** 用户在设置中切换语言
- **THEN** 加载对应语言资源文件，界面文本更新

#### Scenario: 中文输入法
- **WHEN** 用户在文本框输入拼音
- **THEN** 输入法框架提供候选词，用户选择后输入中文字符

### Requirement: 虚拟文件系统（VFS）核心数据结构
系统 SHALL 实现 VFS 核心数据结构：struct file（打开文件）、struct inode（索引节点）、struct dentry（目录项）。系统 SHALL 实现每进程独立文件描述符表，支持 dup、dup2、close-on-exec。系统 SHALL 实现 mount/umount 挂载机制，将不同文件系统挂载到目录树任意节点。系统 SHALL 遵循"一切皆文件"哲学。

#### Scenario: 文件描述符
- **WHEN** 进程 open 文件
- **THEN** 在进程 fd 表分配最小可用 fd，struct file 引用 inode
- **WHEN** 调用 dup2(oldfd, newfd)
- **THEN** newfd 指向同一 struct file，引用计数增加

#### Scenario: 挂载文件系统
- **WHEN** 调用 mount(dev, "/mnt/usb", "fat32")
- **THEN** 在 /mnt/usb 节点挂载 FAT32，路径访问走该文件系统

### Requirement: procfs 虚拟文件系统
系统 SHALL 实现 procfs 挂载到 /proc，暴露进程信息与内核状态：/proc/[pid]/status（进程状态）、/proc/[pid]/mem（进程内存）、/proc/meminfo、/proc/cpuinfo。系统 SHALL 让 ps/top 与任务管理器通过读取 /proc 获取进程列表，无需特殊系统调用。

#### Scenario: 读取进程信息
- **WHEN** 读取 /proc/3/status
- **THEN** 返回 pid 3 进程的状态文本（名称/状态/优先级/内存）
- **WHEN** ps 命令列出 /proc
- **THEN** 通过遍历 /proc/[pid] 获取所有进程

### Requirement: 信号机制
系统 SHALL 实现 UNIX 信号机制：kill() 系统调用发送信号、signal/sigaction 注册处理函数、内核返回用户态时检查并调用处理函数。系统 SHALL 支持常见信号：SIGINT(Ctrl+C)、SIGKILL、SIGTERM、SIGSEGV(段错误)、SIGPIPE(管道破裂)。任务管理器"结束任务"SHALL 通过发送 SIGKILL/SIGTERM 实现。

#### Scenario: 发送信号
- **WHEN** 调用 kill(pid, SIGTERM)
- **THEN** 目标进程在下次返回用户态时处理信号
- **WHEN** 进程注册 SIGINT 处理函数并收到 SIGINT
- **THEN** 内核调用注册的处理函数

#### Scenario: 段错误
- **WHEN** 进程访问非法内存触发缺页
- **THEN** 内核向进程发送 SIGSEGV，默认终止进程

### Requirement: 终端子系统（TTY）
系统 SHALL 实现终端驱动层：键盘输入回显、行缓冲、Ctrl+C 转为 SIGINT。系统 SHALL 实现伪终端（PTY）让 GUI 终端模拟器运行 shell。系统 SHALL 实现虚拟终端切换（Alt+F1~F7 多独立会话）。

#### Scenario: 终端行缓冲
- **WHEN** 用户在终端输入字符
- **THEN** 字符回显，行缓冲至回车后提交给 shell
- **WHEN** 用户按 Ctrl+C
- **THEN** 终端驱动向前台进程发送 SIGINT

#### Scenario: PTY 与 GUI 终端
- **WHEN** GUI 终端模拟器打开
- **THEN** 分配 PTY，shell 从从端读输入，模拟器写主端

### Requirement: 定时器与时钟服务
系统 SHALL 实现 RTC 驱动从 CMOS 读取/设置实时时钟，关机后时间不丢失。系统 SHALL 实现 sleep()/usleep() 系统调用让进程休眠指定时间。系统 SHALL 实现 gettimeofday() 获取当前日期时间。系统 SHALL 维护内核定时器列表，时钟中断检查到期定时器唤醒进程。

#### Scenario: 进程休眠
- **WHEN** 调用 sleep(seconds)
- **THEN** 进程阻塞加入定时器队列，到期后唤醒
- **WHEN** 系统启动
- **THEN** 从 RTC 读取时间，任务栏显示正确日期时间

### Requirement: 管道与 shell 重定向
系统 SHALL 实现匿名管道 pipe() 系统调用（父子进程通信）。系统 SHALL 实现命名管道 FIFO（存在于文件系统，无关进程按路径通信）。系统 SHALL 在 GSHELL 实现 I/O 重定向（>、<、>>）与管道连接（|），通过 dup2 实现。

#### Scenario: 管道连接
- **WHEN** GSHELL 执行 `cmd1 | cmd2`
- **THEN** 创建管道，cmd1 stdout 重定向到管道写端，cmd2 stdin 重定向到读端
- **WHEN** GSHELL 执行 `cmd > file`
- **THEN** cmd stdout 重定向到 file

### Requirement: 事件通知机制
系统 SHALL 实现 select()/poll() 系统调用，允许一个线程同时等待多个 fd（管道、socket、设备）的可读/可写事件。系统 SHALL 预留 epoll/kqueue 高效事件通知接口（远期目标），支撑高并发网络服务。

#### Scenario: 多路等待
- **WHEN** 应用 select 等待多个 socket
- **THEN** 任一 socket 可读时 select 返回，应用处理对应 fd

### Requirement: POSIX 兼容层（远期愿景）
系统 SHALL 实现 POSIX API 子集，便于移植 Linux/BSD 程序。系统 SHALL 实现系统调用翻译层，将 Linux 系统调用号映射到 NEO-OS 等价调用（类似 WSL1）。

#### Scenario: 移植 Linux 程序
- **WHEN** 重新编译 Linux 程序链接 NEO libc
- **THEN** 程序在 NEO-OS 运行（POSIX 子集兼容）

## MODIFIED Requirements

### Requirement: kernel_main 启动流程
内核启动流程 SHALL 在 memory_init 后增加：HAL 探测、进程子系统初始化、文件系统挂载、设备表初始化、系统调用注册、音频/显卡驱动加载、应用商店初始化。GUI 作为默认入口。

### Requirement: IDT 中断处理
IDT SHALL 新增：缺页中断（#PF, int 14）、系统调用门（int 0x80, DPL=3）、调度器时钟（PIT 100Hz）、音频/显卡/磁盘硬件中断。现有键盘/鼠标 ISR 保持不变。

### Requirement: 内存管理
PMM/VMM 既有接口保持不变，新增缺页处理钩子、用户态页表（user 位）。高分辨率帧缓冲需映射更大虚拟区域。

### Requirement: GUI 渲染管线
GUI SHALL 引入 back_buffer 作为唯一绘制目标。gui_dirty 扩展为脏矩形区域列表。所有 fb_* 绘制目标改为 back_buffer。新增壁纸渲染层、阴影渲染、圆角裁剪。

### Requirement: 浏览器渲染
浏览器 SHALL 重写渲染管线，从当前的内联样式标记升级为完整 CSS 盒模型布局。保留现有 HTML 解析、UTF-8 中文支持、超链接功能。

## REMOVED Requirements
（无移除项，所有改动均为增量演进）
