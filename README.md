# EmberSigh · 听眠

> 基于 **ESP32-S3-EYE** 的端侧睡眠环境管家 —— 不佩戴、无摄像头，只用「听」感知环境，
> 在安静与噪音之间学会**退让**，把打扰降到最低。

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4.3-E7352C?logo=espressif&logoColor=white)](https://docs.espressif.com/projects/esp-idf/)
[![Target](https://img.shields.io/badge/Target-ESP32--S3-000000?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Platform](https://img.shields.io/badge/Board-ESP32--S3--EYE-4B5563)](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-eye/user_guide.html)
[![Status](https://img.shields.io/badge/Status-Alpha-F59E0B)]()

---

## 项目简介

EmberSigh（听眠）是一台运行在 **ESP32-S3-EYE** 上的睡眠环境自适应调节设备。

它刻意舍弃了摄像头与可穿戴传感器，只保留麦克风这一种输入方式：实时读取环境音量，
判断房间此刻是「安静」还是「吵闹」，并通过屏幕把状态回馈给用户。当检测到用户翻身、
说话等突发噪声时，设备会主动**退让**——降低自身喇叭音量、暂停香氛释放，避免在
用户最敏感的浅睡阶段制造新的干扰。

核心设计原则只有一条：**懂退让，不打扰。**

## 功能特性

| 能力 | 状态 | 说明 |
| :--- | :---: | :--- |
| 纯听觉感知 | ✅ | 板载 ES8311 Codec 麦克风，I2S 读取后取左声道计算 RMS 音量 |
| 状态可视化 | ✅ | ST7789 240×240 LCD：安静显示蓝色 `Listening...`，噪声显示红色 `Noise!` |
| 阈值联动 | ✅ | RMS 超过阈值即切换状态，带状态去抖，避免屏幕频繁闪烁 |
| 自适应退让 | 🚧 开发中 | 识别到翻身/噪声后自动降低喇叭音量、停止香氛释放 |
| 功放与喇叭 | ⏳ 待硬件 | 外部功放模块尚未接入 |
| 香氛雾化 | ⏳ 待硬件 | MOS 管与雾化器尚未接入 |

## 硬件平台

| 模块 | 型号 / 规格 | 接口 |
| :--- | :--- | :--- |
| 主控 | ESP32-S3-EYE（8 MB Flash / 8 MB Octal PSRAM） | — |
| 音频采集 | ES8311 Codec（板载） | I2S + I2C，由 BSP 初始化 |
| 显示 | ST7789 LCD（240×240） | SPI2，40 MHz |
| 扬声器 | 外部功放 + 喇叭 | 待接入 |
| 香氛 | MOS 管 + 雾化片 | 待接入 |

### LCD 引脚分配

| 信号 | GPIO | | 信号 | GPIO |
| :--- | :---: | --- | :--- | :---: |
| MOSI | 13 | | DC | 5 |
| SCLK | 14 | | RST | 9 |
| CS | 10 | | BL（背光） | 15 |

> 音频、按键、摄像头等外设的引脚由 `espressif/esp32_s3_eye` BSP 统一管理，
> 无需在应用层重复定义。

## 工作原理

```
麦克风 → ES8311 → I2S → 512 样本缓冲 → 左声道 RMS → 阈值判断 → LCD 状态 / 退让动作
```

1. 以 16 kHz 采样率持续读取音频，每次读取 512 个 16 bit 样本（左右声道交错）。
2. 取左声道样本计算均方根（RMS），作为当前环境音量。
3. RMS 大于 `NOISE_THRESHOLD`（默认 `500.0f`）判定为噪声，否则为安静。
4. 状态发生变化时才刷新屏幕，减少 SPI 写入与视觉干扰。

## 快速开始

### 环境准备

- [ESP-IDF v5.4.3](https://docs.espressif.com/projects/esp-idf/en/v5.4.3/esp32s3/get-started/index.html)（含 `idf.py` 与 xtensa-esp32s3 工具链）
- ESP32-S3-EYE 开发板与 USB 数据线
- Windows / macOS / Linux 均可

### 获取源码

```bash
git clone https://github.com/huang2399/EmberSigh.git
cd EmberSigh
```

### 配置与编译

```bash
idf.py set-target esp32s3
idf.py build
```

首次编译会自动从 [ESP Component Registry](https://components.espressif.com/) 拉取
`espressif/esp32_s3_eye` 等依赖，生成的 `managed_components/`、`build/` 与
`sdkconfig` 均已加入 `.gitignore`，无需提交。

### 烧录并查看串口日志

```bash
idf.py -p COM3 flash monitor
```

Windows 下将 `COM3` 替换为实际串口号；退出监视器使用 `Ctrl + ]`。

### 调整灵敏度

修改 `main/pdm_mic_volume.c` 中的阈值后重新编译：

```c
#define NOISE_THRESHOLD 500.0f   // 数值越低越灵敏，按实际房间底噪微调
```

## 目录结构

```
EmberSigh/
├── CMakeLists.txt              # 顶层工程定义（project: pdm_mic_volume）
├── sdkconfig.defaults          # 目标芯片、PSRAM、Flash 等默认配置
├── main/
│   ├── CMakeLists.txt          # 组件注册与依赖声明
│   ├── pdm_mic_volume.c        # 应用入口：音频采集、RMS 计算、LCD 刷新（含 5×7 点阵字库）
│   └── idf_component.yml       # 组件依赖（esp32_s3_eye ^6）
├── dependencies.lock           # 依赖版本锁定，保证构建可复现
└── README.md
```

## 路线图

- [x] 基础工程与 Git 环境搭建
- [x] 攻克 ES8311 音频底层驱动，读取真实音量 RMS
- [x] 打通 ST7789 显示与音量阈值联动
- [ ] 接入外部功放与喇叭
- [ ] 接入 MOS 管与香氛雾化器
- [ ] 实现「翻身检测 → 音量自动下调」的退让策略
- [ ] 加入夜间自动调光与低功耗策略
- [ ] 设计外壳与整机结构

## 已知限制

- 当前仅做**瞬时**音量判断，尚未引入滑动窗口与频域特征，易受偶发噪声影响。
- 退让逻辑依赖尚未接入的功放与香氛硬件，目前只在屏幕上体现状态。
- 烧录后若出现 PSRAM 初始化失败，请确认 `sdkconfig.defaults` 中的 Octal PSRAM
  配置与实物一致。

## 贡献

欢迎提交 Issue 与 Pull Request。提交前请确保：

1. `idf.py build` 在本机可干净通过；
2. 不提交 `build/`、`sdkconfig`、`managed_components/` 等生成物；
3. 提交信息使用「类型: 说明」格式，例如 `feat: 支持滑动窗口均值降噪`。

## 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) 与 [ESP32-S3-EYE BSP](https://components.espressif.com/components/espressif/esp32_s3_eye)
- [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) 音频编解码组件

---

<sub>EmberSigh · 听眠 —— 安静地待在床边，只在必要时退让。</sub>
