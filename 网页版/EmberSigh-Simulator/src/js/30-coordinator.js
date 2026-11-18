/* ═══════════════════════════════════════════════
   30-coordinator.js · 统一执行器协调器
   全系统唯一有权决定声/香/光状态的模块
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;
  var U = ES.utils;

  ES.Coordinator = function (config, stateMachine, audio, aroma, light) {
    this.config = config;
    this.sm = stateMachine;
    this.audio = audio;
    this.aroma = aroma;
    this.light = light;
    this.lastMode = 'WARM';
    this.lastEffective = 'QUIET';
    this.userAromaEnabled = true;
    this.command = {
      audioTarget: config.userVolume,
      aromaOn: true,
      lightMode: 'WARM',
      lightBrightness: config.userBrightness
    };
  };

  ES.Coordinator.prototype.update = function (dt, breathing) {
    var eff = this.sm.effectiveState();
    var cfg = this.config;
    var battery = ES.faults.battery;
    var reason = this.sm.reason || ('effective state ' + eff);

    var target;
    if (eff === 'QUIET') {
      target = {
        audioTarget: cfg.userVolume * (battery ? 0.7 : 1),
        aromaOn: this.userAromaEnabled,
        lightMode: 'WARM',
        lightBrightness: cfg.userBrightness * (battery ? 0.7 : 1)
      };
    } else {
      /* DISTURBANCE 与 TRANSITION 均保持退让，直到 3 秒确认完成 */
      target = {
        audioTarget: 0,
        aromaOn: false,
        lightMode: 'DIM',
        lightBrightness: cfg.userBrightness * (battery ? 0.7 : 1)
      };
    }

    this.command = target;
    this.audio.setTarget(target.audioTarget, reason);
    this.aroma.setCommand(target.aromaOn, reason);

    if (target.lightMode !== this.lastMode) {
      if (target.lightMode === 'DIM') {
        ES.logger.info('LED', 'Light ring entered low-brightness dim-red mode · ' + reason);
      } else {
        ES.logger.ok('LED', 'Light ring returning to warm mode · ' + reason);
      }
      ES.bus.emit('light:mode', { mode: target.lightMode });
      this.lastMode = target.lightMode;
    }
    if (this.lastEffective !== eff) {
      ES.bus.emit('coordinator:state', { effective: eff, state: this.sm.state });
      this.lastEffective = eff;
    }

    this.audio.update(dt);
    this.aroma.update(dt);
    this.light.update(dt, target.lightMode, target.lightBrightness, breathing);
  };

  ES.Coordinator.prototype.setUserVolume = function (v) {
    this.config.userVolume = U.clamp(v, 0, 100);
  };
  ES.Coordinator.prototype.setUserBrightness = function (v) {
    this.config.userBrightness = U.clamp(v, 0, 100);
  };

  ES.Coordinator.prototype.applyFaults = function () {
    if (ES.faults.battery && !this._batteryLogged) {
      ES.logger.warn('PWR', 'Low battery · actuators derated to 70%');
      this._batteryLogged = true;
    }
    if (!ES.faults.battery) this._batteryLogged = false;
  };

  ES.Coordinator.prototype.reset = function () {
    this.lastMode = 'WARM';
    this.lastEffective = 'QUIET';
    this._batteryLogged = false;
    this.command = {
      audioTarget: this.config.userVolume,
      aromaOn: true,
      lightMode: 'WARM',
      lightBrightness: this.config.userBrightness
    };
  };
})(window);
