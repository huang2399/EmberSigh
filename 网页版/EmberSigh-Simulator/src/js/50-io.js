/* ═══════════════════════════════════════════════
   50-io.js · 物理联动层（主站串口代理 + 真实麦克风 + 声景）
   A. 串口：不创建第二个 writer，只经主站 window.emberWriter / sendToHardware 下发
   B. 麦克风：getUserMedia + AnalyserNode 实时 RMS，替代模拟场景输入
   C. 声景：Web Audio 合成粉红噪音，GainNode 做状态渐变
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES || {};
  var STATE_CMD = { QUIET: 'CALM\n', TRANSITION: 'CALMING\n', DISTURBANCE: 'NOISE\n' };
  var currentState = 'QUIET';
  var watchers = [];

  function log(level, source, msg) {
    if (ES.logger && typeof ES.logger[level] === 'function') ES.logger[level](source, msg);
    else if (global.console) global.console.log('[' + source + '] ' + msg);
  }
  function notify(state) {
    for (var i = 0; i < watchers.length; i++) {
      try { watchers[i](state); }
      catch (e) { if (global.console) global.console.error('[ES.io] watcher error', e); }
    }
  }
  function watchState(fn) {
    watchers.push(fn);
    try { fn(currentState); } catch (e) { if (global.console) global.console.error('[ES.io] watcher error', e); }
  }
  function commitState(state) {
    if (!STATE_CMD[state] || state === currentState) return;
    currentState = state;
    notify(state);
  }
  if (ES.bus && typeof ES.bus.on === 'function') {
    ES.bus.on('state:change', function (d) { if (d && d.to) commitState(d.to); });
  }
  global.setInterval(function () {
    var api = global.EmberSighSim, sm = api && api.ctx && api.ctx.sm;
    if (sm && sm.state) commitState(sm.state);
  }, 250);

  /* ═══════════════════════════════════════════════
     A. 串口代理：全系统唯一指令出口
     ═══════════════════════════════════════════════ */
  var SerialLink = (function () {
    var connected = false;
    function send(text) {
      var cmd = String(text || '').trim().toUpperCase();
      if (!cmd) return false;
      var sent = false;
      /* 首选：主站统一全局函数（唯一 writer 在主站） */
      try {
        if (global.parent && typeof global.parent.sendToHardware === 'function') {
          global.parent.sendToHardware(cmd);
          sent = true;
        }
      } catch (e) { /* file:// 跨域时退到 postMessage */ }
      if (!sent) {
        try { global.parent.postMessage({ type: 'embersigh:command', command: cmd }, '*'); }
        catch (e) {}
      }
      log('info', 'SERIAL', '→ ESP32 ' + cmd.replace(/\n$/, '') + (sent ? ' · 主站串口' : ' · 消息通道'));
      return sent;
    }
    global.addEventListener('message', function (e) {
      var d = e && e.data;
      if (!d || typeof d !== 'object') return;
      if (d.type === 'embersigh:serial') {
        connected = !!d.connected;
        log(connected ? 'ok' : 'info', 'SERIAL',
          connected ? '主站串口已连接 · 指令通道就绪' : '主站串口已断开 · 指令通道关闭');
      }
      if (d.type === 'embersigh:ack') {
        log('ok', 'HW', '收到硬件物理响应信号 · ' + (d.line || 'ACK'));
      }
    });
    watchState(function (state) { send(STATE_CMD[state]); });
    return { send: send, isConnected: function () { return connected; } };
  })();

  /* ═══════════════════════════════════════════════
     B. 真实麦克风：getUserMedia + AnalyserNode RMS
     ═══════════════════════════════════════════════ */
  var MicInput = (function () {
    var stream = null, ctx = null, analyser = null, samples = null;
    var active = false, starting = false;
    var parentMode = false, parentRms = 0;
    var SCALE = 1600;   /* 归一化 RMS → 0..300 的 UI 量程 */
    var btn = document.getElementById('btnMic');
    var tag = document.getElementById('micState');

    /* 主站推送的实时 RMS（顶层 getUserMedia，iframe 直接申请会被 file:// 拦截） */
    global.addEventListener('message', function (e) {
      var d = e && e.data;
      if (!d || typeof d !== 'object') return;
      if (d.type === 'embersigh:rms' && parentMode) parentRms = Number(d.value) || 0;
    });

    function requestParentMic() {
      return new Promise(function (resolve) {
        var done = false, timer = null;
        function finish(ok) {
          if (done) return;
          done = true;
          global.clearTimeout(timer);
          global.removeEventListener('message', onAck);
          resolve(ok);
        }
        function onAck(e) {
          var d = e && e.data;
          if (!d || d.type !== 'embersigh:mic' || d.action !== 'started') return;
          finish(!!d.ok);
        }
        global.addEventListener('message', onAck);
        try { global.parent.postMessage({ type:'embersigh:mic', action:'start' }, '*'); }
        catch (e) { finish(false); return; }
        timer = global.setTimeout(function () { finish(false); }, 1200);
      });
    }

    function rawRms() {
      if (parentMode) return parentRms;
      if (!analyser || !samples) return 0;
      analyser.getFloatTimeDomainData(samples);
      var sum = 0;
      for (var i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
      return Math.sqrt(sum / samples.length);
    }
    function scaledRms() { return Math.min(300, rawRms() * SCALE); }
    function paint() {
      if (btn) {
        btn.textContent = active ? '关闭真实麦克风' : '启用真实麦克风';
        btn.classList.toggle('connected', active);
      }
      if (tag) tag.textContent = active ? '真实麦克风输入' : '模拟输入';
    }
    function install() {
      if (install.done) return;
      install.done = true;
      var proto = ES.VirtualMicrophone && ES.VirtualMicrophone.prototype;
      if (!proto) return;
      var original = proto.sample;
      proto.sample = function (dt) {
        if (!active) return original.call(this, dt);
        var v = scaledRms();
        this.level = v;
        this.lastValid = v;
        this.invalid = false;
        return v;
      };
    }
    async function startLocal() {
      if (!global.navigator.mediaDevices || !global.navigator.mediaDevices.getUserMedia) {
        throw new Error('当前浏览器不支持 getUserMedia');
      }
      stream = await global.navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false }
      });
      var AC = global.AudioContext || global.webkitAudioContext;
      ctx = new AC();
      var src = ctx.createMediaStreamSource(stream);
      analyser = ctx.createAnalyser();
      analyser.fftSize = 2048;
      samples = new Float32Array(analyser.fftSize);
      src.connect(analyser);   /* 不接 destination，避免扬声器回授 */
      if (ctx.state === 'suspended' && ctx.resume) await ctx.resume();
    }

    async function start() {
      if (active || starting) return;
      starting = true;
      try {
        var embedded = !!(global.parent && global.parent !== global);
        if (embedded && await requestParentMic()) {
          parentMode = true;
          log('ok', 'MIC', '真实麦克风已启用 · 主站采集并推送实时 RMS');
        } else {
          await startLocal();
          log('ok', 'MIC', '真实麦克风已启用 · RMS 由 AnalyserNode 实时计算');
        }
        active = true;
        install();
        paint();
      } catch (err) {
        log('err', 'MIC', '麦克风启动失败 · ' + ((err && err.message) ? err.message : err));
      } finally { starting = false; }
    }
    function stop() {
      if (!active) return;
      if (parentMode) {
        try { global.parent.postMessage({ type:'embersigh:mic', action:'stop' }, '*'); } catch (e) {}
      }
      parentMode = false;
      parentRms = 0;
      active = false;
      try { if (stream) stream.getTracks().forEach(function (t) { t.stop(); }); } catch (e) {}
      try { if (ctx && ctx.close) ctx.close(); } catch (e) {}
      stream = null; ctx = null; analyser = null; samples = null;
      paint();
      log('info', 'MIC', '真实麦克风已关闭 · 恢复模拟场景输入');
    }
    if (btn) btn.addEventListener('click', function () { active ? stop() : start(); });
    paint();
    return {
      start: start, stop: stop, rawRms: rawRms, scaledRms: scaledRms,
      isActive: function () { return active; }
    };
  })();

  /* ═══════════════════════════════════════════════
     B. Web Audio 粉红噪音声景
     ═══════════════════════════════════════════════ */
  var Soundscape = (function () {
    /* 状态 → 目标增益 / 渐变时长（秒） */
    var PROFILE = {
      QUIET: { gain: 0.5, fade: 3.0 },
      TRANSITION: { gain: 0.2, fade: 2.0 },
      DISTURBANCE: { gain: 0.0, fade: 0.2 }
    };

    var ctx = null;
    var master = null;      /* GainNode：唯一的音量控制节点 */
    var highpass = null;
    var source = null;
    var ready = false;
    var booting = false;
    var hint = document.getElementById('sndHint');
    var hideTimer = 0;

    /* -----------------------------------------------------------
       粉红噪音生成函数
       1) Paul Kellet 滤波器组把白噪声整形为 -3dB/oct 粉红频谱
       2) 多生成一段尾音折叠回开头做交叉淡化，循环点无爆音
       ----------------------------------------------------------- */
    function createPinkNoise(audioCtx, seconds) {
      var rate = audioCtx.sampleRate;
      var frames = Math.max(1, Math.floor(rate * seconds));
      var fade = Math.min(Math.floor(rate * 0.35), Math.floor(frames / 4));
      var raw = new Float32Array(frames + fade);

      var b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
      var peak = 0;
      var i, white, pink, abs;

      for (i = 0; i < raw.length; i++) {
        white = Math.random() * 2 - 1;
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11;
        b6 = white * 0.115926;
        raw[i] = pink;
        abs = pink < 0 ? -pink : pink;
        if (abs > peak) peak = abs;
      }

      var norm = peak > 0 ? 0.92 / peak : 1;
      var buffer = audioCtx.createBuffer(1, frames, rate);
      var data = buffer.getChannelData(0);

      for (i = 0; i < frames; i++) {
        var mix = i < fade ? (i / fade) : 1;
        var head = i < fade ? raw[frames + i] : 0;
        data[i] = (raw[i] * mix + head * (1 - mix)) * norm;
      }
      return buffer;
    }

    /* 平滑渐变：从当前值线性推进到目标值 */
    function rampTo(target, seconds) {
      if (!ctx || !master) return;
      var t = ctx.currentTime;
      var param = master.gain;
      var from = param.value;
      param.cancelScheduledValues(t);
      param.setValueAtTime(from, t);
      if (!seconds || seconds <= 0.01) param.setValueAtTime(target, t);
      else param.linearRampToValueAtTime(target, t + seconds);
    }

    function applyState(state) {
      var profile = PROFILE[state] || PROFILE.QUIET;
      rampTo(profile.gain, profile.fade);
    }

    function setHintText(text) {
      var label = hint && hint.querySelector('span');
      if (label) label.textContent = text;
    }

    function paintHint(started) {
      if (!hint) return;
      if (!started) { hint.classList.add('show'); return; }
      hint.classList.add('show', 'live');
      setHintText('环境声景已启动 · 粉红噪音');
      global.clearTimeout(hideTimer);
      hideTimer = global.setTimeout(function () {
        hint.classList.remove('show');
        global.setTimeout(function () { hint.style.display = 'none'; }, 520);
      }, 2600);
    }

    /* 首次用户手势内初始化 AudioContext（浏览器自动播放策略要求） */
    function boot() {
      if (ready || booting) return;
      var AC = global.AudioContext || global.webkitAudioContext;
      if (!AC) {
        log('warn', 'SOUND', 'Web Audio 不可用 · 环境声景无法启动');
        setHintText('当前浏览器不支持 Web Audio');
        return;
      }
      booting = true;
      try {
        ctx = new AC();

        master = ctx.createGain();
        master.gain.value = 0;                 /* 从静音开始，由状态渐变接管 */

        highpass = ctx.createBiquadFilter();   /* 滤掉次声 / 直流漂移 */
        highpass.type = 'highpass';
        highpass.frequency.value = 22;
        highpass.Q.value = 0.7;

        source = ctx.createBufferSource();
        source.buffer = createPinkNoise(ctx, 6);
        source.loop = true;                    /* 6 秒无缝循环，无需任何 mp3 */

        source.connect(highpass);
        highpass.connect(master);
        master.connect(ctx.destination);
        source.start(0);

        if (ctx.state === 'suspended' && typeof ctx.resume === 'function') ctx.resume();

        ready = true;
        booting = false;

        log('ok', 'SOUND', '环境声景已启动 · Web Audio 实时合成粉红噪音（6.0s 无缝循环）');
        applyState(currentState);
        paintHint(true);
      } catch (err) {
        ready = false;
        booting = false;
        if (ctx && ctx.state !== 'closed') { try { ctx.close(); } catch (e) {} }
        ctx = null; master = null; source = null; highpass = null;
        log('warn', 'SOUND', '声景初始化失败 · ' + ((err && err.message) ? err.message : err));
        setHintText('环境声景启动失败 · 请检查浏览器音频权限');
      }
    }

    /* 任意位置首次点击即启动；之后若上下文被挂起，任意点击恢复 */
    document.addEventListener('pointerdown', function () {
      if (!ready) { boot(); return; }
      if (ctx && ctx.state !== 'running' && typeof ctx.resume === 'function') ctx.resume();
    }, true);
    document.addEventListener('keydown', function () {
      if (!ready) boot();
    }, true);

    /* 状态机联动：QUIET→0.5/3s · TRANSITION→0.2/2s · DISTURBANCE→0/0.2s */
    watchState(function (state) { if (ready) applyState(state); });

    return {
      start: boot,
      applyState: applyState,
      rampTo: rampTo,
      createPinkNoise: createPinkNoise,
      isReady: function () { return ready; },
      context: function () { return ctx; },
      level: function () { return master ? master.gain.value : 0; }
    };
  })();

  /* 唯一对外句柄，便于调试与验收 */
  global.EmberSighIO = {
    serial: SerialLink,
    soundscape: Soundscape,
    mic: MicInput,
    stateCommand: STATE_CMD,
    currentState: function () { return currentState; }
  };
})(window);
