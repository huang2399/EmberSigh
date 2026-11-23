/* ═══════════════════════════════════════════════
   60-ui.js · 界面渲染与交互绑定
   只读引擎状态，不做任何执行器决策
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;
  var U = ES.utils;
  var $ = function (id) { return document.getElementById(id); };

  var STATE_TEXT = {
    QUIET: { zh: '环境安静', desc: '声景低声播放，香氛持续释放，灯环保持柔和暖光。系统正在守护睡眠。', nav: 'SYSTEM READY' },
    TRANSITION: { zh: '正在确认安静', desc: '噪声已消失，3 秒确认计时中。执行器仍保持退让，避免频繁开关。', nav: 'QUIET CONFIRMING' },
    DISTURBANCE: { zh: '主动退让中', desc: '检测到持续噪声。声景淡出、香氛关闭、灯环转暗红——系统选择不打扰。', nav: 'YIELDING' }
  };

  ES.UI = function (ctx) {
    this.ctx = ctx;
    this.chart = null;
    this.lastRender = 0;
    this.lastChart = 0;
    this.breathing = true;
    this.paused = false;
  };

  ES.UI.prototype.init = function () {
    var self = this;
    var ctx = this.ctx;

    /* ---------- ECharts ---------- */
    if (global.echarts && $('chart')) {
      this.chart = echarts.init($('chart'), null, { renderer: 'canvas' });
      this.chart.setOption({
        animation: false,
        grid: { left: 42, right: 16, top: 16, bottom: 28 },
        tooltip: {
          trigger: 'axis', backgroundColor: 'rgba(255,253,249,.96)', borderColor: 'rgba(47,74,60,.2)',
          textStyle: { color: '#2b2e28', fontSize: 10, fontFamily: 'monospace' },
          axisPointer: { type: 'line', lineStyle: { color: 'rgba(47,74,60,.28)' } }
        },
        xAxis: {
          type: 'category', data: [], boundaryGap: false,
          axisLine: { lineStyle: { color: 'rgba(47,74,60,.22)' } },
          axisTick: { show: false },
          axisLabel: { color: '#9b9d91', fontSize: 9, fontFamily: 'monospace', interval: 19 }
        },
        yAxis: {
          type: 'value', min: 0, max: 300, interval: 50,
          axisLine: { show: false }, axisTick: { show: false },
          axisLabel: { color: '#9b9d91', fontSize: 9, fontFamily: 'monospace' },
          splitLine: { lineStyle: { color: 'rgba(47,74,60,.08)' } }
        },
        series: [
          { name: 'Raw RMS', type: 'line', data: [], showSymbol: false, smooth: 0.2, lineStyle: { width: 1.1, color: '#9aa79c', opacity: .8 }, itemStyle: { color: '#9aa79c' } },
          { name: 'Sliding Avg', type: 'line', data: [], showSymbol: false, smooth: 0.25, lineStyle: { width: 1.5, color: '#c29150' }, itemStyle: { color: '#c29150' } },
          {
            name: 'EMA', type: 'line', data: [], showSymbol: false, smooth: 0.25,
            lineStyle: { width: 2.4, color: '#2f4a3c' }, itemStyle: { color: '#2f4a3c' },
            markLine: {
              silent: true, symbol: 'none', animation: false,
              data: [
                { yAxis: 50, name: '安静阈 50', lineStyle: { color: 'rgba(126,154,131,.75)', type: 'dashed', width: 1 }, label: { formatter: '50 QUIET', color: '#6c8a72', fontSize: 9, position: 'insideEndTop' } },
                { yAxis: 150, name: '噪声阈 150', lineStyle: { color: 'rgba(142,59,50,.7)', type: 'dashed', width: 1 }, label: { formatter: '150 NOISE', color: '#8e3b32', fontSize: 9, position: 'insideEndTop' } }
              ]
            }
          }
        ]
      });
      global.addEventListener('resize', function () { self.chart && self.chart.resize(); });
    }

    /* ---------- 16 颗 LED ---------- */
    var ring = $('ledRing');
    for (var i = 0; i < 16; i++) {
      var led = document.createElement('i');
      var a = (-90 + i * 22.5) * Math.PI / 180;
      led.style.left = (50 + Math.cos(a) * 42) + '%';
      led.style.top = (50 + Math.sin(a) * 42) + '%';
      ring.appendChild(led);
    }

    /* ---------- 波形条 ---------- */
    var wave = $('audioWave');
    for (var k = 0; k < 44; k++) wave.appendChild(document.createElement('i'));

    /* ---------- 事件订阅 ---------- */
    ES.bus.on('log', function (entry) { self.appendLog(entry); });
    ES.bus.on('log:cleared', function () { $('logList').innerHTML = ''; });
    ES.bus.on('environment:change', function (d) {
      var btns = document.querySelectorAll('.scenario');
      for (var i = 0; i < btns.length; i++) btns[i].classList.toggle('active', btns[i].dataset.scenario === d.key);
    });
    ES.bus.on('demo:stage', function (d) {
      var steps = document.querySelectorAll('#demoTimeline div');
      for (var i = 0; i < steps.length; i++) steps[i].classList.toggle('active', i === d.index);
    });
    ES.bus.on('demo:running', function (d) {
      $('btnDemo').textContent = d.running ? '■ 停止演示' : '▶ Demo Mode';
      $('btnDemo').classList.toggle('running', d.running);
      $('btnDemoStart').textContent = d.running ? '停止演示' : '开始路演演示';
    });
    ES.bus.on('demo:stopped', function () {
      var steps = document.querySelectorAll('#demoTimeline div');
      for (var i = 0; i < steps.length; i++) steps[i].classList.remove('active');
    });
    ES.bus.on('state:change', function (d) { self.renderTimeline(); });

    /* ---------- 控件绑定 ---------- */
    function stopDemo() { if (ctx.demo.running) ctx.demo.stop(false); }

    var scenarioBtns = document.querySelectorAll('.scenario');
    for (var b = 0; b < scenarioBtns.length; b++) {
      scenarioBtns[b].addEventListener('click', function () {
        stopDemo();
        var key = this.dataset.scenario;
        if (key === 'custom') ctx.mic.setScenario('custom', { base: +$('rmsSlider').value, duration: +$('durationSlider').value });
        else ctx.mic.setScenario(key);
      });
    }
    $('rmsSlider').addEventListener('input', function () { $('rmsValue').textContent = this.value; });
    $('durationSlider').addEventListener('input', function () { $('durationValue').textContent = (+this.value).toFixed(1) + ' s'; });
    $('btnApplyCustom').addEventListener('click', function () {
      stopDemo();
      ctx.mic.setScenario('custom', { base: +$('rmsSlider').value, duration: +$('durationSlider').value });
    });
    $('btnStopEnv').addEventListener('click', function () { stopDemo(); ctx.mic.setScenario('bedroom'); });

    $('btnApplyParams').addEventListener('click', function () {
      var cfg = ctx.config;
      cfg.alpha = U.clamp(+$('alphaInput').value || 0.2, 0.01, 1);
      cfg.quietThreshold = U.clamp(+$('quietThInput').value || 50, 0, 300);
      cfg.noiseThreshold = U.clamp(+$('noiseThInput').value || 150, 0, 300);
      cfg.quietSeconds = U.clamp(+$('quietSecInput').value || 3, 0.5, 20);
      cfg.disturbanceHold = U.clamp(+$('holdInput').value || 0, 0, 5);
      cfg.windowSize = U.clamp(+$('windowInput').value || 12, 2, 120);
      $('alphaInput').value = cfg.alpha; $('quietThInput').value = cfg.quietThreshold;
      $('noiseThInput').value = cfg.noiseThreshold; $('quietSecInput').value = cfg.quietSeconds;
      $('holdInput').value = cfg.disturbanceHold; $('windowInput').value = cfg.windowSize;
      var ml = self.chart && self.chart.getOption().series[2].markLine.data;
      if (ml) { ml[0].yAxis = cfg.quietThreshold; ml[1].yAxis = cfg.noiseThreshold; self.chart.setOption({ series: [{}, {}, { markLine: { data: ml } }] }); }
      document.querySelector('.formula-box small').textContent =
        'α = ' + cfg.alpha.toFixed(2) + ' · 阈值 ' + cfg.quietThreshold + ' / ' + cfg.noiseThreshold + ' · 安静确认 ' + cfg.quietSeconds + 's';
      ES.logger.info('CFG', 'Parameters updated · α=' + cfg.alpha.toFixed(2) + ' quiet=' + cfg.quietThreshold + ' noise=' + cfg.noiseThreshold + ' hold=' + cfg.disturbanceHold + 's');
    });

    $('audioVolSlider').addEventListener('input', function () {
      ctx.coordinator.setUserVolume(+this.value);
      $('audioVolValue').textContent = this.value + '%';
    });
    $('btnAudioPlay').addEventListener('click', function () {
      var playing = ctx.audio.togglePlay();
      this.textContent = playing ? '暂停播放' : '继续播放';
    });
    $('btnAudioMute').addEventListener('click', function () {
      ctx.audio.toggleMute();
      this.textContent = ctx.audio.muted ? '取消静音' : '静音';
    });
    $('ledBrightness').addEventListener('input', function () {
      ctx.coordinator.setUserBrightness(+this.value);
      $('ledBrightnessValue').textContent = this.value + '%';
    });
    $('btnBreath').addEventListener('click', function () {
      self.breathing = !self.breathing;
      ctx.config.breathing = self.breathing;
      this.textContent = '呼吸效果：' + (self.breathing ? '开' : '关');
      ES.logger.info('LED', 'Breathing effect ' + (self.breathing ? 'enabled' : 'disabled'));
    });

    var faultIds = { faultMic: 'mic', faultAudio: 'audio', faultAroma: 'aroma', faultLight: 'light', faultBattery: 'battery', faultSensor: 'sensor' };
    Object.keys(faultIds).forEach(function (id) {
      $(id).addEventListener('change', function () {
        var key = faultIds[id];
        ES.faults[key] = this.checked;
        ES.faults[key] ? ES.logger.warn('FAULT', 'Injected fault: ' + key) : ES.logger.ok('FAULT', 'Fault cleared: ' + key);
        self.renderFaults();
      });
    });

    $('btnClearLog').addEventListener('click', function () { ES.logger.clear(); });
    $('btnDemo').addEventListener('click', function () { ctx.demo.running ? ctx.demo.stop(false) : ctx.demo.start(); });
    $('btnDemoStart').addEventListener('click', function () { ctx.demo.running ? ctx.demo.stop(false) : ctx.demo.start(); });
    $('btnReset').addEventListener('click', function () { if (ctx.demo.running) ctx.demo.stop(false); ctx.reset(); });
    $('btnPause').addEventListener('click', function () {
      ctx.paused = !ctx.paused;
      this.textContent = ctx.paused ? '继续' : '暂停';
      ES.logger.info('SYS', ctx.paused ? 'Simulation engine paused' : 'Simulation engine resumed');
    });

    this.renderFaults();
    this.renderTimeline();
    this.render(0);
  };

  /* ---------- 日志 ---------- */
  ES.UI.prototype.appendLog = function (entry) {
    var list = $('logList');
    if (!list) return;
    var row = document.createElement('div');
    row.className = 'lv-' + entry.level;
    var t = document.createElement('time'); t.textContent = '[' + entry.time + ']';
    var s = document.createElement('b'); s.textContent = entry.source;
    var m = document.createElement('span'); m.textContent = entry.message;
    row.appendChild(t); row.appendChild(s); row.appendChild(m);
    list.appendChild(row);
    while (list.children.length > 220) list.removeChild(list.firstChild);
    list.scrollTop = list.scrollHeight;
  };

  /* ---------- 状态时间线 ---------- */
  ES.UI.prototype.renderTimeline = function () {
    var host = $('stateTimeline');
    if (!host) return;
    var segs = this.ctx.segments || [];
    var total = 0;
    for (var i = 0; i < segs.length; i++) total += segs[i][2];
    if (total <= 0) total = 1;
    host.innerHTML = '';
    for (var j = 0; j < segs.length; j++) {
      var seg = segs[j];
      var bar = document.createElement('i');
      bar.className = 'seg-' + seg[0].toLowerCase();
      bar.style.width = (seg[2] / total * 100) + '%';
      bar.title = seg[0] + ' · ' + seg[2].toFixed(1) + 's';
      host.appendChild(bar);
    }
  };

  /* ---------- 故障面板 ---------- */
  ES.UI.prototype.renderFaults = function () {
    var list = ES.faults.list();
    var box = $('faultSummary');
    box.textContent = list.length ? ('当前故障：' + list.join(' · ')) : '所有模块正常';
    box.classList.toggle('has-fault', list.length > 0);
    $('audioCard').classList.toggle('fault-active', ES.faults.audio);
    $('aromaCard').classList.toggle('fault-active', ES.faults.aroma);
    $('lightCard').classList.toggle('fault-active', ES.faults.light);
  };

  /* ---------- 图表 ---------- */
  ES.UI.prototype.renderChart = function () {
    if (!this.chart || !this.ctx.history.length) return;
    var h = this.ctx.history;
    var labels = [], raw = [], avg = [], ema = [];
    for (var i = 0; i < h.length; i++) {
      labels.push(h[i].t);
      raw.push(+h[i].raw.toFixed(1));
      avg.push(+h[i].average.toFixed(1));
      ema.push(+h[i].ema.toFixed(1));
    }
    this.chart.setOption({
      xAxis: { data: labels },
      series: [{ data: raw }, { data: avg }, { data: ema }]
    });
  };

  /* ---------- 主渲染 ---------- */
  ES.UI.prototype.render = function (now) {
    var ctx = this.ctx;
    if (now - this.lastRender < 80) return;
    this.lastRender = now;
    var sm = ctx.sm, snap = ctx.signal.snapshot, cfg = ctx.config;
    var state = sm.state, eff = sm.effectiveState();
    var info = STATE_TEXT[state] || STATE_TEXT.QUIET;

    /* 产品与概览 */
    var stage = $('productStage');
    stage.classList.remove('state-quiet', 'state-transition', 'state-disturbance');
    stage.classList.add('state-' + state.toLowerCase());
    $('heroStateChip').textContent = state;
    $('heroStateChip').className = 'hero-state-chip ' + (state === 'DISTURBANCE' ? 'disturbance' : (state === 'TRANSITION' ? 'transition' : ''));
    var pills = document.querySelectorAll('.hs-pill');
    var activeKey = state === 'DISTURBANCE' ? 'noise' : (state === 'TRANSITION' ? 'calming' : 'calm');
    for (var pi = 0; pi < pills.length; pi++) {
      pills[pi].classList.toggle('on', pills[pi].dataset.hs === activeKey);
    }
    $('heroStateName').textContent = info.zh;
    $('heroStateDesc').textContent = info.desc;
    $('heroRms').textContent = Math.round(snap.raw);
    $('heroEma').textContent = snap.ema.toFixed(0);
    $('heroQuiet').textContent = sm.quietTimer.toFixed(1) + 's';
    $('navStatusText').textContent = info.nav;
    $('navStatusPill').classList.toggle('alert', eff === 'DISTURBANCE');

    /* 决策面板 */
    $('mRaw').textContent = snap.raw.toFixed(0);
    $('mAvg').textContent = snap.average.toFixed(0);
    $('mEma').textContent = snap.ema.toFixed(0);
    $('mState').textContent = state;
    var reason = sm.reason;
    if (ctx.mic.invalid) reason = '⚠ 输入无效：' + (ES.faults.mic ? '麦克风失效' : '传感器数据异常') + ' · ' + reason;
    $('decisionReason').textContent = reason;

    /* TFT 屏（产品叠加 + 独立屏幕若存在） */
    var d = new Date();
    var timeStr = U.pad(d.getHours()) + ':' + U.pad(d.getMinutes());
    var tfts = document.querySelectorAll('[data-tft]');
    for (var t = 0; t < tfts.length; t++) {
      var root = tfts[t];
      root.classList.remove('s-quiet', 's-transition', 's-disturbance');
      root.classList.add('s-' + state.toLowerCase());
      root.querySelector('[data-tft-state]').textContent = state;
      root.querySelector('[data-tft-time]').textContent = timeStr;
      root.querySelector('[data-tft-icon]').textContent = state === 'DISTURBANCE' ? '!' : (state === 'TRANSITION' ? '◐' : '☾');
      root.querySelector('[data-tft-mode]').textContent = state === 'DISTURBANCE' ? 'YIELDING' : (state === 'TRANSITION' ? 'CONFIRMING' : 'SLEEP');
      root.querySelector('[data-tft-rms]').textContent = U.pad(Math.round(snap.raw), 3);
      root.querySelector('[data-tft-env]').textContent = snap.ema >= cfg.noiseThreshold ? 'NOISE' : (snap.ema < cfg.quietThreshold ? 'CALM' : 'MID');
      root.querySelector('[data-tft-meter]').style.width = U.clamp(snap.raw / 300 * 100, 3, 100) + '%';
      root.querySelector('[data-tft-audio]').textContent = ES.faults.audio ? 'ERR' : (ctx.audio.muted ? 'MUTE' : (ctx.audio.playing ? Math.round(ctx.audio.effectiveVolume()) + '%' : 'PAUSE'));
      root.querySelector('[data-tft-aroma]').textContent = ES.faults.aroma ? 'ERR' : (ctx.aroma.on ? 'ON' : 'OFF');
      root.querySelector('[data-tft-light]').textContent = ES.faults.light ? 'ERR' : (ctx.light.mode === 'WARM' ? 'WARM' : 'DIM');
    }

    /* 产品画面镜像：把左侧 product-stage 的实时状态推给顶层，右侧展示卡逐帧同步 */
    var tftRoot = tfts[0];
    if (tftRoot && global.parent && global.parent !== global && global.parent.postMessage) {
      try {
        global.parent.postMessage({
          type: 'embersigh:device-mirror',
          state: state.toLowerCase(),
          mist: ctx.aroma.on && !ES.faults.aroma,
          tft: {
            state: state,
            time: timeStr,
            icon: tftRoot.querySelector('[data-tft-icon]').textContent,
            mode: tftRoot.querySelector('[data-tft-mode]').textContent,
            rms: tftRoot.querySelector('[data-tft-rms]').textContent,
            env: tftRoot.querySelector('[data-tft-env]').textContent,
            meter: tftRoot.querySelector('[data-tft-meter]').style.width,
            audio: tftRoot.querySelector('[data-tft-audio]').textContent,
            aroma: tftRoot.querySelector('[data-tft-aroma]').textContent,
            light: tftRoot.querySelector('[data-tft-light]').textContent
          }
        }, '*');
      } catch (e) { /* 顶层不可达时静默降级 */ }
    }

    /* 环境输入 */
    $('envActive').textContent = ctx.mic.name();
    var rem = ctx.mic.remaining();
    $('envRemaining').textContent = isFinite(rem) ? ('剩余 ' + rem.toFixed(1) + 's') : '持续输入';
    $('mistLayer').classList.toggle('on', ctx.aroma.on && !ES.faults.aroma);

    /* 声景 */
    var av = ctx.audio.effectiveVolume();
    $('audioVolValue').textContent = Math.round(ctx.audio.volume) + '%';
    $('audioTarget').textContent = Math.round(ctx.audio.target) + '%';
    var audioBadge = $('audioStatus');
    audioBadge.className = 'act-badge';
    if (ES.faults.audio) { audioBadge.textContent = 'OFFLINE'; audioBadge.classList.add('off'); }
    else if (ctx.audio.muted) { audioBadge.textContent = 'MUTED'; audioBadge.classList.add('fade'); }
    else if (!ctx.audio.playing) { audioBadge.textContent = 'PAUSED'; audioBadge.classList.add('fade'); }
    else if (Math.abs(ctx.audio.volume - ctx.audio.target) > 1) { audioBadge.textContent = 'FADING'; audioBadge.classList.add('fade'); }
    else audioBadge.textContent = 'PLAYING';
    var bars = $('audioWave').children;
    for (var w = 0; w < bars.length; w++) {
      var env = Math.abs(Math.sin(ctx.audio.phase * 1.9 + w * 0.42) * Math.cos(ctx.audio.phase * 0.63 + w * 0.19));
      bars[w].style.height = (av > 0 ? (5 + av / 100 * 42 * (0.28 + 0.72 * env)) : 4).toFixed(1) + 'px';
    }

    /* 香氛 */
    $('gpioLevel').textContent = ctx.aroma.gpio;
    $('gpioLevel').className = ctx.aroma.gpio === 'LOW' ? 'gpio-low' : 'gpio-high';
    $('gpioLabel').textContent = ES.faults.aroma ? '故障 · 通道无响应' : (ctx.aroma.gpio === 'LOW' ? '导通 · 雾化器开启' : '断开 · 雾化器关闭');
    $('aromaRuntime').textContent = U.duration(ctx.aroma.runtime);
    $('aromaStatus').textContent = ES.faults.aroma ? 'FAULT' : (ctx.aroma.on ? 'ON' : 'OFF');
    $('aromaStatus').className = 'act-badge' + (ctx.aroma.on && !ES.faults.aroma ? '' : ' off');
    $('aromaMist').classList.toggle('on', ctx.aroma.on && !ES.faults.aroma);

    /* 灯环 */
    var ledBadge = $('ledStatus');
    ledBadge.textContent = ES.faults.light ? 'FAULT' : (ctx.light.mode === 'WARM' ? 'WARM' : 'DIM');
    ledBadge.className = 'act-badge' + (ctx.light.mode === 'WARM' && !ES.faults.light ? '' : ' off');
    var ledEls = $('ledRing').children;
    for (var l = 0; l < 16; l++) {
      var led = ctx.light.leds[l];
      var a = U.clamp(led.brightness, 0, 1) * (ES.faults.light ? 0.3 : 1);
      ledEls[l].style.background = U.rgb(led.color[0], led.color[1], led.color[2], a);
      ledEls[l].style.boxShadow = a > 0.05 ? U.rgb(led.color[0], led.color[1], led.color[2], a * 0.55) + ' 0 0 10px' : 'none';
    }

    /* 演示面板 */
    var dm = ctx.demo.info();
    $('demoStage').textContent = dm.title;
    $('demoDesc').textContent = dm.desc;
    $('demoCountdown').textContent = dm.running ? (dm.remaining.toFixed(1) + 's LEFT') : 'READY';
    $('demoProgress').style.width = (dm.progress * 100) + '%';
    $('demoReason').textContent = dm.reason;

    /* 图表（低频更新） */
    if (now - this.lastChart > 180) { this.lastChart = now; this.renderChart(); this.renderTimeline(); }
  };
})(window);
