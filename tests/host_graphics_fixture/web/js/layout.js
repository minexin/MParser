window.MExec = window.MExec || {};
(() => {
  const main = document.getElementById('main');
  const key = 'mexec.desktop-layout';
  const defaults = {folder:22, workspace:21, command:33};
  let sizes = {...defaults};
  try {
    const stored = JSON.parse(localStorage.getItem(key));
    for (const name of Object.keys(sizes)) if (Number.isFinite(stored?.[name])) sizes[name] = stored[name];
  } catch (_) { /* A damaged preference must not prevent startup. */ }
  const clamp = (n,min,max) => Math.max(min,Math.min(max,n));
  function apply() {
    sizes.folder=clamp(sizes.folder,14,30);
    sizes.workspace=clamp(sizes.workspace,16,30);
    sizes.command=clamp(sizes.command,20,60);
    main.style.setProperty('--folder-width', sizes.folder+'%');
    main.style.setProperty('--workspace-width', sizes.workspace+'%');
    main.style.setProperty('--command-height', sizes.command+'%');
    for (const [id,name] of [['split-left','folder'],['split-right','workspace'],['split-command','command']]) {
      document.getElementById(id).setAttribute('aria-valuenow',Math.round(sizes[name]));
    }
    MExec.plot.draw();
  }
  function save() {try{localStorage.setItem(key,JSON.stringify(sizes));}catch(_){} }
  for (const [id,name] of [['split-left','folder'],['split-right','workspace'],['split-command','command']]) {
    const divider=document.getElementById(id);
    const minimum=name==='command'?20:name==='folder'?14:16;
    const maximum=name==='command'?60:30;
    divider.setAttribute('aria-valuemin',minimum);
    divider.setAttribute('aria-valuemax',maximum);
    divider.title='拖动或方向键调整；Home/End 到最小/最大；双击恢复默认比例';
    divider.addEventListener('dblclick',()=>{sizes[name]=defaults[name];apply();save();});
    let pointer=null;
    divider.addEventListener('pointerdown',event=>{
      if(event.button!==0)return;
      pointer=event.pointerId;divider.setPointerCapture(pointer);event.preventDefault();
    });
    divider.addEventListener('pointermove',event=>{
      if(pointer!==event.pointerId)return;
      const r=main.getBoundingClientRect();
      if(r.width<=0 || r.height<=0)return;
      sizes[name]=name==='command'?(r.bottom-event.clientY)/r.height*100:
        name==='folder'?(event.clientX-r.left)/r.width*100:(r.right-event.clientX)/r.width*100;
      apply();
    });
    divider.addEventListener('lostpointercapture',()=>{pointer=null;save();});
    divider.addEventListener('pointerup',event=>{if(pointer===event.pointerId)divider.releasePointerCapture(pointer);});
    divider.addEventListener('keydown',event=>{
      if(event.defaultPrevented || event.isComposing || event.keyCode===229 ||
          event.ctrlKey || event.altKey || event.metaKey || event.shiftKey)return;
      if(event.key==='Home' || event.key==='End') {
        event.preventDefault();sizes[name]=event.key==='Home'?minimum:maximum;apply();save();return;
      }
      const horizontal=name!=='command';
      const decrement=horizontal?'ArrowLeft':'ArrowUp',increment=horizontal?'ArrowRight':'ArrowDown';
      if(event.key!==decrement && event.key!==increment)return;
      event.preventDefault();
      sizes[name]+=(event.key===increment?2:-2)*(name==='folder'?1:-1);apply();save();
    });
  }
  document.getElementById('layout-reset').addEventListener('click',()=>{sizes={...defaults};apply();save();});
  apply();

  const panels = [
    ['files-panel', '.panel-title', '当前文件夹'],
    ['center-panel', '.tabs', '编辑器 / 绘图'],
    ['right-panel', '.panel-title', '工作区'],
    ['command-panel', '.panel-title', '命令窗口'],
  ].map(([id, selector, label]) => ({
    element: document.getElementById(id),
    title: document.getElementById(id).querySelector(selector), label,
  }));
  let maximized = null;
  // Preserve the live DOM, editor undo history and execution state.
  function setMaximized(panel) {
    maximized = panel;
    main.classList.toggle('has-maximized-panel', Boolean(panel));
    for (const item of panels) {
      item.element.classList.toggle('panel-maximized', item === panel);
      item.element.classList.toggle('panel-concealed', Boolean(panel) && item !== panel);
      item.element.inert = Boolean(panel) && item !== panel;
      const action = item === panel ? '恢复' : '最大化';
      item.button.setAttribute('aria-pressed', String(item === panel));
      item.button.setAttribute('aria-label', action + item.label);
      item.button.title = action + item.label + '（双击标题栏；Esc 恢复）';
      item.button.textContent = item === panel ? '❐' : '□';
    }
    if (panel && !panel.element.contains(document.activeElement)) panel.button.focus();
    MExec.plot.draw();
  }
  for (const panel of panels) {
    panel.title.classList.add('dock-title');
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'panel-maximize';
    panel.button = button;
    panel.title.appendChild(button);
    button.addEventListener('click', () => setMaximized(maximized === panel ? null : panel));
    panel.title.addEventListener('dblclick', event => {
      if (event.target.closest('button, input, select, a')) return;
      setMaximized(maximized === panel ? null : panel);
    });
    panel.element.addEventListener('focusin', () => {
      for (const item of panels) item.element.classList.toggle('dock-active', item === panel);
    });
  }
  window.addEventListener('keydown', event => {
    if (event.defaultPrevented || event.isComposing || event.keyCode === 229 ||
        event.key !== 'Escape' || !maximized ||
        document.querySelector('#modal-root .modal-mask')) return;
    event.preventDefault();
    const button = maximized.button;
    setMaximized(null);
    button.focus();
  });
  document.getElementById('layout-reset').addEventListener('click', () => setMaximized(null));
  MExec.layout = {reveal(id) { if (maximized && maximized.element.id !== id) setMaximized(null); }};
  setMaximized(null);
})();
