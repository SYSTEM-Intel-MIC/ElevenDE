# ElevenDE 更新日志

本文件按版本倒序记录 ElevenDE 的变更。`*.deb` 不进入仓库，只作为同版本 GitHub Release 的附件发布。

## 3.6

发布重点：补齐 Windows 11 的日常交互手感，修掉一批视觉与可用性缺陷，并把发布流程迁移到 GitHub Actions。

### 构建与发布

- 新增 `.github/workflows/build-deb.yml`：push 到 `main`、`v*` 标签、PR 与手动触发时自动执行 `build-deb.sh`，产出 `elevende_3.6_amd64.deb`。
- CI 依次执行：包内容校验（含 `elevende-lock` setuid 位与已移除的 `elevende-photos` 二进制）、行为断言、依赖解析 dry run、Xvfb 下 8 秒 Shell 冒烟测试、`.deb` 制品上传。
- 打 `v*` 标签时校验 `v<包版本>` 与标签一致，并把 `.deb` 自动附加到同版本 GitHub Release。
- 新增 `tools/elevende-setup-autologin`，README 增加「启动 ElevenDE 并设为默认桌面」教程（GDM/LightDM 选择、开机直进、手动 `startx` 三条路径）。

### 桌面与窗口

- 移除内置 `elevende-photos` 程序：开始菜单保留「照片」入口，改为 `xdg-mime` 解析系统默认图片查看器 + `gtk-launch`，回退 eog/Loupe/xviewer 等；新增 `tools/elevende-open-photo` 助手与无 `MimeType=` 的 `photos.desktop`，`image/*` 关联不再指向已删除的启动器。
- 资源管理器三个视图（图标/列表/详情）均可将文件拖出（Qt XDND 源），桌面 Shell 实现 XDND v5 目标，拖到桌面的文件复制进 `~/Desktop` 并立即生成图标；复制在子进程执行，拖入大目录不阻塞 Shell。拖放语义统一为**复制**，从不移动或删除源文件。
- 资源管理器窗口之间、外部应用与资源管理器之间双向拖放落点均执行复制。
- 开始菜单搜索结果点击不再穿透到下方固定项（按行命中判定，滚动后坐标同步）。
- 桌面图标拖拽对齐资源管理器交互：拖拽阈值、光标跟随浮动图标、落位格动画、占位交换，不再出现图标重叠或手抖破坏双击。
- 桌面图标点击不再闪烁：选中高亮改为与资源管理器一致的单色 `#CBE4F6` 淡入，选中动画仅在选中项变化时重排，去掉绘制中的 `XFlush`；离开桌面不再清除选中状态。
- 磁盘分区不再作为桌面图标出现。
- 检测 `_NET_WM_STATE_FULLSCREEN` 后自动隐藏任务栏，指针贴近屏幕底边时临时露出，离开后重新隐藏；隐藏时同步收起状态区浮层。
- 托盘图标在槽位内居中对齐，状态区宽度与网络图标位置对齐（`net_r` 命中区域修正）。
- 任务栏图标缓存拆分为「已请求 / 已就绪」两级并按 `W%lu@` 窗口前缀失效；只有 `_NET_WM_ICON` / `WM_CLASS` 变化才重建，`WM_NAME` 变化不再触发重绘；`StartupWMClass` 与启动器文件名匹配不到时回退到启动器自带静态图标，消除启动瞬间的空白与闪烁。
- 默认壁纸更换为 `wallpaper-bloom-blue.png`；安装脚本清理旧的 `lindows-light` 残留。

### 触摸与输入

- Shell 新增 `gp_*` 手势状态机：开始菜单行/固定磁贴、搜索结果、任务栏任务按钮、桌面图标按下后延迟到释放时激活，按住 0.5 秒弹出与右键一致的上下文菜单，移动超过 6 像素转为拖动——结果列表随手指滚动，桌面图标照常按格位拖拽。手势由 16ms 定时层轮询，并以 `XQueryPointer` 作为丢失释放的安全网。
- 资源管理器三个视图支持触摸：`WA_AcceptTouchEvents` + `QTouchEvent`，点按选中、400ms 内双击进入、500ms 长按弹出上下文菜单、拖动滚动列表，不再依赖合成鼠标事件。
- 开始菜单键盘导航：高亮行改为绝对行号与可见区域（`apps_scroll + r`）对齐，修正 `search_rows_vis()`，PageUp/PageDown 翻页自动把高亮项滚入可视区，选中框不再在滚动后消失或错位。

### 应用与设置

- 设置 → 默认应用扩展为 11 类：网页、邮件（mailto/smtp）、Word/Excel/PowerPoint、图像、视频、音频、压缩包、文本、PDF、磁盘映像，MIME 集合扩充到 gif/webp/svg、mkv/webm/avi、flac/ogg/wav、rar/tar/gzip、markdown/json、iso/cab、`x-scheme-handler`/ftp 等。
- `.desktop` 本地化：按 `LC_ALL` → `LC_MESSAGES` → `LANG` 优先级解析 `Name[xx_XX]`/`Name[xx]` 与 `Icon[]` 本地化键；`Icon=` 支持绝对路径并自动去掉 `.png/.svg/.xpm/.ico` 后缀；启动改走 `gio launch`，回退 `xdg-open`。
- 点击托盘网络图标弹出原生 Win11 风格 Wi-Fi 快捷面板（`win_net`），替代 `sas-screen --network`：`nmcli` 异步扫描/连接/射频开关、信号格与锁标识、点按连接、已保存/开放/需密码三种模式，受保护网络缺失凭据时进入密码输入并聚焦；面板与音量、日历、电源、开始菜单、搜索互斥，同一时间只展开一个，面板外点击收起。
- 资源管理器侧栏快速访问改用 `QStandardPaths` 的 XDG 标准目录，缺失时 `QDir().mkpath()` 自动创建，不再提示「找不到路径」；`style.cpp` 为 `QListView` 补齐与 `QTreeView` 一致的选中/悬停配色（浅色 `#CBE4F6`/`#1F1F1F`，深色 `#3A3A4E`/`#FFFFFF`）。

### 会话与动画

- `elevende-session` 增加监督循环：picom、Shell、通知守护、SAS 守护崩溃后有限次（每会话 3 次）自动重启，并补上 TERM/HUP 清理会话进程的 trap。
- 开始菜单关闭时增加与打开对称的下滑动画，资源管理器目录树启用展开动画，picom ≥ 12 自动附加窗口打开/关闭动画预设（旧版本按版本跳过并在启动失败时回退到稳定配置）。

### 命名与文档

- 应用内与文档中的「Lindows 资源管理器」统一改称**资源管理器**；上游 MIT 归属与品牌边界表述保持不变。
- README 全面同步以上行为，新增触摸与手势、开始菜单键盘导航、`.desktop` 适配、任务栏图标、桌面图标、全屏与任务栏、托盘与 Wi-Fi、默认应用、资源管理器侧栏等行为表条目。
- 版本号由 3.5.1 升至 **3.6**（包名 `elevende_3.6_amd64.deb`，设置页显示「版本 3.6」）。

## 3.5.1

发布重点：集中完善桌面可用性与 Windows 11 风格的一致性。

- 任务栏按 X11 `WM_CLASS` 优先解析受控图标资源，`xterm`、设置、计算器、记事本、任务管理器、资源管理器均有稳定的第一方图标映射。
- 开始菜单监测用户、系统与 Flatpak 应用目录，安装/删除/更新 `.desktop` 后运行中自动刷新；启动条目优先走 `gtk-launch`/GIO 标准 desktop ID，避免把启动器或 Desktop Action 错交给资源管理器。
- 壁纸切换使用原子文件替换 + 根窗口版本属性 + 客户端消息通知，减少单次 X11 通知遗漏造成的延迟刷新。
- 登录与锁屏使用系统账户显示名、Fluent 默认头像、密码框焦点状态与无密码账户兼容的认证路径。
- 资源管理器提供侧栏、命令栏、路径栏、彩色位置图标、This PC 磁盘卡片、双击进入磁盘等交互（基于上游 Explorer-for-Linux 的 MIT 代码增强）。
- 资源采用项目自有生成资产、Microsoft Fluent System Icons 与按来源保留说明的 WindowsIcons 转换资源。
