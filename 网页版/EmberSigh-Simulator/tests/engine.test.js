/**
 * 算法引擎单元测试（Node 环境，无需浏览器）
 * 运行: node tests/engine.test.js
 */
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const SRC = path.join(__dirname, '..', 'src', 'js');
const sandbox = { console, Date, Math, setTimeout, clearTimeout };
sandbox.window = sandbox;
vm.createContext(sandbox);
['00-core.js', '10-engine.js'].forEach(function (f) {
  vm.runInContext(fs.readFileSync(path.join(SRC, f), 'utf8'), sandbox, { filename: f });
});
const ES = sandbox.ES;

let passed = 0, failed = 0;
function assert(cond, msg) {
  if (cond) { passed++; console.log('  ✓ ' + msg); }
  else { failed++; console.log('  ✗ ' + msg); }
}
function makeEngine(overrides) {
  const cfg = Object.assign({}, ES.DEFAULT_CONFIG, overrides || {});
  return { cfg, signal: new ES.SignalProcessor(cfg), sm: new ES.StateMachine(cfg) };
}
function run(e, seconds, value, hz = 10) {
  const steps = Math.round(seconds * hz);
  for (let i = 0; i < steps; i++) {
    const s = e.signal.push(value);
    e.sm.update(s.ema, 1 / hz);
  }
}
function st(e) { return e.sm.state + '/eff=' + e.sm.effectiveState() + '/ema=' + e.signal.ema.toFixed(1); }

console.log('\n=== EmberSigh 算法引擎单元测试 ===');

console.log('\n[1] 安静基线与瞬时脉冲不触发');
let e = makeEngine();
run(e, 2, 42);
assert(e.sm.state === 'QUIET', '安静输入保持 QUIET (' + st(e) + ')');
run(e, 0.1, 250);
assert(e.sm.state === 'QUIET', '0.1s 瞬时脉冲不触发退让 (' + st(e) + ')');

console.log('\n[2] 持续噪声进入退让');
e = makeEngine();
run(e, 1, 42);
run(e, 5, 180);
assert(e.sm.state === 'DISTURBANCE', '持续噪声进入 DISTURBANCE (' + st(e) + ')');
assert(e.sm.effectiveState() === 'DISTURBANCE', '有效状态为 DISTURBANCE');

console.log('\n[3] 迟滞区间不新增独立状态、不累计确认');
run(e, 3, 120);
assert(e.sm.effectiveState() === 'DISTURBANCE', '50~150 区间保持退让有效状态 (' + st(e) + ')');
assert(e.sm.quietTimer === 0, '迟滞区间不累计安静确认');

console.log('\n[4] 安静确认计时与噪声重置');
run(e, 2.5, 30);
assert(e.sm.state === 'TRANSITION', '低于安静阈进入 TRANSITION (' + st(e) + ')');
assert(e.sm.quietTimer > 0.8, '安静确认累计 >0.8s，实测 ' + e.sm.quietTimer.toFixed(2) + 's');
run(e, 3, 180);
assert(e.sm.quietTimer === 0, '噪声重新出现时确认计时立即重置');
assert(e.sm.state === 'DISTURBANCE', '重新进入 DISTURBANCE (' + st(e) + ')');

console.log('\n[5] 真实 3 秒安静后恢复');
run(e, 2, 30);
assert(e.sm.state === 'TRANSITION', '2s 安静仍在确认或 EMA 尚未回落 (' + st(e) + ')');
run(e, 2.8, 30);
assert(e.sm.state === 'QUIET', 'EMA 回落且满足 3s 确认后恢复 QUIET (' + st(e) + ')');
assert(e.sm.effectiveState() === 'QUIET', '恢复后有效状态为 QUIET');

console.log('\n[6] EMA 公式正确性');
const p = new ES.SignalProcessor({ alpha: 0.2, windowSize: 4 });
[100, 100, 100, 100].forEach(v => p.push(v));
const before = p.ema;
p.push(200);
const average = (100 + 100 + 100 + 200) / 4;
const expected = 0.2 * average + 0.8 * before;
assert(Math.abs(p.ema - expected) < 0.001, 'EMA = 0.2×' + average + ' + 0.8×' + before + ' = ' + expected + '，实测 ' + p.ema);

console.log('\n[7] 参数热调整生效');
e = makeEngine({ quietThreshold: 60, noiseThreshold: 140, quietSeconds: 1 });
run(e, 1, 42);
run(e, 4, 145);
assert(e.sm.state === 'DISTURBANCE', '新噪声阈 140 生效 (' + st(e) + ')');
run(e, 4, 55);
assert(e.sm.state === 'QUIET', '新安静阈 60 与 1s 确认生效 (' + st(e) + ')');

console.log('\n结果: ' + passed + ' 通过 / ' + failed + ' 失败\n');
process.exit(failed ? 1 : 0);
