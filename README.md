# EmberSigh (听眠) — 沉浸式多感官睡眠环境管家

基于 ESP32-S3-EYE 的端侧睡眠环境自适应调节设备。
不佩戴、无摄像头、纯听觉感知，主打"懂退让、不打扰"的轻量级智能睡眠体验。
未来致力于打造"声景 + 香氛"深度融合的沉浸式疗愈空间。

## 💡 项目愿景与核心理念

**沉浸式声景（Soundscape）**：不止于白噪音，未来将引入森林鸟鸣、溪流等真实自然声景。

**动态香氛配方（Aroma Blending）**：突破单一香味，通过多通道雾化控制，根据时间与用户状态，动态混合"森林调/木质调"香氛（如清晨松木+柑橘提神，深夜薰衣草+苔藓安眠）。

**最高级的智能是"退让"**：当识别到用户翻身或环境噪音突增时，设备会主动降低声景音量、暂停香氛释放，待环境恢复安静后平滑恢复。

## 🚀 核心功能实现 (当前版本)

1. **纯听觉感知与 RMS 平滑滤波**：利用板载 ES8311 Codec 麦克风，通过 I2S 读取立体声数据并提取左声道。内置滑动平均滤波 + 迟滞区间防抖动逻辑（安静 < 50，噪音 > 150），有效过滤突发噪音。

2. **产品级三级状态机 UI**：基于 LVGL 构建 240×240 UI。
   - **Calm** (深蓝)：安静环境，脉动光圈缓慢呼吸。
   - **Noise!** (紫红)：噪音触发，光条变红。
   - **Calming...** (橙色过渡)：噪音结束后的 3 秒冷却态。
   - 所有视觉元素加入阻尼平滑处理（EMA）与背景渐变动画，拒绝闪屏。

3. **自适应退让机制 (HAL 预留)**：识别到噪音时，自动执行退让策略：降低白噪音音量、关闭 MOS 管香氛释放、WS2812 灯带切换至防守暗红色。安静 3 秒后，逐步恢复。

## 技术栈

| 层级 | 技术 |
| :--- | :--- |
| 主控 | ESP32-S3 (QFN56) @ 240MHz, 8MB Octal PSRAM |
| 音频感知 | ES8311 Codec (I2C 0x18/0x12) → I2S RX (48kHz, 16bit, 左声道) |
| 音频输出 | MAX98357 I2S 功放 ← I2S TX，纯软件 LFSR 白噪音生成（未来支持 WAV 声景） |
| 显示 | ST7789 240×240 SPI 液晶屏，LVGL v9.6 |
| UI 框架 | LVGL + esp_lvgl_port，阻尼平滑 EMA 动画 |
| 外设控制 | GPIO (MOS 管)、RMT (WS2812 灯带)、LEDC (背光 PWM) |
| 架构设计 | FreeRTOS 多任务并发 (LVGL + 音频循环 + 白噪音任务)，HAL 抽象层预留 |
| 开发环境 | ESP-IDF v5.4.3, CMake/Ninja |

## 引脚分配表

| 功能 | 引脚 | 备注 |
| :--- | :---: | :--- |
| LCD SPI MOSI | GPIO47 | ST7789 数据线 |
| LCD SPI CLK | GPIO21 | — |
| LCD DC / CS | GPIO43 / GPIO44 | — |
| LCD 背光 | GPIO48 | LEDC PWM |
| I2C SDA / SCL | GPIO4 / GPIO5 | ES8311 Codec |
| I2S0 RX (麦克风) | BCLK=41, WS=42, DIN=2 | ES8311 → ESP32 |
| I2S1 TX (Speaker) | BCLK=27, LRC=26, DIN=14 | ESP32 → MAX98357 |
| WS2812 | GPIO17 | RMT，8 个 LED 灯珠 |
| MOS 管 | GPIO1 | 加湿器电源控制 |

> ⚠️ **引脚冲突教训**：GPIO19 是 ESP32-S3 的 USB D-，不可配置为 I2S 等功能引脚，否则会导致 USB 断连陷入下载死锁。GPIO18 与 Camera D5 共用，使用时需注意（建议拔掉摄像头排线）。

## 当前进度

- [x] 基础工程与 Git 环境搭建
- [x] 攻克 ES8311 音频底层驱动与 I2C 死锁问题，成功读取真实音量 RMS
- [x] 打通 LVGL 屏幕高级 UI 动画与音频感知联动
- [x] 实现白噪音生成任务与 HAL 硬件控制接口预留
- [x] 解决 GPIO19 引脚冲突导致的 USB 下载模式死锁（板子已成功复活）
- [ ] 硬件接入：接入外部功放、喇叭、MOS 管与单路香氛雾化器（预计本周完成）
- [ ] V2.0 研发规划：引入真实 WAV 森林声景，开发多通道 PWM 香氛动态混合算法，实现"时间-情绪-气味"矩阵联动

## 快速开始

```bash
# 克隆仓库
git clone https://github.com/huang2399/EmberSigh.git
cd EmberSigh

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录 (如果自动复位失效，先按 BOOT+RST 进入下载模式)
idf.py -p COM3 flash

# 串口监视
idf.py -p COM3 monitor
```

> **注意**：项目依赖 ESP-IDF v5.4+ 及多个 Espressif 托管组件（lvgl、esp_lvgl_port、esp32_s3_eye BSP 等），首次编译会自动下载。