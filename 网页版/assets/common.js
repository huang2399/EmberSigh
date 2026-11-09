/* ═══════════════════════════════════════
   EmberSigh 听眠 · 网站共享脚本
   时钟 / 滚动浮现 / FAQ 手风琴 / 导航指示器
   ═══════════════════════════════════════ */
(function(){
'use strict';

/* ── 0. 工具 ── */
var $ = function(id){ return document.getElementById(id); };
window.ES = { $: $ };

/* ── 1. 导航栏时钟 ── */
(function clock(){
  var el = $('clock');
  if(!el) return;
  function tick(){
    var d = new Date(), p = function(n){ return String(n).padStart(2,'0'); };
    el.textContent = p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());
  }
  tick(); setInterval(tick,1000);
})();

/* ── 2. 滚动浮现：进入视口后淡入（克制的模糊过渡） ── */
(function reveal(){
  var els = document.querySelectorAll('.reveal');
  if(!('IntersectionObserver' in window)){
    els.forEach(function(el){ el.classList.add('in'); });
    return;
  }
  var io = new IntersectionObserver(function(entries){
    entries.forEach(function(en){
      if(en.isIntersecting){ en.target.classList.add('in'); io.unobserve(en.target); }
    });
  }, { threshold:.12, rootMargin:'0px 0px -40px 0px' });
  els.forEach(function(el){ io.observe(el); });
})();

/* ── 3. FAQ 手风琴：点击展开/收起，同时只开一条 ── */
(function faq(){
  document.querySelectorAll('.faq').forEach(function(item){
    var q = item.querySelector('.faq-q');
    var a = item.querySelector('.faq-a');
    if(!q || !a) return;
    q.addEventListener('click', function(){
      var open = item.classList.contains('open');
      /* 收起其它已展开项 */
      document.querySelectorAll('.faq.open').forEach(function(o){
        if(o !== item){ o.classList.remove('open'); o.querySelector('.faq-a').style.maxHeight = '0px'; }
      });
      if(open){
        item.classList.remove('open');
        a.style.maxHeight = '0px';
      }else{
        item.classList.add('open');
        a.style.maxHeight = a.scrollHeight + 'px';
      }
    });
  });
})();

/* ── 4. 导航金色细线指示器（跟随当前页高亮） ── */
(function glider(){
  var g = $('glider');
  var active = document.querySelector('.tab.active');
  if(!g || !active) return;
  function move(){ g.style.width = active.offsetWidth+'px';
    g.style.transform = 'translateX('+(active.offsetLeft-4)+'px)'; }
  window.addEventListener('resize', move);
  setTimeout(move, 60);
})();

/* ── 5. Toast 提示条（子页面可复用） ── */
window.toast = function(msg, ico, type){
  var el = $('toast');
  if(!el) return;
  $('toastMsg').textContent = msg;
  $('toastIco').textContent = ico || '✦';
  el.className = 'toast show' + (type ? ' '+type : '');
  clearTimeout(window._toastTimer);
  window._toastTimer = setTimeout(function(){ el.classList.remove('show'); }, 2600);
};

})();
