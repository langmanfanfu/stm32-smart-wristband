# STM32F103C8T6 智能健康监测手环（单按键重构 + USB CDC + PC 监控上位机）

基于 **STM32F103C8T6（Blue Pill 最小系统板）** 的智能健康监测手环固件与配套 Windows 桌面监控上位机。  
针对精简硬件（**仅有板载 `RESET` 刷新键、无外部矩阵按键、无蜂鸣器、无 DS1302**）进行了全面重构，实现了单按键多页面交互、OLED 中文显示、纯视觉 LED 报警与跌倒求救、Flash 历史掉电存储，以及免外接模块的 **USB CDC 虚拟串口 + USART1 双通道遥测与双向控制**。

---

## 目录结构

```text
stm32-smart-wristband/
├── firmware/                    # Keil MDK5 固件工程源码（ARMCLANG V6.21，ROM 27.66 KB < 32 KB）
│   ├── Project.uvprojx
│   ├── HARDWARE/                # ADXL345 / ALARM / DS18B20 / FALL / FLASH / LED / MAX30102 / OLED / USB
│   ├── SYSTEM/                  # delay / bkp_state / timer / usart(双通道指令解析)
│   ├── User/                    # main.c / stm32f10x_it.c
│   ├── Start/                   # CMSIS 与启动文件
│   └── Library/                 # STM32F10x 标准外设库
├── pc_ui/                       # 零第三方依赖 Windows 上位机（Python 3 + Tkinter + Win32 API）
│   ├── wristband_monitor.py     # 实时看板、心率曲线、阈值修改、历史导出、一键诊断 USB
│   └── 启动手环上位机.bat        # 便携启动脚本
├── release/
│   └── smart_wristband_stm32f103c8.hex   # 可直接通过 ST-Link 烧录的预编译固件
├── .gitignore
└── README.md
```

---

## 核心功能

1. **体征监测与算法优化**
   - **MAX30102 心率 & 血氧**：100Hz 非阻塞采集与抗干扰平滑滤波；支持**离指检测**，连续 2 秒未检测到有效手指脉搏时自动归零，避免空置时出现虚高心率。
   - **DS18B20 体温采集**：过滤上电初期首次转换前默认值，支持 `0.1 ℃` 分辨率测量与超限判定。
   - **ADXL345 计步 & 跌倒检测**：`50ms` 高频三轴采集计算步数与姿态角；跌倒采用 **`> 2.2g` 撞击 / `< 0.5g` 失重 + 2 秒落地缓冲期 + 4 秒静止确认**状态机，兼顾日常抗误触与演示可复现性。
2. **单 `RESET` 按键全功能交互（BKP 备份域识别）**
   - 利用 STM32 BKP 备份寄存器区分冷上电与物理按键复位（保留步数、页面与运行计时）：
   - **单按 `RESET`**：在 `主页 -> 阈值/报警页 -> 历史记录页` 之间循环切换。
   - **报警 / 求救状态下按 `RESET`**：一键确认消警或取消求救并返回主页。
   - **主页 / 阈值页 1.5 秒内连续双击**：手动触发 SOS 求救。
   - **历史页 1.5 秒内连续双击**：立即保存一条统计快照至内部 Flash。
3. **OLED 中文显示 & 纯视觉报警**
   - 精简 16×16 宋体中文字库，按需整帧刷新无闪屏。
   - 移除声音/蜂鸣器逻辑，改为 **OLED 全屏告警提示 + `PC13` 板载 LED 提醒**（体征超限稳定 2 秒后 500ms 闪烁；跌倒/SOS 150ms 快闪）。
4. **内部 Flash 掉电保存**
   - 第 61 页保存报警阈值配置（默认：心率 `50~120 BPM`、血氧下限 `90%`、体温 `20~40 ℃`）。
   - 第 62~63 页保存历史统计记录，支持每 5 分钟自动存储、按键/指令手动保存、上位机一键导出及清空。
5. **USB CDC + USART1 双通道通信 & PC 上位机**
   - 支持 STM32 片上 **USB CDC 虚拟串口**（`PA11/PA12`，VID `0x0483` / PID `0x5740`，Win10/11 免驱 `usbser.sys`）与 **USART1**（`PA9/PA10`, `115200 8N1`）同步收发。
   - Python 上位机无需 `pip install pyserial`，内置 Win32 API 串口驱动与 `pnputil` 一键 USB 诊断。

---

## 硬件引脚连接

| 外设 / 功能 | STM32F103C8T6 引脚 | 说明 |
| :--- | :--- | :--- |
| OLED 屏幕（软件 I2C） | `PB12` (SCL), `PB13` (SDA) | 128×64 OLED 中文界面 |
| MAX30102 心率血氧 | `PB7` (SCL), `PB8` (SDA), `PB9` (INT) | 软件 I2C + 硬件中断引脚读取 FIFO |
| ADXL345 加速度计 | `PA4` (SCL), `PA5` (SDA) | 计步与跌倒检测 |
| DS18B20 温度传感器 | `PB1` (DQ) | 单总线测温 |
| 状态 / 报警指示灯 | `PC13` | 超限 500ms 闪烁，SOS 150ms 快闪 |
| 物理交互键 | `RESET` | 单按切页/消警，1.5s 双击 SOS/存历史 |
| USB 虚拟串口 | `PA11` (D-), `PA12` (D+) | 板上 Micro-USB 口（需 4 芯数据线） |
| 备用硬件串口 | `PA9` (TX), `PA10` (RX) | USART1 `115200 8N1`，可接 USB-TTL |

---

## 串口 / USB 通信协议

文本行协议（`\r\n` 结尾），USB CDC 与 `USART1 (115200)` 行为一致：

- **设备上行（1 Hz 遥测与事件）**
  - `DATA,HR=<bpm>,SPO2=<pct>,TEMP=<val>,STEP=<n>,ALM=<hex>,SOS=<0|1>,PAGE=<0..2>,T=<HH:MM:SS|UHH:MM:SS>`
  - `EVENT,SOS=MANUAL` / `EVENT,SOS=FALL` / `EVENT,ALM=0x<hex>`
- **上位机下行指令（不区分大小写）**
  - `HELP` / `?`：查看可用指令
  - `PAGE 0` | `PAGE 1` | `PAGE 2`：远程切换手环页面
  - `ACK`：确认消警 / 取消 SOS
  - `SOS`：远程触发手动 SOS
  - `TIME=HH:MM:SS`：同步时间至手环
  - `SAVE` / `DUMP` / `CLR`：立即存一条历史 / 导出全部 Flash 历史 / 清空历史
  - `SET HR_MAX=<v>` / `SET HR_MIN=<v>` / `SET SPO2=<v>` / `SET T_MAX=<v>` / `SET T_MIN=<v>`：修改报警阈值并写入 Flash 保存

---

## 快速使用

### 1. 编译或直接烧录固件
- **方式 A（直接烧录）**：使用 ST-Link 工具将 [`release/smart_wristband_stm32f103c8.hex`](release/smart_wristband_stm32f103c8.hex) 烧录至 STM32F103C8T6。
- **方式 B（Keil MDK 编译）**：打开 `firmware/Project.uvprojx`（使用 ArmClang V6、`STM32F103C8`），全编译（`F7`）并下载（`F8`）。在 MDK-Lite 32KB 限制下当前总 ROM 为 `28,324 B`（`27.66 KB`）。

### 2. 启动 PC 上位机
- 环境要求：Windows 10 / 11 + Python 3.10+（仅使用标准库 `tkinter` / `ctypes` 等，无需安装额外依赖包）。
- 直接双击 [`pc_ui/启动手环上位机.bat`](pc_ui/启动手环上位机.bat)（或运行 `python pc_ui/wristband_monitor.py`）。
- 用**确认可传输数据的 4 芯 Micro-USB 数据线**连接核心板与电脑，在上位机点击「刷新」选择新增 `COM` 口（波特率 `115200`）并点击「连接」。

### 3. 硬件连线排查提示
- 独立 4 线 ST-Link V2（仅 SWD 接口）只能烧录程序，本身不会在电脑产生 COM 口。
- 若使用部分国产 Blue Pill 核心板的 Micro-USB 口时电脑完全无识别反应：请优先排查是否为**纯充电线**；若换线后仍不识别，常见原因是核心板背面 `PA12 (D+)` 上拉电阻 `R10` 为 `10kΩ`（标准应为 `1.5kΩ`），可在 `PA12` 与 `3.3V` 之间并联电阻校准，或直接使用 **USB 转 TTL 模块**连接 `PA9(TX)` / `PA10(RX)` / `GND`（`115200` 波特率）。
