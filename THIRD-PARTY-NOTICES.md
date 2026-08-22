# ElevenDE 版权、许可证与第三方声明

## 适用范围

ElevenDE 的根目录 [`LICENSE`](LICENSE) 是 GNU General Public License version 3 的完整未修改文本。SYSTEM-Intel-MIC 对其拥有或有权授权的 ElevenDE 自主代码、构建脚本、配置、集成逻辑、品牌资源以及对上游项目的新增修改，采用 **GPL-3.0-or-later**。

> 根目录 GPL 不会自动改写本仓库中原始第三方文件的作者、版权或许可证。再发布时必须同时满足相应上游许可证、版权声明和本项目 GPL 声明。

## 代码来源与文件级边界

| 组件或范围 | 原始来源与原始许可 | ElevenDE 的内容与许可 | 随仓库保留的文件 |
|---|---|---|---|
| `Explorer-for-Linux/` | [macOS-Terminal/Explorer-for-Linux][explorer]，MIT。[1] | Lindows 资源管理器的 UI、布局、图标、命令栏、路径栏、This PC 和交互改动属于 ElevenDE 的新增修改，GPL-3.0-or-later；原始 MIT 代码仍保留 MIT 许可。 | [`LICENSES/Explorer-for-Linux-MIT.txt`](LICENSES/Explorer-for-Linux-MIT.txt) |
| `SAS-for-Linux/` | [macOS-Terminal/SAS-for-Linux][sas]，MIT。[2] | Shell 会话启动、快捷键、部署、调用链与 ElevenDE 自主整合代码为 GPL-3.0-or-later；SAS 原始代码仍为 MIT。 | [`LICENSES/SAS-for-Linux-MIT.txt`](LICENSES/SAS-for-Linux-MIT.txt) |
| `runbox-linux/` | SYSTEM-Intel-MIC 维护的 RunBox 组件。 | 由权利人随 ElevenDE 发布；本仓库内的 RunBox 源码及其 ElevenDE 集成按 GPL-3.0-or-later 提供。 | 根目录 [`LICENSE`](LICENSE) |
| `shell/`、`apps/`、`wm/`、`session/`、`scripts/`、`tools/` | ElevenDE 自主组件。 | GPL-3.0-or-later。 | 根目录 [`LICENSE`](LICENSE) |

### Lindows 资源管理器的具体说明

Lindows 资源管理器不是把 Explorer-for-Linux 更名后声明为完全自研。它以该项目的 MIT 代码为基础，并在以下文件中叠加 ElevenDE/Lindows 的增量实现：

| 文件 | ElevenDE/Lindows 增量方向 |
|---|---|
| `Explorer-for-Linux/src/explorer/filelist.cpp` | 文件列表刷新、选择与交互可靠性。 |
| `Explorer-for-Linux/src/explorer/main.cpp` | 应用身份、启动和 Lindows 命名衔接。 |
| `Explorer-for-Linux/src/explorer/mainwindow.cpp` | 顶部命令栏、路径栏、侧栏、主题图标和 Windows 风格布局。 |
| `Explorer-for-Linux/src/explorer/style.cpp` | 浅色 Fluent 风格、间距、圆角和表面样式。 |
| `Explorer-for-Linux/src/explorer/thispc.cpp` | This PC 驱动器卡片、主题图标、单击选择与双击进入。 |

这些新增修改由 SYSTEM-Intel-MIC 按 GPL-3.0-or-later 授权。原始 MIT 代码的署名和许可权利不受影响；修改或再发布 Explorer 部分时，应同时保留 MIT 许可证副本和本项目 GPL 声明。

## 图标、壁纸与品牌资源

| 资产 | 来源与许可边界 | 本仓库中的处理 |
|---|---|---|
| Fluent System Icons | Microsoft 的 MIT 许可图标集。[3] [4] | `assets/import_fluent_icons.py` 只导入项目使用的资源；MIT 文本保存在 [`LICENSES/Fluent-System-Icons-MIT.txt`](LICENSES/Fluent-System-Icons-MIT.txt)。 |
| WindowsIcons 转换 PNG | [HaydenReeve/WindowsIcons][windowsicons] 的 ICO 资源。上游的许可证、商标和再分发条件以其仓库文件为准。 | 仅保存构建所需的转换 PNG 和转换脚本；详见 [`assets/WINDOWSICONS-NOTICE.md`](assets/WINDOWSICONS-NOTICE.md)。ElevenDE 不主张 Windows、Windows 11 或 WindowsIcons 商标。 |
| ElevenDE 壁纸、Mica 噪声和自有配置资源 | SYSTEM-Intel-MIC 的 ElevenDE 资源。 | 按 GPL-3.0-or-later 发布，除非文件旁另有明确说明。 |

## 发布要求

发布二进制包时，应让对应源码提交可公开取得，并保留 `LICENSE`、本文件、`LICENSES/`、各资源来源说明以及原始版权声明。生成的 `.deb` 作为 GitHub Release 附件发布，而不是提交进仓库；该附件必须可由发布标签指向的源码和 `build-deb.sh` 重建。

## 参考资料

[1]: https://github.com/macOS-Terminal/Explorer-for-Linux "Explorer-for-Linux"
[2]: https://github.com/macOS-Terminal/SAS-for-Linux "SAS-for-Linux"
[3]: https://github.com/microsoft/fluentui-system-icons "Microsoft Fluent System Icons"
[4]: https://github.com/microsoft/fluentui-system-icons/blob/main/LICENSE "Fluent System Icons MIT License"
[explorer]: https://github.com/macOS-Terminal/Explorer-for-Linux "Explorer-for-Linux"
[sas]: https://github.com/macOS-Terminal/SAS-for-Linux "SAS-for-Linux"
[windowsicons]: https://github.com/HaydenReeve/WindowsIcons "WindowsIcons"
