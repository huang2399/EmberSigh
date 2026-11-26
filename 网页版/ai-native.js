/* ═══════════════════════════════════════════════════════════════
   EmberSigh 听眠 · AI Native 升级模块逻辑
   独立 IIFE，不修改既有官网脚本。所有能力为概念演示：
   - 本地关键词规则引擎模拟「意图理解 / 参数生成」（非大模型推理）
   - 偏好存于 localStorage（演示数据，预留真实记忆接口）
   - 预留 window.EmberSighAI.setModelAdapter() / setMemoryAdapter()
     —— 未来接入真实模型与持久化记忆时替换适配器即可。
   ═══════════════════════════════════════════════════════════════ */
(function () {
  'use strict';
  var $ = function (id) { return document.getElementById(id); };

  /* ─────────────────────────────────────────────────────────
     1. 适配器接口（预留真实服务接入点）
     ───────────────────────────────────────────────────────── */
  var adapters = {
    // 模型适配器：接收文本与上下文，返回场景配置 Promise
    model: null,
    // 记忆适配器：load()/save()/clear() 偏好
    memory: null
  };
  var API = {
    setModelAdapter: function (fn) { adapters.model = fn; },
    setMemoryAdapter: function (obj) { adapters.memory = obj; },
    getPreference: getPreference,
    adapters: adapters
  };
  window.EmberSighAI = API;

  /* ─────────────────────────────────────────────────────────
     2. 本地偏好存储（演示数据）
     ───────────────────────────────────────────────────────── */
  var PREF_KEY = 'embersigh_ai_demo_pref_v1';
  var DEFAULT_PREF = {
    scene: '雨夜安眠', sound: '森林雨声 · 低音量', aroma: '深夜不开香氛',
    light: '暖色 · 低亮度', feedback: '雨声再小一点更好', recommend: '海浪轻眠'
  };
  function getPreference() {
    if (adapters.memory && typeof adapters.memory.load === 'function') {
      var r = adapters.memory.load(); if (r) return normalize(r);
    }
    try {
      var raw = localStorage.getItem(PREF_KEY);
      return raw ? normalize(JSON.parse(raw)) : Object.assign({}, DEFAULT_PREF);
    } catch (e) { return Object.assign({}, DEFAULT_PREF); }
  }
  function savePreference(p) {
    if (adapters.memory && typeof adapters.memory.save === 'function') { adapters.memory.save(p); return; }
    try { localStorage.setItem(PREF_KEY, JSON.stringify(p)); } catch (e) {}
  }
  function clearPreference() {
    if (adapters.memory && typeof adapters.memory.clear === 'function') { adapters.memory.clear(); return; }
    try { localStorage.removeItem(PREF_KEY); } catch (e) {}
  }
  function normalize(r) {
    var out = {}; Object.keys(DEFAULT_PREF).forEach(function (k) { out[k] = r[k] || DEFAULT_PREF[k]; }); return out;
  }
  function summary(p) { return '你通常偏好' + p.sound + '，' + p.aroma + '。'; }

  /* ─────────────────────────────────────────────────────────
     3. 本地规则意图引擎（概念模拟，非真实 AI）
     ───────────────────────────────────────────────────────── */
  function parseIntent(text) {
    var t = String(text || '');
    var scene = { sound: '森林雨声 · 低音量', soundKey: 'forest_rain', vol: 25,
                  light: '暖色 · 低亮度', lightKey: 'warm_dim', bright: 18,
                  aroma: '关闭', aromaKey: 'off', name: '雨夜安眠' };

    // 声音类型
    if (/海浪|海边|海洋|wave/.test(t)) { scene.sound = '轻柔海浪'; scene.soundKey = 'ocean_wave'; scene.name = '海浪轻眠'; }
    else if (/溪流|小溪|流水|stream/.test(t)) { scene.sound = '山间溪流'; scene.soundKey = 'stream'; scene.name = '溪畔静夜'; }
    else if (/雨|rain/.test(t)) { scene.sound = '森林雨声'; scene.soundKey = 'forest_rain'; }
    else if (/静音|无声|安静点/.test(t)) { scene.sound = '静音'; scene.soundKey = 'silence'; scene.vol = 0; }

    // 音量：小/低 → 更小；大/亮 → 更高
    if (/再小|更小|小一点|小声|低音量|轻一点/.test(t)) scene.vol = 15;
    else if (/大一点|大声|高音量|响一点/.test(t)) scene.vol = 45;

    // 香氛：不要/关闭 → off；薰衣草/松木/柑橘 → 对应
    if (/不要香|关.*香|不开香|无香|不要.*味/.test(t)) { scene.aroma = '关闭'; scene.aromaKey = 'off'; }
    else if (/薰衣草|lavender|助眠香/.test(t)) { scene.aroma = '薰衣草 · 淡'; scene.aromaKey = 'lavender'; }
    else if (/松木|pine|森林味/.test(t)) { scene.aroma = '松木 · 淡'; scene.aromaKey = 'pine'; }
    else if (/柑橘|citrus|橙/.test(t)) { scene.aroma = '柑橘 · 中'; scene.aromaKey = 'citrus'; }

    // 灯光：暗→低亮；亮→高亮；关→off
    if (/灯光暗|灯暗|暗一点|低亮|微光/.test(t)) { scene.light = '暖色 · 低亮度'; scene.lightKey = 'warm_dim'; scene.bright = 18; }
    else if (/灯光亮|亮一点|亮些|中等亮/.test(t)) { scene.light = '中性 · 中等亮度'; scene.lightKey = 'neutral_mid'; scene.bright = 55; }
    else if (/关灯|灯光关|不要灯/.test(t)) { scene.light = '关闭'; scene.lightKey = 'off'; scene.bright = 0; }

    // 情绪基调
    if (/累|疲惫|放松|助眠|安静下来/.test(t)) scene.name = scene.soundKey === 'ocean_wave' ? '海浪轻眠' : scene.name;
    if (/专注|看书|阅读|工作/.test(t)) { scene.name = '专注阅读'; scene.light = '中性 · 中等亮度'; scene.lightKey = 'neutral_mid'; scene.bright = 55; }

    scene.volText = scene.vol === 0 ? '静音' : scene.vol + '%';
    scene.preview = 'SOUND=' + scene.soundKey + ' · VOL=' + scene.vol + ' · LIGHT=' + scene.lightKey + ' · AROMA=' + scene.aromaKey;
    return scene;
  }

  function generateScene(text) {
    if (adapters.model && typeof adapters.model === 'function') {
      return Promise.resolve(adapters.model(text, getPreference())).then(function (r) { return normalizeScene(r); });
    }
    return Promise.resolve(parseIntent(text));
  }
  function normalizeScene(r) {
    return Object.assign(parseIntent(''), r || {});
  }

  /* ─────────────────────────────────────────────────────────
     4. Vision · AI Native Experience 交互
     ───────────────────────────────────────────────────────── */
  var lastScene = null;
  function initExperience() {
    var genBtn = $('aiGenBtn'), prompt = $('aiPrompt');
    if (!genBtn || !prompt) return;

    // 示例按钮
    document.querySelectorAll('.ai-examples button').forEach(function (b) {
      b.addEventListener('click', function () { prompt.value = b.dataset.example || ''; prompt.focus(); });
    });

    genBtn.addEventListener('click', function () {
      var text = prompt.value.trim();
      if (!text) { window.toast && window.toast('请先描述你想要的睡前环境', '✎'); return; }
      genBtn.disabled = true;
      var steps = document.querySelectorAll('#aiSteps .ai-step');
      steps.forEach(function (s) { s.classList.remove('show'); });
      var i = 0;
      var timer = setInterval(function () {
        if (steps[i]) steps[i].classList.add('show');
        i++;
        if (i >= steps.length) clearInterval(timer);
      }, 420);

      setTimeout(function () {
        generateScene(text).then(function (scene) {
          lastScene = scene;
          clearInterval(timer); steps.forEach(function (s) { s.classList.add('show'); });
          renderScene(scene);
          genBtn.disabled = false;
          window.toast && window.toast('概念场景已生成（本地模拟）', '✦');
        });
      }, 1500);
    });

    // 继续调整
    var adjustBtn = $('aiAdjustBtn'), adjustInput = $('aiAdjustInput');
    if (adjustBtn && adjustInput) {
      adjustBtn.addEventListener('click', function () {
        if (!lastScene) { window.toast && window.toast('请先生成一个场景', '✎'); return; }
        var extra = adjustInput.value.trim();
        if (!extra) return;
        generateScene(extra).then(function (delta) {
          lastScene = Object.assign({}, lastScene, delta,
            { volText: delta.volText || lastScene.volText, preview: delta.preview || lastScene.preview });
          renderScene(lastScene);
          adjustInput.value = '';
          window.toast && window.toast('已按你的调整更新（概念演示）', '✦');
        });
      });
    }
  }

  function renderScene(scene) {
    var empty = $('aiResultEmpty'), card = $('aiSceneCard');
    if (!empty || !card) return;
    empty.style.display = 'none'; card.style.display = 'block';
    $('aiSceneTitle').textContent = scene.name || '概念场景';
    $('apSound').textContent = scene.sound + (scene.vol ? ' · ' + (scene.vol <= 20 ? '低音量' : '中音量') : '');
    $('apSoundTag').textContent = scene.volText || (scene.vol + '%');
    $('apLight').textContent = scene.light;
    $('apLightTag').textContent = (scene.bright !== undefined ? scene.bright + '%' : '');
    $('apAroma').textContent = scene.aroma;
    $('apAromaTag').textContent = scene.aromaKey === 'off' ? 'OFF' : 'ON';
    $('aiApplyNote').textContent = '执行任务预览：' + (scene.preview || '') + '　（仅预览，不向硬件下发 · 概念演示）';
  }

  /* ─────────────────────────────────────────────────────────
     5. Vision · Preference Memory 交互
     ───────────────────────────────────────────────────────── */
  function initMemory() {
    var grid = $('aiMemGrid');
    if (!grid) return;
    renderMemory(getPreference());

    grid.querySelectorAll('.ai-mem-item').forEach(function (item) {
      var edit = item.querySelector('.mi-edit');
      var field = item.querySelector('select, input[type=text]');
      if (!edit || !field) return;
      edit.addEventListener('click', function () {
        if (item.classList.contains('editing')) {
          // 保存
          var key = item.dataset.key;
          var p = getPreference(); p[key] = field.value;
          savePreference(p);
          item.classList.remove('editing');
          edit.textContent = '编辑';
          renderMemory(p);
          syncConsoleMemory();
          window.toast && window.toast('偏好已更新（演示数据）', '✓');
        } else {
          item.classList.add('editing');
          edit.textContent = '保存';
        }
      });
    });

    var resetBtn = $('aiMemReset'), clearBtn = $('aiMemClear');
    if (resetBtn) resetBtn.addEventListener('click', function () {
      savePreference(Object.assign({}, DEFAULT_PREF));
      renderMemory(getPreference()); syncConsoleMemory();
      window.toast && window.toast('已恢复演示默认值', '↻');
    });
    if (clearBtn) clearBtn.addEventListener('click', function () {
      clearPreference();
      var empty = {}; Object.keys(DEFAULT_PREF).forEach(function (k) { empty[k] = ''; });
      renderMemory(empty); syncConsoleMemory();
      window.toast && window.toast('已清除全部偏好（演示数据）', '⌫');
    });
  }

  function renderMemory(p) {
    var grid = $('aiMemGrid'); if (!grid) return;
    grid.querySelectorAll('.ai-mem-item').forEach(function (item) {
      var key = item.dataset.key, val = p[key] || '（未设置）';
      var v = item.querySelector('.mi-value'), f = item.querySelector('select, input[type=text]');
      if (v) v.textContent = val;
      if (f) f.value = p[key] || '';
    });
    var sum = $('aiMemSummary');
    if (sum) sum.textContent = (p.sound || p.aroma) ? summary(p) : '尚无偏好数据。可在右侧编辑或恢复演示默认值。';
  }
  function syncConsoleMemory() {
    var el = $('consoleMemSummary'); if (el) el.textContent = summary(getPreference());
  }

  /* ─────────────────────────────────────────────────────────
     6. Console 增强交互
     ───────────────────────────────────────────────────────── */
  function initConsole() {
    // 标签切换
    document.querySelectorAll('.ai-cb-tab').forEach(function (tab) {
      tab.addEventListener('click', function () {
        document.querySelectorAll('.ai-cb-tab').forEach(function (t) { t.classList.toggle('on', t === tab); });
        document.querySelectorAll('.ai-cb-panel').forEach(function (p) { p.classList.toggle('on', p.dataset.panel === tab.dataset.panel); });
      });
    });
    // AI Scene Planner
    var btn = $('consoleAiBtn'), input = $('consoleAiInput'), out = $('consoleAiOut');
    if (btn && input && out) btn.addEventListener('click', function () {
      var text = input.value.trim(); if (!text) { window.toast && window.toast('请输入场景描述', '✎'); return; }
      generateScene(text).then(function (s) {
        out.textContent = '意图：' + s.name + ' ｜ 声音 ' + s.sound + '(' + s.volText + ') ｜ 灯光 ' + s.light + ' ｜ 香氛 ' + s.aroma + '。执行任务预览：' + s.preview + '（概念演示，不向硬件下发）';
        window.toast && window.toast('场景已解析（概念演示）', '✦');
      });
    });
    // Preference Memory 编辑入口
    var edit = $('consoleMemEdit');
    if (edit) edit.addEventListener('click', function () {
      var tab = document.querySelector('.tab[data-view="vision"]');
      if (tab) tab.click();
      setTimeout(function () {
        var el = $('preference'); if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
      }, 180);
    });
    syncConsoleMemory();
  }

  /* ─────────────────────────────────────────────────────────
     7. Runtime 状态桥接：读取主站真实/模拟状态（只读，不改写）
     ───────────────────────────────────────────────────────── */
  function initRuntimeBridge() {
    function refresh() {
      var set = function (id, v) { var e = $(id); if (e && e.textContent !== v) e.textContent = v; };
      // 连接状态来自主站真实串口（只读 DOM）
      var serialText = $('serialText');
      set('rtConn', serialText && /已连接/.test(serialText.textContent) ? '已连接' : '未连接');
      set('rtConnDesc', serialText ? serialText.textContent : 'Web Serial · 未连接');
      set('rtLogState', serialText && /真实数据流/.test(serialText.textContent) ? '硬件数据流' : '本地模拟');
      // 三态与环境（来自主站终端状态，模拟数据）
      var st = $('termState');
      if (st) {
        var m = /STATE\s*·\s*(\w+)/.exec(st.textContent);
        if (m) set('rtState', m[1].toUpperCase());
      }
      var rms = $('ftRms');
      if (rms) set('rtAudio', (parseInt(rms.textContent, 10) || 0) + '%');
      var aroma = $('ftAroma'); if (aroma) set('rtAroma', /OFF|关|切断/i.test(aroma.textContent) ? 'OFF' : 'ON');
      var led = $('ftLed'); if (led) set('rtLight', /b0442f|8,59,50|暗红/i.test(led.textContent) ? '暗红' : '暖光');
    }
    refresh();
    setInterval(refresh, 800);
  }

  /* ─────────────────────────────────────────────────────────
     8. Lab 增强交互
     ───────────────────────────────────────────────────────── */
  function initLab() {
    var btn = $('labAiBtn'), input = $('labAiInput');
    if (btn && input) btn.addEventListener('click', function () {
      var text = input.value.trim(); if (!text) { window.toast && window.toast('请描述你想要的香气', '✎'); return; }
      var t = text, pine = 20, lav = 30, cit = 0, name = '概念配方';
      if (/松|森林|木质|雨/.test(t)) pine = 60;
      if (/薰衣草|助眠|安眠|放松/.test(t)) lav = 55;
      if (/柑橘|橙|清新|明亮/.test(t)) cit = 40;
      if (/不要.*柑橘|无柑橘/.test(t)) cit = 0;
      if (/不要.*松|无松/.test(t)) pine = 0;
      var sum = pine + lav + cit || 1;
      pine = Math.round(pine / sum * 100); lav = Math.round(lav / sum * 100); cit = 100 - pine - lav;
      var combo = $('labComboBar');
      if (combo) combo.innerHTML =
        '<i style="width:' + pine + '%;background:#7d9471"></i>' +
        '<i style="width:' + lav + '%;background:#9d8ec7"></i>' +
        '<i style="width:' + cit + '%;background:#e2a24b"></i>';
      set('labAiName', /雨|森林|松/.test(t) ? '雨后松林' : (/柑橘|橙/.test(t) ? '晨光柑橘' : '宁静薰衣草'));
      set('labComboTxt', '松木 ' + pine + '% · 薰衣草 ' + lav + '% · 柑橘 ' + cit + '%');
      set('labPvSound', /雨|森林/.test(t) ? '森林雨声' : '轻柔海浪');
      set('labPvAroma', pine >= lav ? '松木为主' : '薰衣草为主');
      set('labPvLight', '暖色低亮');
      var empty = $('labAiEmpty'), out = $('labAiOutput');
      if (empty) empty.style.display = 'none';
      if (out) out.style.display = 'block';
      window.toast && window.toast('概念配方已生成（本地模拟）', '✦');
    });

    // 反馈按钮
    document.querySelectorAll('.ai-fb-btn').forEach(function (b) {
      b.addEventListener('click', function () {
        document.querySelectorAll('.ai-fb-btn').forEach(function (x) { x.classList.remove('picked'); });
        b.classList.add('picked');
        var p = getPreference(); p.feedback = '对香氛推荐：' + (b.dataset.fb || '') + '（演示）';
        savePreference(p);
        var s = $('labFbSaved'); if (s) s.classList.add('show');
        window.toast && window.toast('反馈已记录（演示数据）', '✓');
      });
    });
  }
  function set(id, v) { var e = $(id); if (e) e.textContent = v; }

  /* ─────────────────────────────────────────────────────────
     9. 启动
     ───────────────────────────────────────────────────────── */
  function boot() {
    initExperience();
    initMemory();
    initConsole();
    initRuntimeBridge();
    initLab();
    syncConsoleMemory();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
