# AshDesk 极帧

AshDesk 是一款低延迟远程桌面。这个仓库是它的**桌面程序**和**对外发布页**：

- 源码：Qt 界面、液态玻璃渲染内核、自动更新器、安装与卸载脚本（Apache-2.0）；
- [Releases](../../releases)：完整的安装包（界面 + 远程桌面引擎 + 驱动）；
- [Issues](../../issues)：问题反馈。

## 仓库结构

```
AshDesk（本仓库，公开）
├── ui/            Qt 界面（Widgets）
├── glass/         液态玻璃像素内核（C17 + SSE2 / NEON 汇编，不依赖 Qt）
├── updater/       自动更新器 ashdesk_update（验 Ed25519 签名后整目录换新，失败换回）
├── packaging/     Windows 的 install.ps1 / uninstall.ps1，macOS 的 Info.plist、更新公钥
├── translations/  界面翻译（源文字是中文）
├── tests/         玻璃内核、更新器、界面测试（界面测试用假核心，不需要引擎）
├── docs/          界面设计、界面和引擎之间的接口约定（docs/核心接口.md）
├── server/        → AshDeskServer：信令与中继服务器（公开，AGPL-3.0）
├── apple/         → AshDeskApple：iPad / iOS 客户端（公开，Apache-2.0，筹备中）
├── android/       → AshDeskAndroid：安卓客户端（公开，Apache-2.0，筹备中）
├── harmony/       → AshDeskHarmony：鸿蒙客户端（公开，Apache-2.0，筹备中）
└── private/       以下子库不公开，外人克隆时自动跳过
    ├── engine/    → AshDeskEngine：远程桌面引擎（主机、客户端、系统服务）
    └── drivers/   → AshDeskDrivers：虚拟显示器、虚拟声卡驱动
```

界面和引擎之间没有链接关系：界面以子进程方式拉起引擎的 `adrd_host` / `adrd_client`，经命令行参数和控制管道（文本行）交流，
约定见 [docs/核心接口.md](docs/核心接口.md)。所以本仓库不需要引擎就能编译、运行界面测试。

## 克隆

```
git clone --recursive https://github.com/mrQzs/AshDesk.git
```

`private/` 下的子库在 `.gitmodules` 里设了 `update = none`，没有权限也不会让克隆失败。有权限的人用
`git submodule update --init --checkout private/engine`（其余同理）取下来。

## 构建（Windows）

要：Visual Studio 2022、CMake ≥ 3.24、Ninja、NASM、Qt 6.10（MSVC 2022 64 位）；OpenSSL 3.5.9 和 GoogleTest 按
`third_party/*/README.AshDesk.md` 用 vcpkg 编出来放进 `third_party/`。

```
set QT_DIR=C:\Qt\6.10.1\msvc2022_64
build.bat
ctest --test-dir build --output-on-failure
```

界面测试在 Windows 上用真实的窗口平台插件，会在桌面上开窗口。

## 发布

正式安装包由私有的引擎仓库的发布脚本出：编译引擎、本仓库（作为子目录检出）和驱动，拼成一个包、签名、生成更新清单，
再上传到本仓库的 Releases。本仓库的 CI 只编译开源部分、跑测试，不接触签名证书和更新私钥。

## 许可

本仓库的源码按 [Apache License 2.0](LICENSE) 发布，见 [NOTICE](NOTICE)。安装包里的远程桌面引擎和驱动是闭源的，
只以二进制发布，不在 Apache-2.0 的范围内。
