# F411 固件升级 · Android

用于 Android 8.0 及以上手机的经典蓝牙 SPP 客户端。仅使用 Android 系统 API。手机可以独立通过公网 HTTPS 获取 F411 固件，不需要 Windows 转发、SSH 密钥或管理员令牌。

## 安装与软件验收

本机生成的测试 APK 在 `app/build/outputs/apk/debug/F411-OTA-debug.apk`。传到手机，点开按系统提示安装。“检查云端更新”可以在没有蓝牙设备时运行。验证通过后显示版本、大小和 SHA-256。点击“选择本地 BIN”通过系统文件选择器读取镜像，也可离线使用。

每次云端检查会先清空候选固件。客户端验证 HTTPS 证书、发布目标、APP 地址、版本绑定的同源路径、长度、向量、内嵌 APP_VERSION、SHA-256 和 CRC32，并拒绝重定向。固件只缓存在内存。已知设备版本时只下载较高的云端版本；本地 BIN 可用于同版重刷。未配对或未连接设备时仍可检查固件。

## 单独进行硬件验收时

在 Android 系统设置配对 JDY-31，再点“选择已配对设备”。Android 12 及以上需允许“附近设备”。手机端只列出已配对设备，不扫描附近设备或申请定位权限。如 Windows 正占用模块，请先断开 Windows 的蓝牙串口。

连接后发 V 并显示实际 APP 版本。本版从可回复版本的运行中 APP 发起升级。用户选择固件并确认写入后才发 U。共用当前协议：H/h 与 16 字节小端元数据、E 擦除、128 字节停等 ACK、C 安装、最终再次查询 V。单包失败时终止。显示 100% 后仍等待安装结果，最终以 APP 实际回复目标版本验收。传输期间保持 App 在前台，系统终止进程会断开连接。本次未执行真实手机或实板测试。

## 构建和验证

使用 Android Studio 打开本目录，或使用 JDK 17、Gradle 8.7、Android SDK 34 和 Build Tools 34.0.0 执行 `gradle :app:assembleDebug`。Windows 也可使用本目录的 `build-apk.ps1`，按 JDK、SDK platform、build-tools 的实际位置传入 `-Jdk`、`-Platform`、`-BuildTools`。该脚本使用官方 aapt2、javac、D8、zipalign 和 apksigner。测试签名存放于仓库外的当前用户 LocalAppData/F411-OTA 目录。同一台手机覆盖安装需使用同一签名。

`tests/SoftwareCheck.java` 在桌面 JVM 验证实际 Firmware 和 Cloud 类。把这两个类和测试编译至同一个 classpath，桌面测试需提供 org.json（本次使用 20240303）；Android 运行时直接使用系统 JSON。执行 `io.f411.ota.SoftwareCheck <F411 BIN> --public` 还会将公网最新版与参考 BIN 逐字节比对。普通测试不打开蓝牙或串口。
