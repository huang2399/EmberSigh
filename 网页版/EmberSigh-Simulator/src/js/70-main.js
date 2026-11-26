/* ═══════════════════════════════════════════════
   70-main.js · 启动与主循环
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES;
  var U = ES.utils;

  var ctx = {
    config: ES.config,
    signal: new ES.SignalProcessor(ES.config),
    sm: new ES.StateMachine(ES.config),
    mic: new ES.VirtualMicrophone(),
    audio: new ES.VirtualSpeaker(),
    aroma: new ES.VirtualAroma(),
    light: new ES.VirtualLight(),
    history: [],
    segments: [],
    paused: false
  };
  ctx.coordinator = new ES.Coordinator(ES.config, ctx.sm, ctx.audio, ctx.aroma, ctx.light);
  ctx.demo = new ES.Demo(ctx, function () { resetAll(); });

  var currentSegment = { state: 'QUIET', start: 0, duration: 0 };
  var historyAcc = 0;

  function openSegment(state, now) {
    currentSegment = { state: state, start: now, duration: 0 };
  }
  function closeSegment(now) {
    currentSegment.duration = (now - currentSegment.start) / 1000;
    ctx.segments.push([currentSegment.state, currentSegment.start, currentSegment.duration]);
    if (ctx.segments.length > 24) ctx.segments.shift();
  }

  ES.bus.on('state:change', function (d) {
    closeSegment(performance.now());
    openSegment(d.to, performance.now());
  });

  function resetAll() {
    ctx.signal.reset();
    ctx.sm.reset();
    ctx.mic.reset();
    ctx.audio.reset();
    ctx.aroma.reset();
    ctx.light.reset();
    ctx.coordinator.reset();
    ctx.demo.reset();
    ctx.paused = false;
    ctx.history.length = 0;
    ctx.segments.length = 0;
    historyAcc = 0;
    ['mic', 'audio', 'aroma', 'light', 'battery', 'sensor'].forEach(function (k) { ES.faults[k] = false; });
    var boxes = document.querySelectorAll('.fault input');
    for (var i = 0; i < boxes.length; i++) boxes[i].checked = false;
    openSegment('QUIET', performance.now());
    if (ctx.ui) ctx.ui.renderFaults();
    ES.logger.ok('SYS', 'System reset · all virtual modules and parameters restored');
  }
  ctx.reset = resetAll;

  function boot() {
    ctx.ui = new ES.UI(ctx);
    ctx.ui.init();
    openSegment('QUIET', performance.now());
    ES.logger.info('SYS', 'EmberSigh virtual HAL initialized · ESP32-S3-EYE (Virtual)');
    ES.logger.info('ENGINE', 'Signal engine ready · EMA α=' + ES.config.alpha.toFixed(2) + ' · thresholds ' + ES.config.quietThreshold + '/' + ES.config.noiseThreshold + ' · quiet hold ' + ES.config.quietSeconds + 's');
    ES.logger.info('HAL', 'Virtual MIC / MAX98357A / MOS / WS2812×16 online');
    ctx.mic.setScenario('bedroom');
    ES.logger.ok('ENV', 'Environment input initialized · quiet bedroom');
    requestAnimationFrame(frame);
  }

  function frame(now) {
    var dt = Math.min(0.1, Math.max(0, (now - (frame.last || now)) / 1000));
    frame.last = now;

    if (!ctx.paused) {
      var raw = ctx.mic.sample(dt);
      if (raw !== null && !isNaN(raw)) {
        var snap = ctx.signal.push(raw);
        ctx.sm.update(snap.ema, dt);
        historyAcc += dt;
        if (historyAcc >= 0.1) {
          historyAcc = 0;
          var d = new Date();
          var stamp = U.pad(d.getMinutes()) + ':' + U.pad(d.getSeconds()) + '.' + String(Math.floor(d.getMilliseconds() / 100));
          ctx.history.push({ t: stamp, raw: snap.raw, average: snap.average, ema: snap.ema });
          if (ctx.history.length > 180) ctx.history.shift();
        }
      }
      ctx.demo.update(dt);
      ctx.coordinator.applyFaults();
      ctx.coordinator.update(dt, ES.config.breathing);
      currentSegment.duration = (now - currentSegment.start) / 1000;
    }

    ctx.ui.render(now);
    requestAnimationFrame(frame);
  }

  /* 暴露只读调试句柄，便于测试与验收 */
  global.EmberSighSim = {
    ctx: ctx,
    reset: resetAll,
    snapshot: function () {
      return {
        state: ctx.sm.state,
        effective: ctx.sm.effectiveState(),
        ema: ctx.signal.snapshot.ema,
        raw: ctx.signal.snapshot.raw,
        volume: ctx.audio.effectiveVolume(),
        aroma: ctx.aroma.on,
        gpio: ctx.aroma.gpio,
        lightMode: ctx.light.mode,
        quietTimer: ctx.sm.quietTimer,
        demoRunning: ctx.demo.running,
        transitions: ctx.sm.transitionCount
      };
    }
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})(window);
