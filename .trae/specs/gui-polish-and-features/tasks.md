# Tasks

## 阶段零：登录流程修复（最高优先级）
- [ ] Task 0: 修复启动直接进入桌面的问题，强制登录流程
  - [ ] SubTask 0.1: 排查 kernel_main 与 gui_init 中跳过锁屏/登录的代码路径
  - [ ] SubTask 0.2: 确保 gui_state 初始化为 GUI_STATE_LOCKSCREEN
  - [ ] SubTask 0.3: 修复任何绕过 login_handle_key 直接切到 DESKTOP 的逻辑
  - [ ] SubTask 0.4: 验证默认账户（admin/空密码或文档化默认凭据）能登录成功
- [ ] Task 0B: Windows 11 风格锁屏界面
  - [ ] SubTask 0B.1: 背景使用当前壁纸叠加模糊层（无壁纸时使用主题背景色）
  - [ ] SubTask 0B.2: 中央底部大字时钟（HH:MM 加粗，使用 24×24 字体）
  - [ ] SubTask 0B.3: 时钟下方显示日期（YYYY 年 M 月 D 日 星期X）
  - [ ] SubTask 0B.4: 右下角显示"按任意键登录"提示文字
  - [ ] SubTask 0B.5: 按任意键切换到登录面板
- [ ] Task 0C: Windows 11 风格登录面板
  - [ ] SubTask 0C.1: 圆角卡片居中显示（360×480）
  - [ ] SubTask 0C.2: 卡片顶部圆形头像占位（96px SVG，默认用户图标）
  - [ ] SubTask 0C.3: 用户名显示（加粗）
  - [ ] SubTask 0C.4: 密码输入框（圆角 6px，焦点态主色边框）
  - [ ] SubTask 0C.5: 登录按钮（主色 #0078D4 填充，圆角 6px，悬停态加亮）
  - [ ] SubTask 0C.6: 错误提示区（红色文字，登录失败时显示）
  - [ ] SubTask 0C.7: 回车键触发登录验证
  - [ ] SubTask 0C.8: 登录成功切换到 DESKTOP 状态
- [ ] Task 0D: 字体迁移到 24×24（弃用 MiSans 16×16）
  - [ ] SubTask 0D.1: 修改 tools/gen_font.py 输出 24×24 字模（ASCII 96 字 + GB2312 6763 字）
  - [ ] SubTask 0D.2: 重新生成 font_misans.h（或重命名为 font24.h），FONT_WIDTH=24 FONT_HEIGHT=24
  - [ ] SubTask 0D.3: 修改 draw_char/draw_char_cn 使用 24×24 字模（每字 72 字节）
  - [ ] SubTask 0D.4: 修改 draw_string_bold/draw_string_bold_cn 使用 24×24 字模
  - [ ] SubTask 0D.5: 修改 utf8_decode/font_cn_find 适配新字模（如字节布局变化）
  - [ ] SubTask 0D.6: 全局检查所有 FONT_WIDTH/FONT_HEIGHT 用法适配新尺寸
  - [ ] SubTask 0D.7: 调整窗口默认尺寸（标题栏高度 32px、按钮尺寸、列表行高适配 24px 字体）
  - [ ] SubTask 0D.8: 编译验证无警告，QEMU 渲染正常无错位

## 阶段一：渲染管线升级（双缓冲 + 脏矩形 + 缓动）
- [ ] Task 1: 实现 back_buffer 作为唯一绘制目标
  - [ ] SubTask 1.1: gfx.c 中分配 back_buffer（按当前分辨率动态分配）
  - [ ] SubTask 1.2: 所有 fb_* 绘制函数改为写入 back_buffer
  - [ ] SubTask 1.3: 保留 fb_blit 全屏合成用于壁纸切换等场景
- [ ] Task 2: 实现脏矩形列表
  - [ ] SubTask 2.1: 定义 `dirty_rects[8]` + `dirty_count`，提供 `mark_dirty(x,y,w,h)` 与 `dirty_clear()`
  - [ ] SubTask 2.2: `fb_blit_dirty()` 仅复制脏区域从 back_buffer 到 LFB
  - [ ] SubTask 2.3: 替换 gui_dirty 标志为脏矩形机制（保留兼容接口）
- [ ] Task 3: 实现缓动动画系统 `easing.c/h`
  - [ ] SubTask 3.1: ease_out_cubic / ease_in_out_cubic / ease_in_quad / ease_out_back 函数
  - [ ] SubTask 3.2: 动画管理器（启动/更新/完成回调，最大并发 16 个动画）
- [ ] Task 4: 热路径性能优化
  - [ ] SubTask 4.1: 字符宽度查找表（首次查询后缓存）
  - [ ] SubTask 4.2: fb_fillrect 标记 static inline + 4 字节对齐写入
  - [ ] SubTask 4.3: 窗口渲染按 z-order 跳过被完全遮挡窗口

## 阶段二：主题引擎与图片解码
- [ ] Task 5: 实现主题引擎 `theme.c/h`
  - [ ] SubTask 5.1: 定义 theme_t 结构（主色/强调/背景/文本/圆角/阴影/字体）
  - [ ] SubTask 5.2: 预置 Windows 11 浅色 theme_light（主色 #0078D4）与深色 theme_dark（背景 #1F1F1F、强调色 #60CDFF）
  - [ ] SubTask 5.3: theme_load/save 从 /etc/theme.conf 读写
  - [ ] SubTask 5.4: theme_apply 立即重绘所有窗口
- [ ] Task 6: 实现 BMP 解码 `bmp.c/h`
  - [ ] SubTask 6.1: 解析 BMP 头（44/108 字节头、24/32 bpp）
  - [ ] SubTask 6.2: 输出 32bpp RGBA 像素缓冲
- [ ] Task 7: 实现 JPEG 解码 `jpeg.c/h`
  - [ ] SubTask 7.1: 基准 DCT 流程（哈夫曼表 + 反量化 + IDCT）
  - [ ] SubTask 7.2: YCbCr 4:4:4 / 4:2:2 / 4:2:0 上采样
  - [ ] SubTask 7.3: 输出 32bpp RGBA
- [ ] Task 8: 提供统一 image_load 接口（按扩展名分发 BMP/JPEG）

## 阶段三：壁纸与桌面美化
- [ ] Task 9: 实现壁纸子系统 `wallpaper.c/h`
  - [ ] SubTask 9.1: wallpaper_load(path, mode) 加载并按模式铺满/居中/拉伸
  - [ ] SubTask 9.2: wallpaper_save_config 写 /etc/wallpaper.conf
  - [ ] SubTask 9.3: 启动时自动加载上次壁纸
- [ ] Task 10: 桌面背景层升级
  - [ ] SubTask 10.1: 默认渐变背景（无壁纸时，Windows 11 风格蓝色渐变）
  - [ ] SubTask 10.2: 壁纸渲染层优先于桌面图标与窗口
- [ ] Task 11: 桌面图标美化（Windows 11 风格）
  - [ ] SubTask 11.1: 悬停高光（半透明白色叠加 + 圆角）
  - [ ] SubTask 11.2: 选中态主色边框 + 标签阴影
  - [ ] SubTask 11.3: 右键菜单扩展（Win11 风格分组：打开/重命名/属性，分隔线）

## 阶段四：Windows 11 风格窗口美化
- [ ] Task 12: 窗口投影阴影
  - [ ] SubTask 12.1: 渲染窗口前先在右下偏移 8px 绘制半透明阴影矩形
  - [ ] SubTask 12.2: alpha 渐变（边缘减弱，半径 8px）
- [ ] Task 13: 窗口圆角裁剪（Windows 11 标准 8px）
  - [ ] SubTask 13.1: 实现 fb_draw_round_rect（圆角 8px 矩形填充）
  - [ ] SubTask 13.2: 窗口背景与标题栏使用圆角
- [ ] Task 14: 标题栏垂直渐变
  - [ ] SubTask 14.1: 实现 fb_fillrect_vgrad（顶部色→底部色）
  - [ ] SubTask 14.2: 标题栏使用主色→稍暗主色渐变
- [ ] Task 15: Windows 11 风格 SVG 窗口按钮
  - [ ] SubTask 15.1: 绘制 minimize（细横线）、close（细 X）、restore（方框）SVG 图标
  - [ ] SubTask 15.2: 按钮悬停态背景变化（Win11 风格浅灰/红色关闭按钮）
- [ ] Task 16: 窗口最小化/恢复动画改用缓动
  - [ ] SubTask 16.1: 替换原有线性动画为 ease-out cubic（200ms）
  - [ ] SubTask 16.2: 动画期间允许输入

## 阶段五：Windows 11 风格任务栏与开始菜单
- [ ] Task 17: 任务栏重构为 Windows 11 居中风格
  - [ ] SubTask 17.1: 开始按钮居左，应用图标居中排列，右侧系统托盘
  - [ ] SubTask 17.2: 半透明背景（alpha=200）+ box blur 模糊桌面背景
  - [ ] SubTask 17.3: 任务栏高度 48px（Win11 标准）
- [ ] Task 18: 任务栏按钮状态（Windows 11 风格）
  - [ ] SubTask 18.1: 活动窗口按钮下方显示主色短下划线指示器
  - [ ] SubTask 18.2: 悬停态浅灰色背景圆角
  - [ ] SubTask 18.3: 最小化窗口按钮无下划线
- [ ] Task 19: 时钟加粗 + 日期悬浮
  - [ ] SubTask 19.1: 任务栏时钟加粗渲染
  - [ ] SubTask 19.2: 悬停时钟显示日期气泡
- [ ] Task 20: Windows 11 风格开始菜单
  - [ ] SubTask 20.1: 从屏幕底部居中向上弹出（非贴左）
  - [ ] SubTask 20.2: 圆角 8px + 毛玻璃背景
  - [ ] SubTask 20.3: 顶部搜索框
  - [ ] SubTask 20.4: 应用图标网格布局（按字母序）
  - [ ] SubTask 20.5: 推荐区（最近文件/常用应用）
  - [ ] SubTask 20.6: 底部电源按钮
  - [ ] SubTask 20.7: 展开/收起使用 ease-out 动画（200ms）

## 阶段六：系统托盘与通知中心
- [ ] Task 21: 实现系统托盘 `systray.c/h`
  - [ ] SubTask 21.1: 任务栏右侧三个图标位（音量/网络/时钟）
  - [ ] SubTask 21.2: 左键单击展开小弹窗（音量滑块/网络状态/日历）
  - [ ] SubTask 21.3: 右键菜单（打开设置/静音）
- [ ] Task 22: 实现通知中心 `notify.c/h`（Windows 11 风格右下角）
  - [ ] SubTask 22.1: notify_send(title, body, level) API
  - [ ] SubTask 22.2: 右下角卡片渲染（含图标/标题/正文/时间）
  - [ ] SubTask 22.3: 队列管理 + 自动消失（4 秒）
  - [ ] SubTask 22.4: 滑入/滑出使用 ease-out 动画
  - [ ] SubTask 22.5: 托盘通知中心图标显示未读数

## 阶段七：新增应用（文件管理器 + 设置中心）
- [ ] Task 23: 实现文件管理器 `file_manager.c/h`
  - [ ] SubTask 23.1: 路径导航栏（前进/后退/上一级/地址栏）
  - [ ] SubTask 23.2: 目录列表（图标+名称+大小+日期四列对齐）
  - [ ] SubTask 23.3: 双击进入目录或打开文件（按扩展名分发）
  - [ ] SubTask 23.4: 右键菜单（Win11 风格：复制/粘贴/删除/重命名/新建文件夹，分隔线分组）
  - [ ] SubTask 23.5: 文件类型图标映射（文件夹/.txt/.mp3/.mp4/.bmp/.jpg）
  - [ ] SubTask 23.6: 删除/重命名二次确认对话框
- [ ] Task 24: 实现设置中心 `settings.c/h`
  - [ ] SubTask 24.1: 左侧分类导航（显示/外观/声音/用户/关于，Win11 风格）
  - [ ] SubTask 24.2: 显示设置（分辨率列表/刷新率，复用 gfx_settings 能力）
  - [ ] SubTask 24.3: 外观设置（主题切换/壁纸选择/强调色）
  - [ ] SubTask 24.4: 声音设置（音量滑块/静音开关）
  - [ ] SubTask 24.5: 用户设置（账户信息/修改密码）
  - [ ] SubTask 24.6: 关于（版本/构建时间/作者/许可）
- [ ] Task 25: 桌面右键菜单扩展（Windows 11 风格）
  - [ ] SubTask 25.1: 添加"刷新/更改壁纸/显示设置/新建文件夹/属性"，分隔线分组
  - [ ] SubTask 25.2: 各项对接相应模块
- [ ] Task 26: 注册文件管理器/设置中心窗口类型与桌面图标
  - [ ] SubTask 26.1: gui.h 新增 WINDOW_TYPE_FILE_MANAGER / WINDOW_TYPE_SETTINGS
  - [ ] SubTask 26.2: 桌面新增"文件管理器""设置"图标（Win11 风格）
  - [ ] SubTask 26.3: 开始菜单新增应用入口

## 阶段八：既有应用 UI 重构
- [ ] Task 27: 播放器 UI 重构
  - [ ] SubTask 27.1: 唱片样式封面占位（圆形 SVG）
  - [ ] SubTask 27.2: 旋转动画（播放时持续旋转，暂停停止）
  - [ ] SubTask 27.3: 波形进度条（已播放高亮 + 拖动 seek）
  - [ ] SubTask 27.4: SVG 按钮（上一首/播放暂停/下一首/停止）+ 悬停态
  - [ ] SubTask 27.5: 音量滑块 + 播放列表（可折叠）
- [ ] Task 28: 浏览器 UI 重构
  - [ ] SubTask 28.1: 标签页栏（最多 4 tab，可关闭）
  - [ ] SubTask 28.2: SVG 工具栏（前进/后退/刷新/主页）
  - [ ] SubTask 28.3: 圆角 URL 栏 + 加载进度条
  - [ ] SubTask 28.4: 底部状态栏显示链接悬停 URL
- [ ] Task 29: 下载管理器 UI 重构
  - [ ] SubTask 29.1: 列表项含文件类型 SVG 图标
  - [ ] SubTask 29.2: 进度条渐变填充 + 速度显示 + 百分比
  - [ ] SubTask 29.3: 顶部工具栏（全部暂停/全部清除/添加 URL）
  - [ ] SubTask 29.4: 右键菜单（打开目录/取消/移除）
- [ ] Task 30: 任务管理器 UI 重构
  - [ ] SubTask 30.1: CPU/内存折线图（保留最近 60 采样）
  - [ ] SubTask 30.2: 进程列表状态色块 + 列对齐
  - [ ] SubTask 30.3: 结束任务二次确认对话框
  - [ ] SubTask 30.4: 刷新按钮
- [ ] Task 31: 计算器与关于窗口 UI 重构
  - [ ] SubTask 31.1: 计算器按钮圆角 + 悬停态
  - [ ] SubTask 31.2: 关于窗口加版本/构建时间/作者信息
- [ ] Task 32: GSHELL 美化
  - [ ] SubTask 32.1: 标题栏渐变 + 圆角
  - [ ] SubTask 32.2: 输出区滚动条美化
  - [ ] SubTask 32.3: 输入提示符高亮

## 阶段九：输入与桌面交互优化
- [ ] Task 33: 鼠标移动节流
  - [ ] SubTask 33.1: 合并 <8ms 内连续移动事件为最后一次
  - [ ] SubTask 33.2: 节流期间仍渲染光标
- [ ] Task 34: 软件光标优化
  - [ ] SubTask 34.1: 光标绘制前先保存脏区背景
  - [ ] SubTask 34.2: 移动时恢复背景再绘制新位置
- [ ] Task 35: 窗口拖动流畅度
  - [ ] SubTask 35.1: 拖动期间仅标记窗口旧位置+新位置为脏
  - [ ] SubTask 35.2: 避免全屏重绘

## 阶段十：集成与验证
- [ ] Task 36: kernel_main 初始化新模块
  - [ ] SubTask 36.1: theme_init / wallpaper_init / file_manager_init / settings_init / systray_init / notify_init / easing_init
  - [ ] SubTask 36.2: 加载持久化配置（主题/壁纸）
  - [ ] SubTask 36.3: 确保 gui_state 初始化为 LOCKSCREEN（与 Task 0 一致）
- [ ] Task 37: cmd.c 新增命令
  - [ ] SubTask 37.1: `wallpaper <path> [stretch|center|scale]`
  - [ ] SubTask 37.2: `theme [light|dark]`
  - [ ] SubTask 37.3: `notify <title> <body>`
  - [ ] SubTask 37.4: `lock` 命令（手动锁屏回 LOCKSCREEN）
- [ ] Task 38: 下载完成发送通知
  - [ ] SubTask 38.1: download.c 完成时调用 notify_send
- [ ] Task 39: 编译验证 + 修复警告
- [ ] Task 40: QEMU 测试
  - [ ] SubTask 40.1: 启动强制进入锁屏，不直接进桌面
  - [ ] SubTask 40.2: 锁屏按任意键进入登录面板
  - [ ] SubTask 40.3: 错误密码提示，正确密码进入桌面
  - [ ] SubTask 40.4: 壁纸/主题切换正常
  - [ ] SubTask 40.5: 文件管理器浏览/删除/重命名正常
  - [ ] SubTask 40.6: 设置中心各分类正常
  - [ ] SubTask 40.7: 托盘音量/网络/时钟交互正常
  - [ ] SubTask 40.8: 通知弹出与消失正常
  - [ ] SubTask 40.9: 播放器/浏览器/下载/任务管理器 UI 正常
  - [ ] SubTask 40.10: 任务栏居中风格 + 开始菜单居中弹出
  - [ ] SubTask 40.11: 窗口圆角阴影 + 拖动/最小化动画流畅
  - [ ] SubTask 40.12: 桌面图标悬停/选中/双击正常

## Task Dependencies
- Task 0 最高优先级，独立先行
- Task 0B 与 Task 0C 依赖 Task 0
- Task 0C 依赖 Task 5（主题颜色）的可选，可先用硬编码颜色
- Task 2 依赖 Task 1
- Task 3 独立（可并行）
- Task 4 独立（可并行）
- Task 5 独立（主题）
- Task 6/7/8 串行（图片解码）
- Task 9 依赖 Task 8（壁纸用图片解码）
- Task 10 依赖 Task 9
- Task 11 依赖 Task 10
- Task 12/13/14/15 可并行（窗口美化不同方面）
- Task 16 依赖 Task 3（缓动）与 Task 12-15
- Task 17 依赖 Task 2（脏矩形）
- Task 18/19/20 依赖 Task 17
- Task 21 独立（托盘）
- Task 22 依赖 Task 3（缓动）与 Task 21（托盘）
- Task 23 独立（文件管理器）
- Task 24 依赖 Task 5（主题）与 Task 9（壁纸）
- Task 25 依赖 Task 24（设置中心）
- Task 26 依赖 Task 23 与 Task 24
- Task 27/28/29/30/31/32 可并行（既有应用重构）
- Task 33/34/35 依赖 Task 2（脏矩形）
- Task 36 依赖 Task 5/9/23/24/21/22/3
- Task 37 依赖 Task 36
- Task 38 依赖 Task 22
- Task 39 依赖所有前置任务
- Task 40 依赖 Task 39
