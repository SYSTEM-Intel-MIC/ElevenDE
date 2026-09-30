# ElevenDE 3.6

**ElevenDE** 是面向 Debian 系发行版的 X11 桌面环境。项目以 C/Xlib Shell、Openbox、picom、Qt 6 与 GTK 3 为基础，提供接近 Windows 11 的浅色视觉层次、桌面、任务栏、开始菜单、登录页、设置与 资源管理器，同时保持 Linux 应用和 X11 窗口管理器生态的兼容性。

![ElevenDE 桌面预览](docs/ElevenDE.png)

> **发布原则。** 源码、构建脚本、配置和可再生成资源保存在 Git 仓库；`*.deb` 不提交到仓库，而是仅作为 GitHub Release 的附件发布。这样发布包始终能够对应一个可审计的源代码提交。

## 3.6 发布重点

3.6 把日常交互手感补齐到 Windows 11 的水平，并修复一批视觉与可用性缺陷：触摸/长按、键盘导航、图标闪烁、全屏遮挡、托盘对齐、侧栏失效和默认应用覆盖范围。本版本同时把发布流程迁移到 GitHub Actions——push、`v*` 标签、PR 与手动触发都会自动构建、校验并冒烟测试 `.deb`，打标签时自动把包附加到同版本 GitHub Release，`*.deb` 始终不进仓库。

**触摸与长按**：开始菜单、搜索结果、任务栏任务按钮和桌面图标按下后不再立刻触发，而是等释放时激活；按住 0.5 秒弹出与右键完全相同的上下文菜单，超过 6 像素的移动转为拖动——结果列表随手指滚动、桌面图标继续原有的格位拖拽。资源管理器三个视图支持点按选中、400 毫秒内双击进入、500 毫秒长按菜单和手指滚动（`WA_AcceptTouchEvents` + `QTouchEvent`，不再依赖合成鼠标事件）。

**原生 Wi-Fi 快捷面板**：点击托盘网络图标弹出 Win11 风格面板，`nmcli` 异步扫描、开关射频、信号格与锁标识、点按连接，受保护网络缺失凭据时进入密码输入；面板与音量、日历、电源、开始菜单、搜索互斥，同一时间只展开一个，点击面板外收起，不再跳转到其他设置程序。

**默认应用覆盖网页**：设置 → 默认应用扩展为 11 类，覆盖网页、邮件、Word/Excel/PowerPoint、图像、视频、音频、压缩包、文本、PDF 与磁盘映像，MIME 集合扩充到 gif/webp/svg、mkv/webm/avi、flac/ogg/wav、rar/tar/gzip、markdown/json 等常用格式。

**其余修复与打磨**：开始菜单键盘高亮改为绝对行号与可见区对齐，Page 键翻页自动滚入可视区，选中框不再在滚动后消失；`.desktop` 按 `LC_ALL` → `LC_MESSAGES` → `LANG` 解析本地化 `Name[]`/`Icon[]`，图标支持绝对路径与去后缀，启动走 `gio launch` 回退 `xdg-open`；任务栏图标缓存拆成「已请求 / 已就绪」两级并按窗口前缀失效，只有 `_NET_WM_ICON`/`WM_CLASS` 变化才重建，`StartupWMClass` 匹配不到时回退到启动器自带图标，消除启动瞬间的空白与闪烁；桌面图标点击不再因选中动画重启而闪烁，选择高亮改为与资源管理器一致的 `#CBE4F6` 浅色填充，磁盘分区不再作为桌面图标出现；检测 `_NET_WM_STATE_FULLSCREEN` 后自动隐藏任务栏，指针贴近底边临时露出；托盘图标在槽位内居中、状态区宽度与网络图标位置对齐；资源管理器侧栏快速访问改用 `QStandardPaths` 的 XDG 目录并在缺失时自动创建，`QListView` 补齐与 `QTreeView` 一致的选中配色。

本版本还完成一批基础改造：**内置照片程序移除**，开始菜单保留“照片”入口并改为启动系统默认图片查看器（`xdg-mime` 解析 + `gtk-launch`，回退到 eog/Loupe/xviewer 等常见查看器，`image/*` 关联不再指向已删除的启动器）；**文件拖放**，资源管理器的图标/列表/详情三个视图都可将文件拖出（Qt XDND 源），桌面 Shell 实现 XDND v5 目标，把拖到桌面的文件复制进 `~/Desktop` 并立即显示为图标（复制在子进程执行，拖入大目录不会卡住 Shell），资源管理器窗口之间以及外部应用到资源管理器之间的落点同样执行复制而不移动源文件——拖放语义统一为**复制**；**会话稳定性**，`elevende-session` 增加监督循环，picom、Shell、通知守护和 SAS 守护崩溃后可有限次自动重启，并补上 TERM/HUP 清理会话进程的 trap；**动画打磨**，开始菜单关闭时增加与打开对称的下滑动画，资源管理器目录树启用展开动画，picom ≥ 12 的系统会自动附加窗口打开/关闭动画预设（旧版本如 Ubuntu 24.04 的 picom 10.x 不认识该配置，会话脚本按版本跳过并在启动失败时自动回退到稳定配置）；**命名统一**，应用内与文档中的“Lindows 资源管理器”改称**资源管理器**，上游 MIT 归属与品牌边界表述保持不变；**默认壁纸**更换为 `wallpaper-bloom-blue.png`。终端 `xterm` 本身不支持 XDND，无法接收拖放——这是上游程序限制，拖放目标为桌面和其他 XDND 程序。

完整的历史变更见 [`CHANGELOG.md`](CHANGELOG.md)。

| 区域 | 3.6 行为 |
|---|---|
| 桌面与任务栏 | 支持动态任务按钮布局、窗口销毁后的即时按钮清理、网络/音量/电池状态图标、输入法标识、显示桌面与 Win11 风格开始菜单；开始菜单可发现新安装的 XDG/Flatpak 应用并按 desktop ID 启动。 |
| 登录与锁屏 | 使用系统账户显示名、官方 Fluent 默认用户头像、密码框、焦点状态与无密码账户兼容的认证路径。 |
| 设置 | 固定浅色界面；支持壁纸立即应用、显示重排、紧凑声音页和 GPL/版本信息。 |
| 资源管理器 | 使用侧栏、命令栏、路径栏、彩色位置图标、This PC 磁盘卡片、文件操作与双击进入磁盘等交互，并支持窗口之间或外部应用与资源管理器之间的文件拖放（落点复制，不移动源文件）。 |
| 资源 | 采用项目自有生成资产、Microsoft Fluent System Icons 与按来源保留说明的 WindowsIcons 转换资源。 |
| 照片 | 内置 `elevende-photos` 程序移除；开始菜单保留“照片”入口，通过 `xdg-mime` + `gtk-launch` 启动系统默认图片查看器。 |
| 文件拖放 | 资源管理器三个视图可拖出文件；桌面为 XDND v5 目标，拖入的文件复制到 `~/Desktop` 并生成图标。 |
| 会话稳定性 | 会话监督循环自动重启崩溃的 compositor/Shell/通知/SAS 守护（每会话限 3 次），TERM/HUP 时清理全部会话进程。 |
| 动画 | 开始菜单关闭下滑动画、资源管理器目录树展开动画；picom ≥ 12 自动启用窗口打开/关闭动画预设，旧版本自动回退。 |
| 触摸与手势 | 开始菜单、搜索结果、任务栏任务按钮和桌面图标按下后延迟到释放时激活，按住 0.5 秒弹出与右键一致的上下文菜单，移动超过阈值后转为拖动（列表随手指滚动、桌面图标照常拖拽）；资源管理器三个视图支持点按选中、双击进入、长按菜单和手指滚动。 |
| 开始菜单键盘导航 | 方向键/PageUp/PageDown 的高亮行改为绝对行号与可见区域对齐，翻页时自动滚动到高亮项，选中框不再在滚动后消失或错位。 |
| `.desktop` 适配 | 按 `LC_ALL`/`LC_MESSAGES`/`LANG` 解析 `Name[xx_XX]`/`Name[xx]` 与 `Icon` 本地化键，图标路径可为绝对路径并自动去掉 `.png/.svg/.xpm/.ico` 后缀；启动命令走 `gio launch`，回退 `xdg-open`。 |
| 任务栏图标 | 图标缓存区分“已请求”和“已就绪”，窗口切换时不再重复创建；`_NET_WM_ICON`/`WM_CLASS` 变化才失效缓存，`WM_NAME` 变化不再触发重绘；为 `StartupWMClass` 与启动器文件名匹配的应用提供静态兜底图标，消除启动瞬间的空白与闪烁。 |
| 桌面图标 | 点击不再因选中动画重启而闪烁，选择高亮改为与资源管理器一致的浅色 `#CBE4F6` 填充；离开桌面不再清除选中状态；磁盘分区不再作为桌面图标出现。 |
| 全屏与任务栏 | 检测 `_NET_WM_STATE_FULLSCREEN` 后自动隐藏任务栏，指针贴近屏幕底边时临时露出，离开后重新隐藏。 |
| 托盘与 Wi-Fi | 托盘图标在槽位内居中对齐，状态区宽度与网络图标位置对齐；点击网络图标弹出原生 Wi-Fi 快捷面板（`nmcli` 异步扫描、开关 Wi-Fi、信号格与锁标识、点按连接、受保护网络弹出密码框），不再跳转到其他设置程序。 |
| 默认应用 | 设置 → 默认应用覆盖网页、邮件、Word/Excel/PowerPoint、图像、视频、音频、压缩包、文本、PDF 与磁盘映像共 11 类，MIME 集合扩展到 gif/webp/svg、mkv/webm/avi、flac/ogg/wav、rar/tar/gzip、markdown/json 等常用格式。 |
| 资源管理器侧栏 | 快速访问改用 XDG 标准目录（`QStandardPaths`），目录不存在时自动创建，侧栏按钮不再因路径缺失而提示“找不到路径”；`QListView` 补齐选中/悬停配色，与 `QTreeView` 一致。 |
| 持续集成 | `build-deb.yml` 在 push/`v*` 标签/PR/手动触发时自动构建 `elevende_3.6_amd64.deb`，依次执行包内容校验、行为断言、依赖解析 dry run、Xvfb 冒烟测试与制品上传；`v*` 标签校验通过后自动附加到同版本 GitHub Release。 |

当前版本已经在隔离 Xvfb 与 Openbox 会话中回归验证：开始菜单、登录页、连续壁纸切换、终端任务栏图标、任务栏多窗口清理、设置和资源管理器关键交互均通过实际截图或窗口属性检查。开始菜单回归额外验证了在菜单保持打开时新增 `.desktop` 条目会显示为新结果，并确认 Desktop Action 中故意错误的 `explorer.exe` 命令不会取代主应用启动命令。3.6 的行为断言（`nmcli` 面板、触摸长按菜单、全屏自动隐藏、`.desktop` 本地化、README 文档关键词）由 CI 在解包后的二进制与 README 上强制校验。

## 快速构建与安装

推荐使用 Debian、Ubuntu、Kali、Mint 或其他 apt 系发行版。构建脚本会构建 Shell、Qt/GTK 应用、资源管理器、SAS、RunBox 和包资源；若本地已有上游图标源，可传入其路径以离线刷新转换资源。

持续集成（`.github/workflows/build-deb.yml`）会在 push、`v*` 标签、PR 和手动触发时自动执行 `build-deb.sh`，校验包内容与行为断言、做依赖解析 dry run，并在 Xvfb 下冒烟运行 Shell——日常开发无需本地构建；推送 `v3.6` 这类标签时还会校验 `v<包版本>` 与标签一致并把 `.deb` 附加到 GitHub Release。

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  libx11-dev libxft-dev libxtst-dev libxi-dev libpng-dev \
  libgdk-pixbuf-2.0-dev librsvg2-bin \
  qt6-base-dev qt6-base-dev-tools libgtk-3-dev \
  openbox picom xterm

# 可选：指定本地的图标源目录；没有也可使用已提交的转换资源构建。
WINDOWSICONS_SOURCE=/path/to/WindowsIcons/Icons \
FLUENT_SOURCE=/path/to/fluentui-system-icons/assets \
./build-deb.sh

sudo apt install ./elevende_3.6_amd64.deb
```

安装后可从显示管理器选择 ElevenDE 会话，或按下方教程配置开机直进；源码安装适用于开发环境：

```sh
sudo ./install.sh
```

## 启动 ElevenDE 并设为默认桌面

安装（`.deb` 或源码安装均可）后，会话入口位于 `/usr/share/xsessions/elevende.desktop`（会话 ID 为 `elevende`，显示名 **ElevenDE**），可用下述任一方式进入。

### 方式一：登录界面选择（保留显示管理器）

1. 注销当前桌面（右上角电源菜单 → Log Out）。
2. GDM 登录页点击右上角**齿轮 ⚙**（会话选择器）→ 选择 **ElevenDE** → 登录。GDM 会记住每个用户上次的会话，之后登录默认即为 ElevenDE。
3. 若希望新用户也默认进入（无需每次选择），固定为全局默认：

   ```sh
   # /var/lib/AccountsService/users/<用户名> 的 [User] 段下添加：
   # XSession=elevende
   sudo systemctl restart accounts-daemon   # 或直接重启
   ```

   使用 LightDM 的机器（用 `cat /etc/X11/default-display-manager` 查看当前显示管理器）改在 `/etc/lightdm/lightdm.conf` 的 `[Seat:*]` 段设置 `user-session=elevende`；需要自动登录时对应设置 `autologin-session=elevende`。

### 方式二：开机直进 ElevenDE（`elevende-setup-autologin`）

随包安装的 `elevende-setup-autologin` 会把机器配置为**开机绕过图形登录界面、直接进入 ElevenDE**：

```sh
sudo elevende-setup-autologin [用户名]
sudo reboot
```

- 用户名可省略：默认取 `$SUDO_USER`（即执行 sudo 的用户），否则取 `/home` 下的第一个用户。
- 工具具体做的事（每一步都可手工还原）：
  1. 写入 systemd drop-in `/etc/systemd/system/getty@tty1.service.d/autologin.conf`，让 tty1 控制台开机**免密自动登录**到该用户；
  2. 写入 `/etc/profile.d/elevende-session.sh`：当该用户在 tty1 登录且没有 `DISPLAY` 时自动 `exec startx /usr/local/bin/elevende-session`，直接进入 ElevenDE；
  3. 将 systemd 默认启动目标设为 `multi-user.target`（文本控制台，不启动图形登录界面）；
  4. 禁用 `gdm3`/`gdm`/`lightdm`/`sddm`/`slim`，避免显示管理器重新抢占开机流程。
- **注意**：该方式为免密自动登录，任何能接触这台机器的人都会直接进入桌面，仅建议家用单人机器使用；需要登录保护时请用方式一。

  还原（恢复显示管理器 + 图形启动）：

  ```sh
  sudo rm /etc/profile.d/elevende-session.sh \
         /etc/systemd/system/getty@tty1.service.d/autologin.conf
  sudo systemctl set-default graphical.target
  sudo systemctl enable gdm3     # 或你原来使用的显示管理器
  ```

### 方式三：手动启动（临时体验或排查）

不经过显示管理器，从任意 TTY 控制台执行：

```sh
startx /usr/local/bin/elevende-session
```

退出该会话即返回控制台，对系统配置无任何改动，适合安装后的首次验证。

## 目录与维护边界

下表是本仓库的维护边界。**“ElevenDE 自主代码”** 指由 SYSTEM-Intel-MIC 为 ElevenDE 编写的代码、配置、构建逻辑、集成逻辑和对上游的增量修改；该部分以 GPL-3.0-or-later 发布。原始上游文件仍保留其原始版权与许可证，不因放入本仓库而被追溯改写。

| 路径 | 内容 | 维护与许可证边界 |
|---|---|---|
| `shell/`、`apps/`、`wm/`、`session/`、`scripts/`、`tools/` | ElevenDE Shell、内置应用、会话、窗口主题、构建辅助与集成代码 | SYSTEM-Intel-MIC 的自主代码，GPL-3.0-or-later。 |
| `assets/` | 启动器、壁纸、图标转换脚本、转换结果、Mica 材料和来源说明 | ElevenDE 自主脚本和配置为 GPL；第三方图标分别遵循其来源条款。 |
| `Explorer-for-Linux/` | 资源管理器的上游基础及本地增强 | 原始基础为 MIT；本项目的增量修改为 GPL-3.0-or-later，原 MIT 声明必须保留。 |
| `SAS-for-Linux/` | Ctrl+Alt+Delete 安全选项屏幕上游基础 | 原始基础为 MIT；ElevenDE 的构建、会话和调用整合为 GPL-3.0-or-later。 |
| `runbox-linux/` | Win+R 对话框及其 ElevenDE 集成 | 由 SYSTEM-Intel-MIC 维护的组件；随本发布由权利人以 GPL-3.0-or-later 提供。 |
| `LICENSES/` | 上游 MIT 许可证副本 | 仅用于保留上游许可与版权通知，不改变原始文件的作者归属。 |

### 资源管理器与 Explorer-for-Linux 的关系

资源管理器以 [macOS-Terminal/Explorer-for-Linux][explorer-upstream] 的 MIT 代码为基础；ElevenDE 并不将原始上游全部表述为自研代码。3.5.1/3.6 的本地重构主要位于 `src/explorer/filelist.cpp`、`main.cpp`、`mainwindow.cpp`、`style.cpp` 和 `thispc.cpp`，覆盖命令栏与路径栏布局、Windows 风格侧栏、图标解析、This PC 驱动器卡片以及单击选择/双击进入等交互。

> 这些修改的新增表达由 SYSTEM-Intel-MIC 以 GPL-3.0-or-later 发布；上游 MIT 版权与许可证同时适用于其原始部分。任何再发布者都必须同时保留 `LICENSES/Explorer-for-Linux-MIT.txt`、本仓库 `LICENSE` 和 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) 中的声明。

## 图标与品牌资源

Microsoft Fluent System Icons 为 MIT 许可，项目保留其许可证副本并通过 `assets/import_fluent_icons.py` 导入需要的 SVG/PNG 图标。[1] [2] 部分 Windows 风格 PNG 由 WindowsIcons ICO 资源转换而来；来源、转换方式和商标边界见 [`assets/WINDOWSICONS-NOTICE.md`](assets/WINDOWSICONS-NOTICE.md)。Windows、Windows 11 和相关商标归其各自权利人所有；ElevenDE 不主张这些商标的所有权。[3]

## 发布与贡献

每一个发布都应遵循相同流程：先在干净工作树中构建 `elevende_3.6_amd64.deb`，再提交源代码、[`CHANGELOG.md`](CHANGELOG.md) 更新日志与 README，最后打 `v<版本>` 标签——CI 校验 `v<包版本>` 与标签一致后自动把生成的 DEB 附加到同一版本的 GitHub Release。请不要提交本地二进制、`build/`、临时截图、回归日志或 `*.deb`；这些内容已由 `.gitignore` 排除。

对 Shell、设置、资源管理器或资源文件的改动应保留可构建源码，并在提交前至少执行：

```sh
git diff --check
./build-deb.sh
dpkg-deb -I elevende_3.6_amd64.deb
git status --short
```

## 许可证

根目录 [`LICENSE`](LICENSE) 是未经改写的 GNU General Public License version 3。SYSTEM-Intel-MIC 对 ElevenDE 自主代码及其可授权的新增修改采用 **GPL-3.0-or-later**。MIT 上游代码、Microsoft Fluent 图标和其他第三方资产并不因根目录 GPL 而自动被改写；准确的文件级边界、许可证副本和再发布义务见 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)。

## 参考资料

[1]: https://github.com/microsoft/fluentui-system-icons "Microsoft Fluent System Icons"
[2]: https://github.com/microsoft/fluentui-system-icons/blob/main/LICENSE "Fluent System Icons MIT License"
[3]: https://github.com/HaydenReeve/WindowsIcons "HaydenReeve WindowsIcons"
[explorer-upstream]: https://github.com/macOS-Terminal/Explorer-for-Linux "Explorer-for-Linux"
