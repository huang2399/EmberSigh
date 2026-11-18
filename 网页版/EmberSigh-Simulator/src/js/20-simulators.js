/* ═══════════════════════════════════════════════
   20-simulators.js · Virtual HAL
   虚拟麦克风 / 功放 / 香氛 MOS / WS2812 灯环
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;
  var U = ES.utils;

  /* ---------------- 虚拟麦克风（场景输入源） ---------------- */
  ES.PROFILES = {
    bedroom:    { name: '安静卧室',     base: 42,  amp: 5,  duration: Infinity, transient: false },
    talk:       { name: '轻声交谈',     base: 96,  amp: 24, duration: 9,        transient: false },
    continuous: { name: '持续环境噪声', base: 168, amp: 16, duration: 14,       transient: false },
    sudden:     { name: '突发噪声',     base: 215, amp: 24, duration: 1.8,      transient: true },
    cough:      { name: '咳嗽',         base: 236, amp: 28, duration: 0.7,      transient: true },
    turnover:   { name: '翻身',         base: 124, amp: 22, duration: 1.4,      transient: true },
    door:       { name: '关门声',       base: 258, amp: 20, duration: 0.9,      transient: true },
    custom:     { name: '自定义噪声',   base: 120, amp: 8,  duration: 6,        transient: false }
  };

  ES.VirtualMicrophone = function () {
    this.rand = U.seededRandom(20260831);
    this.noise = 0;
    this.level = 42;
    this.lastValid = 42;
    this.profile = 'bedroom';
    this.elapsed = 0;
    this.duration = Infinity;
    this.overrideBase = null;
    this.micFailedLogged = false;
    this.sensorFailedLogged = false;
    this.invalid = false;
  };

  ES.VirtualMicrophone.prototype.setScenario = function (key, opts) {
    opts = opts || {};
    var p = ES.PROFILES[key] || ES.PROFILES.bedroom;
    this.profile = key;
    this.elapsed = 0;
    this.duration = opts.duration !== undefined ? opts.duration : p.duration;
    this.overrideBase = opts.base !== undefined ? opts.base : (key === 'custom' ? (opts.base || 120) : null);
    this.noise = 0;
    this.invalid = false;
    this.micFailedLogged = false;
    this.sensorFailedLogged = false;
    var detail = { key: key, name: p.name, base: this.base(), duration: this.duration };
    ES.bus.emit('environment:change', detail);
    ES.logger.info('ENV', 'Environment input: ' + p.name + ' · target RMS ' + detail.base);
    return detail;
  };

  ES.VirtualMicrophone.prototype.base = function () {
    if (this.overrideBase !== null) return this.overrideBase;
    return ES.PROFILES[this.profile].base;
  };

  ES.VirtualMicrophone.prototype.name = function () { return ES.PROFILES[this.profile].name; };

  ES.VirtualMicrophone.prototype.sample = function (dt) {
    var p = ES.PROFILES[this.profile];
    this.elapsed += dt;

    if (isFinite(this.duration) && this.elapsed >= this.duration) {
      ES.logger.info('ENV', 'Scenario "' + p.name + '" ended · falling back to quiet bedroom');
      this.setScenario('bedroom');
    }

    var amp = p.amp;
    var target = this.base() + (this.rand() * 2 - 1) * amp;
    this.noise += (target - this.noise) * U.clamp(dt * 9, 0, 1);

    /* 快攻击、慢释放：模拟真实麦克风包络 */
    var rate = this.noise > this.level ? 14 : 5;
    this.level += (this.noise - this.level) * U.clamp(dt * rate, 0, 1);

    if (ES.faults.mic) {
      this.invalid = true;
      if (!this.micFailedLogged) {
        ES.logger.error('MIC', 'Virtual microphone failure · no valid samples');
        this.micFailedLogged = true;
      }
      return null;
    }
    if (ES.faults.sensor) {
      this.invalid = true;
      if (!this.sensorFailedLogged) {
        ES.logger.error('SENSOR', 'Sensor data abnormal · RMS input frozen at ' + this.lastValid.toFixed(0));
        this.sensorFailedLogged = true;
      }
      return this.lastValid;
    }
    this.invalid = false;
    this.lastValid = this.level;
    return this.level;
  };

  ES.VirtualMicrophone.prototype.remaining = function () {
    return isFinite(this.duration) ? Math.max(0, this.duration - this.elapsed) : Infinity;
  };

  ES.VirtualMicrophone.prototype.reset = function () {
    this.noise = 0; this.level = 42; this.lastValid = 42;
    this.invalid = false; this.micFailedLogged = false; this.sensorFailedLogged = false;
    this.setScenario('bedroom');
  };

  /* ---------------- 虚拟功放 / 喇叭 ---------------- */
  ES.VirtualSpeaker = function () {
    this.volume = 62;
    this.target = 62;
    this.playing = true;
    this.muted = false;
    this.failedLogged = false;
    this.phase = 0;
    this.lastTarget = 62;
  };

  ES.VirtualSpeaker.prototype.setTarget = function (v, reason) {
    v = U.clamp(v, 0, 100);
    if (Math.abs(v - this.target) < 0.5) return;
    this.target = v;
    this.lastTarget = v;
    ES.bus.emit('audio:target', { volume: v, reason: reason });
    if (v < this.volume - 8) ES.logger.info('I2S', 'Audio fade-out started → ' + Math.round(v) + '% · ' + reason);
    if (v > this.volume + 8) ES.logger.info('I2S', 'Audio fade-in started → ' + Math.round(v) + '% · ' + reason);
  };

  ES.VirtualSpeaker.prototype.update = function (dt) {
    var rate = this.target < this.volume ? 38 : 20; /* 退让比恢复更快 */
    var step = rate * dt;
    if (Math.abs(this.volume - this.target) <= step) {
      if (Math.abs(this.volume - this.target) > 0.5) ES.logger.info('I2S', 'Audio volume settled at ' + Math.round(this.target) + '%');
      this.volume = this.target;
    } else {
      this.volume += (this.target > this.volume ? step : -step);
    }
    if (ES.faults.audio) {
      if (!this.failedLogged) { ES.logger.error('I2S', 'MAX98357A virtual module failure · amp offline'); this.failedLogged = true; }
    } else this.failedLogged = false;
    this.phase += dt * (1.4 + this.volume / 45);
    return this.effectiveVolume();
  };

  ES.VirtualSpeaker.prototype.effectiveVolume = function () {
    return ES.faults.audio ? 0 : ((this.playing && !this.muted) ? this.volume : 0);
  };

  ES.VirtualSpeaker.prototype.togglePlay = function () {
    this.playing = !this.playing;
    ES.logger.info('I2S', this.playing ? 'Forest soundscape resumed by user' : 'Forest soundscape paused by user');
    return this.playing;
  };

  ES.VirtualSpeaker.prototype.toggleMute = function () {
    this.muted = !this.muted;
    ES.logger.info('I2S', this.muted ? 'Audio muted by user' : 'Audio unmuted by user');
    return this.muted;
  };

  ES.VirtualSpeaker.prototype.reset = function () {
    this.volume = 62; this.target = 62; this.playing = true; this.muted = false;
    this.phase = 0; this.failedLogged = false;
  };

  /* ---------------- 虚拟香氛 MOS ---------------- */
  ES.VirtualAroma = function () {
    this.on = false;
    this.gpio = 'HIGH';
    this.runtime = 0;
    this.offTime = 99;
    this.failedLogged = false;
    this.safetyLogged = false;
    this.minOffTime = 2.0;
  };

  ES.VirtualAroma.prototype.setCommand = function (wantOn, reason) {
    if (this.failed) return;
    if (wantOn && !this.on) {
      if (this.offTime < this.minOffTime) {
        if (!this.safetyLogged) {
          ES.logger.warn('MOS', '安全策略：雾化器最小停机间隔 ' + this.minOffTime.toFixed(1) + 's 未满，延迟恢复');
          this.safetyLogged = true;
        }
        return;
      }
      this.on = true;
      this.gpio = 'LOW';
      this.safetyLogged = false;
      ES.bus.emit('aroma:change', { on: true, gpio: 'LOW' });
      ES.logger.ok('MOS', 'GPIO LOW · MOS conducting · Aroma actuator switched ON · ' + reason);
    } else if (!wantOn && this.on) {
      this.on = false;
      this.gpio = 'HIGH';
      this.offTime = 0;
      this.safetyLogged = false;
      ES.bus.emit('aroma:change', { on: false, gpio: 'HIGH' });
      ES.logger.warn('MOS', 'GPIO HIGH · MOS open · Aroma actuator switched OFF · ' + reason);
    }
  };

  ES.VirtualAroma.prototype.update = function (dt) {
    this.failed = !!ES.faults.aroma;
    if (this.failed) {
      this.on = false;
      this.gpio = 'HIGH';
      if (!this.failedLogged) { ES.logger.error('MOS', 'Aromatherapy module failure · GPIO forced HIGH'); this.failedLogged = true; }
    } else this.failedLogged = false;

    if (this.on) this.runtime += dt;
    else this.offTime += dt;
  };

  ES.VirtualAroma.prototype.reset = function () {
    this.on = false; this.gpio = 'HIGH'; this.runtime = 0; this.offTime = 99;
    this.failedLogged = false; this.safetyLogged = false;
  };

  /* ---------------- 虚拟 WS2812 灯环 ---------------- */
  var WARM = [232, 186, 118];
  var DIM_RED = [142, 59, 50];
  var GRAY = [120, 124, 116];

  ES.VirtualLight = function () {
    this.leds = [];
    for (var i = 0; i < 16; i++) {
      this.leds.push({ color: WARM.slice(), level: 0.7, t: 1, brightness: 0.7 });
    }
    this.time = 0;
    this.mode = 'WARM';
    this.failedLogged = false;
  };

  ES.VirtualLight.prototype.update = function (dt, mode, brightness, breathing) {
    this.time += dt;
    this.mode = mode;
    var failed = !!ES.faults.light;
    if (failed && !this.failedLogged) { ES.logger.error('LED', 'WS2812 ring failure · all LEDs offline'); this.failedLogged = true; }
    if (!failed) this.failedLogged = false;

    var targetColor = failed ? GRAY : (mode === 'WARM' ? WARM : DIM_RED);
    var baseLevel = failed ? 0.06 : U.clamp(brightness / 100, 0, 1) * (mode === 'WARM' ? 1 : 0.34);
    var pulse = (breathing && !failed) ? 0.88 + 0.12 * Math.sin(this.time * 1.7) : 1;

    for (var i = 0; i < this.leds.length; i++) {
      var led = this.leds[i];
      var lag = 0.55 + i * 0.035; /* 逐颗过渡，避免整环跳变 */
      led.t += U.clamp(dt / lag, 0, 1) * (1 - led.t);
      led.color = U.mixRgb(led.color, targetColor, U.clamp(dt / lag, 0, 1));
      var wave = failed ? 1 : (0.94 + 0.06 * Math.sin(this.time * 1.1 + i * 0.62));
      led.level = U.lerp(led.level, baseLevel * pulse * wave, U.clamp(dt / lag, 0, 1));
      led.brightness = led.level;
    }
    return this.leds;
  };

  ES.VirtualLight.prototype.reset = function () {
    this.time = 0;
    this.mode = 'WARM';
    for (var i = 0; i < 16; i++) {
      this.leds[i] = { color: WARM.slice(), level: 0.7, t: 1, brightness: 0.7 };
    }
    this.failedLogged = false;
  };
})(window);
