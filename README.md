# EmberSigh (听眠) — 桌面睡眠环境管家

基于 ESP32-S3-EYE 的端侧睡眠环境自适应调节设备。
不佩戴、无摄像头、纯听觉感知，主打"懂退让、不打扰"的轻量级智能睡眠体验。

## 核心功能

1. **纯听觉感知与 RMS 平滑滤波**
   利用板载 ES8311 Codec 麦克风，通过 I2S 读取立体声数据并提取左声道，实时计算 RMS 音量。内置**滑动平均滤波 + 迟滞区间防抖动逻辑**（阈值：安静 < 50，噪音 > 150，灰色地带保持状态不变），有效过滤敲击键盘等短促环境噪音，避免误触发。

2. **产品级三级状态机 UI**
   基于 LVGL 构建的 240×240 触控级 UI，包含：
   - **Calm（深蓝）**：安静环境，脉动光圈缓慢呼吸
   - **Noise!（紫红）**：噪音触发，光圈放大，电平条变红
   - **Calming...（橙色过渡）**：噪音结束后连续安静 3 秒的冷却态
   - 所有视觉元素加入**阻尼平滑处理**（EMA 指数移动平均）与**背景色渐变动画**（~800ms 平滑过渡），杜绝闪屏和生硬跳变。

3. **纯软件白噪音生成**
   基于 16-bit LFSR 经典多项式 `(15,14,13,4)` 实时生成白噪音，通过 I2S DMA 输出至 MAX98357 功放。支持 `hal_audio_volume` 全局音量动态调节（0.0 ~ 1.0），噪音触发时自动静音退让。

4. **自适应退让机制（HAL 预留）**
   当识别到环境噪音突增时，自动执行退让策略：降低白噪音音量、关闭 MOS 管香氛释放、WS2812 灯带切换至防守暗红色。待环境恢复安静 3 秒后，逐步恢复所有设备至正常状态。

## 技术栈

| 层级 | 技术 |
|------|------|
| **主控** | ESP32-S3 (QFN56) @ 240MHz, 8MB Octal PSRAM |
| **音频采集** | ES8311 Codec (I2C 地址 0x18/0x12) → I2S0 RX (48kHz, 16-bit, Mono) |
| **音频输出** | MAX98357 I2S 功放 ← I2S1 TX, 纯软件 LFSR 白噪音算法 |
| **显示** | ST7789 240×240 SPI LCD (SPI3, MOSI=GPIO47), LVGL v9.6 |
| **UI 框架** | LVGL v9.6 + esp_lvgl_port, 阻尼平滑 EMA 动画, 背景色渐变插值 |
| **外设控制** | GPIO (MOS 管), RMT (WS2812×8), LEDC (背光 PWM) |
| **架构设计** | FreeRTOS 多任务并发 (LVGL 任务 + 音频循环 + 白噪音任务), HAL 抽象层预留 |
| **开发环境** | ESP-IDF v5.4.3, CMake/Ninja, xtensa-esp-elf-gcc 14.2.0 |

### 引脚分配

| 功能 | 引脚 | 备注 |
|------|------|------|
| LCD SPI MOSI | GPIO47 | ST7789 数据线 |
| LCD SPI CLK | GPIO21 | |
| LCD DC / CS | GPIO43 / GPIO44 | |
| LCD 背光 | GPIO48 | LEDC PWM |
| I2C SDA / SCL | GPIO4 / GPIO5 | ES8311 Codec |
| I2S0 RX (Mic) | BCLK=41, WS=42, DIN=2 | ES8311 → ESP32 |
| I2S1 TX (Spk) | BCLK=27, LRC=26, DIN=14 | ESP32 → MAX98357 |
| WS2812 | GPIO17 | RMT, 8 LEDs |
| MOS 管 | GPIO1 | 加湿器电源控制 |

> ⚠️ **引脚冲突教训**：GPIO19 是 ESP32-S3 的 USB D-，不可配置为 I2S 等功能引脚，否则会导致 USB 断连陷入下载死锁。GPIO18 与 Camera D5 共用，使用时需注意。

## 当前进度

- [x] 基础工程与 Git 环境搭建
- [x] 攻克 ES8311 音频底层驱动与 I2C 死锁问题，成功读取真实音量 RMS
- [x] 打通 LVGL 屏幕高级 UI 动画与音频感知联动
- [x] 实现白噪音生成任务与 HAL 硬件控制接口预留
- [x] 解决 GPIO19 引脚冲突导致的 USB 下载模式死锁（板子已成功复活）
- [ ] 接入外部功放、喇叭、MOS 管与香氛雾化器（硬件待接入）

## 快速开始

```bash
# 克隆仓库
git clone https://github.com/huang2399/EmberSigh.git
cd EmberSigh

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录（如果自动复位失效，先按 BOOT+RST 进入下载模式）
idf.py -p COM3 flash

# 串口监视
idf.py -p COM3 monitor
```

> **注意**：项目依赖 ESP-IDF v5.4+ 及多个 Espressif 托管组件（lvgl、esp_lvgl_port、esp32_s3_eye BSP 等），首次编译会自动下载。