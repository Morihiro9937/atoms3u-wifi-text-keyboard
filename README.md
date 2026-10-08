# 自动热点打字机开源项目

一次烧录，设备自建热点；在手机上粘贴文本，由 USB HID 键盘输入目标电脑。

[项目网页](https://zidongdazi.pages.dev/home/) · [最新下载](https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard/releases/latest)

## 下载安装包

| 系统 | 下载 | 运行方式 |
| --- | --- | --- |
| Windows x64 | [r9 免安装 EXE](https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard/releases/download/installer-r9/auto-hotspot-typer-Windows-x64-r9.exe) | 双击运行 |
| macOS Apple 芯片 | [r9 DMG](https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard/releases/download/installer-r9/auto-hotspot-typer-macOS-arm64-r9.dmg) | 打开映像后运行应用 |

安装包内置烧录工具及公开版固件，烧录时无需联网。macOS 包使用临时签名，尚未经过 Apple 公证。文件校验值见 [r9 发布页](https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard/releases/tag/installer-r9)。

## 硬件与操作

| 开发板 | 闪存 | 进入烧录模式 |
| --- | --- | --- |
| ESP32-S2-WROOM 最小系统板 | 4 MB | 按住 BOOT 插入 USB 后松开 |
| M5Stack AtomS3U K125 | 8 MB | 插入后长按 RESET 约 2 秒 |

1. 在自己的电脑打开烧录程序，按界面提示连接开发板。
2. 选择热点名称和密码，确认后烧录。
3. 显示全部 5 段写入并通过校验后，拔下开发板重新插入。
4. 手机连接开发板热点，打开 `http://192.168.4.1`，粘贴文本并在目标电脑的输入框中开始打字。

## 版本与源码

最新安装包为 r9，内置 ESP32-S2 与 AtomS3U 公开版固件。当前仓库 `main` 分支仍是早期 AtomS3U r19 固件源码，**并非 r9 安装包内固件的对应源码**；GitHub 自动生成的“Source code”压缩包也只对应这个旧分支。不要用旧版源码包重现 r9 安装包。

旧版 AtomS3U 固件的编译入口：`sh scripts/pio run -e phase_e`。旧版镜像仍在 [r19 发布页](https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard/releases/tag/v2026.10.02-r19)。

## 验证状态

Windows r9 已通过云端构建、离线测试及内置资源自检；macOS r9 已通过应用自检与 DMG 校验。Windows 实机烧录和 AtomS3U 实机仍待验收。运行时 USB 仅提供标准 HID Keyboard。

项目采用 [GPL-3.0 许可证](LICENSE)。
