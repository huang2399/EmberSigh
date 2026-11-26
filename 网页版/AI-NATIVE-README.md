# EmberSigh 听眠 · AI Native 官网升级说明

本次将官网从「智能环境控制硬件展示站」升级为「AI Native 睡眠环境调节系统品牌官网」，采用**增量升级**，完整保留原有功能与硬件交互。

---

## 一、在线地址与部署

- **官网**：https://huang2399.github.io/EmberSigh/
- **仓库**：https://github.com/huang2399/EmberSigh
- **部署机制**：`.github/workflows/deploy-pages.yml` 在 `main` 分支的 `网页版/**` 发生变化时，自动把 `网页版/index(1).html` 复制为 `网页版/index.html`，并将整个 `网页版/` 目录发布到 GitHub Pages。
- **本地运行**：双击 `网页版/index.html`（或 `index(1).html`）即可，无需构建、无需联网。新增的 `ai-native.css` / `ai-native.js` 与 HTML 同目录，相对路径引用，Pages 与本地均可正确加载。

> 注意：`index.html` 是发布产物，源码以 `index(1).html` 为准。改动源码后本地可用 `cp "网页版/index(1).html" "网页版/index.html"` 同步测试；推送后 workflow 会自动同步。

---

## 二、本次新增文件

| 文件 | 作用 |
| --- | --- |
| `index(1).html` | 主站源码（Hero 升级 + 四个新模块区 + 数据源徽章 + 页脚更新） |
| `ai-native.css` | 新增模块的全部样式（独立文件，不改既有组件） |
| `ai-native.js` | 新增模块的全部交互逻辑（独立 IIFE，挂 `window.EmberSighAI`） |
| `index.html` | 发布入口（由源码复制生成） |

---

## 三、信息架构（保留 4 个主入口）

- **01 Vision｜理念**：新 Hero + AI Native Experience 概念交互 + Preference Memory 概念原型
- **02 Console｜控制台**：新增 AI Scene Planner / Environment Runtime / Preference Memory 三区块，保留原数字孪生 iframe + HAL 日志 + 串口/麦克风
- **03 Lab｜嗅觉实验室**：保留精油调配/时间轴，新增 AI 香氛构思、联合预览、反馈
- **04 Developer｜开发者**：新增五层技术架构与状态标签，保留爆炸图/引脚图/避坑清单/开源卡

---

## 四、能力边界（严格区分 · 重要）

网站用统一的「数据来源徽章」标识每类信息，避免把规划能力误认为已实现：

| 徽章 | 含义 | 对应能力 |
| --- | --- | --- |
| 🟢 真实硬件数据 | Web Serial 实际回传 | 串口 RMS 帧、ACK 回执、连接状态 |
| 🟡 模拟数据 | 数字孪生引擎产生 | RMS/EMA 曲线、三态退让、声/香/光执行器仿真 |
| ⚪ 概念 AI 数据 | 本地规则模拟 | AI 场景生成、偏好记忆、香氛构思、五层架构中的 L1/L2/L5 |

### 已完成（真实硬件/固件已验证）
- ESP32-S3-EYE 主控基础开发
- ES8311 麦克风环境声音采集
- RMS 特征提取、滑动平均、EMA 平滑
- Calm / Noise / Calming 三态判断
- UI 状态显示
- 真实森林 PCM 声景播放
- 网站与数字孪生控制台基础版本
- MOS 香氛单路验证

### 正在开发（联调中）
- 功放与扬声器（MAX98357A + 3070）
- WS2812 灯环
- 多执行器联动

### 未来规划（概念原型演示，非当前能力）
- 自然语言意图理解
- 用户偏好记忆
- AI 场景生成
- Agent 任务编排
- 用户反馈优化
- 多通道香氛

> **关键澄清**：RMS、EMA、迟滞阈值属于**端侧确定性信号处理与控制**，运行于 ESP32-S3-EYE 本地，**不是大模型推理**。AI 场景生成等概念交互使用本地关键词规则模拟，明确标注，未接入任何大模型 API，也未伪造硬件连接成功状态。

---

## 五、概念交互原型说明

### 1. AI Scene Planner（理念页 + 控制台）
- 输入自然语言 → 本地规则引擎解析意图 → 生成声音/香氛/灯光参数 → 输出场景卡片与「执行任务预览」。
- 支持继续调整（如「声音再小一点」）。
- 所有结果标注「概念演示 · 不向硬件下发」。

### 2. Preference Memory（理念页 + 控制台入口）
- 六项偏好：常用场景/声音/香氛/灯光/最近反馈/推荐场景。
- 可逐项编辑、恢复默认、清除，数据存于 `localStorage`（演示数据）。
- 控制台页显示摘要，编辑入口跳转到理念页对应区块。

### 3. 香氛 AI 构思（实验室）
- 自然语言 → 生成概念配方比例与声音/香氛/灯光联合预览。
- 明确区分「单通道已验证」与「多通道规划中」。

---

## 六、未来接入真实服务的接口（已预留）

在 `ai-native.js` 中通过 `window.EmberSighAI` 暴露：

```js
// 接入真实大模型：设置模型适配器
EmberSighAI.setModelAdapter(function (text, preference) {
  return fetch('/api/plan', { method: 'POST', body: JSON.stringify({ text, preference }) })
    .then(function (r) { return r.json(); });
  // 返回 { name, sound, vol, volText, light, bright, aroma, aromaKey, preview }
});

// 接入真实记忆：设置记忆适配器
EmberSighAI.setMemoryAdapter({
  load: function () { /* 返回偏好对象 */ },
  save: function (pref) { /* 持久化 */ },
  clear: function () { /* 清除 */ }
});
```

设置适配器后，场景生成会自动改用真实服务，UI 无需改动。当前默认使用本地规则与 localStorage。

---

## 七、测试结论

- 新增交互端到端测试：**36 / 36 通过**（品牌定位、CSS/JS 加载、四个模块交互、数据源标注、回归检查、零 JS 异常）
- 桌面 1440px 与手机 390px 视觉检查通过，无横向溢出
- 原串口出口 `window.sendToHardware`、Toast、数字孪生 iframe、状态机均正常（无回归）

## 八、备份

改造前的完整网站已备份至：
`C:\Users\27568\Desktop\DreamState\备份\EmberSigh官网_备份_20261126_1913`
（含原 `index(1).html`、`index.html`、`EmberSigh-Simulator/` 等 48 个文件）
