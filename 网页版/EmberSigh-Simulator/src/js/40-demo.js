/* ═══════════════════════════════════════════════
   40-demo.js · 路演 Demo Mode
   30 秒脚本只切换环境输入，其余全部由真实算法与协调器完成
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;

  var STAGES = [
    {
      start: 0, end: 6, title: '01 · 安静卧室',
      desc: '环境安静。森林声景低声播放，香氛 MOS 导通，灯环保持柔和暖光。',
      run: function (ctx) { ctx.mic.setScenario('bedroom'); }
    },
    {
      start: 6, end: 12, title: '02 · 说话声进入',
      desc: '模拟轻声交谈，RMS 上升到 96 左右。滑动平均与 EMA 开始抬升，但未越过噪声阈值，系统保持安静。',
      run: function (ctx) { ctx.mic.setScenario('talk', { duration: 6 }); }
    },
    {
      start: 12, end: 18, title: '03 · 触发退让',
      desc: '持续环境噪声使 EMA 越过 150 阈值。状态机切入 DISTURBANCE：声景淡出、香氛关闭、灯环转暗红。全程无提示音。',
      run: function (ctx) { ctx.mic.setScenario('continuous', { duration: 6 }); }
    },
    {
      start: 18, end: 24, title: '04 · 安静确认',
      desc: '噪声消失，RMS 回落。EMA 低于 50 后启动 3 秒安静确认计时；此阶段三个执行器仍保持退让。',
      run: function (ctx) { ctx.mic.setScenario('bedroom'); }
    },
    {
      start: 24, end: 30, title: '05 · 平滑恢复',
      desc: '满足真实 3 秒确认条件后状态机切回 QUIET。声景渐入、香氛按安全策略恢复、灯环回到暖光。恢复时刻由算法决定。',
      run: function () { /* 不干预：让算法自然完成恢复 */ }
    }
  ];

  ES.Demo = function (ctx, resetFn) {
    this.ctx = ctx;
    this.resetFn = resetFn;
    this.running = false;
    this.elapsed = 0;
    this.stage = -1;
  };

  ES.Demo.prototype.start = function () {
    this.resetFn();
    /* 演示必须真正跑起来：若引擎处于暂停态则自动恢复 */
    if (this.ctx.paused) { this.ctx.paused = false; }
    this.running = true;
    this.elapsed = 0;
    this.stage = -1;
    ES.logger.info('DEMO', 'Roadshow Demo Mode started · 30s script');
    ES.bus.emit('demo:running', { running: true });
  };

  ES.Demo.prototype.stop = function (completed) {
    if (!this.running) return;
    this.running = false;
    ES.bus.emit('demo:running', { running: false });
    if (completed) {
      ES.logger.ok('DEMO', 'Roadshow Demo Mode completed · ' + this.ctx.sm.transitionCount + ' state transitions observed');
    } else {
      ES.logger.warn('DEMO', 'Roadshow Demo Mode stopped early at ' + this.elapsed.toFixed(1) + 's');
    }
    ES.bus.emit('demo:stopped', { completed: !!completed });
  };

  ES.Demo.prototype.update = function (dt) {
    if (!this.running) return;
    this.elapsed += dt;
    var idx = -1;
    for (var i = 0; i < STAGES.length; i++) {
      if (this.elapsed >= STAGES[i].start && this.elapsed < STAGES[i].end) { idx = i; break; }
    }
    if (idx < 0 && this.elapsed >= 30) idx = STAGES.length - 1;
    if (idx >= 0 && idx !== this.stage) {
      this.stage = idx;
      STAGES[idx].run(this.ctx);
      ES.logger.info('DEMO', 'Stage ' + (idx + 1) + '/5 · ' + STAGES[idx].title);
      ES.bus.emit('demo:stage', { index: idx, stage: STAGES[idx] });
    }
    if (this.elapsed >= 30) this.stop(true);
  };

  /* 重置：顶部「重置」必须把演示进度条也归零 */
  ES.Demo.prototype.reset = function () {
    this.running = false;
    this.elapsed = 0;
    this.stage = -1;
    ES.bus.emit('demo:running', { running: false });
    ES.bus.emit('demo:stopped', { completed: false });
  };

  ES.Demo.prototype.info = function () {
    var ctx = this.ctx;
    var s = STAGES[Math.max(0, this.stage)] || STAGES[0];
    var reason = 'EMA ' + (ctx.sm.ema || 0).toFixed(1) +
      ' · 状态 ' + ctx.sm.state +
      ' · 安静确认 ' + ctx.sm.quietTimer.toFixed(1) + 's' +
      ' · 音量 ' + Math.round(ctx.audio.effectiveVolume()) + '%' +
      ' · 香氛 ' + (ctx.aroma.on ? 'ON' : 'OFF') +
      ' · 灯环 ' + (ctx.light.mode === 'WARM' ? '暖光' : '暗红');
    return {
      running: this.running,
      elapsed: this.elapsed,
      remaining: Math.max(0, 30 - this.elapsed),
      progress: Math.min(1, this.elapsed / 30),
      stage: this.stage,
      title: this.running ? s.title : '待机',
      desc: this.running ? s.desc : '点击「开始路演演示」运行完整 30 秒脚本。演示严格遵循真实算法与 3 秒安静确认逻辑。',
      reason: this.running ? ('判断依据：' + reason) : '判断依据：—'
    };
  };

  ES.Demo.STAGES = STAGES;
})(window);
