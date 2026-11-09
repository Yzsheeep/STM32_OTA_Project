# F103 → F411：迁移差异与本次实测

记录日期：2026-10-31。本文记录当前工程与真实硬件结果，不把编译或分包 ACK 当作完整升级成功。

## 当前结果：已完成一次成功的 F411 OTA

已确认 ST-Link 连接的是 F411，目标电压约 3.25 V；已在首次写入前备份 512 KB 片内 Flash。Bootloader 下载并校验通过，COM14 收到启动日志。App 没有通过 ST-Link 预先下载。

蓝牙发送使用现有 `U → H/16字节元数据 → E → 128字节停等 → C → V` 协议：固件 9536 字节，75 包，CRC32 `C13C6E20`。最终一次传输收齐 75 个字符 `0` ACK，发送 C 后设备复位、Bootloader 跳转到 App，并实际回复 `APP_VERSION=1.0.1`。

最终只读核对确认：片内 App 的 9536 字节与发送文件逐字节一致，SHA256 为 `46b9e50da7a881d3bcc6f0b5e96e33e5865148a161248627cdcabb720717edcc`；EEPROM 内容为正常标志 0、长度 9536、CRC C13C6E20、版本字段 00010001；VTOR=08010000。片内 Bootloader 的 14320 个有效镜像字节与最新 HEX 一致。

此前几次传输虽然收齐 ACK，仍被外部 CRC 拦截，未进入内部 App 擦除。只读读回发现暂存内容大量为 FF。经用户允许，在不与当前固件重叠的 0x3000～0x3FFF 空白区诊断：原速率单页写入失败，降速后同样 128 字节写读一致；两次诊断后都擦除恢复为全 FF 并逐字节核对。修改 CS 输出速度、增加 1 ms 写使能间隔均未单独解决问题；无效的额外间隔已撤销。

有效处理是在当前接线下把 Boot SPI 从 100 MHz/32（3.125 MHz）降为 100 MHz/256（390.625 kHz），随后完整 OTA 通过。这是本次硬件链路的实测配置，不代表 F411 或 W25Q64 的最高工作频率只有 390.625 kHz；具体电气原因仍需波形等证据进一步确认。

实测发现 JDY-31 在串口断开时向 MCU 输出 `+DISC:SUCCESS` 通知，原 Bootloader 会把这类未识别内容当作原始数据写入暂存区。当前没有修改这项协议行为；它不能单独解释本次大面积 FF。

## 芯片迁移必须处理的内容

| 项目 | 原 F103 工程 | 当前 F411 工程 | 对应位置 |
| --- | --- | --- | --- |
| Flash 分区 | Boot 16 KB，App 48 KB，App 起点 0x08004000 | Boot 64 KB，App 448 KB，App 起点 0x08010000 | bootloader.h、两个 Keil 工程的 IROM1 |
| App 擦除 | 1 KB/页，按地址擦除 48 页 | 按扇区擦除 4、5、6、7，共 448 KB | Bootloader_EraseApp |
| RAM 范围 | 20 KB，末端栈顶 0x20005000 | 128 KB，末端栈顶 0x20020000 | Bootloader_JumpToApp、Keil IRAM1 |
| 向量表 | App 设置 VTOR=0x08004000 | App 设置 VTOR=0x08010000，再开启中断 | App main.c 的 USER CODE BEGIN 1 |
| 时钟 | 8 MHz HSE ×9，72 MHz | 25 MHz HSE /25 ×200 /2，100 MHz | SystemClock_Config |
| SPI1 时钟 | APB2 72 MHz /8 = 9 MHz | 初始 100 MHz /32，实测后采用 /256 = 390.625 kHz | SPI 初始化、Boot main.c USER CODE BEGIN 2 |
| EEPROM 引脚 | I2C2：PB10 SCL、PB11 SDA | I2C2：PB10 SCL、PB3 SDA；PB3 使用 AF9 | CubeMX、i2c.c |
| 启动代码和 HAL | Cortex-M3/F1 启动与 HAL | Cortex-M4/F4 启动与 HAL | startup_stm32f411xe.s、F4 驱动及芯片宏 |

VTOR 是 CPU 中记录“去哪里查中断向量表”的寄存器，赋值不会复制 Flash 内容。Boot 跳转目标、App 链接起点、App 的 VTOR 必须一致。

## Flash 写入：接口相似，底层不同

两个版本都可以调用 `HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ...)` 处理 4 字节，因此不需要仅为了 F411 把外层循环改成另一套协议。

工程内的 F1 HAL 在 WORD 模式下分两次写 16 位半字；F4 HAL 的 WORD 路径配置写入宽度后执行 32 位写入。当前 F411 擦除使用芯片 2.7～3.6 V 对应的 `FLASH_VOLTAGE_RANGE_3`，它描述芯片供电，不是 USB 的 5 V。

## 与换芯片无关的改进

- F411 写入函数增加了空指针、地址范围、4 字节对齐和长度检查；这些检查也适用于 F103。
- `memcpy(&word_data, &p_data[i], 4)` 是把 RAM 中连续四个字节复制到一个 uint32_t 变量里，避免直接把源数组强转成 uint32_t 指针带来的对齐等假设；它不是 F411 独有要求。
- 跳转后补 `while (1)` 是当前 ARMCC 编译结果的修正。旧产物在修改 MSP 后从新栈恢复旧现场，导致栈顶增加 16 字节；加循环后的产物是在换 MSP 后直接调用入口，随后停留在循环。此结论只针对本次实际检查过的编译产物，未来更换编译器或优化设置需重新检查。
- Boot 主循环的 LED 改用计时翻转，减少阻塞；不是芯片强制差异。
- 原 F103 的 CS 配置为高速，初始 F411 为低速。本次把 F411 对齐到高速，但单独修改它未解决 CRC 故障。当前还采用了经完整实测通过的 SPI /256 配置。GPIO 输出速度影响信号边沿，SPI 波特率由外设时钟与分频值决定，两者含义不同。为了兼容重新生成，运行期设置位于 main.c 的 USER CODE BEGIN 2，IOC 中也保存了对应配置。

## 沿用的逻辑

UART 单字符命令、128 字节分包 ACK、16 字节元数据布局、EEPROM 两页写入与读回核对、整包 CRC32、W25Q64 暂存、App 的 U/V 处理均保留原思路。

App 内部 Flash 分区虽为 448 KB，当前 OTA 外部暂存限制仍为 16 KB，不能直接发送更大的固件。自动回滚尚未实现。

## 本次改动和后续操作

1. boot_f411/Core/Src/bootloader.c：在换栈调用后补纯 C 死循环，没有添加汇编函数。
2. boot_f411/Core/Src/main.c：在 USER CODE 区域把 PA4 CS 配置为高速输出，并应用 SPI /256 分频。
3. boot_f411/boot_f411.ioc：保存 PA4 高速和 SPI /256 配置。
4. boot_f411/Core/Inc/bootloader.h、main.c：将过期的“尚未实现”注释更新为当前状态。

Bootloader 针对实际修改的编译通过，无错误、无警告。最终 Code=13774、RO=466、RW=80、ZI=2232；Flash 合计 14320 字节，RAM 合计 2312 字节。已下载这个 Bootloader。最终构建后仅更新了说明性注释，没有修改其运行逻辑。F103 工程未修改。

当前板子运行 F411 App 1.0.1，无需为了本次实测再次下载。不要将已有 F103 app.bin 当作 F411 App 升级。F411 的固件为 app_f411/MDK-ARM/app_f411/app_f411.bin。

证据保存在 debug-artifacts/20261031-f411-ota-live：原片内备份、各次串口日志、元数据与片内 App 读回、诊断区恢复核对、编译及 Bootloader 下载日志；最终结果见 verified-result.json。本次仅验证一次成功升级，不包含断电故障注入、自动回滚或长期稳定性测试。
