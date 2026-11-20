# WLAN_Monitor

Windows 小工具：把 Wi-Fi 连到**非 DFS 的 5 GHz 信道**，让移动热点（软 AP）能以 5 GHz 正常启动。

![界面](docs/ui.png)

单文件 Win32 + WlanAPI 实现，纯 C++，无第三方依赖。

## 功能

- 列出当前 SSID 下所有 BSSID：信道、频段、信号、以及该信道能否用于热点
- 把连接切到选中的 BSSID，或自动挑信号最好的可用信道
- 一键跳转 Windows 移动热点设置
- 只改变「连到哪个 BSSID」，不改注册表、不改系统设置

## 使用

1. 双击 `bin\WLAN_Monitor.exe`，UAC 允许（**需要管理员权限**）
2. 等扫描完成（约 4 秒）
3. 点「自动选可用」，或选中一行后点「切换到选中」
4. 状态栏提示可用后，打开移动热点即可用 5 GHz

命令行模式：

```powershell
.\WLAN_Monitor.exe --selftest               # 只读自检，输出 selftest.txt
.\WLAN_Monitor.exe --pin 6C:87:20:28:05:20  # 直接切到指定 BSSID
```

## 构建

Visual Studio 打开 `WLAN_Monitor.sln`（`Release | x64`）编译，或直接运行：

```bat
build.bat
```

输出：`out\x64\Release\WLAN_Monitor.exe`，同时复制一份到 `bin\`。

## 说明

- **需要管理员权限**：非提权进程查询 WLAN 状态会被系统拒绝。
- **漫游后会失效**：系统切回 DFS 信道（信号最强）时热点会再次起不来，重新点一次「自动选可用」即可。
- **6 GHz 不支持**：Windows 热点 API 上限为 5 GHz（网卡硬件本身支持 6 GHz）。
- 详细背景与排查记录见 [docs/热点故障诊断报告.md](docs/热点故障诊断报告.md)。

## 目录

```
├─ src/WLAN_Monitor.cpp     源码
├─ docs/                    截图与背景文档
├─ bin/                     预编译产物（未纳入版本库）
├─ build.bat                命令行编译
└─ WLAN_Monitor.sln/.vcxproj
```

## License

MIT
