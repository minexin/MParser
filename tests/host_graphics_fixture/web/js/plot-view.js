window.MExec = window.MExec || {};
MExec.plotView = (() => {
  const views = new Map();
  let key = null, frame = null, redraw = () => {}, panMode = false, drag = null;
  const control = id => document.getElementById?.(id);
  function valid(a, b) {
    if (!Number.isFinite(a) || !Number.isFinite(b) || a >= b) return false;
    const scale = Math.max(Math.abs(a),Math.abs(b));
    return b / scale - a / scale > 32 * Number.EPSILON;
  }
  function interpolate(a,b,t) {
    return Number.isFinite(b-a) ? a+(b-a)*t : a*(1-t)+b*t;
  }
  function scaledSpan(a,b,t) { return Number.isFinite(b-a) ? (b-a)*t : b*t-a*t; }
  function sync() {
    for (const id of ['plot-zoom-in','plot-zoom-out','plot-pan','plot-reset-view']) {
      const button = control(id);
      if (button) button.disabled = !frame;
    }
    const button = control('plot-pan');
    if (button) button.setAttribute('aria-pressed', String(panMode));
    const element = document.querySelector('#plot-canvas');
    if (element?.style) element.style.cursor = panMode && frame ? (drag ? 'grabbing' : 'grab') : 'default';
    const status = control('plot-view-status');
    if (status) {
      const format = value => MExec.value.formatNumber(value);
      status.textContent = !frame ? '' :
        'X: [' + format(frame.xlog ? 10 ** frame.xa : frame.xa) + ', ' +
          format(frame.xlog ? 10 ** frame.xb : frame.xb) + '] · Y: [' +
          format(frame.ylog ? 10 ** frame.ya : frame.ya) + ', ' +
          format(frame.ylog ? 10 ** frame.yb : frame.yb) + ']';
    }
  }
  function updateFrame(value) {
    frame = key && value && valid(value.xa, value.xb) && valid(value.ya, value.yb) &&
      value.width > 0 && value.height > 0 ? value : null;
    sync();
  }
  function select(value) { if (key !== value) drag = null; key = value; }
  function limits(base) { return views.get(key) || base; }
  function apply(range) {
    if (!key || !frame || !valid(range.xa, range.xb) || !valid(range.ya, range.yb)) return false;
    views.set(key, {...range});
    redraw();
    return true;
  }
  function zoom(factor, x = 0.5, y = 0.5) {
    if (!frame || !Number.isFinite(factor) || factor <= 0) return false;
    const ax = interpolate(frame.xa,frame.xb,x), ay = interpolate(frame.ya,frame.yb,y);
    return apply({xa:interpolate(ax,frame.xa,factor), xb:interpolate(ax,frame.xb,factor),
      ya:interpolate(ay,frame.ya,factor), yb:interpolate(ay,frame.yb,factor)});
  }
  function pan(x, y) {
    if (!frame || !Number.isFinite(x) || !Number.isFinite(y)) return false;
    const dx = scaledSpan(frame.xa,frame.xb,x), dy = scaledSpan(frame.ya,frame.yb,y);
    return apply({xa:frame.xa+dx, xb:frame.xb+dx, ya:frame.ya+dy, yb:frame.yb+dy});
  }
  function reset() {
    if (!key || !frame) return false;
    views.delete(key); drag = null; redraw(); return true;
  }
  function resetAll() {
    views.clear(); key = null; frame = null; drag = null; sync();
  }
  function point(event, element) {
    if (!frame) return null;
    const box = element.getBoundingClientRect();
    const x = event.clientX - box.left - frame.left, y = event.clientY - box.top - frame.top;
    return x >= 0 && y >= 0 && x <= frame.width && y <= frame.height ? {x,y} : null;
  }
  function attach(draw) {
    redraw = draw;
    const bind = (id, handler) => control(id)?.addEventListener('click', handler);
    bind('plot-zoom-in', () => zoom(0.8));
    bind('plot-zoom-out', () => zoom(1.25));
    bind('plot-reset-view', reset);
    bind('plot-pan', () => { panMode = !panMode; drag = null; sync(); });
    const element = document.querySelector('#plot-canvas');
    if (!element?.addEventListener) return;
    element.addEventListener('wheel', event => {
      const p = point(event, element);
      if (!p || !event.deltaY) return;
      const factor = Math.exp(Math.max(-100, Math.min(100, event.deltaY)) * 0.002);
      if (zoom(factor, p.x/frame.width, 1-p.y/frame.height)) event.preventDefault();
    }, {passive:false});
    element.addEventListener('pointerdown', event => {
      if (!panMode || event.button !== 0 || !point(event, element)) return;
      drag = {id:event.pointerId, x:event.clientX, y:event.clientY, frame:{...frame}};
      element.setPointerCapture?.(event.pointerId);
      event.preventDefault(); sync();
    });
    element.addEventListener('pointermove', event => {
      if (!drag || drag.id !== event.pointerId || !frame) return;
      const f = drag.frame, dx=scaledSpan(f.xa,f.xb,(event.clientX-drag.x)/f.width),
        dy=scaledSpan(f.ya,f.yb,(event.clientY-drag.y)/f.height);
      apply({xa:f.xa-dx,xb:f.xb-dx,ya:f.ya+dy,yb:f.yb+dy});
    });
    const release = event => {
      if (!drag || drag.id !== event.pointerId) return;
      drag = null;
      if (event.type !== 'lostpointercapture') {
        try { element.releasePointerCapture?.(event.pointerId); } catch (_) {}
      }
      sync();
    };
    for (const name of ['pointerup','pointercancel','lostpointercapture']) element.addEventListener(name, release);
    element.addEventListener('dblclick', event => { if (point(event, element)) reset(); });
    element.addEventListener('keydown', event => {
      if (event.isComposing || event.keyCode===229 || event.ctrlKey || event.metaKey || event.altKey) return;
      let changed=false;
      if (event.key==='+' || event.key==='=') changed=zoom(0.8);
      else if (event.key==='-') changed=zoom(1.25);
      else if (event.key==='Home') changed=reset();
      else if (event.key==='ArrowLeft') changed=pan(-0.1,0);
      else if (event.key==='ArrowRight') changed=pan(0.1,0);
      else if (event.key==='ArrowUp') changed=pan(0,0.1);
      else if (event.key==='ArrowDown') changed=pan(0,-0.1);
      if (changed) event.preventDefault();
    });
    sync();
  }
  return {attach, select, limits, updateFrame, zoom, pan, reset, resetAll};
})();
