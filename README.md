# STM32 蓝牙 OTA Bootloader

在 STM32F103C8T6 上实现蓝牙固件升级。电脑从 GitHub 下载固件或选择本地 `.bin`，通过 JDY-31 发送到 W25Q64，CRC32 校验通过后写入内部 APP 分区，安装后再次校验并启动 APP。

使用 C、STM32 HAL、UART 中断、SPI、I2C；固件通过 Keil MDK 编译，Python 上位机负责发送文件和显示升级结果。

## 功能

- 运行中的 APP 接收 `U` 指令，保存升级标志并复位进入 Bootloader。
- 以 128 字节分包、停等应答的方式接收固件；AT24C02 保存升级信息并读回核验。
- 擦除 APP 前校验外部暂存固件，搬运后校验内部 APP；失败时退出安装流程。
- GitHub 托管版本清单和固件；上位机比较版本，下载后核对大小、CRC32 和固件内版本标识。
- 打开串口时发送 `V` 查询当前版本；升级后确认 APP 启动，并检查回复版本与目标版本一致。

升级流程：

```text
云端下载 / 本地文件 → 蓝牙传输 → 外部暂存校验 → 擦写 APP → 安装校验 → 启动并确认版本
```

## 实板验证

已通过 CH340 有线和 JDY-31 蓝牙升级验证。云端升级实测下载 V1.0.1 固件，发送 11684 字节、92 个分包，设备启动后回复 `APP_VERSION=1.0.1`。重新打开上位机连接设备后可查询版本，再次检查显示已是最新版本。

在 Keil 中人为修改运行时 CRC，也验证了两处校验的失败分支：暂存校验失败时不擦除 APP，安装后校验失败时不清升级标志、不复位。之后可重新升级恢复运行。

## 使用

1. 用 Keil 编译 `boot/MDK-ARM/boot.uvprojx` 和 `app/MDK-ARM/app.uvprojx`，首次通过 ST-Link 下载 Bootloader。
2. 在仓库根目录启动上位机：

   ```powershell
   uv run --with pyserial --with requests python ota_gui.py
   ```

3. 选择蓝牙虚拟 COM 口或有线串口，参数为 `9600 / 8N1`，打开后自动查询设备版本。
4. 点击“检查云端更新”，有新版本时选用云端固件升级；也可手动选择 `app/MDK-ARM/Objects/app.bin`。

日常更新 APP 通过 OTA 安装，无需用 ST-Link 直接下载。串口助手与上位机不要同时占用同一个 COM 口。

## 存储与限制

Bootloader 占内部 Flash 前 16 KB，APP 从 `0x08004000` 开始，分区大小 48 KB；W25Q64 用作固件暂存。

当前暂存区固定擦除 16 KB，升级文件需在 **16 KB 以内、长度为 4 字节的整数倍**。当前为单 APP 方案，没有自动回滚；安装期间断电可能导致 APP 不完整。云端分发使用 GitHub 静态文件和 Release 附件，没有独立后端。

接线、协议、元数据格式和验证记录见 [详细说明](docs/ota-details.md)。源码入口：[`boot/`](boot/)、[`app/`](app/)、[`ota_gui.py`](ota_gui.py)。
