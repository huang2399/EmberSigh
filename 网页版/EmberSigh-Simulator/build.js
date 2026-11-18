/**
 * EmberSigh 模拟器构建脚本
 * 用法: node build.js
 * 产出: 单文件 HTML（内联 CSS / ECharts / 全部模块 JS / 产品图 base64）
 * 无第三方构建依赖，零网络运行。
 */
const fs = require('fs');
const path = require('path');

const ROOT = __dirname;
const SRC = path.join(ROOT, 'src');
const OUT_LOCAL = path.join(ROOT, 'EmberSigh-全功能数字孪生模拟器.html');
const OUT_PARENT = path.join(ROOT, '..', 'EmberSigh-全功能数字孪生模拟器.html');

const MODULES = [
  '00-core.js',
  '10-engine.js',
  '20-simulators.js',
  '30-coordinator.js',
  '40-demo.js',
  '60-ui.js',
  '70-main.js'
];

function read(rel) {
  return fs.readFileSync(path.join(SRC, rel), 'utf8');
}

function safeScript(code) {
  /* 防止内联脚本被字符串中的 </script> 提前闭合 */
  return code.replace(/<\/script/gi, '<\\/script');
}

function build() {
  let html = read('template.html');

  const css = read('styles.css');
  const echarts = fs.readFileSync(path.join(SRC, 'vendor', 'echarts.min.js'), 'utf8');
  const hero = fs.readFileSync(path.join(SRC, 'vendor', 'hero-product.jpg'));
  const heroData = 'data:image/jpeg;base64,' + hero.toString('base64');

  const js = MODULES.map(function (m) {
    return '\n/* ===== ' + m + ' ===== */\n' + read(path.join('js', m));
  }).join('\n');

  html = html
    .replace('/*__CSS__*/', css)
    .replace('__HERO__', heroData)
    .replace('/*__ECHARTS__*/', safeScript(echarts))
    .replace('/*__JS__*/', safeScript(js));

  if (html.indexOf('/*__') !== -1 || html.indexOf('__HERO__') !== -1) {
    throw new Error('模板占位符未被完全替换');
  }

  fs.writeFileSync(OUT_LOCAL, html, 'utf8');
  fs.writeFileSync(OUT_PARENT, html, 'utf8');

  const kb = (Buffer.byteLength(html, 'utf8') / 1024).toFixed(1);
  console.log('✓ 构建完成');
  console.log('  ' + OUT_LOCAL + '  (' + kb + ' KB)');
  console.log('  ' + OUT_PARENT);
}

try { build(); }
catch (err) { console.error('✗ 构建失败:', err.message); process.exit(1); }
