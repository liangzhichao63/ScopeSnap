# ScopeSnap

用于 **SIGLENT SDS3000X HD 系列示波器**的 Qt/C++ 截图上位机，首先面向 SDS3054X HD。支持 USB VISA、网络 LAN 和离线演示，提供中文界面、图片预览、保存与复制。

本项目自有代码采用 [MIT 许可证](LICENSE)。当前版本 **0.2.0**，目标平台 **Windows 11 x64**。

> 已通过 Release 构建和离线协议/UI 测试；真实示波器的 USB、LAN 通信与截图尚未验证。离线演示画面带有非实测水印。

## 功能

- USB：扫描并连接 SIGLENT USB 仪器，动态加载电脑上已安装的 VISA 运行库。
- LAN：通过示波器 IP 和 SCPI TCP 端口连接，默认端口 `5025`，不依赖 VISA。
- 截图：一键或 F5 获取 PNG，自动保存、另存为、复制图片；Ctrl+Shift+C 复制到剪贴板。
- 归档：选择保存目录和工况文件名前缀，自动添加时间及短随机标记。
- 预览：适应窗口、100% 显示，双击保存记录打开图片。
- 文字标注：点击图片放置中文或多行文字，设置字号与颜色；拖动、双击编辑、删除、撤销与清空。
- 校验：检查 PNG 长度、CRC、结尾和解码结果，报告超时、断线与损坏数据。
- 演示：无仪器时体验界面和保存流程。

程序只发送 `*IDN?` 和 `:PRINt? PNG`，不改变通道、触发、量程或采集设置。保存内容是仪器屏幕截图，不是原始波形数据。设备身份检查目前限定 SIGLENT SDS3000X HD 系列；其他型号未支持。

## 使用

从 [GitHub Release 下载 Windows 11 x64 便携包](https://github.com/liangzhichao63/ScopeSnap/releases/tag/v0.2.0)，选择附件 `ScopeSnap-0.2.0-Windows11-x64.zip`。包内包含运行库、使用说明、源码、许可证和 NI-VISA 官方在线安装器；安装驱动时需要联网。

已有完整 Windows 便携包时，解压全部文件并运行 `ScopeSnap.exe`，保留同目录 DLL、`platforms`、许可证及配套源码。GitHub 的 **Download ZIP 是源码归档**，不包含可直接运行的 EXE。

1. **USB**：按供应商说明安装 VISA，用数据线连接示波器背面的 USB Device 方口；选择“USB 数据线”，刷新并连接设备。
2. **LAN**：选择“网络 LAN”，输入示波器 IP 与端口后连接。电脑与示波器需要网络可达。
3. **演示**：选择“离线演示”并开始演示，无需仪器或驱动。
4. 点击“截图”或按 F5。默认自动保存到当前用户图片目录下的“示波器截图”，也可选择其他目录。

USB 驱动由供应商独立提供；[NI-VISA 官方下载](https://www.ni.com/en/support/downloads/drivers/download.ni-visa.html)。ScopeSnap 不会自动安装驱动。NI 软件不适用本项目的 MIT 许可，安装、使用和部署遵循 NI 条款。LAN 连接不需要安装 VISA。

详细操作见 [使用说明](使用说明.md)。

## 在截图上写文字

截图后点击预览区的 **「＋ 文字」**，再点击图片上的位置，输入文字并选择字号、颜色。单击可选中，拖动可移动，双击或点「编辑」可修改；Delete 删除，画布获得焦点时 Ctrl+Z 撤销，Esc 退出放置或取消本次拖动。

「保存标注」会在设置的保存目录生成带 `_标注_` 的新 PNG；「另存为」与「复制图片」也会包含文字。截图后的自动保存保留未标注原图，默认保存标注不会覆盖它。字号和位置按原图记录，适应窗口或100%切换不改变导出尺寸。再次截图或关闭窗口前，有未保存标注时会提示保存、放弃或取消；复制到剪贴板不清除未保存状态。

标注在当前截图上可编辑；导出的 PNG 中文字已合并为图片像素，关闭或切换截图后不能再把该 PNG 中的文字单独编辑。

## 从源码构建

要求：CMake **3.21+**、C++17 编译器、Qt **6.5+** 的 Widgets / Network / Test 模块。已验证的 Windows 工具链为 **Qt 6.5.3 MinGW 64-bit + MinGW 11.2 + Ninja**；其他工具链尚未验证。当前 CMake 配置即使关闭测试也会查找 Qt Test 模块。

```powershell
git clone https://github.com/liangzhichao63/ScopeSnap.git
cd ScopeSnap
```

如果 Qt 安装在默认的 `C:\Qt` 路径，运行：

```powershell
.\构建.ps1 -SkipPackage
```

该命令配置 Release、编译程序并执行离线测试，不需要下载 Qt 源码归档或 NI 安装器。安装位置不同时可覆盖全部工具路径：

```powershell
.\构建.ps1 -SkipPackage `
  -QtRoot 'D:\Qt\6.5.3\mingw_64' `
  -CompilerRoot 'D:\Qt\Tools\mingw1120_64' `
  -CMake 'D:\Qt\Tools\CMake_64\bin\cmake.exe' `
  -Ninja 'D:\Qt\Tools\Ninja\ninja.exe'
```

构建目录中的程序名为 `ScopeCapture.exe`，便携打包时改名为 `ScopeSnap.exe`。直接运行构建产物时，需要让 Qt 和 MinGW 的 `bin` 目录位于当前终端的 `PATH` 中；上述脚本会为当前终端设置它们。

```powershell
.\build\ScopeCapture.exe --demo
# 连接真实设备时不带 --demo：
.\build\ScopeCapture.exe
```

## 测试与验证范围

`构建.ps1 -SkipPackage` 会自动运行 CTest，并临时将 `QT_QPA_PLATFORM` 设为 `offscreen`。也可在配置好工具链的终端中单独运行：

```powershell
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:QT_QPA_PLATFORM = 'offscreen'
    & 'C:\Qt\Tools\CMake_64\bin\ctest.exe' --test-dir build --output-on-failure
} finally {
    $env:QT_QPA_PLATFORM = $previousPlatform
}
```

测试使用本机模拟 SCPI 服务器和离线演示，不需要连接仪器。覆盖 PNG 分包、CRC 损坏、截断、非法尾部与尺寸、连续 LAN 截图、错误设备身份、断传会话关闭、自动保存及保存失败；还验证标注缩放/滚动坐标、拖动/撤销、取消编辑、保存/复制像素、原图保留与未保存保护。CTest 注册一个包含多项功能案例及数据行的 QtTest 程序。

2026-11-04 完成过便携包的独立解压启动检查。真实 USB 与仪器 LAN 通信、首次读取设备固件和真实截图仍待验证。协议依据 [SIGLENT SDS 编程手册 CN11F](https://www.siglent.com/wp-content/uploads/2024/08/SDS_ProgrammingGuide_CN11F.pdf)；其中 SDS3000X HD 适用固件为 1.0.3.0 及以后，程序当前仅显示固件版本，不强制比较版本号。

## 生成完整便携包

源码仓库不包含构建产物、Qt 原始源码归档或 NI 安装器。当前打包脚本固定配套 Qt 6.5.3 和 NI-VISA 26.8；先按以下说明准备外部文件：

- [Qt 源码来源与核验](third_party/qt-source/源码来源与核验.md)：放入 `third_party/qt-source/qtbase-everywhere-src-6.5.3.tar.xz`。
- [驱动安装说明](third_party/drivers/驱动安装说明.txt)：放入 `third_party/drivers/ni-visa_26.8_online.exe`，并确认相应供应商使用与分发条款。

准备完成后运行 `.\构建.ps1`，脚本会检查归档哈希及安装器签名，编译、测试，并在 `dist/` 中生成包含运行库、源码、许可和在线驱动安装器的包。NI 安装器需要联网，不是离线完整驱动介质。更换 Qt 版本时，须同步更新配套源码、归属说明及哈希，不能只替换 `QtRoot`。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `src/` | 界面、通信线程、USB VISA / LAN、PNG 校验 |
| `tests/` | QtTest 离线协议与界面测试 |
| `resources/` | 图标、Windows 版本资源、第三方许可与归属 |
| `third_party/` | 外部下载来源、校验及许可资料 |
| `CMakeLists.txt` | CMake 工程定义 |
| `构建.ps1` | Windows 构建、测试、便携打包 |
| `使用说明.md` | 操作说明 |

## 许可与反馈

自有代码见 [MIT LICENSE](LICENSE)。Qt、MinGW 及其内含第三方组件保留各自许可；NI-VISA 为独立供应商软件。详细说明见 [第三方组件与源码](resources/licenses/第三方说明.md)，根目录 MIT 许可不替代这些第三方条款。

欢迎通过 Issues 或 Pull Requests 反馈问题。通信问题请注明系统、连接方式、仪器型号、固件版本和复现步骤；请勿附带密码、令牌或无关的私人数据。
