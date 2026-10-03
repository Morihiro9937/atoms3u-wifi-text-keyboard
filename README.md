# AtomS3U Wi-Fi 长文本 USB 键盘

这是运行在 **M5Stack AtomS3U（K125、ESP32-S3）** 上的开源无线长文本 USB 键盘固件。手机或平板通过设备自建的 Wi-Fi 打开网页、粘贴文本，AtomS3U 再以标准 USB HID Keyboard 的方式把文本输入 Windows。

当前固定版本：`2026.10.02-r19-progress-sync`

> 目前公开版本只支持 AtomS3U。ESP32-S2 Mini 移植版尚未完成，不能直接烧录本仓库的固件。

## 主要功能

- 设备自建 Wi-Fi，无需安装 App；默认网络名 `karute`，密码 `autokarute`。
- 访问 `http://karute.local`，无法解析时使用 `http://192.168.4.1`。
- 正文最大 32 KiB，保存到 LittleFS，断电后保留。
- GBK、ENG、VUC 三种输入模式。
- 网页可开始、暂停、继续、停止、清空，并显示预计剩余时间和打印进度。
- Wi-Fi 名称、密码和 `.local` 登录地址可在设置页修改。
- 忘记连接信息时，开启 Num Lock，在非输入状态长按实体业务键约 3 秒：设备会打开 Windows 记事本并打印当前连接信息。
- 设置保存到 NVS，正文保存到 LittleFS。

运行时 USB **只暴露标准 HID Keyboard**，不启用 CDC 串口、Mass Storage、DFU、USB 网卡、鼠标或其他复合接口。ESP32-S3 的 ROM 下载模式在烧录期间临时显示串口，这是芯片本身的烧录接口，不是应用固件的运行形态。

## 输入模式

### GBK

适合 Windows 中文文本。Windows 切换到 ENG 英文键盘并开启 Num Lock。固件将每个字符严格转换为 GBK 编码，再使用 `Alt + 数字小键盘` 输入。五项时序参数可独立调整：Alt 按下后间隔、数字按下时间、数字间隔、Alt 松开前间隔、字间间隔。

打印开始前会检查整篇文本；发现 GBK 无法表示的字符时会报告位置并拒绝启动，不会静默跳过。

### ENG

直接按 US/QWERTY 键盘映射输入可打印 ASCII，适合英文、数字和常见半角符号。

### VUC

通过微软拼音的 `vuc + 小写十六进制码 + Space` 规则输入 Unicode 字符。必须在 Windows 微软拼音中文状态下使用；支持范围仍以目标系统和输入法的实际行为为准。

## 使用方法

1. 把 AtomS3U 插入 Windows 电脑。
2. 手机或 iPad 连接 Wi-Fi `karute`，密码 `autokarute`。系统提示“无互联网连接”时选择继续使用。
3. 浏览器打开 `http://karute.local`；若打不开，访问 `http://192.168.4.1`。
4. 在设置中选择模式并按上面的要求准备 Windows 输入法/Num Lock。
5. 先在 Windows 记事本中用短文本测试，再逐步使用较长内容。

首次建议测试：

```text
中文ABC，体温36.5℃。
```

不同 Windows 版本、输入法、键盘布局和目标软件可能产生不同结果。记事本测试通过不代表 Word、HIS 或其他软件一定兼容。

## 从源码编译

需要 Python 3 和 [PlatformIO Core](https://platformio.org/install/cli)。项目固定使用 `espressif32@6.12.0`（Arduino-ESP32 2.0.17）。

```sh
git clone https://github.com/Morihiro9937/atoms3u-wifi-text-keyboard.git
cd atoms3u-wifi-text-keyboard
pio run -e phase_e
```

也可以使用包装脚本；它会优先使用项目内已有的本地工具链，否则调用系统中的 `pio` 或 `platformio`：

```sh
sh scripts/pio run -e phase_e
```

应用固件位于 `.pio/build/phase_e/firmware.bin`。构建时会把 `web/*.html` 以 gzip 压缩并生成 C++ 头文件；网页应直接修改 `web/` 下的源文件。

## 烧录 AtomS3U

### 从源码烧录

1. 插入 AtomS3U。
2. 长按 RESET 约 2 秒，看到内部绿色灯后松开，进入 ROM 下载模式。
3. 查看串口：

   ```sh
   sh scripts/pio device list
   ```

4. 指定串口烧录：

   ```sh
   sh scripts/pio run -e phase_e -t upload --upload-port /dev/cu.usbmodemXXXX
   ```

   Windows 把端口替换为类似 `COM7`。

5. 成功后拔下再插入设备，退出下载模式并运行 HID 固件。

### 使用 GitHub Release 固件

Release 提供两种文件：

- `atoms3u-wifi-text-keyboard-2026.10.02-r19-factory.bin`：新设备使用的完整镜像，从地址 `0x0` 写入。
- `atoms3u-wifi-text-keyboard-2026.10.02-r19-update.bin`：只含应用程序，供已有相同分区布局的设备从地址 `0x10000` 更新。

新设备示例（安装 `esptool` 后执行）：

```sh
python -m esptool --chip esp32s3 --port <串口> --baud 460800 write_flash 0x0 atoms3u-wifi-text-keyboard-2026.10.02-r19-factory.bin
```

不需要全片擦除，也不要另行上传文件系统镜像。网页资源已经包含在应用固件内。

## 开发与测试

默认环境和当前主程序分别是 `phase_e` 与 `src/phase_e.cpp`：

```sh
sh scripts/pio run -e phase_e
node tests/alt_web_test.js
node tests/phase_e_web_test.js
node tests/progress_sync_web_test.js
```

早期阶段和独立原型仍保留在源码中供参考，但不是当前发布版本。提交修改时请保持运行态 USB 为单一 HID Keyboard，并先在 Windows 记事本中完成实机验证。

## 数据与医疗使用提示

- 本项目是键盘自动输入工具，不是医疗器械，也不判断文本内容是否正确。
- 所有医疗文本必须由使用者人工终审后再提交到业务系统。
- 正文保存在设备闪存；网页“清空”不等于对闪存做安全擦除。不要保存患者姓名、住院号等可识别身份的信息。
- 软件按现状提供，不保证适用于任何具体 Windows 版本、输入法、HIS 或工作流程。

## 许可证

本项目以 [GNU General Public License v3.0](LICENSE) 开源。你可以免费使用、研究、修改和再发布；分发修改版或衍生版时必须继续提供相应源代码并保留同一许可证。GPL 允许销售软件或硬件成品，但不允许把本项目的衍生版本改成闭源。

第三方工具链与依赖分别遵循各自的许可证。
