# NEO-OS 桌面 GUI 优化与美化 Spec

## Why
NEO-OS 内核已完成进程调度、文件系统、网络栈、浏览器 CSS 引擎、下载管理、AC97 音频、MP3/MP4 解码、3D 渲染管线等能力，但这些功能在 GUI 中没有充分呈现：
- 播放器、下载管理器、浏览器、任务管理器等应用窗口仍是基础色块布局，缺乏现代设计语言
- 桌面无壁纸、无图标高光、无窗口阴影/圆角，整体观感简陋
- 任务栏与开始菜单无半透明/毛玻璃效果，无系统托盘
- 无文件管理器应用，磁盘/FAT32 能力未暴露给用户
- 无统一设置中心（显示/音频/壁纸/主题散落）
- 双缓冲、脏矩形、缓动动画尚未落地，窗口拖动/最小化卡顿
- 现有应用图标在桌面/任务栏/开始菜单中状态、悬停反馈不一致
- **登录流程缺失**：系统启动直接进入桌面，绕过锁屏/登录验证，账户系统形同虚设
- 现有 UI 风格混杂，缺少统一设计语言

本规范在不破坏既有内核能力的前提下，采用 **Windows 11 Fluent Design** 设计语言统一界面，补齐缺失应用，修复登录流程，提升渲染流畅度，让 NEO-OS 在视觉与交互上对齐现代操作系统。

## 设计语言基线（Windows 11 Fluent Design）
- **圆角**：窗口 8px、按钮 6px、菜单 8px、卡片 6px
- **毛玻璃（Mica/Acrylic）**：任务栏、开始菜单、通知中心使用半透明 + box blur
- **居中任务栏**：任务栏图标居中排列（左侧仅留开始按钮），右侧系统托盘
- **开始菜单**：从屏幕底部居中向上弹出，圆角 + 毛玻璃 + 应用网格
- **窗口**：投影阴影（4-8px 偏移）、圆角、标题栏垂直渐变、SVG 按钮
- **配色**：默认浅色主题（强调色 #0078D4 蓝）、深色主题（背景 #1F1F1F、强调色 #60CDFF）
- **字体**：**弃用 MiSans 16×16，改用 24×24 字模**（ASCII + 中文 GB2312 全部 24×24），更接近 Win11 视觉；按钮/标题加粗
- **动画**：200ms ease-out 标准过渡，窗口最小化 200ms 缩放滑入任务栏

## What Changes
- **修复登录流程**：启动强制进入锁屏状态（GUI_STATE_LOCKSCREEN），用户按任意键进入登录面板输入用户名密码，验证成功才进入桌面；移除"直接进桌面"路径
- **锁屏界面升级**：Windows 11 风格锁屏（大字时钟 + 日期 + 用户头像占位 + 输入框 + 登录按钮），背景支持壁纸模糊
- **登录面板升级**：圆角卡片居中显示，用户名/密码输入框圆角带焦点态，登录按钮主色填充
- 新增图片解码模块 `bmp.c/h`、`jpeg.c/h`（为壁纸/图标/图片预览提供基础）
- 新增壁纸子系统 `wallpaper.c/h`：支持 BMP/JPEG 加载、铺满/居中/拉伸、配置持久化到 `/etc/wallpaper.conf`
- 新增文件管理器应用 `file_manager.c/h`：基于已有 FAT32 实现目录浏览/打开/复制/删除/重命名
- 新增设置中心应用 `settings.c/h`：统一显示/音频/壁纸/主题/用户设置入口，替代散落的 `gfx_settings`
- 新增系统托盘 `systray.c/h`：任务栏右侧展示音量/网络/时钟图标，可点击展开气泡
- 新增通知中心 `notify.c/h`：右下角弹窗通知（Windows 11 风格在右下角，非右上），支持队列与自动消失
- 新增主题引擎 `theme.c/h`：定义配色/圆角/阴影/字体尺寸常量，支持运行时切换浅色/深色，预置 Windows 11 浅色与深色两套
- 新增缓动动画系统 `easing.c/h`：ease-in/out/in-out 函数，供窗口/菜单/通知使用
- 渲染管线升级：`back_buffer` 作为唯一绘制目标，`dirty_rects` 脏矩形列表，`fb_blit_dirty` 仅复制变化区域
- **任务栏重构为 Windows 11 居中风格**：开始按钮居左，应用图标居中排列，右侧系统托盘；半透明 + box blur；活动窗口下划线指示器（Win11 风格）
- **开始菜单重构为 Windows 11 风格**：从屏幕底部居中向上弹出，圆角 8px + 毛玻璃 + 搜索框 + 应用网格 + 推荐区 + 电源按钮
- 窗口美化：投影阴影（右下偏移 8px）、圆角 8px、标题栏垂直渐变、关闭/最小化按钮采用 SVG 图标（Win11 风格细线图标）
- 桌面图标美化：悬停高光、选中态、双击打开、右键菜单（Win11 风格带分隔线分组）
- 播放器 UI 重构：唱片样式封面占位、波形进度条、音量滑块、播放列表、文件拖入支持
- 浏览器 UI 重构：标签页（多 tab）、前进/后退/刷新 SVG 按钮、URL 栏圆角、加载进度条、状态栏
- 下载管理器 UI 重构：列表项含文件类型图标、进度条带渐变、速度显示、右键菜单
- 任务管理器 UI 重构：CPU/内存折线图、进程条目带状态色块、结束任务二次确认
- 计算器/关于窗口 UI 重构：按钮圆角、悬停态、关于页加版本/构建时间/作者信息
- 桌面右键菜单增加"刷新/更改壁纸/显示设置"项（Win11 风格分组）
- 性能优化：鼠标移动节流（合并连续移动事件）、字体渲染查找表缓存、热路径函数标记 `static inline`

## Impact
- Affected specs:
  - `os-core-evolution/spec.md` 的"桌面极致美化"、"流畅极致"、"壁纸自定义"需求被本规范具体化并落地，原 spec 中 Task 40-48 仍作为能力清单，本规范定义实现路径
- Affected code:
  - 新增：`kernel/bmp.c/h`、`kernel/jpeg.c/h`、`kernel/wallpaper.c/h`、`kernel/file_manager.c/h`、`kernel/settings.c/h`、`kernel/systray.c/h`、`kernel/notify.c/h`、`kernel/theme.c/h`、`kernel/easing.c/h`
  - 修改：`kernel/gui.c`（双缓冲、脏矩形、壁纸层、托盘、通知渲染）
  - 修改：`kernel/gui_window.c`（阴影、圆角、按钮 SVG、动画）
  - 修改：`kernel/gui_apps.c`（播放器/浏览器/下载/任务管理器/计算器/关于 UI 重构）
  - 修改：`kernel/gui_internal.h`（新增模块头文件包含与 extern 声明）
  - 修改：`kernel/gfx.c`（back_buffer 分配、fb_blit_dirty、box blur）
  - 修改：`kernel/cmd.c`（新增 `wallpaper`、`theme`、`notify` 命令）
  - 修改：`kernel/kernel.c`（初始化壁纸、主题、托盘、通知、文件管理器）

## ADDED Requirements

### Requirement: 启动强制登录流程
系统 SHALL 在启动完成后强制进入 `GUI_STATE_LOCKSCREEN` 状态，禁止直接进入桌面。系统 SHALL 用户按任意键从锁屏切换到登录面板。系统 SHALL 用户在登录面板输入正确凭据后才进入 `GUI_STATE_DESKTOP`。系统 SHALL 验证失败时显示错误提示并清空密码框，不允许进入桌面。系统 SHALL 登出/锁屏后回到 `GUI_STATE_LOCKSCREEN`。

#### Scenario: 首次启动
- **WHEN** 系统启动完成进入 GUI
- **THEN** 状态为 `GUI_STATE_LOCKSCREEN`，显示锁屏界面，不显示桌面

#### Scenario: 登录失败
- **WHEN** 用户输入错误密码
- **THEN** 显示"密码错误"提示，密码框清空，焦点回到密码框，仍停留在登录面板

#### Scenario: 登录成功
- **WHEN** 用户输入正确凭据
- **THEN** 切换到 `GUI_STATE_DESKTOP`，加载桌面、任务栏、开始菜单

### Requirement: Windows 11 风格锁屏界面
系统 SHALL 锁屏界面采用 Windows 11 风格：背景使用当前壁纸并叠加模糊层，中央底部显示大字时钟（HH:MM）与日期（YYYY 年 M 月 D 日 星期X），右下角显示"按任意键登录"提示。系统 SHALL 按任意键切换到登录面板。

#### Scenario: 锁屏时钟
- **WHEN** 锁屏界面渲染
- **THEN** 中央底部显示加粗大字时钟，下方显示日期与星期

### Requirement: Windows 11 风格登录面板
系统 SHALL 登录面板采用圆角卡片居中显示（卡片宽 360px 高 480px）。卡片内容自上而下：用户头像占位（圆形 96px SVG）、用户名显示、密码输入框（圆角带焦点边框）、登录按钮（主色填充圆角）、错误提示区。系统 SHALL 输入框获得焦点时显示主色边框。系统 SHALL 登录按钮可点击或按回车触发验证。

#### Scenario: 输入框焦点
- **WHEN** 用户点击密码输入框
- **THEN** 输入框边框变为主色，光标开始闪烁

### Requirement: 图片解码基础模块
系统 SHALL 实现 BMP（24/32 位）与 JPEG（基准 DCT）解码器，输出 32bpp RGBA 到调用方提供的缓冲。系统 SHALL 提供统一接口 `image_load(path, &w, &h)` 返回堆分配的像素缓冲。

#### Scenario: 加载 BMP 壁纸
- **WHEN** 壁纸子系统请求加载 `/etc/wallpaper.bmp`
- **THEN** BMP 解码器读取文件头、调色板（若有）、像素数据，返回 32bpp 像素

#### Scenario: 加载 JPEG 图标
- **WHEN** 文件管理器请求图标缩略
- **THEN** JPEG 解码器执行反 Zig-Zag、反量化、IDCT、YCbCr→RGB，输出 32bpp

### Requirement: 壁纸子系统
系统 SHALL 支持桌面壁纸，格式 BMP/JPEG。系统 SHALL 支持三种适配模式：铺满（stretch）、居中（center）、拉伸（scale）。系统 SHALL 在启动时从 `/etc/wallpaper.conf` 读取路径与模式。系统 SHALL 在显示设置中提供壁纸选择 GUI。

#### Scenario: 设置壁纸
- **WHEN** 用户在设置中心选择 `/photos/sky.jpg` 并选择"铺满"
- **THEN** 加载图片，按桌面尺寸缩放铺满，写入配置文件，立即刷新桌面

#### Scenario: 启动恢复
- **WHEN** 系统启动
- **THEN** 读取 `/etc/wallpaper.conf`，加载并渲染壁纸，无配置时使用默认渐变背景

### Requirement: 主题引擎
系统 SHALL 定义主题结构（主色/强调色/背景/文本/圆角半径/阴影偏移/字体大小）。系统 SHALL 预置浅色与深色两套主题。系统 SHALL 支持运行时切换并立即生效。系统 SHALL 将主题持久化到 `/etc/theme.conf`。

#### Scenario: 切换深色主题
- **WHEN** 用户在设置中心选择"深色"
- **THEN** 所有窗口/任务栏/菜单立即按深色主题重绘，配置写入文件

### Requirement: 文件管理器应用
系统 SHALL 提供文件管理器 GUI 应用，基于已有 FAT32 实现。功能 SHALL 包含：路径导航栏、目录列表（图标+名称+大小+日期）、双击进入目录/打开文件、右键菜单（复制/粘贴/删除/重命名/新建文件夹）、文件类型图标映射。

#### Scenario: 浏览目录
- **WHEN** 用户打开文件管理器
- **THEN** 显示根目录内容，双击文件夹进入，地址栏显示当前路径

#### Scenario: 删除文件
- **WHEN** 用户右键文件选择"删除"
- **THEN** 弹出确认对话框，确认后调用 fs_unlink，刷新列表

### Requirement: 设置中心应用
系统 SHALL 提供统一设置中心，包含分类：显示（分辨率/刷新率）、外观（主题/壁纸/强调色）、声音（音量/静音）、用户（账户/密码）、关于（版本/构建/作者）。系统 SHALL 替代散落的 `gfx_settings` 弹窗，将显示设置并入。

#### Scenario: 调整音量
- **WHEN** 用户在设置-声音中拖动音量滑块
- **THEN** 实时调用 audio_set_volume，滑块旁显示当前值

### Requirement: 系统托盘
系统 SHALL 在任务栏右侧展示系统托盘，包含：音量图标、网络图标、时钟。系统 SHALL 支持左键单击展开小弹窗（音量滑块/网络状态/日历）、右键菜单（打开设置/静音）。

#### Scenario: 调节音量
- **WHEN** 用户左键点击托盘音量图标
- **THEN** 弹出音量滑块，拖动实时生效

### Requirement: 通知中心
系统 SHALL 提供通知机制，应用通过 `notify_send(title, body, level)` 发送通知。系统 SHALL 在屏幕右上角以卡片形式展示，自动消失（默认 4 秒），支持队列。系统 SHALL 在通知中心图标（托盘左侧）显示未读数。

#### Scenario: 下载完成通知
- **WHEN** 下载管理器完成一个任务
- **THEN** 调用 `notify_send("下载完成", "file.zip 已保存到 /downloads", NOTIFY_SUCCESS)`，右上角弹出卡片

### Requirement: 缓动动画系统
系统 SHALL 实现 `easing.c/h`，提供 `ease_out_cubic`、`ease_in_out_cubic`、`ease_in_quad` 等函数（输入 0.0-1.0，输出 0.0-1.0）。系统 SHALL 在窗口最小化/恢复、开始菜单展开/收起、通知滑入/滑出、登录过渡中使用缓动。

#### Scenario: 窗口最小化动画
- **WHEN** 用户点击窗口最小化按钮
- **THEN** 窗口使用 ease-out 从原位置缩放滑入任务栏，持续 200ms，期间不阻塞输入

### Requirement: 双缓冲与脏矩形渲染
系统 SHALL 将 `back_buffer` 作为所有绘制操作的唯一目标。系统 SHALL 维护脏矩形列表（最多 8 个矩形）。系统 SHALL 在每帧末尾通过 `fb_blit_dirty(dirty_list)` 将脏区域从 back_buffer 复制到 LFB。系统 SHALL 在无脏区域时跳过 blit。

#### Scenario: 仅重绘变化区域
- **WHEN** 仅光标移动
- **THEN** 只标记光标旧位置与新位置两个脏矩形，blit 仅复制这两块

### Requirement: 窗口美化
系统 SHALL 为所有窗口绘制投影阴影（右下偏移 4px，alpha 渐变，半径 8px）。系统 SHALL 对窗口边框采用圆角（半径 6px）。系统 SHALL 对标题栏使用垂直渐变（顶部主色 → 底部稍暗）。系统 SHALL 用 SVG 风格图标替代 "_" / "X" 文字按钮。

#### Scenario: 圆角阴影窗口
- **WHEN** 任意窗口渲染
- **THEN** 窗口矩形四角圆滑，右下方有半透明阴影，标题栏从上到下渐变

### Requirement: 任务栏与开始菜单美化
系统 SHALL 任务栏使用半透明背景（alpha=200）+ 简易 box blur 模糊桌面背景。系统 SHALL 活动窗口按钮高亮，悬停态浅化。系统 SHALL 时钟加粗显示。系统 SHALL 开始菜单使用半透明背景、应用图标网格布局、悬停高光、分隔线分组。

#### Scenario: 毛玻璃任务栏
- **WHEN** 任务栏渲染
- **THEN** 背景半透明可见桌面，叠加 box blur 模糊层

### Requirement: 播放器 UI 重构
系统 SHALL 重构播放器界面：顶部唱片样式封面占位（圆形带旋转动画）、中部歌曲名/艺人、波形进度条（已播放部分高亮）、底部 SVG 按钮（上一首/播放暂停/下一首/停止）、右侧音量滑块、左侧播放列表（可折叠）。系统 SHALL 支持文件拖入（从文件管理器）。

#### Scenario: 播放状态
- **WHEN** 用户播放 MP3
- **THEN** 唱片占位旋转，进度条推进，暂停时停止旋转

### Requirement: 浏览器 UI 重构
系统 SHALL 重构浏览器界面：顶部标签页栏（支持多 tab，可关闭）、工具栏含 SVG 前进/后退/刷新/主页按钮、URL 栏圆角带加载进度条、底部状态栏显示链接悬停 URL。系统 SHALL 标签页数量上限 4。

#### Scenario: 多标签
- **WHEN** 用户点击 URL 栏旁"+"按钮
- **THEN** 新建空白标签页，地址栏聚焦

### Requirement: 下载管理器 UI 重构
系统 SHALL 重构下载列表项：左侧文件类型 SVG 图标、中间文件名+大小+速度、右侧状态文字+进度条（渐变填充）、右键菜单（打开目录/取消/移除）。系统 SHALL 顶部工具栏含"全部暂停/全部清除/添加 URL"按钮。

#### Scenario: 下载进行中
- **WHEN** 下载任务进行
- **THEN** 进度条渐变填充，速度显示 "KB/s"，进度百分比右对齐

### Requirement: 任务管理器 UI 重构
系统 SHALL 重构任务管理器：顶部 CPU/内存使用率折线图（保留最近 60 个采样）、中部进程列表（带状态色块、PID/名称/CPU/内存列对齐）、底部"结束任务"按钮（带二次确认对话框）、右上角刷新按钮。

#### Scenario: 结束任务确认
- **WHEN** 用户选中进程并点击"结束任务"
- **THEN** 弹出确认对话框，确认后发送终止信号，列表刷新

### Requirement: 桌面图标美化
系统 SHALL 桌面图标支持悬停高光（半透明白色叠加）、选中态（蓝色边框）、双击打开、右键菜单（打开/重命名/属性）。系统 SHALL 图标标签使用阴影提升可读性。

#### Scenario: 选中图标
- **WHEN** 用户单击桌面图标
- **THEN** 图标加蓝色边框，标签加阴影

### Requirement: 输入响应优化
系统 SHALL 对鼠标移动事件进行节流（合并 <8ms 内连续事件为最后一次）。系统 SHALL 动画期间不阻塞输入（动画驱动放在主循环非阻塞段）。系统 SHALL 鼠标光标使用软件硬件双绘制（先恢复脏区背景再绘制新位置）。

#### Scenario: 流畅拖动窗口
- **WHEN** 用户拖动窗口
- **THEN** 鼠标移动节流合并，窗口位置即时更新，无明显延迟

### Requirement: 热路径性能优化
系统 SHALL 字体渲染使用预计算字符宽度查找表。系统 SHALL `fb_fillrect` 标记 `static inline` 并对齐 4 字节写入。系统 SHALL 窗口列表渲染按 z-order 跳过被完全遮挡的窗口。

#### Scenario: 字体渲染加速
- **WHEN** 大量文本渲染（浏览器长页面）
- **THEN** 通过查找表跳过重复宽度计算，渲染耗时下降

## MODIFIED Requirements

### Requirement: GUI 渲染管线
原 `gui_dirty` 标志位扩展为脏矩形列表 `dirty_rects[]` + `dirty_count`。`render_gui` 入口先清空脏列表，渲染过程中各模块调用 `mark_dirty(x,y,w,h)` 累积脏区域，末尾 `fb_blit_dirty` 一次性合成。壁纸与桌面图标层在首次渲染或壁纸切换时全屏脏。

### Requirement: kernel_main 启动流程
启动流程在 GUI 初始化前增加：壁纸加载、主题加载、文件管理器注册、设置中心注册、托盘初始化、通知中心初始化、缓动系统初始化。

### Requirement: 桌面右键菜单
原桌面右键菜单（仅"刷新"）扩展为：刷新、更改壁纸、显示设置、新建文件夹、属性。各项对接相应模块。

## REMOVED Requirements
（无移除项，所有改动均为增量演进；既有 `gfx_settings` 弹窗功能并入设置中心，但其能力保留不删除）
