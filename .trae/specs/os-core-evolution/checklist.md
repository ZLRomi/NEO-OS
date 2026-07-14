# Checklist

## 阶段一：进程管理与调度
- [x] PCB 结构定义完整（pid、状态、上下文、页目录、优先级、时间片、名称）
- [x] process_create 能分配 PCB 并加入就绪队列，返回 pid
- [x] process_exit 能回收进程资源并从队列移除
- [x] switch_to 正确保存/恢复寄存器与 CR3
- [x] PIT 重配为 100Hz，时钟 ISR 触发 schedule
- [x] round-robin 调度在时间片耗尽时切换进程
- [x] 优先级调度使高优先级进程抢占
- [x] kernel_main 中调用 process_init 启动首个进程

## 阶段二：同步原语与 IPC
- [x] 自旋锁在多进程下互斥正确
- [x] 信号量 wait 在计数为 0 时阻塞
- [x] 信号量 post 唤醒等待队列首进程
- [x] 互斥锁 owner 字段正确设置与释放
- [x] 死锁检测能识别资源环并输出警告
- [x] 管道写入满时阻塞，读出空时阻塞
- [x] 消息队列支持定长消息收发
- [x] 共享内存页能被两个进程同时访问

## 阶段三：内存保护与虚拟内存
- [x] 缺页中断（#PF）被正确捕获
- [x] 缺页处理能读取 CR2 获取缺页地址
- [x] 合法地址缺页时分配物理页并映射，指令重新执行
- [x] 非法地址访问导致进程终止并记录 klog
- [x] 用户页表 user 位置位，内核页表 user 位清零
- [x] 进程切换时 CR3 指向新进程页目录
- [x] 请求分页：用户进程访问未映射页时按需分配

## 阶段四：系统调用接口
- [x] IDT 0x80 门 DPL=3，ring3 可调用
- [x] 系统调用分发器按 eax 正确分发
- [x] sys_write/sys_read 功能正确
- [x] sys_exit 正确终止调用进程
- [x] sys_yield 主动让出 CPU
- [x] sys_fork 复制父进程页表（深拷贝用户空间，trampoline 子进程返回0）
- [ ] sys_exec 加载新镜像（待文件系统）
- [x] sys_open/sys_close/sys_read/sys_write 对接 FAT32 文件系统
- [x] sys_alloc/sys_draw_frame/sys_play_audio 可供应用使用

## 阶段五：磁盘驱动与文件系统
- [x] ATA 驱动能正确读取 LBA28 扇区
- [x] ATA 驱动能正确写入扇区
- [x] FAT32 BPB 解析正确，FAT 表读取正确
- [x] 根目录遍历能列出文件与子目录
- [x] 路径解析支持多级目录
- [x] fs_open 返回正确文件描述符
- [x] fs_read 按 FAT 链读取簇数据正确
- [x] fs_create/fs_delete 能分配/回收簇
- [x] 空闲簇位示图与实际 FAT 一致
- [x] GSHELL 的 ls/cat/cd/mkdir/rm 命令可用

## 阶段六：设备驱动框架
- [x] 字符设备与块设备结构定义清晰
- [x] dev_open 按 /dev/xxx 路径返回设备描述符
- [x] dev_read/dev_write 调用对应驱动函数
- [x] 键盘、鼠标、磁盘、帧缓冲已注册为设备
- [x] 块设备缓冲区 LRU 淘汰正常

## 阶段七：用户认证与权限
- [ ] 用户账户记录包含用户名、密码哈希、uid
- [ ] 登录验证用 SHA256 比对密码
- [ ] 锁屏登录面板对接账户系统
- [ ] 文件权限位（rwx）正确存储
- [ ] open/read/write 时按 uid 校验权限，无权限返回 EACCES

## 阶段八：系统监控与审计
- [ ] klog 环形缓冲实现，klog_write 写入正确
- [ ] 进程创建/退出、缺页、错误、IPC 事件被记录
- [ ] 任务管理器列出进程 pid/名称/状态/优先级/CPU 占用/内存占用
- [ ] 任务管理器可结束选定进程
- [ ] 任务管理器 CPU/内存图表显示正确
- [ ] GSHELL ps/meminfo/top/klog 命令可用
- [ ] 统一中断分发器工作正常，现有键鼠 ISR 不受影响

## 阶段九：显卡驱动与高分辨率
- [ ] 显卡驱动接口定义清晰
- [ ] Bochs/QEMU VBE 驱动基于现有代码工作
- [ ] VMware SVGA-II 驱动在 VMware 中工作
- [ ] 通用 VESA VBE 回退驱动在无专用驱动时工作
- [ ] 支持 1920×1080 32bpp
- [ ] 刷新率达到 60Hz+
- [ ] back_buffer 动态适配分辨率
- [ ] 显示设置面板可切换分辨率
- [ ] 切换后帧缓冲重映射并刷新桌面

## 阶段九B：3D 加速显卡驱动
- [ ] 3D API 接口定义完整（顶点/纹理/矩阵/绘制/清深度缓冲）
- [ ] 矩阵运算（模型/视图/投影/相乘）正确
- [ ] 顶点变换管线（变换+裁剪+透视除法+视口变换）正确
- [ ] 深度缓冲（Z-buffer）分配与测试正确
- [ ] 三角形扫描转换（边函数/重心坐标）正确
- [ ] 透视校正纹理映射正确
- [ ] 颜色混合与深度测试写入正确
- [ ] SVGA3D 命令缓冲初始化与提交成功
- [ ] 硬件表面/上下文管理正确
- [ ] 3D API 命令正确转译为 SVGA3D 命令
- [ ] 启动时探测 SVGA3D 能力位
- [ ] 无 3D 硬件时回退到软件渲染器
- [ ] 3D 演示应用（旋转立方体）正确渲染
- [ ] 深度缓冲正确处理前后遮挡
- [ ] 有 3D 硬件时使用硬件加速，无时使用软件回退

## 阶段十：浏览器增强
- [ ] CSS 解析器支持标签/类/ID/后代选择器
- [ ] 盒模型（margin/border/padding/content）布局正确
- [ ] 颜色（十六进制/rgb/命名）正确解析
- [ ] 字体（大小/粗细/样式）正确应用
- [ ] 背景色、边框、内边距正确渲染
- [ ] 块级/行内元素流式布局正确
- [ ] DOM 节点树构建正确
- [ ] 样式正确应用到节点
- [ ] HTTP/HTTPS 下载文件到文件系统
- [ ] 下载管理器显示进度、可取消
- [ ] 浏览器点击下载链接触发下载
- [ ] 现有 HTML 解析、UTF-8 中文、超链接功能保留

## 阶段十一：多媒体播放
- [ ] AC97 音频驱动 PCI 探测成功
- [ ] 音频 play/stop/pause 接口工作
- [ ] MP3 帧头解析与同步正确
- [ ] MP3 Huffman 解码与反量化正确
- [ ] MP3 IMDCT 输出正确 PCM
- [ ] MP3 播放有声音输出
- [ ] MP4 容器 box 结构解析正确
- [ ] MP4 视频帧软解正确（验证管线）
- [ ] MP4 帧渲染到屏幕
- [ ] 音视频同步播放
- [ ] 硬件加速解码接口（video_decode_iface/audio_decode_iface）已定义
- [ ] 软解实现该接口，播放器上层仅依赖接口
- [ ] 硬件解码后端注册点已预留
- [ ] 音频播放器 UI（播放/暂停/停止/进度）可用
- [ ] 视频播放器 UI（播放/暂停/进度/全屏）可用
- [ ] 双击 .mp3/.mp4 启动播放器

## 阶段十二：桌面美化与壁纸
- [x] BMP 解码（24/32 位）正确
- [x] JPEG 解码（基准 DCT）正确
- [x] 壁纸加载并渲染（铺满/居中/拉伸）
- [x] 显示设置中可选壁纸
- [x] 壁纸配置持久化到文件系统
- [ ] 重启后恢复上次壁纸
- [x] 窗口投影阴影渲染正确（半透明，右下偏移）
- [x] 窗口圆角边框渲染正确
- [x] 标题栏垂直渐变渲染正确
- [x] 任务栏半透明 + 背景模糊
- [x] 开始菜单/右键菜单半透明效果
- [x] 图标悬停高光、选中态正常

## 阶段十三：流畅极致
- [x] back_buffer 为唯一绘制目标
- [x] 脏矩形列表机制生效，仅重绘变化区域
- [x] fb_blit 仅复制脏区域
- [x] ease-out/ease-in-out/ease-in 缓动函数正确
- [x] 窗口最小化/恢复动画使用缓动，视觉平滑
- [x] 开始菜单/登录动画使用缓动
- [x] 鼠标移动事件节流，无抖动
- [x] 动画期间输入不阻塞
- [x] 字体渲染、填充等热路径已优化

## 阶段十四：第三方应用与跨平台
- [x] 系统调用 ABI（编号与参数布局）已严格定义并冻结
- [x] ABI 稳定性约束已文档化
- [x] .neap 可执行格式定义完整（头部 + 代码段 + 数据段 + manifest）
- [x] .neap 加载器正确解析并加载段、创建进程
- [x] NEO libc 用户态标准库提供系统调用封装
- [x] NEO libc 提供 POSIX 风格 API（open/read/write/close/exit/fork/exec）
- [x] NEO libc 提供内存管理（malloc/free）
- [x] NEO libc 提供字符串/格式化函数
- [x] NEO 应用编译工具链说明已提供（GCC -ffreestanding 静态链接，C 语言）
- [x] 使用 NEO libc 编译的 C 静态链接 .neap 包可在 NEO-OS 运行
- [ ] 应用商店可浏览应用列表
- [ ] 安装功能：下载、校验、解压到 /apps
- [ ] 卸载功能：终止进程、删除文件、移除注册
- [ ] 已安装应用出现在开始菜单与桌面
- [ ] 应用启动：加载 .neap，创建进程运行
- [ ] HAL 显示抽象工作（VBE/SVGA/VMSVGA）
- [ ] HAL 音频抽象工作（AC97/HDA）
- [ ] HAL 磁盘抽象工作（ATA/AHCI）
- [ ] HAL 输入抽象工作（PS/2/USB）
- [ ] 运行时探测自动选择驱动
- [ ] USB 主机控制器驱动至少支持一种（UHCI/OHCI/EHCI/xHCI）
- [ ] USB HID 驱动支持 USB 键盘/鼠标
- [ ] USB Mass Storage 驱动支持 U 盘读写
- [ ] USB 设备热插拔检测与注册
- [ ] GRUB 引导在真实硬件可用
- [ ] 通用 VESA VBE 回退在无专用驱动时工作
- [ ] ISO 可刻录到 U 盘启动

## 阶段十五：系统服务增强
- [ ] ARP/ICMP/UDP/TCP 协议完整实现
- [ ] DHCP 客户端自动获取 IP 成功
- [ ] DHCP 失败回退静态配置
- [ ] Socket API（socket/connect/bind/listen/accept）系统调用可用
- [ ] send/recv/sendto/recvfrom 系统调用可用
- [ ] TCP 流式套接字通信成功
- [ ] UDP 数据报套接字通信成功
- [ ] ACPI 表读取成功
- [ ] 软关机通过 ACPI 命令断电
- [ ] 重启功能正常
- [ ] 空闲时执行 HLT 降功耗
- [x] /etc/passwd 与 /etc/shadow 读写正确
- [x] 权限提升（sudo/su）机制工作
- [ ] A/B 启动分区或内核热更新可用
- [ ] 包管理器可更新系统组件
- [ ] 在线更新检查工作
- [ ] 更新失败可回滚
- [ ] 内核级 debugger 可通过串口/网络调试
- [ ] 性能剖析输出热点报告
- [ ] 内核 panic 时崩溃转储到磁盘
- [ ] Unicode 字符串比较/大小写/排序正确
- [ ] 多语言资源文件加载与切换
- [ ] 输入法框架支持中文输入
- [ ] 动态链接器解析动态符号并重定位
- [ ] 共享库内存映射共享代码页
- [x] VFS 统一文件操作接口定义
- [x] 挂载点管理正确
- [x] FAT32 接入 VFS
- [x] 磁盘调度算法（电梯/SSTF）减少寻道
- [x] SPOOLing 虚拟化独占设备（打印机）

## 阶段十六：UNIX/POSIX 子系统
- [ ] struct file / struct inode / struct dentry 定义完整
- [ ] 每进程独立 fd 表工作正常
- [ ] dup/dup2/close-on-exec 正确
- [ ] mount/umount 挂载机制工作
- [ ] FAT32 与 devfs 接入 VFS
- [x] procfs 挂载到 /proc
- [x] /proc/[pid]/status 返回进程信息
- [x] /proc/meminfo、/proc/cpuinfo 可读
- [x] ps/top 通过 /proc 获取进程列表
- [x] kill() 系统调用发送信号成功
- [x] signal/sigaction 注册处理函数工作
- [x] 内核返回用户态时正确调用信号处理函数
- [x] SIGINT/SIGKILL/SIGTERM/SIGSEGV/SIGPIPE 支持完整
- [x] 任务管理器"结束任务"通过信号实现
- [x] 终端驱动回显、行缓冲正确
- [x] Ctrl+C 转为 SIGINT
- [x] PTY 让 GUI 终端模拟器运行 shell
- [x] 虚拟终端切换（Alt+F1~F7）工作
- [x] RTC 驱动从 CMOS 读取时间正确
- [x] sleep()/usleep() 系统调用工作
- [x] gettimeofday() 返回正确日期时间
- [x] 内核定时器列表到期唤醒进程
- [x] pipe() 匿名管道系统调用工作
- [x] 命名管道 FIFO 在文件系统可创建与通信
- [x] GSHELL I/O 重定向（> < >>）工作
- [x] GSHELL 管道连接（|）工作
- [x] select()/poll() 多路等待工作
- [x] epoll/kqueue 接口已预留
- [ ] POSIX API 子集可移植 Linux 程序
- [ ] Linux 系统调用号翻译层工作

## 总体
- [ ] 所有新模块通过 -mgeneral-regs-only 与 -ffreestanding 编译
- [ ] QEMU 启动 -vga std 下正常进入 GUI
- [ ] VMware/VirtualBox 启动正常进入 GUI
- [ ] 真实 PC 启动正常进入 GUI
- [ ] 现有 GUI 功能（窗口/任务栏/开始菜单/浏览器/计算器）不受影响
- [ ] 编译无新增警告
