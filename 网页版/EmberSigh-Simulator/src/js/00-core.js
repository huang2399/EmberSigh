/* ═══════════════════════════════════════════════
   00-core.js · 基础设施：配置 / 事件总线 / 日志 / 工具
   ═══════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var ES = global.ES = global.ES || {};

  /* ---------- 默认参数（与界面双向绑定） ---------- */
  ES.DEFAULT_CONFIG = {
    alpha: 0.2,
    quietThreshold: 50,
    noiseThreshold: 150,
    quietSeconds: 3,
    windowSize: 12,
    disturbanceHold: 0.4,
    userVolume: 62,
    userBrightness: 70,
    breathing: true
  };
  ES.config = Object.assign({}, ES.DEFAULT_CONFIG);

  /* ---------- 工具 ---------- */
  ES.utils = {
    clamp: function (v, a, b) { return v < a ? a : (v > b ? b : v); },
    lerp: function (a, b, t) { return a + (b - a) * t; },
    pad: function (n, w) { return String(Math.floor(n)).padStart(w || 2, '0'); },
    clock: function (d) {
      d = d || new Date();
      return ES.utils.pad(d.getHours()) + ':' + ES.utils.pad(d.getMinutes()) + ':' + ES.utils.pad(d.getSeconds());
    },
    duration: function (s) {
      s = Math.max(0, Math.floor(s));
      return ES.utils.pad(Math.floor(s / 60)) + ':' + ES.utils.pad(s % 60);
    },
    /* 可复现的伪随机源：模拟输入抖动，固定种子便于路演复现 */
    seededRandom: function (seed) {
      var s = seed >>> 0;
      return function () {
        s |= 0; s = (s + 0x6D2B79F5) | 0;
        var t = Math.imul(s ^ (s >>> 15), 1 | s);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
      };
    },
    rgb: function (r, g, b, a) {
      return 'rgba(' + Math.round(r) + ',' + Math.round(g) + ',' + Math.round(b) + ',' + (a === undefined ? 1 : a) + ')';
    },
    mixRgb: function (c1, c2, t) {
      return [
        ES.utils.lerp(c1[0], c2[0], t),
        ES.utils.lerp(c1[1], c2[1], t),
        ES.utils.lerp(c1[2], c2[2], t)
      ];
    }
  };

  /* ---------- 事件总线 ---------- */
  ES.bus = (function () {
    var map = {};
    return {
      on: function (name, fn) {
        (map[name] = map[name] || []).push(fn);
        return function () { ES.bus.off(name, fn); };
      },
      off: function (name, fn) {
        if (!map[name]) return;
        map[name] = map[name].filter(function (f) { return f !== fn; });
      },
      emit: function (name, detail) {
        var list = (map[name] || []).slice();
        for (var i = 0; i < list.length; i++) {
          try { list[i](detail); }
          catch (e) { console.error('[ES.bus] handler error on ' + name, e); }
        }
      }
    };
  })();

  /* ---------- 系统日志（只记录真实发生的事件） ---------- */
  ES.logger = (function () {
    var entries = [];
    function push(level, source, message) {
      var entry = { time: ES.utils.clock(), level: level, source: source, message: message };
      entries.push(entry);
      if (entries.length > 400) entries.shift();
      ES.bus.emit('log', entry);
      return entry;
    }
    return {
      push: push,
      info: function (s, m) { return push('info', s, m); },
      ok: function (s, m) { return push('ok', s, m); },
      warn: function (s, m) { return push('warn', s, m); },
      error: function (s, m) { return push('err', s, m); },
      all: function () { return entries.slice(); },
      clear: function () { entries.length = 0; ES.bus.emit('log:cleared'); }
    };
  })();

  /* ---------- 故障状态（各模块独立读取） ---------- */
  ES.faults = {
    mic: false, audio: false, aroma: false, light: false, battery: false, sensor: false,
    list: function () {
      var names = { mic: '麦克风失效', audio: '音频模块失效', aroma: '香氛模块失效', light: '灯环失效', battery: '低电量', sensor: '传感器数据异常' };
      var out = [];
      Object.keys(names).forEach(function (k) { if (ES.faults[k]) out.push(names[k]); });
      return out;
    }
  };
})(window);
