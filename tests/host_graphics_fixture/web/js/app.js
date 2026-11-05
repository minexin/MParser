window.MExec = window.MExec || {};

MExec.ui = (() => {
  function modal(options) {
    const previousFocus = document.activeElement;
    const mask = document.createElement('div');
    mask.className = 'modal-mask';
    const box = document.createElement('div');
    box.className = 'modal';
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-modal', 'true');
    box.setAttribute('aria-label', options.title || '对话框');
    box.tabIndex = -1;

    const head = document.createElement('div');
    head.className = 'modal-head';
    const title = document.createElement('span');
    title.textContent = options.title || '';
    const closeButton = document.createElement('button');
    closeButton.textContent = '×';
    closeButton.setAttribute('aria-label', '关闭');
    head.appendChild(title);
    head.appendChild(closeButton);

    const body = document.createElement('div');
    body.className = 'modal-body';
    body.innerHTML = options.body || '';

    const foot = document.createElement('div');
    foot.className = 'modal-foot';
    foot.innerHTML = options.foot || '';

    box.appendChild(head);
    box.appendChild(body);
    box.appendChild(foot);
    mask.appendChild(box);
    document.querySelector('#modal-root').appendChild(mask);

    let closed = false;
    const close = () => {
      if (closed) return;
      closed = true;
      const restoreFocus = mask.contains(document.activeElement);
      mask.remove();
      if (restoreFocus && previousFocus && previousFocus.isConnected) previousFocus.focus();
      if (options.onClose) options.onClose();
    };
    closeButton.addEventListener('click', close);
    for (const button of foot.querySelectorAll('[data-close]')) {
      button.addEventListener('click', close);
    }
    mask.addEventListener('click', (event) => {
      if (event.target === mask) close();
    });
    mask.addEventListener('keydown', (event) => {
      if (options.onInterrupt && MExec.command?.isInterruptKey?.(event) &&
          !MExec.command.hasSelection(event.target || document.activeElement) &&
          !MExec.command.hasSelection(mask.querySelector('#ui-prompt'))) {
        event.preventDefault(); event.stopPropagation(); options.onInterrupt(); return;
      }
      // Input dialogs must not also trigger editor commands underneath them.
      // Keep the global stop shortcut available during an input() prompt.
      if (event.shiftKey && event.key === 'F5' && !event.isComposing && event.keyCode !== 229) return;
      event.stopPropagation();
      if (event.key === 'Tab' && !event.isComposing && event.keyCode !== 229) {
        const controls = Array.from(box.querySelectorAll('button, input, select, textarea, a[href], [tabindex]'))
          .filter(control => !control.disabled && control.tabIndex >= 0 && control.getClientRects().length);
        const first = controls[0];
        const last = controls[controls.length - 1];
        if (!first) {
          event.preventDefault();
          box.focus();
        } else if (event.shiftKey && (document.activeElement === first || !controls.includes(document.activeElement))) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && (document.activeElement === last || !controls.includes(document.activeElement))) {
          event.preventDefault();
          first.focus();
        }
        return;
      }
      if (event.key === 'Escape' && !event.isComposing && event.keyCode !== 229) {
        event.preventDefault();
        close();
        return;
      }
      if ((event.ctrlKey || event.metaKey) && ['o', 'n', 's'].includes(event.key.toLowerCase())) {
        event.preventDefault();
      }
    });
    closeButton.focus();
    return { element: mask, close };
  }

  function alert(title, message) {
    const modalBox = modal({
      title,
      body: `<div class="err">${String(message).replace(/</g, '&lt;')}</div>`,
      foot: '<button data-close="ok">确定</button>',
    });
    return modalBox;
  }

  function prompt(title, placeholder, options = {}) {
    if (options.signal && options.signal.aborted) return Promise.resolve(null);
    return new Promise((resolve) => {
      const modalBox = modal({
        title: title || '输入',
        body: `<input id="ui-prompt" style="width:100%;padding:8px;background:var(--bg-input);color:var(--text);border:1px solid var(--border);border-radius:4px" placeholder="${String(
          placeholder || ''
        ).replace(/"/g, '&quot;')}">`,
        foot: '<button data-close="cancel">取消</button><button id="ui-ok" class="primary">确定</button>',
        onClose: () => finish(null),
        onInterrupt: options.onInterrupt,
      });
      const input = modalBox.element.querySelector('#ui-prompt');
      input.focus();
      let finished = false;
      const onAbort = () => finish(null);
      const finish = (value) => {
        if (finished) return;
        finished = true;
        if (options.signal) options.signal.removeEventListener('abort', onAbort);
        modalBox.close();
        resolve(value);
      };
      if (options.signal) {
        options.signal.addEventListener('abort', onAbort, {once:true});
        if (options.signal.aborted) onAbort();
      }
      modalBox.element
        .querySelector('#ui-ok')
        .addEventListener('click', () => finish(input.value));
      input.focus();
      input.addEventListener('keydown', (event) => {
        if (event.isComposing || event.keyCode === 229) return;
        if (event.key === 'Enter') finish(input.value);
      });
    });
  }

  return { modal, alert, prompt };
})();

MExec.app = (() => {
  const state = {
    health: null,
    sessionId: null,
    cwd: '',
    functions: [],
    variables: [],
    valueCache: {},
    lastRun: null,
  };

  function statusElement() {
    return document.querySelector('#server-status');
  }

  function setStatus(text, cls) {
    const element = statusElement();
    element.textContent = text;
    element.className = 'status-badge' + (cls ? ` ${cls}` : '');
  }

  function updateSessionLabel() {
    MExec.desktop?.syncExecution?.();
    MExec.files?.syncDirectoryHistory?.();
    MExec.files?.syncDocumentControls?.();
    MExec.analysis?.documentChanged();
    const label = document.querySelector('#session-label');
    if (label) {
      label.textContent = `会话 ${state.sessionId} · 目录 /${state.cwd} · 函数 ${state.functions.length}`;
    }
    const cwdLabel = document.querySelector('#status-cwd');
    if (cwdLabel) {
      cwdLabel.textContent = `当前目录: /${state.cwd}`;
    }
  }

  let sessionRefresh = 0;
  async function refreshSession() {
    if (!state.sessionId) return;
    const ownerSession = state.sessionId;
    const generation = ++sessionRefresh;
    const initialCwd = state.cwd;
    const initialEpoch = MExec.files?.directoryEpoch?.(ownerSession);
    const info = await MExec.api.session(ownerSession);
    if (state.sessionId !== ownerSession || generation !== sessionRefresh) return;
    state.functions = info.functions || [];
    if (state.cwd === initialCwd && MExec.files?.directoryEpoch?.(ownerSession) === initialEpoch)
      state.cwd = info.cwd || '';
    updateSessionLabel();
    if (MExec.files && MExec.files.refreshFilesPanel) {
      MExec.files.refreshFilesPanel().catch(() => {});
    }
    const status = document.querySelector('#status-session');
    if (status) {
      status.textContent = `会话 ${state.sessionId.slice(0, 18)}`;
    }
  }

  let graphicsRefresh = 0, graphicsEpoch = 0;
  async function refreshGraphics() {
    if (!state.sessionId || !((state.health || {}).features || []).includes('session-graphics-snapshot')) return;
    const ownerSession = state.sessionId, generation = ++graphicsRefresh, epoch = graphicsEpoch;
    const result = await MExec.api.graphics(ownerSession);
    if (state.sessionId !== ownerSession || generation !== graphicsRefresh || epoch !== graphicsEpoch) return;
    MExec.plot.renderGraphicsSnapshot(result.graphics || {schema:'mparser.graphics',version:1,objects:[]}, false, false);
  }

  let workspaceRefresh = 0;
  async function refreshWorkspace() {
    if (!state.sessionId) return;
    const ownerSession = state.sessionId;
    const generation = ++workspaceRefresh;
    const data = await MExec.api.workspace(ownerSession, false, true);
    if (state.sessionId !== ownerSession || generation !== workspaceRefresh) return;
    state.variables = data.variables || [];
    state.valueCache = {};
    MExec.workspace.render(state.variables, {
      ownerSession,
      inspect: (name) => inspectVariable(name),
      plot: (name) => plotVariable(name),
      remove: ((state.health || {}).features || []).includes('conditional-workspace-delete') && typeof data.revision === 'string'
        ? (name) => removeVariable(name, ownerSession, data.revision) : undefined,
    });
  }

  // MATLAB 风格回显：第一行 "name = 值"，矩阵后续行逐行输出。
  function printValueEcho(label, value) {
    const lines = MExec.value.toDisplay(value).split('\n');
    MExec.command.print(`${label} = ${lines.shift() || ''}`, 'out');
    for (const line of lines) {
      MExec.command.print(line, 'out');
    }
  }

  function handleExecuteResult(result, kind, editorPlan = null) {
    ++graphicsEpoch;
    const execution = result.execution || {};
    const elapsed =
      execution.elapsed_nanoseconds !== undefined
        ? `${(execution.elapsed_nanoseconds / 1e6).toFixed(2)} ms`
        : '';
    const tier = execution.effective_tier
      ? ` · tier=${execution.effective_tier}`
      : '';
    const statusClass =
      result.status === 'succeeded'
        ? 'ok'
        : result.status === 'request-rejected'
          ? 'warn'
          : 'err';
    // MATLAB 不打印“执行成功”横幅；只在失败/拒绝时给出状态行。
    if (result.status !== 'succeeded') {
      MExec.command.print(
        `状态: ${result.status}${tier}${elapsed ? ` · ${elapsed}` : ''}`,
        statusClass
      );
    }
    // MParser v1.2 host output (disp/fprintf) arrives as ordered events.
    for (const event of result.output_events || []) {
      if (!event.text) continue;
      MExec.command.writeOutput(event.text);
    }
    // MParser v1.2 reports top-level expression results; MATLAB stores the
    // last displayed result in ans.
    for (const expression of result.top_level_expressions || []) {
      if (expression.output_suppressed) continue;
      printValueEcho('ans', expression.value);
    }
    if (result.graphics && MExec.plot && MExec.plot.renderGraphicsSnapshot) {
      MExec.plot.renderGraphicsSnapshot(result.graphics);
    }
    const relabelAns =
      (kind === 'function-call' ||
        kind === 'file-call' ||
        kind === 'pasted-function' ||
        kind === 'command') &&
      (result.outputs || []).length === 1;
    // Render only returned outputs; assignment display requires kernel events,
    // rather than parsing LHS names and reading final workspace values.
    if (result.status === 'succeeded' && kind !== 'multi-assign') {
      for (const output of result.outputs || []) {
        printValueEcho(
          relabelAns ? 'ans' : output.name || 'ans',
          output.value
        );
      }
    }
    const sourceLabel = name => editorPlan && name === result.execution_source_name
      ? (editorPlan.path || editorPlan.fileName || name) : name;
    for (const diagnostic of result.diagnostics || []) {
      const location =
        diagnostic.source && diagnostic.source.begin
          ? ` (${sourceLabel(diagnostic.source.name || '')} line ${diagnostic.source.begin.line})`
          : '';
      const message = diagnostic.message;
      function locateSource(sourceName, targetLine) {
        const origin = result.execution_source_name;
        if (!editorPlan || typeof origin !== "string" || !origin || sourceName !== origin ||
            !Number.isInteger(targetLine) || targetLine < 1 || targetLine > editorPlan.content.split("\n").length) return undefined;
        return () => {
          if (!revealEditorPlan(editorPlan,targetLine)) {
            MExec.command.print('诊断对应的文档已变化，请重新运行以获取当前位置', 'warn'); return;
          }

        };
      }
      const locate = locateSource(diagnostic.source?.name, diagnostic.source?.begin?.line);
      // MATLAB 命令窗以红色/黄色显示错误与警告消息本身。
      MExec.command.print(
        `${message}${location}`,
        diagnostic.severity === 'warning' ? 'warn' : 'err',
        locate
      );
      for (const row of MExec.diagnostics?.details(diagnostic, sourceLabel) || []) {
        MExec.command.print(row.text, row.severity + ' diagnostic-detail', locateSource(row.sourceName,row.line));
      }
      const capabilityNote = systemCapabilityNote(diagnostic);
      if (capabilityNote) {
        MExec.command.print(capabilityNote, 'warn');
      }
    }
    if (result.error) {
      MExec.command.print(String(result.error.message), 'err');
    }
    const sessionInfo = result.session;
    if (
      sessionInfo &&
      typeof sessionInfo.cwd === 'string' &&
      sessionInfo.cwd !== state.cwd
    ) {
      // 脚本内 cd 会同步内核目录并随响应带回，更新前端虚拟目录。
      state.cwd = sessionInfo.cwd;
      updateSessionLabel();
      if (MExec.files && MExec.files.refreshFilesPanel) {
        MExec.files.refreshFilesPanel().catch(() => {});
      }
    }
    state.lastRun = result;
    const lastRun = document.querySelector('#status-last-run');
    if (lastRun) {
      lastRun.textContent = `上次运行: ${result.status}${elapsed ? ` · ${elapsed}` : ''}`;
    }
    if (result.workspace_summary) {
      refreshWorkspace().catch(() => {});
    }
    refreshSession().catch(() => {});
  }

  // MParser v1.3 的会话级系统服务（随机状态/文件系统/环境变量/目录等）
  // 只在 CLI 等内部宿主适配器可用，公开嵌入 API 尚未暴露；网页端无法执行
  // 这类脚本，这里给出明确解释而不是只显示英文能力策略报错。
  function systemCapabilityNote(diagnostic) {
    if (!diagnostic) return null;
    const identifier = diagnostic.identifier;
    if (
      identifier !== 'MParser:SystemOperationFailed' &&
      identifier !== 'MParser:SystemCapabilityDenied'
    ) {
      return null;
    }
    const message = (diagnostic.message || '').toLowerCase();
    let capability = '系统服务';
    if (message.includes('random')) {
      capability = '随机数/随机状态（rng/rand/randn/randi/randperm 等）';
    } else if (
      message.includes('temporary-directory') ||
      message.includes('tempdir')
    ) {
      capability = '临时目录（tempdir）';
    } else if (
      message.includes('file') ||
      message.includes('filesystem') ||
      message.includes('fopen')
    ) {
      capability =
        '文件系统（fopen/fprintf/fscanf/fclose/fseek/ftell/frewind 等）';
    } else if (
      message.includes('environment') ||
      message.includes('getenv')
    ) {
      capability = '环境变量（getenv）';
    } else if (
      message.includes('current-directory') ||
      message.includes('directory')
    ) {
      capability = '当前目录（cd/pwd/dir）';
    }
    return (
      `该脚本使用了 MParser 系统服务（${capability}），` +
      '当前服务端默认关闭该能力（环境变量/进程等不对外开放）。' +
      '若确需使用，请联系管理员调整服务端 SystemContext 配置。'
    );
  }

  async function resolveVariableValue(name) {
    const ownerSession = state.sessionId;
    if (!ownerSession) return null;
    // Fetch a fresh value for each inspection; cached values can precede an
    // execution or arrive after a workspace refresh.
    const data = await MExec.api.workspaceValue(ownerSession, name);
    if (state.sessionId !== ownerSession) throw new Error('会话已切换，请重新打开变量');
    return data.value;
  }

  const stringifyValue = (value, space) => MExec.value.stringify ? MExec.value.stringify(value, space) : JSON.stringify(value, null, space);
  function unavailableValueReason(value, location = '变量') {
    if (!value || typeof value !== 'object') return '';
    if (value.kind === 'omitted') return location + ' 的完整数据未传输（' + (value.reason || '大小限制') + '）';
    if (value.representation === 'opaque' || value.kind === 'function-handle') return location + ' 包含当前接口无法重建的服务端值';
    if (value.kind === 'cell') {
      for (const [index, child] of (value.data || []).entries()) {
        const reason = unavailableValueReason(child, location + '{' + (index + 1) + '}');
        if (reason) return reason;
      }
    } else if (value.kind === 'struct') {
      for (const [index, element] of (value.data || []).entries()) {
        for (const [field, child] of Object.entries(element)) {
          const reason = unavailableValueReason(child, location + '(' + (index + 1) + ').' + field);
          if (reason) return reason;
        }
      }
    }
    return '';
  }

  async function inspectVariable(name, options = {}) {
    const ownerSession = state.sessionId;
    const metadata = state.variables.find(variable=>variable.name===name);
    const canPage = !options.json && MExec.numericViewer && (state.health?.features || []).includes('workspace-numeric-slices');
    const openPages = revision => MExec.numericViewer.open({name,ownerSession,revision,onJson:()=>inspectVariable(name,{json:true})});
    if(canPage && metadata?.kind==='numeric' && metadata.element_count>1000000) return openPages(null);
    try {
      const snapshot = await MExec.api.workspaceValue(ownerSession, name);
      if (state.sessionId !== ownerSession) throw new Error('会话已切换，请重新打开变量');
      const value = snapshot.value;
      if(canPage && value.kind==='omitted' && metadata?.kind==='numeric') return openPages(snapshot.revision);
      const readOnlyReason = unavailableValueReason(value);
      const readOnly = !!readOnlyReason;
      const browser = !options.json && MExec.valueBrowser && MExec.valueBrowser.supports(value);
      const leafEditor = !options.json && !browser && !readOnly && (MExec.matrixEditor?.supports(value) ? MExec.matrixEditor : MExec.textEditor?.supports(value) ? MExec.textEditor : null);
      const grid = !!leafEditor;
      const display = grid || browser ? '' : MExec.value.toDisplay(value);
      const json = grid ? '' : stringifyValue(value, 2);
      const escapeHtml = (text) =>
        String(text)
          .replace(/&/g, '&amp;')
          .replace(/</g, '&lt;')
          .replace(/>/g, '&gt;');
      const saveId = options.json ? 'var-json-save' : 'var-save';
      const modalBox = MExec.ui.modal({
        title: options.json ? 'JSON 编辑 · ' + name : `变量 ${name}`,
        body: browser ?
          '<p class="value-snapshot-note">显示打开时的快照；重新打开可刷新。浏览不改变原变量。</p>' +
          (readOnly ? '<p>此值仅供查看：' + escapeHtml(readOnlyReason) + '。请通过命令窗口操作原变量。</p>' : '') +
          '<div class="value-browser"></div><details class="value-raw"><summary>' +
          (readOnly ? '原始 JSON 快照' : '高级：编辑完整 JSON 值') + '</summary>' +
          (readOnly ? '<pre class="variable-view">' + escapeHtml(json) + '</pre>' :
            '<p class="value-snapshot-note">手动 JSON 修改不会立即更新表格；保存后重新打开查看。表格或文本编辑会同步更新本地草稿。</p>' +
            '<textarea id="var-edit" aria-label="完整 JSON 值" spellcheck="false" style="width:100%;height:180px;font-family:monospace">' + escapeHtml(json) + '</textarea>') + '</details>' :
          readOnly ? '<p>此值仅供查看：' + escapeHtml(readOnlyReason) + '。请通过命令窗口操作原变量。</p><pre>' + escapeHtml(json) + '</pre>' :
          grid ? '<div class="matrix-view text-view"></div>' + (leafEditor === MExec.textEditor ?
            '<details class="value-raw"><summary>高级 JSON 编辑</summary><p>在新窗口读取当前工作区值并编辑完整 JSON；本窗口的文本草稿仍保留，不会带入新窗口。</p><button id="var-open-json" type="button">打开 JSON 编辑窗口</button></details>' : '') : '<div class="variable-view">' + escapeHtml(display) + '</div>' +
            '<div style="margin-top:10px">编辑 JSON 值（不透明值不可编辑）：</div>' +
            '<textarea id="var-edit" aria-label="完整 JSON 值" spellcheck="false" style="width:100%;height:180px;font-family:monospace;margin-top:6px">' + escapeHtml(json) + '</textarea>',
        foot: readOnly ? '<button data-close="cancel">关闭</button>' : '<button data-close="cancel">取消</button><button id="' + saveId + '" class="primary">保存</button>',
      });
      let saving = false;
      if (browser) {
        const host = modalBox.element.querySelector('.value-browser');
        let view;
        view = MExec.valueBrowser.mount(host, value, name, {
          onEdit: readOnly ? undefined : node => {
            try {
              if (saving || state.sessionId !== ownerSession || modalBox.element.isConnected === false)
                throw new Error('会话或变量窗口已变化，请重新打开变量');
              const input = modalBox.element.querySelector('#var-edit');
              const baseline = input.value, root = JSON.parse(baseline);
              const reason = unavailableValueReason(root);
              if (reason) throw new Error(reason + '，无法安全编辑值');
              const current = MExec.valueBrowser.resolvePath(root,node.path);
              if (stringifyValue(current) !== stringifyValue(node.value))
                throw new Error('该值已在 JSON 草稿中变化，请先保存或撤销 JSON 修改后再编辑');
              const childEditor = MExec.matrixEditor?.supports(current) ? MExec.matrixEditor : MExec.textEditor?.supports(current) ? MExec.textEditor : null;
              if (!childEditor) throw new Error('此值暂不支持直接编辑');
              const editLabel = current.kind === 'numeric' ? '数值' : '文本';
              const location = [name];
              let ancestor = root;
              for (const step of node.path) {
                const coordinate = MExec.valueBrowser.coordinates(step.index,ancestor.dimensions);
                location.push(step.kind === 'cell' ? '{' + coordinate + '}' :
                  (ancestor.data.length > 1 ? '(' + coordinate + ') ' : '') + step.field);
                ancestor = MExec.valueBrowser.resolvePath(ancestor,[step]);
              }
              const child = MExec.ui.modal({
                title: '编辑嵌套' + editLabel + ' · ' + location.join(' › '),
                body: '<p>修改先应用到变量草稿；在外层窗口点击保存后才写入工作区。</p><div class="matrix-view text-view"></div>',
                foot: '<button data-close="cancel">取消</button><button id="nested-apply" class="primary">应用到变量草稿</button>',
              });
              childEditor.mount(child.element,current);
              child.element.querySelector('#nested-apply').addEventListener('click',()=>{
                try {
                  if (saving || state.sessionId !== ownerSession || modalBox.element.isConnected === false)
                    throw new Error('会话或变量窗口已变化，未应用修改');
                  if (input.value !== baseline) throw new Error('JSON 草稿已变化，未覆盖草稿；请关闭编辑器后重新打开');
                  const next = childEditor.read(child.element,current);
                  const updated = MExec.valueBrowser.replacePath(root,node.path,next);
                  view.update(updated);
                  input.value = stringifyValue(updated,2);
                  modalBox.element.querySelector('.value-snapshot-note').textContent =
                    '显示本地变量草稿；点击外层保存后才写入工作区，取消将放弃草稿修改。';
                  child.close();
                } catch(error) { MExec.ui.alert('无法应用修改',error.message); }
              });
            } catch(error) { MExec.ui.alert('无法编辑值',error.message); }
          },
        });
      }
      if (readOnly) return;
      if (grid) leafEditor.mount(modalBox.element, value);
      if (grid && leafEditor === MExec.textEditor) modalBox.element.querySelector('#var-open-json').addEventListener('click',()=>{
        if (saving) return;
        if (state.sessionId !== ownerSession) {MExec.ui.alert('无法打开 JSON 编辑','会话已切换，请重新打开变量');return;}
        inspectVariable(name,{json:true});
      });
      const saveButton = modalBox.element.querySelector('#' + saveId);
      saveButton.addEventListener('click', async () => {
          if (saving) return;
          const controls = Array.from(modalBox.element.querySelectorAll('input, textarea, .matrix-pages button, .text-pages button, .value-browser button, #var-open-json'));
          const disabledStates = controls.map(control => control.disabled);
          try {
            const next = grid ? leafEditor.read(modalBox.element, value) : JSON.parse(
              modalBox.element.querySelector('#var-edit').value
            );
            if (state.sessionId !== ownerSession) throw new Error('会话已切换，请重新打开变量');
            saving = true;
            saveButton.disabled = true;
            controls.forEach(control => { control.disabled = true; });
            await MExec.api.setWorkspaceValue(ownerSession, name, next, snapshot.revision);
            if (state.sessionId !== ownerSession) return;
            await refreshWorkspace();
            if (state.sessionId !== ownerSession) return;
            modalBox.close();
            MExec.command.print(`已更新变量 ${name}`, 'ok');
          } catch (error) {
            MExec.ui.alert('保存失败', error.message);
          } finally {
            saving = false;
            saveButton.disabled = false;
            controls.forEach((control, index) => { control.disabled = disabledStates[index]; });
          }
        });
    } catch (error) {
      MExec.ui.alert('读取变量失败', error.message);
    }
  }

  async function plotVariable(name) {
    try {
      if (!/^[A-Za-z_]\w*$/.test(name)) throw new Error('无效变量名称');
      await MExec.command.run(`plot(${name});`);
    } catch (error) {
      MExec.command.print(`绘图失败: ${error.message}`, 'err');
    }
  }

  async function removeVariable(name, ownerSession = state.sessionId, revision) {
    if (state.sessionId !== ownerSession) return;
    if (!((state.health || {}).features || []).includes('conditional-workspace-delete') ||
        typeof revision !== 'string') {
      MExec.ui.alert('无法安全删除', '服务器不支持工作区版本检查，请更新服务器并刷新工作区。');
      return;
    }
    if (!window.confirm('删除工作区变量 ' + name + '？')) return;
    try {
      const result = await MExec.api.deleteWorkspaceValueConditional(ownerSession, name, revision);
      if (state.sessionId !== ownerSession) return;
      await refreshWorkspace();
      if (state.sessionId !== ownerSession) return;
      MExec.command.print(result?.removed === false ? '变量已不存在: ' + name : '已删除 ' + name, 'ok');
    } catch (error) {
      if (state.sessionId !== ownerSession) return;
      if (error.status === 409 && error.data?.error?.code === 'workspace_conflict') {
        await refreshWorkspace().catch(() => {});
        if (state.sessionId !== ownerSession) return;
        MExec.ui.alert('工作区已变化', '未删除变量。请核对当前工作区后重新确认删除。');
      } else if (error.status === undefined) {
        MExec.ui.alert('删除结果未知', '连接中断或请求超时，无法确认删除结果。请刷新工作区后再操作。');
      } else MExec.command.print('删除失败: ' + error.message, 'err');
    }
  }

  async function newSession() {
    try {
      const created = await MExec.api.createSession({ name: 'default' });
      state.sessionId = created.id;
      state.cwd = created.cwd || '';
      updateSessionLabel();
      ++graphicsEpoch;
      MExec.plot.renderGraphicsSnapshot({schema:'mparser.graphics',version:1,objects:[]});
      sessionStorage.setItem('mexec.sessionId', created.id);
      state.functions = [];
      MExec.command.print(`已创建新会话 ${created.id}`, 'ok');
      await refreshWorkspace();
      await refreshSession();
    } catch (error) {
      MExec.command.print(`创建会话失败: ${error.message}`, 'err');
    }
  }

  const stoppingSessions = new Set();
  async function stopExecution() {
    if (!state.sessionId) return;
    const ownerSession = state.sessionId;
    MExec.command.cancelQueued?.(ownerSession);
    if (stoppingSessions.has(ownerSession)) return;
    stoppingSessions.add(ownerSession);
    MExec.desktop?.syncExecution?.();
    try {
      if (MExec.debugger && MExec.debugger.isActive()) {
        await MExec.debugger.sendAction('stop');
      } else {
        const result = await MExec.api.cancelExecution(ownerSession);
        if (state.sessionId !== ownerSession) return;
        MExec.command.print(result.cancellation_requested
          ? '已请求内核停止执行，等待执行返回' : '当前没有可取消的执行', 'dim');
      }
    } catch (error) { if (state.sessionId === ownerSession) MExec.command.print('停止失败: ' + error.message, 'err'); }
    finally { stoppingSessions.delete(ownerSession); MExec.desktop?.syncExecution?.(); }
  }

  async function resetSession() {
    if (!state.sessionId) return;
    const ownerSession = state.sessionId;
    try {
      await MExec.api.reset(ownerSession);
      if (state.sessionId !== ownerSession) return;
      ++graphicsEpoch;
      MExec.plot.renderGraphicsSnapshot({schema:'mparser.graphics',version:1,objects:[]});
      MExec.command.print('会话已重置（工作区清空、函数持久状态重置）', 'ok');
      await refreshWorkspace();
      await refreshSession();
    } catch (error) {
      MExec.command.print(`重置失败: ${error.message}`, 'err');
    }
  }

  async function deleteSession() {
    if (!state.sessionId) return;
    const ownerSession = state.sessionId;
    if (!window.confirm(`删除会话 ${state.sessionId}？`)) return;
    try {
      await MExec.api.deleteSession(ownerSession);
      if (state.sessionId !== ownerSession) return;
      sessionStorage.removeItem('mexec.sessionId');
      state.sessionId = null;
      await newSession();
    } catch (error) {
      MExec.command.print(`删除失败: ${error.message}`, 'err');
    }
  }

  async function compileEditorPlan() {
    const ownerSession = state.sessionId;
    const documentId = MExec.editor.documentId();
    const path = MExec.editor.state.path;
    const content = MExec.editor.state.content;
    if (!content.trim()) {
      MExec.command.print('编辑器内容为空', 'warn');
      return null;
    }
    const fileName = MExec.editor.fileName();
    const usePath = path && !MExec.editor.state.dirty &&
      MExec.editor.isFileBoundToSession && MExec.editor.isFileBoundToSession(ownerSession);
    const fileSnapshot = usePath && MExec.editor.sourceFileSnapshot
      ? MExec.editor.sourceFileSnapshot(ownerSession) : null;
    if (usePath && (!fileSnapshot ||
        !((state.health || {}).features || []).includes('conditional-source-execution'))) {
      MExec.command.print('服务器无法安全运行已保存文件，请更新服务器并重新连接。', 'err');
      return null;
    }
    // MParser v1.2 source metadata decides how to run the file (authoritative,
    // instead of client-side text heuristics).
    let metadata = [];
    let compileOk = true;
    let compileResult = null;
    try {
      compileResult = await MExec.api.compile({
        source: content,
        source_name: fileName,
      });
      compileOk = !!compileResult.valid;
      metadata = compileResult.source_metadata || [];
    } catch (error) {
      MExec.command.print(`编译检查失败: ${error.message}`, 'err');
      return null;
    }
    const primary = metadata[0] || {};
    if (state.sessionId !== ownerSession || MExec.editor.documentId() !== documentId ||
        MExec.editor.state.content !== content || MExec.editor.state.path !== path) {
      MExec.command.print('编译期间会话或文档已变化，请重新运行', 'warn');
      return null;
    }
    const isClassFile = primary.kind === 'class';
    const isFunctionFile =
      primary.kind === 'function' && primary.pure_function_file;
    const mainName = isFunctionFile ? primary.primary_function : null;
    // Class files with top-level statements are scripts (MATLAB executes
    // the statements); only pure class definitions are compile-only.
    const compileOnly = isClassFile && !primary.has_top_level_statements;
    if (!compileOk) {
      MExec.command.print('编辑器内容编译失败：', 'err');
      for (const diagnostic of
        (compileResult && compileResult.diagnostics) || []) {
        const location =
          diagnostic.source && diagnostic.source.begin
            ? ` (${diagnostic.source.name || ''} line ${diagnostic.source.begin.line})`
            : '';
        const source = diagnostic.source;
        const targetLine = source && source.begin && source.begin.line;
        const canLocate = source && (!source.name || source.name === fileName || source.name === path) &&
          Number.isInteger(targetLine) && targetLine > 0 && targetLine <= content.split('\n').length;
        const locate = canLocate ? () => {
          if (!revealEditorPlan({ownerSession,documentId,path,content},targetLine)) {
            MExec.command.print('诊断对应的文档已变化，请重新运行以获取当前位置', 'warn');
            return;
          }

        } : undefined;
        MExec.command.print(
          `[${diagnostic.severity}] ${diagnostic.identifier}: ${diagnostic.message}${location}`,
          diagnostic.severity === 'warning' ? 'warn' : 'err',
          locate
        );
      }
      return null;
    }
    return {
      content,
      documentId,
      expectedSourceContent: fileSnapshot ? fileSnapshot.expectedSourceContent : undefined,
      fileName,
      usePath,
      path,
      ownerSession,
      mainName,
      compileOnly,
    };
  }

  function revealEditorPlan(plan,line) {
    if(state.sessionId!==plan.ownerSession)return false;
    if(MExec.editor.revealDocument) {
      if(!MExec.editor.revealDocument(plan,line))return false;
    } else {
      if(!isEditorPlanCurrent(plan))return false;
      MExec.editor.revealLine(line);
    }
    showTab('editor');return true;
  }

  function isEditorPlanCurrent(plan) {
    return plan && state.sessionId === plan.ownerSession &&
      MExec.editor.documentId() === plan.documentId &&
      MExec.editor.state.path === plan.path && MExec.editor.state.content === plan.content;
  }

  async function executeEditorPlan(plan) {
    if (!isEditorPlanCurrent(plan)) return;
    try {
      let result;
      if (plan.mainName) {
        // MATLAB-like Run: execute the file's main function.
        result = plan.usePath
          ? await MExec.api.execute(state.sessionId, {
              source_path: plan.path,
              expected_source_content: plan.expectedSourceContent,
              entry_function: plan.mainName,
              requested_output_count: 1,
            })
          : await MExec.api.execute(state.sessionId, {
              source: plan.content,
              source_name: plan.fileName,
              entry_function: plan.mainName,
              requested_output_count: 1,
            });
      } else {
        result = plan.usePath
          ? await MExec.api.execute(state.sessionId, {
              source_path: plan.path,
              expected_source_content: plan.expectedSourceContent,
              compile_only: plan.compileOnly,
            })
          : await MExec.api.execute(state.sessionId, {
              source: plan.content,
              source_name: plan.fileName,
              compile_only: plan.compileOnly,
            });
      }
      if (state.sessionId !== plan.ownerSession) return;
      handleExecuteResult(result, 'editor', plan);
    } catch (error) {
      if (state.sessionId !== plan.ownerSession) return;
      const code = error.data && error.data.error && error.data.error.code;
      MExec.command.print(code === 'source_conflict' || code === 'source_unavailable'
        ? '源文件已变化或无法读取，请检查文件并重新打开或保存。当前编辑内容仍保留。'
        : '运行失败: ' + error.message, 'err');
    }
  }

  async function runEditor() {
    if (stoppingSessions.has(state.sessionId)) {
      MExec.command.print('停止请求尚未返回，请稍后再运行。', 'warn');
      return;
    }
    if (MExec.debugger && MExec.debugger.isActive()) {
      if (MExec.debugger.isPaused()) {
        MExec.command.print('继续调试运行…', 'dim');
        await MExec.debugger.sendAction('continue');
      } else {
        MExec.command.print('调试运行中，请使用编辑器下方的调试面板', 'dim');
      }
      return;
    }
    if (MExec.input?.isActive?.(state.sessionId)) {
      MExec.command.print('当前会话正在执行，请等待或停止后再运行。', 'warn');
      return;
    }
    const plan = await compileEditorPlan();
    if (!plan) return;
    const hasBreakpoints =
      MExec.editor.breakpointLines &&
      MExec.editor.breakpointLines().length > 0;
    if (
      hasBreakpoints &&
      !plan.compileOnly &&
      MExec.debugger &&
      MExec.debugger.startDebug
    ) {
      // MATLAB 习惯：编辑器里设置了断点时，点“运行”会停在断点处。
      await MExec.debugger.startDebug(plan, false);
      return;
    }
    await executeEditorPlan(plan);
  }

  async function debugEditor() {
    if (stoppingSessions.has(state.sessionId)) {
      MExec.command.print('停止请求尚未返回，请稍后再运行。', 'warn');
      return;
    }
    if (MExec.debugger && MExec.debugger.isActive()) {
      MExec.command.print('调试运行中，请先“继续”或“退出调试”', 'warn');
      return;
    }
    if (MExec.input?.isActive?.(state.sessionId)) {
      MExec.command.print('当前会话正在执行，请等待或停止后再启动调试。', 'warn');
      return;
    }
    const plan = await compileEditorPlan();
    if (!plan) return;
    if (plan.compileOnly) {
      MExec.command.print('纯类定义文件只做编译加载，没有可调试的运行语句', 'warn');
      return;
    }
    await MExec.debugger.startDebug(plan, true);
  }

  function showTab(name) {
    if (name !== 'editor' && name !== 'plot') return;
    MExec.layout?.reveal('center-panel');
    MExec.desktop?.selectRibbon(name === 'plot' ? 'plot' : 'editor');
    let selectedTab;
    for (const tab of document.querySelectorAll('.tab')) {
      const selected = tab.dataset.tab === name;
      tab.classList.toggle('active', selected);
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
      if (selected) selectedTab = tab;
    }
    for (const page of document.querySelectorAll('.tab-page')) {
      const selected = page.id === `${name}-tab`;
      const holdsFocus = !selected && page.contains(document.activeElement);
      page.classList.toggle('active', selected);
      page.hidden = !selected;
      if (holdsFocus) selectedTab?.focus();
    }
    if (name === 'plot') {
      setTimeout(() => MExec.plot.draw(), 0);
    }
  }

  function wireMenus() {
    for (const button of document.querySelectorAll('[data-action]')) {
      button.addEventListener('click', () => {
        const action = button.dataset.action;
        if (action === 'file-new') {
          if (MExec.editor.newFile() !== false) { showTab('editor'); document.getElementById('editor').focus(); }
        }
        else if (action === 'file-open') MExec.files.openDialog();
        else if (action === 'file-save') MExec.files.save();
        else if (action === 'file-save-all') MExec.files.saveAll();
        else if (action === 'file-save-as') MExec.files.saveAs();
        else if (action === 'file-reload') MExec.files.reload();
        else if (action === 'editor-undo') MExec.editor.undo();
        else if (action === 'editor-redo') MExec.editor.redo();
        else if (action === 'session-new') newSession();
        else if (action === 'session-reset') resetSession();
        else if (action === 'session-delete') deleteSession();
        else if (action === 'view-editor') showTab('editor');
        else if (action === 'editor-check') {
          showTab('editor');
          MExec.analysis.check();
        }
        else if (action === 'editor-goto-line') {
          showTab('editor');
          MExec.editor.showGoToLine();
        }
        else if (action === 'editor-find') {
          showTab('editor');
          document.getElementById('editor-search-toggle').click();
        }
        else if (action === 'editor-run-selection') {
          showTab('editor');
          MExec.editor.runSelection();
        }
        else if (action === 'editor-comment' || action === 'editor-uncomment') {
          showTab('editor');
          MExec.editor.commentSelection(action === 'editor-uncomment');
        }
        else if (action === 'view-plot') {
          showTab('plot');
          refreshGraphics().catch(error => MExec.command.print('图形刷新失败：' + error.message, 'err'));
        }
        else if (action === 'help-about') showAbout();
        else if (action === 'help-api') showApiHelp();
        else if (action === 'swagger-page') {
          window.open('/swagger/ui', '_blank');
        }
      });
    }
  }

  function showAbout() {
    const health = state.health || {};
    const runtime = health.runtime || {};
    MExec.ui.modal({
      title: '关于 MExec Studio',
      body: `<div class="variable-view">MExec Studio v0.1.0
微服务后端: MExecServer（Oat++ + MParser）
MParser 运行时: ${runtime.version || '?'} (ABI ${health.runtime ? health.runtime.abi_major + '.' + health.runtime.abi_revision : '?'})
工作区会话: ${state.sessionId || '-'}
Web 前端不依赖任何框架，通过 REST API 与后端通信。</div>`,
      foot: '<button data-close="ok">确定</button>',
    });
  }

  function showApiHelp() {
    MExec.ui.modal({
      title: 'REST API（/api/v1）',
      body: `<div class="variable-view">完整交互式文档（Swagger UI）：/swagger/ui
OpenAPI 3.0 规范：/openapi.json

GET  /health
GET  /info
POST /sessions                创建会话
GET  /sessions                会话列表
GET  /sessions/{id}           会话详情
DELETE /sessions/{id}         删除会话
POST /sessions/{id}/execute   执行 source/command/entry_function
POST /sessions/{id}/reset     重置会话
GET  /sessions/{id}/workspace 工作区摘要
GET  /sessions/{id}/workspace/{name}  变量值
PUT  /sessions/{id}/workspace/{name}  写入变量
DELETE /sessions/{id}/workspace/{name} 删除变量
POST /execute                 无状态执行
POST /compile                 仅编译
GET/PUT /files                脚本文件存储</div>`,
      foot: '<button data-close="ok">关闭</button>',
    });
  }

  async function restoreSession() {
    const saved = sessionStorage.getItem('mexec.sessionId');
    if (saved) {
      try {
        const info = await MExec.api.session(saved);
        if (info && info.id) {
          state.sessionId = saved;
        }
      } catch (error) {
        state.sessionId = null;
      }
    }
    if (!state.sessionId) {
      const created = await MExec.api.createSession({ name: 'default' });
      state.sessionId = created.id;
      sessionStorage.setItem('mexec.sessionId', created.id);
    }
    await refreshSession();
    await refreshWorkspace();
    try { await refreshGraphics(); }
    catch (error) { MExec.command.print("图形恢复失败：" + error.message, "err"); }
  }

  async function connectWithRetry() {
    try {
      const health = await MExec.api.info();
      state.health = health;
      setStatus(`已连接 ${health.version || ''}`, 'ok');
      const runtime = health.runtime || {};
      const runtimeText = document.querySelector('#status-runtime');
      if (runtimeText) {
        runtimeText.textContent = `MExecServer ${health.version || '?'} · MParser ${(health.kernel_integration || {}).milestone || runtime.version || '?'}`;
      }
      try {
        await restoreSession();
        MExec.command.print(
          'MExec Studio 已连接 MExecServer。输入 MATLAB 风格命令，或编辑脚本后点击运行。',
          'dim'
        );
        MExec.command.print(
          '内核绘图：plot(y)、plot(x,y)；工作区“绘图”按钮执行同一内核命令。',
          'dim'
        );
      } catch (error) {
        MExec.command.print(`初始化会话失败: ${error.message}`, 'err');
      }
    } catch (error) {
      setStatus('连接失败，5 秒后自动重试…', 'err');
      MExec.command.print(
        `无法连接后端: ${error.message}（5 秒后自动重试）`,
        'err'
      );
      setTimeout(connectWithRetry, 5000);
    }
  }

  async function init() {
    if (MExec.workspace && MExec.workspace.init) MExec.workspace.init();
    if (MExec.debugger && MExec.debugger.init) {
      MExec.debugger.init();
    }
    if (MExec.files && MExec.files.initFilesPanel) {
      MExec.files.initFilesPanel();
    }
    MExec.editor.init();
    MExec.analysis?.init();
    MExec.command.init();
    wireMenus();
    document.querySelector('#btn-run').addEventListener('click', runEditor);
    const debugButton = document.querySelector('#btn-debug');
    if (debugButton) {
      debugButton.addEventListener('click', debugEditor);
    }
    document
      .querySelector('#btn-stop')
      .addEventListener('click', stopExecution);
    document
      .querySelector('#btn-new-session')
      .addEventListener('click', newSession);
    document
      .querySelector('#btn-reset-session')
      .addEventListener('click', resetSession);
    for (const tab of document.querySelectorAll('.tab')) {
      tab.addEventListener('click', () => showTab(tab.dataset.tab));
    }
    window.addEventListener('keydown', (event) => {
      if (event.isComposing || event.keyCode === 229) return;
      if (event.shiftKey && event.key === 'F5') {
        event.preventDefault();
        stopExecution();
      } else if (event.ctrlKey && event.key === 'Enter') {
        event.preventDefault();
        runEditor();
      } else if (event.ctrlKey && (event.key === 'o' || event.key === 'O')) {
        event.preventDefault();
        MExec.files.openDialog();
      } else if (event.ctrlKey && (event.key === 'n' || event.key === 'N')) {
        event.preventDefault();
        if (MExec.editor.newFile() !== false) { showTab('editor'); document.getElementById('editor').focus(); }
      } else if (event.ctrlKey && (event.key === 's' || event.key === 'S')) {
        event.preventDefault();
        if (event.shiftKey) MExec.files.saveAs();
        else MExec.files.save();
      }
    });
    await connectWithRetry();
  }

  return {
    state,
    init,
    refreshWorkspace,
    refreshSession,
    refreshGraphics,
    handleExecuteResult,
    resolveVariableValue,
    runEditor,
    isEditorPlanCurrent,
    debugEditor,
    stopExecution,
    isStopping: id => stoppingSessions.has(id),
    showTab,
    updateSessionLabel,
  };
})();

window.addEventListener('beforeunload', (event) => {
  if (MExec.editor && (MExec.editor.hasUnsavedDocuments?.() || MExec.editor.state.dirty)) {
    event.preventDefault();
    event.returnValue = '当前脚本有未保存修改。';
  }
});

window.addEventListener('DOMContentLoaded', () => {
  MExec.app.init();
});
