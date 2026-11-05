window.MExec = window.MExec || {};
MExec.analysis = (() => {
  let generation = 0, snapshot = null, stale = false, busy = false;
  let jumpButtons = [];
  const el = id => document.getElementById('analysis-' + id);
  const sameDocument = item => item &&
    MExec.app.state.sessionId === item.sessionId &&
    MExec.editor.documentId() === item.documentId &&
    MExec.editor.state.path === item.path && MExec.editor.state.content === item.content;
  function setBusy(value) {
    busy = value;
    for (const button of document.querySelectorAll('[data-action="editor-check"]')) button.disabled = value;
  }
  function markStale() {
    stale = true;
    el('status').textContent = '检查结果已过期，请重新检查当前内容。';
    for (const button of jumpButtons) button.disabled = true;
  }
  function documentChanged() {
    if (snapshot && !sameDocument(snapshot) && !stale) {
      generation++;
      setBusy(false);
      markStale();
    }
  }
  function close() {
    generation++;
    snapshot = null;
    jumpButtons = [];
    setBusy(false);
    el('panel').hidden = true;
  }
  async function check() {
    const request = ++generation;
    const item = {
      sessionId:MExec.app.state.sessionId, documentId:MExec.editor.documentId(),
      path:MExec.editor.state.path, content:MExec.editor.state.content,
      sourceName:MExec.editor.fileName(),
    };
    snapshot = item;
    stale = false;
    jumpButtons = [];
    el('panel').hidden = false;
    el('list').innerHTML = '';
    el('functions').textContent = '';
    el('status').textContent = '正在进行内核编译检查…';
    setBusy(true);
    try {
      const result = await MExec.api.compile({source:item.content, source_name:item.sourceName});
      if (request !== generation) return;
      if (!sameDocument(item)) { markStale(); return; }
      const diagnostics = Array.isArray(result.diagnostics) ? result.diagnostics : [];
      el('status').textContent = (result.valid ? '内核编译通过' : '内核编译未通过') +
        ' · ' + diagnostics.length + ' 条诊断';
      const functions = Array.isArray(result.functions) ? result.functions.filter(name => typeof name === 'string') : [];
      if (functions.length) el('functions').textContent = '内核导出函数：' +
        functions.slice(0, 50).join('、') + (functions.length > 50 ? '…' : '');
      for (const diagnostic of diagnostics.slice(0, 200)) {
        const row = document.createElement('li');
        row.className = diagnostic.severity === 'error' ? 'analysis-error' :
          diagnostic.severity === 'warning' ? 'analysis-warning' : '';
        const location = diagnostic.source;
        const line = location?.begin?.line;
        const canLocate = location &&
          (!location.name || location.name === item.sourceName || location.name === item.path) &&
          Number.isInteger(line) && line > 0 && line <= item.content.split('\n').length;
        const label = (diagnostic.severity || 'diagnostic') + ': ' +
          String(diagnostic.message || '') +
          (location?.name ? ' (' + location.name + (Number.isInteger(line) ? ':' + line : '') + ')' : '');
        if (canLocate) {
          const button = document.createElement('button');
          button.type = 'button';
          button.className = 'analysis-location';
          button.textContent = label;
          button.title = '定位到第 ' + line + ' 行';
          button.addEventListener('click', () => {
            if (snapshot !== item || stale) return;
            if (!sameDocument(item)) { documentChanged(); return; }
            MExec.app.showTab('editor');
            MExec.editor.revealLine(line);
          });
          row.appendChild(button);
          jumpButtons.push(button);
        } else row.textContent = label;
        el('list').appendChild(row);
      }
      if (diagnostics.length > 200) {
        const row = document.createElement('li');
        row.textContent = '仅显示前 200 条诊断。';
        el('list').appendChild(row);
      }
    } catch (error) {
      if (request !== generation) return;
      if (!sameDocument(item)) markStale();
      else el('status').textContent = '编译检查失败：' + error.message;
    } finally {
      if (request === generation) setBusy(false);
    }
  }
  function init() { el('close').addEventListener('click', close); }
  return {init, check, close, documentChanged};
})();
