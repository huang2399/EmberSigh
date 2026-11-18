/**
 * 浏览器端到端测试（Chrome DevTools Protocol，零第三方依赖）
 * 运行: node tests/e2e.js
 * 需本机已安装 Chrome（自动查找常见路径）。
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { pathToFileURL } = require('url');

const ROOT = path.join(__dirname, '..');
const HTML = path.join(ROOT, 'EmberSigh-全功能数字孪生模拟器.html');
const PORT = 9333;
const CHROME_CANDIDATES = [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
];
const chrome = CHROME_CANDIDATES.find(p => fs.existsSync(p));
if (!chrome) { console.error('未找到 Chrome/Edge'); process.exit(2); }

const profile = path.join(os.tmpdir(), 'embersigh-cdp-profile-' + Date.now());
const child = spawn(chrome, [
  '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
  '--window-size=1440,1000', '--remote-debugging-port=' + PORT,
  '--user-data-dir=' + profile, 'about:blank'
], { stdio: 'ignore' });

let passed = 0, failed = 0;
const errors = [];
function assert(cond, msg) {
  if (cond) { passed++; console.log('  ✓ ' + msg); }
  else { failed++; console.log('  ✗ ' + msg); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function getTarget() {
  for (let i = 0; i < 50; i++) {
    try {
      const r = await fetch('http://127.0.0.1:' + PORT + '/json/list');
      const list = await r.json();
      const page = list.find(t => t.type === 'page');
      if (page) return page;
    } catch (e) { /* waiting */ }
    await sleep(200);
  }
  throw new Error('CDP 未就绪');
}

(async function main() {
  const target = await getTarget();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let seq = 0;
  const pending = new Map();
  ws.onmessage = function (ev) {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
    if (m.method === 'Runtime.exceptionThrown') {
      errors.push('exception: ' + (m.params.exceptionDetails.text || '') + ' ' + (m.params.exceptionDetails.exception?.description || ''));
    }
    if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') {
      errors.push('console: ' + m.params.entry.text);
    }
  };
  function send(method, params) {
    return new Promise(resolve => {
      const id = ++seq;
      pending.set(id, resolve);
      ws.send(JSON.stringify({ id, method, params: params || {} }));
    });
  }
  async function evaluate(expression) {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.text);
    return r.result?.result?.value;
  }
  const snap = () => evaluate('JSON.stringify(EmberSighSim.snapshot())').then(JSON.parse);
  const click = sel => evaluate('document.querySelector(' + JSON.stringify(sel) + ').click()');

  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: pathToFileURL(HTML).href });
  for (let i = 0; i < 60; i++) {
    if (await evaluate('!!window.EmberSighSim && !!window.EmberSighSim.ctx.ui')) break;
    await sleep(200);
  }
  await sleep(1200);

  console.log('\n=== EmberSigh 浏览器端到端测试 ===');

  console.log('\n[1] 启动与初始状态');
  let s = await snap();
  assert(s.state === 'QUIET', '初始状态 QUIET');
  assert(s.raw > 20 && s.raw < 80, '模拟麦克风产生安静 RMS，实测 ' + s.raw.toFixed(1));
  assert(await evaluate('document.querySelectorAll("#ledRing i").length') === 16, '灯环为 16 颗独立 LED');
  assert(await evaluate('!!document.querySelector("#chart canvas")'), 'ECharts 图表已渲染');
  assert(await evaluate('document.querySelectorAll("#logList div").length') > 0, '系统日志有真实事件');

  console.log('\n[2] 持续噪声 → 退让');
  await click('[data-scenario="continuous"]');
  await sleep(3500);
  s = await snap();
  assert(s.state === 'DISTURBANCE', '状态进入 DISTURBANCE，实测 ' + s.state);
  assert(s.volume <= 2, '声景音量平滑降至 0，实测 ' + s.volume.toFixed(1));
  assert(s.aroma === false && s.gpio === 'HIGH', '香氛 MOS 断开 (GPIO HIGH)');
  assert(s.lightMode === 'DIM', '灯环进入暗红低亮模式');

  console.log('\n[3] 噪声消失 → 确认 → 恢复');
  await click('[data-scenario="bedroom"]');
  await sleep(900);
  s = await snap();
  assert(s.state === 'TRANSITION' || s.state === 'DISTURBANCE', '尚未满足 3s 时保持退让有效状态');
  await sleep(6000);
  s = await snap();
  assert(s.state === 'QUIET', '安静确认后恢复 QUIET，实测 ' + s.state);
  assert(s.volume > 40, '声景音量正在恢复，实测 ' + s.volume.toFixed(1));
  assert(s.aroma === true && s.gpio === 'LOW', '香氛按安全策略恢复，GPIO LOW');
  assert(s.lightMode === 'WARM', '灯环恢复暖光');

  console.log('\n[4] 故障注入隔离');
  await click('#faultMic');
  const before = (await snap()).raw;
  await sleep(1200);
  const after = (await snap()).raw;
  assert(Math.abs(before - after) < 0.01, '麦克风失效后 RMS 输入停止更新');
  assert((await snap()).lightMode === 'WARM', '麦克风失效不影响灯环等正常模块');
  await click('#faultMic');
  await click('#faultAudio');
  await sleep(300);
  assert((await snap()).volume === 0, '音频模块失效后音量输出为 0');
  await click('#faultAudio');

  console.log('\n[5] 路演 Demo Mode（真实算法驱动）');
  const demoInfo = () => evaluate('JSON.stringify(EmberSighSim.ctx.demo.info())').then(JSON.parse);
  async function waitDemo(seconds) {
    for (let i = 0; i < 200; i++) {
      const inf = await demoInfo();
      if (!inf.running || inf.elapsed >= seconds) return inf;
      await sleep(150);
    }
    return demoInfo();
  }
  await click('#btnDemo');
  let d = await waitDemo(2.5);
  assert(d.running && d.stage === 0, 'Demo 启动并进入第 1 阶段');
  d = await waitDemo(10.5);
  s = await snap();
  assert(d.stage === 1 && s.state === 'QUIET', '06–12s 说话声未越过阈值，保持 QUIET（实测 ' + s.state + '）');
  d = await waitDemo(14.5);
  s = await snap();
  assert(d.stage === 2 && s.state === 'DISTURBANCE', '12–18s 触发 DISTURBANCE（实测 ' + s.state + '）');
  d = await waitDemo(19.5);
  s = await snap();
  assert(d.stage === 3 && ['TRANSITION', 'DISTURBANCE'].includes(s.state), '18–24s 噪声消失，保持退让并开始确认（实测 ' + s.state + '）');
  d = await waitDemo(26.5);
  assert(d.running && d.stage === 4, '24–30s 进入平滑恢复阶段');
  d = await waitDemo(31);
  assert(!d.running, 'Demo 在 30s 后自动结束');
  const logText = await evaluate('document.getElementById("logList").innerText');
  assert(logText.includes('Recovery sequence started'), '日志含真实恢复事件');
  assert(logText.includes('State changed: QUIET → DISTURBANCE'), '日志含真实状态切换事件');

  console.log('\n[6] 重置');
  await click('#btnReset');
  await sleep(600);
  s = await snap();
  assert(s.state === 'QUIET' && s.raw > 20 && s.raw < 80, '重置后回到安静基线');
  assert(s.gpio === 'LOW' && s.lightMode === 'WARM' && s.volume > 40, '三个执行器恢复初始状态');
  assert(await evaluate('document.querySelectorAll(".fault input:checked").length') === 0, '故障开关全部复位');

  console.log('\n[7] 运行错误');
  assert(errors.length === 0, '无 JS 异常 / 控制台错误' + (errors.length ? '：' + errors.slice(0, 3).join(' | ') : ''));

  console.log('\n结果: ' + passed + ' 通过 / ' + failed + ' 失败\n');
  ws.close();
  child.kill();
  try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { /* ignore */ }
  process.exit(failed ? 1 : 0);
})().catch(err => {
  console.error('E2E 失败:', err);
  child.kill();
  process.exit(2);
});
