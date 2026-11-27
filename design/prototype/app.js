(() => {
  'use strict';

  const root = document.documentElement;
  const $ = (s, el = document) => el.querySelector(s);
  const $$ = (s, el = document) => [...el.querySelectorAll(s)];
  const mq = q => window.matchMedia(q).matches;

  const NAV_OF = { home: 'home', connecting: 'home', devices: 'devices', pairing: 'devices', settings: 'settings' };
  const DEFAULTS = defaults();
  const state = { ...DEFAULTS, ...fromHash() };
  const timers = { flow: [], tick: 0 };
  let prevScreen = '';
  let lastAnchor = null;
  let modalAnchor = null;
  let rejectLeft = 30, pairLeft = 60, ctlSec = 12 * 60 + 4;

  Glass.init($$('.glass'));
  apply();
  bind();
  root.classList.add('ready');

  function defaults() {
    const p = navigator.userAgentData?.platform || navigator.platform || '';
    const os = /mac/i.test(p) ? 'mac' : /linux|x11/i.test(p) ? 'linux' : 'win';
    const flag = q => (mq(q) ? '1' : '0');
    return {
      screen: 'home', mat: 'glass', q: 'auto', os, theme: 'auto',
      rt: flag('(prefers-reduced-transparency: reduce)'),
      rm: flag('(prefers-reduced-motion: reduce)'),
      hc: flag('(prefers-contrast: more)'),
      pop: '', tb: '1', hud: '0', tab: 'recent', sec: 'quality', pair: 'show', fs: '0',
    };
  }

  function fromHash() {
    const out = {};
    for (const [k, v] of new URLSearchParams(location.hash.slice(1))) if (k in DEFAULTS) out[k] = v;
    return out;
  }

  function set(patch) {
    if ('screen' in patch && patch.screen !== state.screen) patch.pop ??= '';
    Object.assign(state, patch);
    apply();
  }

  function apply() {
    const theme = state.theme === 'auto' ? (mq('(prefers-color-scheme: dark)') ? 'dark' : 'light') : state.theme;
    const q = state.q === 'auto' ? '2' : state.q;
    // pop 不镜像到 <html>：否则 [data-pop] 选择器会命中根元素。
    for (const [k, v] of Object.entries(state)) if (k !== 'pop') root.dataset[k] = v;
    root.dataset.theme = theme;
    root.dataset.q = q;
    history.replaceState(null, '', '#' + new URLSearchParams(state));

    syncControls();
    const blocking = state.screen === 'connecting' || state.screen === 'pairing';
    for (const el of $$('.win-main > .rail, .win-main > .pages, .win-main > .caps')) el.inert = blocking;
    for (const p of $$('.popover')) p.hidden = p.dataset.popid !== state.pop;
    for (const a of $$('[data-pop]')) a.setAttribute('aria-expanded', a.dataset.pop === state.pop);
    placeRail();
    placePopover();
    Glass.configure({ on: state.mat === 'glass' && state.rt !== '1' && +q >= 2, q: +q });
    if (state.screen !== prevScreen) enterScreen(prevScreen, state.screen);
    stat();
  }

  function syncControls() {
    $('.subnav').setAttribute('aria-orientation', mq('(max-width: 850px)') ? 'horizontal' : 'vertical');
    for (const el of $$('[data-go]')) {
      const [k, v] = el.dataset.go.split(':');
      const on = el.classList.contains('rail-btn') ? NAV_OF[state.screen] === v : state[k] === v;
      const role = el.getAttribute('role');
      if (role === 'tab') el.setAttribute('aria-selected', on);
      else if (role === 'radio') el.setAttribute('aria-checked', on);
      else if (el.classList.contains('rail-btn')) on ? el.setAttribute('aria-current', 'page') : el.removeAttribute('aria-current');
      else if (el.closest('.harness')) el.setAttribute('aria-pressed', on);
    }
    for (const el of $$('[data-flip]')) {
      const on = state[el.dataset.flip] === '1';
      el.setAttribute(el.getAttribute('role') === 'switch' ? 'aria-checked' : 'aria-pressed', on);
    }
  }

  function placeRail() {
    const btn = $('.rail-btn[aria-current]');
    if (!btn || !btn.offsetParent) return;
    $('.rail-ind').style.transform = `translateY(${btn.offsetTop}px)`;
  }

  function placePopover() {
    const pop = state.pop && $(`.popover[data-popid="${state.pop}"]`);
    const anchor = state.pop && $(`[data-pop="${state.pop}"]`);
    if (!pop || !anchor || !anchor.offsetParent) return;
    const box = pop.offsetParent.getBoundingClientRect();
    const a = anchor.getBoundingClientRect();
    const left = a.left - box.left + a.width / 2 - pop.offsetWidth / 2;
    pop.style.left = `${Math.max(8, Math.min(left, box.width - pop.offsetWidth - 8))}px`;
    pop.style.top = `${a.bottom - box.top + 10}px`;
  }

  function enterScreen(from, to) {
    const wasModal = ['connecting', 'pairing', 'host-request'].includes(from);
    const modal = activeModal();
    if (modal && !wasModal) modalAnchor = document.activeElement;
    prevScreen = to;
    if (from === 'connecting') clearFlow();
    if (to === 'connecting' && !timers.flow.length) setStep(2);
    if (to === 'host-request') startReject();
    if (to === 'pairing') startPairing();
    if (modal) {
      requestAnimationFrame(() => (to === 'host-request' ? $('#req-reject') : focusables(modal)[0])?.focus());
    } else if (wasModal) {
      requestAnimationFrame(() => {
        const anchor = modalAnchor?.matches('button, input, [tabindex]') && modalAnchor.getClientRects().length ? modalAnchor : null;
        (anchor || focusables($('.stage'))[0])?.focus();
      });
    }
  }

  function activeModal() {
    const selector = { connecting: '.cn-card', pairing: '.sheet', 'host-request': '.req' }[state.screen];
    return selector ? $(selector) : null;
  }

  function focusables(parent) {
    return $$('button:not(:disabled), input:not(:disabled), select:not(:disabled), [tabindex]:not([tabindex="-1"])', parent)
      .filter(el => el.getClientRects().length && !el.closest('[inert]'));
  }

  // ---------- 连接流程 ----------
  function connect(name, id) {
    $('#cn-name').textContent = name;
    $('#cn-id').textContent = id;
    $('#sess-title').textContent = name;
    clearFlow();
    setStep(0);
    const at = (ms, fn) => timers.flow.push(setTimeout(fn, ms));
    at(380, () => setStep(1));
    at(820, () => setStep(2));
    at(1300, () => setStep(3));
    at(1700, () => setStep(4));
    at(1950, () => { clearFlow(); set({ screen: 'session', tb: '1' }); });
    // 先排好定时器再切屏：enterScreen 以“无定时器”判断是否为深链接静态画面。
    set({ screen: 'connecting' });
  }

  function clearFlow() {
    timers.flow.forEach(clearTimeout);
    timers.flow = [];
  }

  function setStep(n) {
    $$('#cn-steps li').forEach((li, i) => {
      li.className = i < n ? 'done' : i === n ? 'active' : '';
      li.setAttribute('aria-current', i === n ? 'step' : 'false');
    });
  }

  // ---------- 倒计时 ----------
  function startReject() { rejectLeft = 30; $('#req-count').textContent = rejectLeft; }
  function startPairing() {
    pairLeft = 60;
    const fg = $('#ring-fg');
    fg.style.animation = 'none';
    void fg.getBoundingClientRect();
    fg.style.animation = '';
    $('#pair-left').textContent = pairLeft;
  }

  function tick() {
    if (state.screen === 'host-request' && --rejectLeft >= 0) {
      $('#req-count').textContent = rejectLeft;
      if (rejectLeft === 0) set({ screen: 'home' });
    }
    if (state.screen === 'pairing' && state.pair === 'show') {
      if (--pairLeft <= 0) {
        $('#pair-code').textContent = `${rnd(100, 999)} ${rnd(100, 999)}`;
        startPairing();
      }
      $('#pair-left').textContent = pairLeft;
    }
    if (state.screen === 'host-active') {
      ctlSec++;
      $('#ctl-timer').textContent = `${String(ctlSec / 60 | 0).padStart(2, '0')}:${String(ctlSec % 60).padStart(2, '0')}`;
    }
    if (state.screen === 'session') {
      const rtt = rnd(3, 6);
      $('#lat').textContent = rtt;
      if (state.hud === '1') {
        $('#h-rtt').textContent = rtt;
        $('#h-fps').textContent = rnd(118, 120);
        $('#h-br').textContent = (30 + Math.random() * 12).toFixed(1);
        $('#h-dec').textContent = (0.9 + Math.random() * 0.5).toFixed(1);
        $('#h-loss').textContent = (Math.random() < 0.8 ? 0 : Math.random() * 0.3).toFixed(1);
      }
    }
  }

  // ---------- 事件 ----------
  function bind() {
    document.addEventListener('click', onClick);
    document.addEventListener('keydown', onKey);
    $('#omni').addEventListener('submit', e => {
      e.preventDefault();
      const v = $('#omni-input').value.trim();
      v ? connect(v, v) : $('#omni-input').focus();
    });
    $('#br').addEventListener('input', e => { $('#br-out').textContent = `${e.target.value} Mb/s`; });
    bindOtp();
    // 纯图标按钮的提示取自 aria-label，悬停时再取以跟上动态改名。
    document.addEventListener('pointerover', e => {
      const b = e.target.closest?.('button[aria-label]');
      if (b && !b.textContent.trim()) b.title = b.getAttribute('aria-label');
    });
    window.addEventListener('resize', () => { placeRail(); placePopover(); });
    window.addEventListener('hashchange', () => { Object.assign(state, fromHash()); apply(); });
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', apply);
    window.matchMedia('(max-width: 850px)').addEventListener('change', syncControls);
    timers.tick = setInterval(tick, 1000);
  }

  function onClick(e) {
    const t = e.target;
    if (state.pop && !t.closest('.popover') && !t.closest('[data-pop]')) {
      set({ pop: '' });
      if (!t.closest('button, a, input, [tabindex]')) return;
    }

    const go = t.closest('[data-go]');
    if (go) {
      const patch = {};
      for (const pair of go.dataset.go.split(',')) { const [k, v] = pair.split(':'); patch[k] = v; }
      return set(patch);
    }
    const flip = t.closest('[data-flip]');
    if (flip) return set({ [flip.dataset.flip]: state[flip.dataset.flip] === '1' ? '0' : '1' });

    const popBtn = t.closest('[data-pop]');
    if (popBtn) {
      lastAnchor = popBtn;
      set({ pop: state.pop === popBtn.dataset.pop ? '' : popBtn.dataset.pop });
      if (state.pop) $(`.popover[data-popid="${state.pop}"]`).querySelector('button, select, input')?.focus();
      return;
    }

    const act = t.closest('[data-act]');
    if (act && (act.dataset.act !== 'scrim-pair' || t === act)) return action(act.dataset.act, act);

    const tog = t.closest('[data-toggle]');
    if (tog) return tog.setAttribute('aria-pressed', tog.getAttribute('aria-pressed') !== 'true');

    const radio = t.closest('[role=radio], [role=menuitemradio]');
    if (radio) {
      const group = radio.closest('[role=radiogroup], [role=menu]');
      for (const r of $$('[role=radio], [role=menuitemradio]', group)) r.setAttribute('aria-checked', r === radio);
      if (radio.matches('[role=menuitemradio]')) {
        $('#mode-label').textContent = radio.textContent.trim();
        set({ pop: '' });
        lastAnchor?.focus();
      }
      return;
    }

    const sw = t.closest('[role=switch]');
    if (sw) return sw.setAttribute('aria-checked', sw.getAttribute('aria-checked') !== 'true');

    const tile = t.closest('.tile');
    if (tile) activateTile(tile);
  }

  function activateTile(tile) {
    const wakeBtn = $('[data-act=wake]', tile);
    if (wakeBtn) { if (!wakeBtn.disabled) wake(wakeBtn); }
    else if (!tile.classList.contains('offline')) connect(tile.dataset.name, tile.dataset.id);
  }

  function action(name, el) {
    const src = el.closest('[data-name]') || el;
    switch (name) {
      case 'connect': return connect(src.dataset.name, src.dataset.id);
      case 'copy-id': return copyId(el);
      case 'pw-eye': return togglePw(el);
      case 'pw-new': return newPw();
      case 'reject': return set({ screen: 'home' });
      case 'accept': return set({ screen: 'host-active' });
      case 'curtain': return toggleCurtain(el);
      case 'wake': return wake(el);
      case 'revoke': return revoke(el);
      case 'scrim-pair': return set({ screen: 'devices' });
    }
  }

  function onKey(e) {
    const modal = activeModal();
    if (e.key === 'Tab' && modal) {
      const items = focusables(modal);
      const first = items[0], last = items[items.length - 1];
      if (first && (!items.includes(document.activeElement) ||
          (e.shiftKey ? document.activeElement === first : document.activeElement === last))) {
        e.preventDefault();
        (e.shiftKey ? last : first).focus();
      }
      return;
    }
    const choice = e.target.closest?.('[role=tab], [role=radio], [role=menuitemradio]');
    if (choice && ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) {
      const group = choice.closest('[role=tablist], [role=radiogroup], [role=menu]');
      const choices = focusables(group);
      const i = choices.indexOf(choice);
      const next = e.key === 'Home' ? 0 : e.key === 'End' ? choices.length - 1 :
        (i + (['ArrowLeft', 'ArrowUp'].includes(e.key) ? -1 : 1) + choices.length) % choices.length;
      e.preventDefault();
      choices[next].focus();
      choices[next].click();
      return;
    }
    const tile = e.target.closest?.('.tile');
    if (tile && (e.key === 'Enter' || e.key === ' ') && e.target === tile) {
      e.preventDefault();
      activateTile(tile);
      return;
    }
    if (e.key !== 'Escape') return;
    if (state.pop) { set({ pop: '' }); lastAnchor?.focus(); return; }
    if (state.screen === 'pairing') return set({ screen: 'devices' });
    if (state.screen === 'connecting') return set({ screen: 'home' });
    if (state.screen === 'host-request') return set({ screen: 'home' });
    if (state.fs === '1') return set({ fs: '0' });
  }

  // ---------- 小交互 ----------
  async function copyId(btn) {
    const use = $('use', btn);
    try {
      await navigator.clipboard.writeText('482913557');
      use.setAttribute('href', '#i-check');
      btn.setAttribute('aria-label', '已复制');
    } catch {
      use.setAttribute('href', '#i-warn');
      btn.setAttribute('aria-label', '复制失败，请选择本机 ID 手动复制');
    }
    btn.title = btn.getAttribute('aria-label');
    setTimeout(() => {
      use.setAttribute('href', '#i-copy');
      btn.setAttribute('aria-label', '复制 ID');
      btn.title = '复制 ID';
    }, 1200);
  }

  let pw = 'K7Q2ZP', pwShown = false;
  function togglePw(btn) {
    pwShown = !pwShown;
    renderPw();
    $('use', btn).setAttribute('href', pwShown ? '#i-eye-off' : '#i-eye');
    btn.setAttribute('aria-label', pwShown ? '隐藏密码' : '显示密码');
  }
  function newPw() {
    const abc = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    pw = Array.from({ length: 6 }, () => abc[rnd(0, abc.length - 1)]).join('');
    renderPw();
  }
  function renderPw() {
    const el = $('#pw');
    el.textContent = pwShown ? pw : '••••••';
    el.setAttribute('aria-label', pwShown ? `一次性密码 ${pw.split('').join(' ')}` : '一次性密码已隐藏');
  }

  function toggleCurtain(btn) {
    const on = btn.getAttribute('aria-pressed') !== 'true';
    btn.setAttribute('aria-pressed', on);
    $('#curtain').hidden = !on;
  }

  function wake(btn) {
    const tile = btn.closest('.tile');
    btn.disabled = true;
    btn.lastChild.textContent = '唤醒中';
    tile.classList.add('waking');
    setTimeout(() => {
      tile.classList.remove('offline', 'waking');
      $('.dot', tile).className = 'dot ok';
      $('.tid', tile).textContent = tile.dataset.id;
      tile.setAttribute('aria-label', `${tile.dataset.name}，在线`);
      btn.replaceWith(Object.assign(document.createElement('button'), {
        className: 'btn primary sm tile-act', textContent: '连接', tabIndex: -1,
      }));
      $('.tile-act', tile).dataset.act = 'connect';
    }, 1800);
  }

  function revoke(btn) {
    if (btn.classList.contains('confirm')) {
      btn.closest('.row').remove();
      return;
    }
    btn.classList.add('confirm');
    btn.dataset.label = btn.getAttribute('aria-label');
    btn.setAttribute('aria-label', '再次点击确认撤销');
    setTimeout(() => {
      if (!btn.isConnected) return;
      btn.classList.remove('confirm');
      btn.setAttribute('aria-label', btn.dataset.label);
    }, 2500);
  }

  function bindOtp() {
    const boxes = $$('.otp input');
    const update = () => { $('.pair-ok').disabled = !boxes.every(box => /^\d$/.test(box.value)); };
    boxes.forEach((box, i) => {
      box.addEventListener('input', () => {
        box.value = box.value.replace(/\D/g, '').slice(-1);
        update();
        if (box.value && boxes[i + 1]) boxes[i + 1].focus();
      });
      box.addEventListener('keydown', e => {
        if (e.key === 'Backspace' && !box.value && boxes[i - 1]) boxes[i - 1].focus();
      });
      box.addEventListener('paste', e => {
        const digits = (e.clipboardData.getData('text').match(/\d/g) || []).slice(0, 6);
        if (!digits.length) return;
        e.preventDefault();
        boxes.forEach((box, j) => { box.value = digits[j] || ''; });
        update();
        boxes[Math.min(digits.length, 5)].focus();
      });
    });
    update();
  }

  // 位移图在 rAF 里生成，延后读取统计。
  function stat() {
    clearTimeout(stat.timer);
    stat.timer = setTimeout(() => {
      const s = Glass.stats();
      const note = !Glass.supported && state.mat === 'glass' ? ' · 非 Chromium 仅 Q1' : '';
      $('#h-stat').textContent = `位移图 ${s.maps} · ${s.ms.toFixed(1)} ms${note}`;
    }, 300);
  }

  function rnd(a, b) { return a + Math.floor(Math.random() * (b - a + 1)); }
})();
