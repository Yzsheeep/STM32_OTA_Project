# F411 App

由现有 `boot_f411` 的 CubeMX 生成代码建立独立工程，迁入原 F103 App 的业务逻辑。此目录不包含 Bootloader 的擦除、搬迁或跳转实现。

## 打开与编译

Keil 打开 `MDK-ARM/app_f411.uvprojx`，选择 `app_f411`，按 F7 编译。

编译成功后，After Build 使用现有 ARMCC 的 fromelf 生成 `MDK-ARM/app_f411/app_f411.bin`。该二进制从 App 向量表开始，不包含前面的 64 KB Bootloader 空白区。不要使用原 F103 的 app.bin 给 F411 升级。

2026-10-31 已完成一次真实蓝牙 OTA：9536 字节、75 包，CRC32=C13C6E20；App 启动并回复 APP_VERSION=1.0.1，片内读回与发送文件逐字节一致。App 没有通过 ST-Link 预先下载。当前 OTA 外部暂存上限仍为 16 KB，不能因为 App 分区变大就直接升级更大的固件。

## 已保留的行为

- USART1 PA9/PA10，9600、8N1；启动提示沿用原 App 文本。
- 接收 V 后在主循环回复 APP_VERSION=1.0.1。
- 接收 U 后在主循环向 AT24C02 写入升级标志，等待写完成、读回核对，再复位。
- EEPROM 使用现有 F411 的 I2C2：PB10 SCL、PB3 SDA；LED 仍每秒翻转。
- 时钟、SPI、I2C、GPIO 初始化直接复用现有 F411 工程；未改通信协议。

## 要学习的差异

| 位置 | 当前配置 | 用途 |
| --- | --- | --- |
| Keil Target 的 IROM1 | 起点 0x08010000，大小 0x70000 | App 在内部 Flash 的链接范围 |
| Keil Target 的 IRAM1 | 起点 0x20000000，大小 0x20000 | F411 的 128 KB SRAM |
| main.c 的 USER CODE BEGIN 1 | VTOR=0x08010000，然后开启中断 | 让 App 使用自己的中断向量表 |

`SCB->VTOR` 只指定 CPU 查表的位置，不会复制向量表。链接起点、这里的 VTOR 和 Bootloader 跳转目标需要一致。

App 的 main.c 中保留原来的启动提示、V/U 处理、标志写入及读取校验；usart.c 的 USER CODE 中保留原来的 printf 串口重定向，Keil 使用与原 App 相同的 MicroLIB。

## 后续验证

Bootloader 的 MSP 范围已对应 F411 的 128 KB 调整；跳转后加了纯 C 死循环，已检查当前 ARMCC 产物在换栈后直接调用 App 入口。本次完整 OTA 已通过，但这不代表断电恢复、自动回滚或长期可靠性已验证。更换编译器或优化设置后，仍需核对跳转交接的实际编译结果。

后续用 Keil 下载 App 时需要保留前 64 KB Bootloader，Flash 下载设置应选择擦除所用扇区（Erase Sectors），不能擦除整片（Erase Full Chip）。正常独立复位仍先进入 Bootloader。本次实测仅通过 ST-Link 下载 Bootloader，App 通过蓝牙安装。

若以后用 CubeMX 重新生成工程，应保留 USER CODE，并复查 Keil 的 App 链接起点、分区大小、MicroLIB 及 After Build 的二进制输出设置。

VS Code 可直接打开本目录，已有独立的 F411 头文件路径及芯片宏配置。
