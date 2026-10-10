# WinFileShare 与 AppFileShare

Windows Qt 程序和 Android Java 程序通过低功耗蓝牙交换 libjuice 1.7.4 的 SDP 与 ICE Candidate，再尝试不经过 STUN/TURN 的 UDP 直连。文件只走这条 UDP 通道。

这是未加密的学习原型。libjuice 连接成功也不等于文件内容经过加密。不要用它传输敏感文件。蓝牙配对、设备握手和接收确认是三件不同的事。

没有信令服务器、文件服务器或中转。同一局域网里的 host candidate 有机会直连。两端分别位于不同 NAT 后面时，直连可能失败，界面会保留失败原因。

## 已确认的本机环境

- Qt 6.12.0：`D:\DevKits\Qt\6.12.0\msvc2022_64`
- Visual Studio 2022 Community，工具集 v143
- libjuice 1.7.4：`E:\vcpkg\installed\x64-windows`
- 源码副本：`third_party/libjuice-1.7.4`，来自 `E:\vcpkg\downloads\paullouisageneau-libjuice-v1.7.4.tar.gz`
- Android SDK：`D:\DevKits\Android\Sdk`
- NDK 27.2.12479018 和 CMake 3.22.1 已安装在该 SDK 中

## Windows

在 `WinFileShare` 目录执行 `debug_build.bat`。它调用 VS2022 的 MSBuild 编译 Debug x64，结束后会暂停。

不需要暂停时可以直接执行：

```bat
"D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" WinFileShare.sln /p:Configuration=Debug /p:Platform=x64 /m
```

产物：

- `WinFileShare\x64\Debug\WinFileShare.exe`
- 同目录的 `juice.dll`
- `WinFileShare\x64\Debug\ProtocolSelfTest.exe`

运行自测：

```bat
WinFileShare\x64\Debug\ProtocolSelfTest.exe
```

通过时最后一行是“全部协议自测通过”。

Windows 预编译会调用 `cppwinrt.exe` 生成 `WinFileShare\WinFileShare\generated`。这是构建输出，可以删除后重新编译。

如果链接找不到 juice，确认 Debug 使用 `E:\vcpkg\installed\x64-windows\debug\lib\juice.lib`，并且 `juice.dll` 在 exe 旁边。

## Android

本机缺少 NDK 时，用 Android Studio 自带的 JBR 安装：

```bat
set JAVA_HOME=D:\Program Files\Android\Android Studio\jbr
D:\DevKits\Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat --sdk_root=D:\DevKits\Android\Sdk --install "ndk;27.2.12479018" "cmake;3.22.1"
```

`AppFileShare\local.properties` 已经指向 `D:\DevKits\Android\Sdk`。编译：

```bat
cd AppFileShare
gradlew.bat :app:assembleDebug
```

APK：`AppFileShare\app\build\outputs\apk\debug\app-debug.apk`。

清理后重编：

```bat
gradlew.bat clean :app:assembleDebug
```

常见问题：

- CMake 找不到 libjuice：确认 `third_party/libjuice-1.7.4` 存在。
- NDK 版本不匹配：`app/build.gradle.kts` 固定为 `27.2.12479018`。
- 原生库加载失败：APK 的 ABI 需要和设备一致。当前包含 `arm64-v8a`、`armeabi-v7a` 和 `x86_64`。
- 广播失败：检查蓝牙是否打开，以及是否授予“附近设备”或蓝牙广播/连接权限。部分手机不支持外设广播。

真机安装：

```bat
adb install -r AppFileShare\app\build\outputs\apk\debug\app-debug.apk
```

Android 先点“开启设备发现”。Windows 再点“发现设备”，选中手机后点“连接手机”。两边都显示“P2P 已连接”后才能发送文件。接收必须由对方明确选择保存位置；拒绝或取消保存不会写文件。

## 代码布局

- `common/protocol`：BLE 分片、文件帧、可靠传输和 SHA-256
- `common/p2p`：libjuice 封装和连接状态机
- `WinFileShare/WinFileShare/ble`：Windows GATT 客户端
- `WinFileShare/WinFileShare/session`：把状态机接到 Qt
- `AppFileShare/app/src/main/java`：Java 界面和 GATT Server
- `AppFileShare/app/src/main/cpp`：JNI

字节布局见 [PROTOCOL.md](PROTOCOL.md)。实际跑过和没跑过的项目见 [TEST.md](TEST.md)。
