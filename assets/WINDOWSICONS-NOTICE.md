# WindowsIcons 转换资源说明

`assets/icons/` 中的部分彩色 PNG 图标由 [HaydenReeve/WindowsIcons][windowsicons] 提供的 ICO 资源转换而来，供 ElevenDE/Lindows 的应用、位置、设备和资源管理器命令栏使用。转换脚本是 [`assets/import_windowsicons.py`](import_windowsicons.py)；它从本地 `Icons/` 源目录生成 16–128px PNG，构建过程不联网。

| 范围 | 原始资源类别 | 本项目用途 |
|---|---|---|
| 应用 | `applications/*.ico` | Explorer、设置、记事本、计算器、任务管理器和终端。 |
| 位置与文件夹 | `folders/*.ico`、`files/*.ico` | 桌面、主页、下载、文档、图片和资源管理器侧栏。 |
| 设备与状态 | `devices/*.ico`、`emblems/*.ico` | This PC、驱动器和兼容的系统状态别名。 |

如需重导入，先取得 WindowsIcons 的 `Icons/` 目录，再执行：

```sh
WINDOWSICONS_SOURCE=/path/to/WindowsIcons/Icons \
  python3 assets/import_windowsicons.py
```

上游仓库的许可证、商标和再分发条件以其随附文件为准。Windows、Windows 11 和 WindowsIcons 的名称及相关商标归各自权利人所有；ElevenDE 不主张其所有权，也不因将 ICO 转换为 PNG 而改变上游资产的权利状态。完整发布边界见 [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)。

## 参考资料

[windowsicons]: https://github.com/HaydenReeve/WindowsIcons "HaydenReeve WindowsIcons"
