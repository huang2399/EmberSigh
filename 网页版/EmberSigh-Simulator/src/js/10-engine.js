/* ═══════════════════════════════════════════════
   10-engine.js · 真实算法引擎
   RMS → 滑动平均 → EMA(α) → 迟滞三态状态机
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;
  var U = ES.utils;

  /* ---------------- 信号处理器 ---------------- */
  ES.SignalProcessor = function (config) {
    this.config = config;
    this.buffer = [];
    this.ema = null;
    this.snapshot = { raw: 0, average: 0, ema: 0, peak: 0 };
  };

  ES.SignalProcessor.prototype.push = function (raw) {
    var cfg = this.config;
    raw = U.clamp(raw, 0, 400);
    this.buffer.push(raw);
    var max = Math.max(2, Math.floor(cfg.windowSize));
    if (this.buffer.length > max) this.buffer.shift();

    var sum = 0;
    for (var i = 0; i < this.buffer.length; i++) sum += this.buffer[i];
    var average = sum / this.buffer.length;

    /* 首次采样以滑动平均初始化 EMA，避免从 0 爬升造成误触发 */
    if (this.ema === null) this.ema = average;
    else this.ema = cfg.alpha * average + (1 - cfg.alpha) * this.ema;

    this.snapshot = {
      raw: raw,
      average: average,
      ema: this.ema,
      peak: Math.max.apply(null, this.buffer)
    };
    return this.snapshot;
  };

  ES.SignalProcessor.prototype.reset = function () {
    this.buffer.length = 0;
    this.ema = null;
    this.snapshot = { raw: 0, average: 0, ema: 0, peak: 0 };
  };

  /* ---------------- 迟滞状态机 ---------------- */
  ES.StateMachine = function (config) {
    this.config = config;
    this.state = 'QUIET';          // QUIET | TRANSITION | DISTURBANCE
    this.lastStable = 'QUIET';     // TRANSITION 保留的前一个有效状态
    this.quietTimer = 0;           // 连续安静确认计时（噪声重新出现即重置）
    this.aboveTimer = 0;           // 超过噪声阈值的持续时间
    this.ema = 0;
    this.reason = '等待环境输入';
    this.transitionCount = 0;
  };

  ES.StateMachine.prototype.effectiveState = function () {
    /* TRANSITION 不是独立的新状态，它保留之前的有效状态（此处必为 DISTURBANCE） */
    return this.state === 'TRANSITION' ? this.lastStable : this.state;
  };

  ES.StateMachine.prototype._change = function (next, reason) {
    var prev = this.state;
    if (prev === next) return;
    if (next !== 'TRANSITION') this.lastStable = next;
    this.state = next;
    this.reason = reason;
    this.transitionCount++;
    var detail = { from: prev, to: next, reason: reason, ema: this.ema, at: Date.now() };
    ES.bus.emit('state:change', detail);
    ES.logger.info('STATE', 'State changed: ' + prev + ' → ' + next + ' · ' + reason);
  };

  ES.StateMachine.prototype.update = function (ema, dt) {
    var cfg = this.config;
    this.ema = ema;
    var prevQuietTimer = this.quietTimer;

    if (ema >= cfg.noiseThreshold) {
      /* ── 噪声区：立即重置安静确认，进入需持续保持 ── */
      if (prevQuietTimer > 0.15) {
        ES.logger.warn('TIMER', '安静确认被噪声中断，计时重置 (' + prevQuietTimer.toFixed(1) + 's)');
        ES.bus.emit('quiet:reset', { at: prevQuietTimer });
      }
      this.quietTimer = 0;
      this.aboveTimer += dt;
      this.reason = 'EMA ' + ema.toFixed(1) + ' ≥ 噪声阈 ' + cfg.noiseThreshold + ' · 持续 ' + this.aboveTimer.toFixed(1) + 's';

      if (this.state !== 'DISTURBANCE' && this.aboveTimer >= cfg.disturbanceHold) {
        this._change('DISTURBANCE', 'EMA 越过噪声阈值并持续 ' + cfg.disturbanceHold.toFixed(1) + 's');
        ES.logger.warn('ENGINE', 'EMA crossed disturbance threshold · ' + ema.toFixed(1));
      }
      return;
    }

    this.aboveTimer = 0;

    if (ema < cfg.quietThreshold) {
      /* ── 安静区：开始/继续 3 秒确认 ── */
      if (this.effectiveState() === 'DISTURBANCE') {
        if (this.state !== 'TRANSITION') {
          this._change('TRANSITION', 'EMA 回落至安静阈 ' + cfg.quietThreshold + ' 以下，开始安静确认');
          ES.logger.info('ENGINE', 'Environment became quiet · EMA ' + ema.toFixed(1));
        }
        this.quietTimer += dt;
        this.reason = '安静确认 ' + this.quietTimer.toFixed(1) + ' / ' + cfg.quietSeconds.toFixed(1) + 's';

        if (this.quietTimer >= cfg.quietSeconds) {
          ES.logger.ok('TIMER', 'Quiet duration reached ' + cfg.quietSeconds.toFixed(1) + ' seconds');
          this._change('QUIET', '安静持续 ' + cfg.quietSeconds.toFixed(1) + 's，满足恢复条件');
          ES.bus.emit('recovery:ready', { ema: ema });
          this.quietTimer = 0;
          ES.logger.ok('ENGINE', 'Recovery sequence started');
        } else if (Math.floor(this.quietTimer) !== Math.floor(prevQuietTimer)) {
          ES.logger.info('TIMER', 'Quiet confirmation ' + Math.floor(this.quietTimer) + 's / ' + cfg.quietSeconds + 's');
        }
      } else {
        this.quietTimer = 0;
        this.reason = '环境安静 · EMA ' + ema.toFixed(1) + ' < ' + cfg.quietThreshold;
      }
      return;
    }

    /* ── 阈值之间（50～150）：保持此前有效状态，不新增独立状态 ── */
    if (this.effectiveState() === 'DISTURBANCE') {
      this.quietTimer = 0;
      this.reason = 'EMA ' + ema.toFixed(1) + ' 位于迟滞区间 · 保持退让状态';
    } else {
      this.quietTimer = 0;
      this.reason = 'EMA ' + ema.toFixed(1) + ' 位于迟滞区间 · 保持安静状态';
    }
  };

  ES.StateMachine.prototype.reset = function () {
    this.state = 'QUIET';
    this.lastStable = 'QUIET';
    this.quietTimer = 0;
    this.aboveTimer = 0;
    this.ema = 0;
    this.reason = '等待环境输入';
    this.transitionCount = 0;
  };
})(window);
