window.MExec = window.MExec || {};

MExec.editor = (() => {
  let documentId = 0, documentSequence = 0;
  let saveSequence = 0;
  let savedContent = '';
  let savedFileContent = '';
  let baselineSession = null;
  let baselinePath = null;
  let fileLineEnding = '\n';
  let mixedLineEndings = false;
  let editRevision = 0;
  let conditionRequest = 0;
  let pendingEdit = null;
  let breakpointHistory = [];
  let historyPosition = -1;
  const state = {
    path: null,
    content: '',
    dirty: false,
    breakpoints: new Map(),
    currentLine: null,
  };
  const textarea = () => document.querySelector('#editor');
  const gutter = () => document.querySelector('#editor-gutter');


  let undoHistory = [], undoIndex = 0, lastEditAt = 0, breakpointEpoch = 0;
  function recordUndo(previous,next,event,beforeBreakpoints,edit) {
    if(previous===next)return;
    let start=0;while(start<previous.length&&start<next.length&&previous[start]===next[start])start++;
    let a=previous.length,b=next.length;while(a>start&&b>start&&previous[a-1]===next[b-1]){a--;b--;}
    if(edit) {
      const insertedEnd=next.length-(previous.length-edit.end);
      if(insertedEnd>=edit.start && previous.slice(0,edit.start)===next.slice(0,edit.start) && previous.slice(edit.end)===next.slice(insertedEnd)){
        start=edit.start;a=edit.end;b=insertedEnd;
      }
    }
    const element=textarea(),now=Date.now(),removed=previous.slice(start,a),inserted=next.slice(start,b);
    undoHistory.length=undoIndex;
    const last=undoHistory.at(-1);
    const group=event.inputType==='insertText' && !removed && inserted && !inserted.includes('\n') &&
      last && last.type==='insertText' && !last.removed && last.start+last.inserted.length===start &&
      last.epoch===breakpointEpoch && now-lastEditAt<750;
    const afterSelection=[element.selectionStart,element.selectionEnd];
    if(group){last.inserted+=inserted;last.after=afterSelection;last.afterBreakpoints=new Map(state.breakpoints);}
    else undoHistory.push({start,removed,inserted,type:event.inputType,epoch:breakpointEpoch,
      before:[edit?.start??start,edit?.end??a],after:afterSelection,
      beforeBreakpoints,afterBreakpoints:new Map(state.breakpoints)});
    undoIndex=undoHistory.length;lastEditAt=now;
    let size=undoHistory.reduce((n,item)=>n+item.removed.length+item.inserted.length,0);
    while(undoHistory.length&&(undoHistory.length>256||size>16000000)){
      const first=undoHistory.shift();size-=first.removed.length+first.inserted.length;undoIndex--;
    }
  }
  function moveUndo(direction) {
    if(direction<0?undoIndex===0:undoIndex===undoHistory.length)return false;
    const item=undoHistory[direction<0?undoIndex-1:undoIndex],element=textarea();
    const remove=direction<0?item.inserted:item.removed,insert=direction<0?item.removed:item.inserted;
    const current=element.value;
    if(current.slice(item.start,item.start+remove.length)!==remove)return false;
    const next=current.slice(0,item.start)+insert+current.slice(item.start+remove.length);
    if(item.epoch===breakpointEpoch)state.breakpoints=new Map(direction<0?item.beforeBreakpoints:item.afterBreakpoints);
    else remapBreakpoints(current,next,null);
    undoIndex+=direction;lastEditAt=0;++editRevision;++conditionRequest;pendingEdit=null;
    clearBreakpointHistory();state.currentLine=null;state.content=next;element.value=next;
    const selection=direction<0?item.before:item.after;
    element.focus();element.setSelectionRange(selection[0],selection[1]);
    refreshGutter();setDirty(state.content!==savedContent);MExec.desktop?.syncCaret?.();return true;
  }

  const documents = new Map(), boundElements = new WeakSet();
  let commonInitialized = false;
  const tabsElement = () => document.getElementById?.('editor-doc-tabs');
  const supportsDocuments = !!tabsElement();
  function storeActive() {
    if (!supportsDocuments) return;
    documents.set(documentId, {
      id:documentId, element:textarea(), state:{...state},
      saveSequence,savedContent,savedFileContent,baselineSession,baselinePath,
      fileLineEnding,mixedLineEndings,editRevision,breakpointHistory,historyPosition,
      undoHistory,undoIndex,breakpointEpoch
    });
  }
  function untitledName(id) { return '未命名' + (id || '') + '.m'; }
  function renderTabs() {
    if (!supportsDocuments) return;
    storeActive();
    for(const button of document.querySelectorAll?.('[data-action="editor-undo"]')||[])button.disabled=undoIndex===0;
    for(const button of document.querySelectorAll?.('[data-action="editor-redo"]')||[])button.disabled=undoIndex===undoHistory.length;
    const list = tabsElement();
    const focusId=list.contains?.(document.activeElement) ? document.activeElement.dataset.documentId : null;
    list.replaceChildren();
    document.getElementById?.('editor-surface')?.setAttribute('aria-labelledby','editor-document-'+documentId);
    for (const record of documents.values()) {
      const group = document.createElement('span'); group.className = 'editor-document-tab';
      const button = document.createElement('button'); button.type = 'button'; button.dataset.documentId = String(record.id);
      button.id='editor-document-'+record.id;button.setAttribute('role','tab'); button.setAttribute('aria-selected',String(record.id === documentId));
      button.setAttribute('aria-controls','editor-surface'); button.tabIndex = record.id === documentId ? 0 : -1;
      button.textContent = (record.state.path ? record.state.path.split('/').pop() : untitledName(record.id)) + (record.state.dirty ? ' *' : '');
      button.title = (record.state.path || '未保存脚本') + '（编辑区 Ctrl+PageUp/PageDown 切换标签）';
      if (record.baselineSession && record.baselineSession !== currentSession()) {button.title += '（其他会话）';button.textContent += ' · 其他会话';}
      button.addEventListener('click',()=>activateDocument(record.id));
      button.addEventListener('keydown',event=>{
        if (event.defaultPrevented || event.isComposing || event.keyCode === 229 || event.ctrlKey || event.metaKey || event.altKey || event.shiftKey) return;
        const ids = [...documents.keys()], index = ids.indexOf(record.id);
        let target;
        if (event.key === 'ArrowLeft') target=ids[(index+ids.length-1)%ids.length];
        else if (event.key === 'ArrowRight') target=ids[(index+1)%ids.length];
        else if (event.key === 'Home') target=ids[0];
        else if (event.key === 'End') target=ids[ids.length-1];
        else if (event.key === 'Delete') { event.preventDefault(); closeDocument(record.id); return; }
        else return;
        event.preventDefault(); activateDocument(target,false);
        tabsElement().querySelector('[data-document-id="' + target + '"]')?.focus();
      });
      const close = document.createElement('button'); close.type='button'; close.className='editor-document-close';
      close.textContent='×'; close.tabIndex=-1; close.setAttribute('aria-label','关闭 ' + button.textContent);
      close.disabled=!!MExec.files?.isDocumentBusy?.(record.id);
      close.addEventListener('click',()=>closeDocument(record.id));
      group.appendChild(button); group.appendChild(close); list.appendChild(group);
    }
    const add = document.createElement('button'); add.type='button'; add.textContent='+';
    add.setAttribute('aria-label','新建脚本标签'); add.addEventListener('click',()=>{newFile();textarea().focus();});
    list.appendChild(add);
    if(focusId)list.querySelector('[data-document-id="' + focusId + '"]')?.focus();
    list.querySelector('[data-document-id="' + documentId + '"]')?.scrollIntoView?.({block:'nearest',inline:'nearest'});
  }
  function setDocumentActive(element, active) {
    element.classList?.add?.('editor-document-input');
    if(active)element.id='editor';else element.removeAttribute('id');
    element.hidden=false;element.inert=!active;
    if(active)element.removeAttribute('aria-hidden');else element.setAttribute('aria-hidden','true');
    if(element.style) {element.style.visibility=active?'visible':'hidden';element.style.pointerEvents=active?'auto':'none';}
  }
  function prepareNew(reuseEmpty) {
    if (!supportsDocuments) return;
    const old = textarea();
    if (reuseEmpty && !state.path && !state.dirty && !state.content) { documents.delete(documentId); return; }
    storeActive();
    setDocumentActive(old,false);
    const next=old.cloneNode(false); setDocumentActive(next,true); next.value='';
    old.parentNode.appendChild(next);
  }
  function activateDocument(id, focus = true, keepCurrent = true) {
    if (!supportsDocuments || !documents.has(id)) return false;
    if (id === documentId) { if(focus)textarea().focus(); return true; }
    if (keepCurrent) storeActive();
    const old=textarea(); setDocumentActive(old,false);
    const record=documents.get(id);
    documentId=record.id; Object.assign(state,record.state);
    ({saveSequence,savedContent,savedFileContent,baselineSession,baselinePath,
      fileLineEnding,mixedLineEndings,editRevision,breakpointHistory,historyPosition,undoHistory,undoIndex,breakpointEpoch}=record);
    lastEditAt=0;
    pendingEdit=null; state.currentLine=null; ++conditionRequest;
    setDocumentActive(record.element,true);
    refreshGutter(); setDirty(state.dirty);
    MExec.desktop?.syncCaret?.();
    if(focus)record.element.focus();
    return true;
  }
  function activateFile(owner,path) {
    if (!supportsDocuments) return false;
    storeActive();
    const canonical=canonicalFilePath(path);
    const record=[...documents.values()].find(item=>item.baselineSession===owner && item.baselinePath===canonical && canonical !== null);
    return record ? activateDocument(record.id) : false;
  }
  function revealDocument(plan,line) {
    if(!plan || plan.ownerSession !== currentSession())return false;
    if(!Number.isInteger(line)||line<1||line>plan.content.split('\n').length)return false;
    if(supportsDocuments)storeActive();
    const target=plan.documentId===documentId ? state : documents.get(plan.documentId)?.state;
    if(!target || target.path!==plan.path || target.content!==plan.content)return false;
    if(plan.documentId!==documentId && !activateDocument(plan.documentId,false))return false;
    return revealLine(line);
  }

  function closeDocument(id) {
    if (!supportsDocuments) return false;
    storeActive(); const record=documents.get(id);
    if(!record)return false;
    if(MExec.files?.isDocumentBusy?.(id)) {MExec.command?.print('文档正在读写，请等待完成后关闭。','warn');return false;}
    if(record.state.dirty && !window.confirm('关闭 ' + (record.state.path || untitledName(record.id)) + ' 并放弃未保存修改？'))return false;
    const order=[...documents.keys()], index=order.indexOf(id);
    documents.delete(id);
    if(id===documentId) {
      const next=order[index+1] ?? order[index-1];
      if(next !== undefined) {activateDocument(next,true,false);record.element.remove();}
      else {newFile(true);textarea().focus();}
    } else {record.element.remove();renderTabs();}
    return true;
  }
  function hasUnsavedDocuments() {
    if(!supportsDocuments)return state.dirty;
    storeActive();return [...documents.values()].some(record=>record.state.dirty);
  }
  function completeDocumentSave(path,snapshot,owner) {
    if(!supportsDocuments || snapshot.documentId===documentId)return markSavedIfUnchanged(path,snapshot,owner);
    const record=documents.get(snapshot.documentId);
    if(!record || record.saveSequence!==snapshot.sequence)return false;
    record.state.path=path;record.baselinePath=canonicalFilePath(path);record.baselineSession=owner;
    Object.assign(record,baselineData(snapshot.fileContent));
    record.state.dirty=record.state.content!==record.savedContent;renderTabs();return !record.state.dirty;
  }


  function refreshGutter() {
    const element = textarea();
    MExec.syntax?.render(element);
    const lines = element.value.split('\n').length;
    let html = '';
    for (let index = 1; index <= lines; ++index) {
      const condition = state.breakpoints.get(index);
      const hasBreakpoint = condition !== undefined;
      const isCurrent = state.currentLine === index;
      const marker = hasBreakpoint
        ? condition
          ? '◆'
          : '●'
        : isCurrent
          ? '▸'
          : ' ';
      const cls =
        'gutter-line' +
        (hasBreakpoint ? ' has-breakpoint' : '') +
        (isCurrent ? ' is-current' : '');
      const title = condition
        ? ` title="条件: ${String(condition)
            .replace(/&/g, '&amp;')
            .replace(/"/g, '&quot;')
            .replace(/</g, '&lt;')}"`
        : '';
      html +=
        `<div class="${cls}" data-line="${index}"${title}>` +
        `<span class="gutter-symbol">${marker}</span>` +
        `<span class="gutter-number">${index}</span></div>`;
    }
    gutter().innerHTML = html;
    gutter().scrollTop = element.scrollTop;
  }

  function clearDebugLocation() {
    if (state.currentLine === null) return;
    state.currentLine = null;
    refreshGutter();
  }

  function markDebugLocation(line) {
    state.currentLine = line || null;
    refreshGutter();
  }

  function toggleBreakpoint(line) {
    if (!line || line < 1) return false;
    breakpointEpoch++;lastEditAt=0;
    ++conditionRequest;
    clearBreakpointHistory();
    if (state.breakpoints.has(line)) {
      state.breakpoints.delete(line);
    } else {
      state.breakpoints.set(line, '');
    }
    refreshGutter();
    return state.breakpoints.has(line);
  }

  function breakpointLines() {
    return Array.from(state.breakpoints.keys()).sort((a, b) => a - b);
  }

  function breakpointDescriptors() {
    return breakpointLines().map((line) => ({
      line,
      condition: state.breakpoints.get(line) || '',
    }));
  }

  function setBreakpointCondition(line, condition) {
    if (!line || line < 1) return false;
    breakpointEpoch++;lastEditAt=0;
    ++conditionRequest;
    clearBreakpointHistory();
    state.breakpoints.set(line, String(condition || '').trim());
    refreshGutter();
    return true;
  }

  function setDirty(dirty) {
    state.dirty = dirty;
    MExec.files?.syncDocumentControls?.();
    MExec.analysis?.documentChanged();
    const mark = document.querySelector('#editor-dirty');
    if (mark) mark.textContent = dirty ? '● 未保存' : '';
    const label = document.querySelector('#editor-file');
    if (label) {
      label.textContent = state.path || untitledName(documentId);
      label.title = label.textContent;
    }
    renderTabs();
  }

  function clearBreakpointHistory() {
    breakpointHistory = [];
    historyPosition = -1;
  }

  function updateBreakpointHistory(next, inputType, edit) {
    if (state.content === next) return;
    if (state.content.length + next.length > 2000000) {
      clearBreakpointHistory();
      remapBreakpoints(state.content, next, edit);
      return;
    }
    const direction = inputType === 'historyUndo' ? -1 : inputType === 'historyRedo' ? 1 : 0;
    if (direction) {
      for (let i = historyPosition + direction; i >= 0 && i < breakpointHistory.length; i += direction) {
        if (breakpointHistory[i].text !== next) continue;
        state.breakpoints.clear();
        for (const [line, condition] of breakpointHistory[i].breakpoints) state.breakpoints.set(line, condition);
        historyPosition = i;
        return;
      }
      // Native undo may span edits outside our bounded history. Never restore
      // an unrelated snapshot just because a shortcut was pressed.
      clearBreakpointHistory();
    }
    if (historyPosition < 0) {
      breakpointHistory.push({text: state.content, breakpoints: new Map(state.breakpoints)});
      historyPosition = 0;
    }
    breakpointHistory.length = historyPosition + 1;
    remapBreakpoints(state.content, next, edit);
    breakpointHistory.push({text: next, breakpoints: new Map(state.breakpoints)});
    historyPosition++;
    let characters = breakpointHistory.reduce((sum, item) => sum + item.text.length, 0);
    while (breakpointHistory.length > 1 && (breakpointHistory.length > 64 || characters > 2000000)) {
      characters -= breakpointHistory.shift().text.length;
      historyPosition--;
    }
  }

  // Track text positions only. Kernel breakpoint validation remains authoritative.
  function remapBreakpoints(previous, next, edit) {
    if (!state.breakpoints.size || previous === next) return;
    if (previous.split('\n').length === next.split('\n').length && next.trim()) return;
    let start = 0;
    while (start < previous.length && start < next.length && previous[start] === next[start]) ++start;
    let oldEnd = previous.length, newEnd = next.length;
    while (oldEnd > start && newEnd > start && previous[oldEnd - 1] === next[newEnd - 1]) {
      --oldEnd; --newEnd;
    }
    // A validated beforeinput range disambiguates repeated identical text.
    if (edit && edit.documentId === documentId && edit.revision === editRevision - 1) {
      const insertedEnd = next.length - (previous.length - edit.end);
      if (insertedEnd >= edit.start &&
          previous.slice(0, edit.start) === next.slice(0, edit.start) &&
          previous.slice(edit.end) === next.slice(insertedEnd)) {
        start = edit.start; oldEnd = edit.end; newEnd = insertedEnd;
      }
    }
    const delta = newEnd - oldEnd;
    const removedLines = previous.slice(start, oldEnd).includes('\n');
    const mapped = new Map(), collisions = new Set();
    let offset = 0;
    for (const [index, text] of previous.split('\n').entries()) {
      const condition = state.breakpoints.get(index + 1);
      if (condition !== undefined) {
        if (oldEnd > start && start <= offset && oldEnd >= offset + text.length && newEnd === start) {
          offset += text.length + 1; continue;
        }
        // Anchor to the first non-whitespace character so indenting and inserting
        // a line before code carry its breakpoint along with the code.
        let anchor = offset + (text.search(/\S/) < 0 ? 0 : text.search(/\S/));
        if (anchor >= oldEnd) anchor += delta;
        else if (anchor >= start) {
          if (removedLines) { offset += text.length + 1; continue; }
          anchor = start;
        }
        const line = next.slice(0, Math.max(0, anchor)).split('\n').length;
        if (mapped.has(line) && mapped.get(line) !== condition) collisions.add(line);
        else mapped.set(line, condition);
      }
      offset += text.length + 1;
    }
    // Two different conditions cannot be silently combined into one breakpoint.
    for (const line of collisions) mapped.delete(line);
    state.breakpoints.clear();
    for (const [line, condition] of mapped) state.breakpoints.set(line, condition);
  }

  function init() {
    const element = textarea();
    if (boundElements.has(element)) return;
    boundElements.add(element);
    if(supportsDocuments)setDocumentActive(element,true);
    const lineGutter = gutter();
    MExec.syntax?.attach(element);
    element.addEventListener('beforeinput', event => {
      if(element !== textarea())return;
      if(supportsDocuments && ['historyUndo','historyRedo'].includes(event.inputType)) {
        event.preventDefault();moveUndo(event.inputType==='historyUndo'?-1:1);return;
      }
      pendingEdit = null;
      if (event.isComposing) return;
      const type = event.inputType || '';
      if (!type.startsWith('insert') && !type.startsWith('delete')) return;
      let start = element.selectionStart, end = element.selectionEnd;
      if (start === end && type.startsWith('delete')) {
        if (type === 'deleteContentBackward' && start > 0) {
          --start;
          if (start > 0 && /[\uDC00-\uDFFF]/.test(element.value[start]) &&
              /[\uD800-\uDBFF]/.test(element.value[start - 1])) --start;
        } else if (type === 'deleteContentForward' && end < element.value.length) {
          ++end;
          if (/[\uD800-\uDBFF]/.test(element.value[end - 1]) &&
              /[\uDC00-\uDFFF]/.test(element.value[end] || '')) ++end;
        } else return;
      }
      pendingEdit = {start, end, documentId, revision: editRevision};
    });
    element.addEventListener('input', event => {
      if(element !== textarea())return;
      ++editRevision;
      state.currentLine = null;
      let edit = pendingEdit;
      pendingEdit = null;
      // Some input paths omit beforeinput. For a pure insertion, the resulting
      // collapsed caret and length delta identify a range; remapping validates it.
      const added = element.value.length - state.content.length;
      if (!edit && added > 0 && !event.isComposing &&
          (!event.inputType || event.inputType.startsWith('insert')) &&
          element.selectionStart === element.selectionEnd) {
        const start = element.selectionEnd - added;
        if (start >= 0) edit = {start, end: start, documentId, revision: editRevision - 1};
      }
      const beforeBreakpoints=new Map(state.breakpoints);
      updateBreakpointHistory(element.value, event.inputType, edit);
      if(supportsDocuments)recordUndo(state.content,element.value,event,beforeBreakpoints,edit);
      state.content = element.value;
      refreshGutter();
      setDirty(state.content !== savedContent);
    });
    element.addEventListener('scroll', () => {
      if(element !== textarea())return;
      lineGutter.scrollTop = element.scrollTop;
    });
    // MATLAB 编辑器风格：点击行号槽设置/清除断点。
    if (!commonInitialized) lineGutter.addEventListener('click', async (event) => {
      const row = event.target.closest
        ? event.target.closest('[data-line]')
        : null;
      if (!row) return;
      const line = Number(row.dataset.line);
      if (!event.shiftKey) {
        toggleBreakpoint(line);
        return;
      }
      const identity = documentId;
      const revision = editRevision;
      const request = ++conditionRequest;
      const condition = await MExec.ui.prompt(
        `第 ${line} 行断点条件`,
        '留空表示无条件；如 i == 3'
      );
      if (condition === null || documentId !== identity ||
          editRevision !== revision || request !== conditionRequest) return;
      clearBreakpointHistory();
      state.breakpoints.set(line, String(condition).trim());
      refreshGutter();
    });
    element.addEventListener('keydown', (event) => {
      if(element !== textarea())return;
      if (event.defaultPrevented || event.isComposing || event.keyCode === 229) return;
      if(supportsDocuments && event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey &&
          (event.key==='PageUp' || event.key==='PageDown')) {
        const ids=[...documents.keys()], index=ids.indexOf(documentId);
        event.preventDefault();
        activateDocument(ids[(index+(event.key==='PageDown'?1:ids.length-1))%ids.length]);
        return;
      }
      if(supportsDocuments && (event.ctrlKey||event.metaKey) && !event.altKey && ['z','y'].includes(event.key.toLowerCase())) {
        event.preventDefault();moveUndo(event.key.toLowerCase()==='y'||event.shiftKey?1:-1);return;
      }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f') {
        event.preventDefault();
        showSearch();
        return;
      }
      if ((event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey && event.key.toLowerCase() === 'g') {
        event.preventDefault();
        if (!event.repeat) showGoToLine();
        return;
      }
      if (event.key === 'Tab') {
        event.preventDefault();
        const start = element.selectionStart;
        const end = element.selectionEnd;
        if (start === end && !event.shiftKey) {
          replaceRange('    ', start, end);
        } else {
          const lineStart = start === 0 ? 0 : element.value.lastIndexOf('\n', start - 1) + 1;
          const last = end > start && element.value[end - 1] === '\n' ? end - 1 : end;
          const nextBreak = element.value.indexOf('\n', last);
          const blockEnd = nextBreak < 0 ? element.value.length : nextBreak;
          const lines = element.value.slice(lineStart, blockEnd).split('\n');
          const updated = lines.map(line => event.shiftKey
            ? line.replace(/^(?: {1,4}|\t)/, '') : '    ' + line).join('\n');
          replaceRange(updated, lineStart, blockEnd, 'select');
        }
      } else if (
        event.key === 'F9' ||
        (event.ctrlKey && (event.key === 'e' || event.key === 'E'))
      ) {
        // MATLAB-like F9 / Ctrl+E: run the selection (or the current line).
        event.preventDefault();
        runSelection();
      } else if (event.key === 'F5' && !event.shiftKey) {
        // MATLAB-like F5: run the editor file.
        event.preventDefault();
        MExec.app.runEditor();
      } else if (event.ctrlKey && !event.altKey && ['r', 't'].includes(event.key.toLowerCase())) {
        // Comment and uncomment are distinct editing actions.
        const uncomment = event.shiftKey || event.key.toLowerCase() === 't';
        event.preventDefault();
        commentSelection(uncomment);
      }
    });
    refreshGutter();
    setDirty(false);
    if (!commonInitialized) { initSearch(); commonInitialized = true; }
  }

  // insertText retains the native undo buffer where supported. Keep the
  // fallback centralized for hosts without native editing commands.
  function replaceRange(text, start, end, selection = 'end') {
    const element = textarea();
    const previous = element.value;
    const expected = previous.slice(0, start) + text + previous.slice(end);
    if (previous === expected) return;
    element.focus();
    element.setSelectionRange(start, end);
    const revision = editRevision;
    pendingEdit = {start, end, documentId, revision};
    try {
      if (typeof document.execCommand === 'function') document.execCommand('insertText', false, text);
    } catch (_) { /* Unsupported native editing command: use the fallback. */ }
    if (element.value === previous) element.setRangeText(text, start, end, 'end');
    if (editRevision === revision) element.dispatchEvent(new Event('input'));
    element.setSelectionRange(selection === 'select' || selection === 'start' ? start : start + text.length,
      selection === 'start' ? start : start + text.length);
  }

  function revealLine(line) {
    const element = textarea(), lines = element.value.split('\n');
    if (!Number.isInteger(line) || line < 1 || line > lines.length) return false;
    let start = 0;
    for (let index = 0; index < line - 1; index++) start += lines[index].length + 1;
    element.focus();
    element.setSelectionRange(start, start + lines[line - 1].length);
    const style = window.getComputedStyle ? window.getComputedStyle(element) : null;
    const height = style ? parseFloat(style.lineHeight) || 20 : 20;
    element.scrollTop = Math.max(0, (line - 1) * height - (element.clientHeight || 0) / 2);
    gutter().scrollTop = element.scrollTop;
    return true;
  }

  function goToLine(line) {
    if (!revealLine(line)) return false;
    const element = textarea();
    element.setSelectionRange(element.selectionStart, element.selectionStart);
    element.scrollLeft = 0;
    return true;
  }

  function showGoToLine() {
    const element = textarea();
    const identity = documentId, owner = currentSession(), content = element.value;
    const lineCount = content.split('\n').length;
    const currentLine = content.slice(0, element.selectionStart).split('\n').length;
    const modal = MExec.ui.modal({
      title: '转到行',
      body: '<p id="goto-line-description"></p>' +
        '<label for="goto-line-input">行号</label> ' +
        '<input id="goto-line-input" type="text" inputmode="numeric" autocomplete="off" ' +
        'aria-describedby="goto-line-description goto-line-error">' +
        '<p id="goto-line-error" role="alert"></p>',
      foot: '<button data-close="cancel">取消</button><button id="goto-line-confirm" class="primary">转到</button>',
    });
    const input = modal.element.querySelector('#goto-line-input');
    const error = modal.element.querySelector('#goto-line-error');
    modal.element.querySelector('#goto-line-description').textContent =
      fileName() + ' · 当前第 ' + currentLine + ' 行，共 ' + lineCount + ' 行';
    input.value = String(currentLine);
    const invalid = message => {
      error.textContent = message;
      input.setAttribute('aria-invalid', 'true');
      input.focus();
      return false;
    };
    const submit = () => {
      if (modal.element.isConnected === false) return false;
      if (identity !== documentId || owner !== currentSession()) {
        return invalid('文档或会话已变化，请重新打开“转到行”。');
      }
      if (element.value !== content) return invalid('编辑内容已变化，请重新打开“转到行”。');
      const raw = input.value.trim(), line = Number(raw);
      if (!/^\d+$/.test(raw) || !Number.isSafeInteger(line) || line < 1 || line > lineCount) {
        return invalid('请输入 1 至 ' + lineCount + ' 之间的整数行号。');
      }
      modal.close();
      return goToLine(line);
    };
    modal.element.querySelector('#goto-line-confirm').addEventListener('click', submit);
    input.addEventListener('keydown', event => {
      if (event.key !== 'Enter' || event.isComposing || event.keyCode === 229 || event.repeat) return;
      event.preventDefault();
      submit();
    });
    input.focus();
    input.select();
    return modal;
  }

  function runSelection() {
    const element = textarea();
    const start = element.selectionStart;
    const end = element.selectionEnd;
    const lineStart = start === 0 ? 0 : element.value.lastIndexOf('\n', start - 1) + 1;
    const lineEnd = element.value.indexOf('\n', start);
    const code = start !== end ? element.value.slice(start, end) :
      element.value.slice(lineStart, lineEnd < 0 ? element.value.length : lineEnd);
    if (code.trim()) MExec.command.run(code.trim());
  }

  function commentSelection(uncomment = false) {
    const element = textarea();
    const start = element.selectionStart;
    const end = element.selectionEnd;
    const lineStart = start === 0 ? 0 : element.value.lastIndexOf('\n', start - 1) + 1;
    const last = end > start && element.value[end - 1] === '\n' ? end - 1 : end;
    const lineEnd = element.value.indexOf('\n', last);
    const blockEnd = lineEnd === -1 ? element.value.length : lineEnd;
    const block = element.value.slice(lineStart, blockEnd);
    const lines = block.split('\n');
    const updated = lines
      .map((line) => {
        if (uncomment) {
          return line.replace(/^(\s*)% ?/, '$1');
        }
        return line.replace(/^(\s*)/, '$1% ');
      })
      .join('\n');
    replaceRange(updated, lineStart, blockEnd, 'select');
    element.focus();
  }

  const searchEl = id => document.querySelector('#editor-' + id);
  function searchStatus(text) { searchEl('search-status').textContent = text; }
  function showSearch() {
    const bar=searchEl('search');
    if (!bar) return;
    bar.hidden=false;
    const element=textarea(), input=searchEl('find');
    if(element.selectionEnd>element.selectionStart) input.value=element.value.slice(element.selectionStart,element.selectionEnd);
    input.focus();input.select();
  }
  function findText(backward=false) {
    const needle=searchEl('find').value, element=textarea();
    if (!needle) { searchStatus('请输入查找文本');return false; }
    const start=element.selectionStart,end=element.selectionEnd,text=element.value;
    let index=backward ? (start===0?-1:text.lastIndexOf(needle,start-1)) : text.indexOf(needle,end);
    let wrapped=false;
    if(index<0) { wrapped=true;index=backward?text.lastIndexOf(needle):text.indexOf(needle); }
    if(index<0) {searchStatus('未找到');return false;}
    element.focus();element.setSelectionRange(index,index+needle.length);
    searchStatus(wrapped?'已从另一端继续查找':'已找到');
    return true;
  }
  function replaceText(all=false) {
    const needle=searchEl('find').value,replacement=searchEl('replacement').value,element=textarea();
    if(!needle) {searchStatus('请输入查找文本');return;}
    if(all) {
      const pieces=element.value.split(needle),count=pieces.length-1;
      if(!count) {searchStatus('未找到');return;}
      replaceRange(pieces.join(replacement),0,element.value.length,'start');
      searchStatus('已替换 '+count+' 处');
    } else {
      if(element.value.slice(element.selectionStart,element.selectionEnd)!==needle) {findText();return;}
      replaceRange(replacement,element.selectionStart,element.selectionEnd);
      findText();
    }
  }
  function initSearch() {
    const bar=searchEl('search');if(!bar) return;
    searchEl('search-toggle').addEventListener('click',showSearch);
    const close=()=>{bar.hidden=true;textarea().focus();};
    searchEl('search-close').addEventListener('click',close);
    searchEl('find-next').addEventListener('click',()=>findText());
    searchEl('find-prev').addEventListener('click',()=>findText(true));
    searchEl('replace').addEventListener('click',()=>replaceText());
    searchEl('replace-all').addEventListener('click',()=>replaceText(true));
    bar.addEventListener('keydown',event=>{
      if(event.isComposing) return;
      if(event.key==='Escape'){event.preventDefault();close();}
      if(event.key==='Enter' && event.target===searchEl('find')){event.preventDefault();findText(event.shiftKey);}
    });
  }

  function mayReplace() {
    return !state.dirty || window.confirm('当前脚本有未保存修改。放弃修改并继续？');
  }

  function baselineData(content) {
    const endings = content.match(/\r\n|\r|\n/g) || [];
    const counts = new Map(); for (const ending of endings) counts.set(ending,(counts.get(ending)||0)+1);
    return {savedFileContent:content,
      savedContent:content.replace(/\r\n|\r/g,'\n'),
      fileLineEnding:endings.length ? endings.reduce((best,e)=>counts.get(e)>counts.get(best)?e:best,endings[0]) : '\n',
      mixedLineEndings:counts.size>1};
  }
  function setFileBaseline(content) {
    ({savedFileContent,savedContent,fileLineEnding,mixedLineEndings}=baselineData(content));
  }
  function canonicalFilePath(path) {
    if (typeof path !== 'string' || !path.startsWith('/')) return null;
    const parts = [];
    for (const part of path.split(/[\\/]+/)) {
      if (!part || part === '.') continue;
      if (part === '..') {
        if (!parts.length) return null;
        parts.pop();
      } else parts.push(part);
    }
    return '/' + parts.join('/');
  }

  function currentSession() {
    return MExec.app && MExec.app.state ? MExec.app.state.sessionId : null;
  }

  function isFileBoundToSession(sessionId) {
    return sessionId != null && baselineSession === sessionId &&
      baselinePath !== null && baselinePath === canonicalFilePath(state.path);
  }

  function sourceFileSnapshot(sessionId) {
    if (state.dirty || !isFileBoundToSession(sessionId)) return null;
    return {documentId, path: state.path, content: state.content,
      expectedSourceContent: savedFileContent};
  }

  function saveCondition(snapshot, path, sessionId) {
    const target = canonicalFilePath(path);
    return target && snapshot.baselinePath === target &&
      sessionId != null && snapshot.baselineSession === sessionId
      ? {expected_content: snapshot.baselineFileContent}
      : {expect_absent: true};
  }

  function open(path, content, ownerSession = currentSession(), options = {}) {
    if (supportsDocuments && !options.replace) {
      if (activateFile(ownerSession,path)) return true;
      prepareNew(true);
    } else if (!mayReplace()) return false;
    if (supportsDocuments && options.replace) documents.delete(documentId);
    documentId=++documentSequence;saveSequence=0;editRevision=0;
    undoHistory=[];undoIndex=0;lastEditAt=0;breakpointEpoch=0;
    state.breakpoints=new Map();
    clearBreakpointHistory();
    pendingEdit = null;
    state.path = path;
    baselinePath = canonicalFilePath(path);
    baselineSession = ownerSession;
    setFileBaseline(content);
    state.content = savedContent;
    state.breakpoints.clear();
    state.currentLine = null;
    const element = textarea();
    element.value = state.content;
    refreshGutter();
    setDirty(false);
    if(supportsDocuments)init();
    return true;
  }

  function newFile(replace = false) {
    if (supportsDocuments && !replace) prepareNew(false);
    else if (!replace && !mayReplace()) return false;
    if (supportsDocuments && replace) documents.delete(documentId);
    documentId=++documentSequence;saveSequence=0;editRevision=0;
    undoHistory=[];undoIndex=0;lastEditAt=0;breakpointEpoch=0;
    state.breakpoints=new Map();
    clearBreakpointHistory();
    pendingEdit = null;
    state.path = null;
    baselinePath = null;
    baselineSession = null;
    state.content = '';
    setFileBaseline('');
    state.breakpoints.clear();
    state.currentLine = null;
    const element = textarea();
    element.value = '';
    refreshGutter();
    setDirty(false);
    if(supportsDocuments)init();
    return true;
  }

  function markSaved(path) {
    if (path) state.path = path;
    baselinePath = canonicalFilePath(state.path);
    baselineSession = currentSession();
    setFileBaseline(state.content.replace(/\n/g, fileLineEnding));
    setDirty(false);
  }

  function saveSnapshot() {
    const content = state.content;
    const unchanged = content === savedContent;
    return {documentId, sequence: ++saveSequence, content,
      baselineSession, baselinePath, baselineFileContent: savedFileContent,
      fileContent: unchanged ? savedFileContent : content.replace(/\n/g, fileLineEnding),
      normalizesMixedLineEndings: mixedLineEndings && !unchanged,
      lineEndingLabel: fileLineEnding === '\r\n' ? 'CRLF' : fileLineEnding === '\r' ? 'CR' : 'LF'};
  }

  function saveSnapshotFor(id) {
    if(id===documentId)return saveSnapshot();
    if(!supportsDocuments)return null;
    storeActive();const record=documents.get(id);if(!record)return null;
    const content=record.state.content,unchanged=content===record.savedContent;
    return {documentId:id,sequence:++record.saveSequence,content,
      baselineSession:record.baselineSession,baselinePath:record.baselinePath,baselineFileContent:record.savedFileContent,
      fileContent:unchanged?record.savedFileContent:content.replace(/\n/g,record.fileLineEnding),
      normalizesMixedLineEndings:record.mixedLineEndings&&!unchanged,
      lineEndingLabel:record.fileLineEnding==='\r\n'?'CRLF':record.fileLineEnding==='\r'?'CR':'LF'};
  }

  function markSavedIfUnchanged(path, snapshot, ownerSession = currentSession()) {
    if (snapshot.documentId !== documentId || snapshot.sequence !== saveSequence ||
        (!supportsDocuments && ownerSession !== currentSession())) return false;
    if (path) state.path = path;
    baselinePath = canonicalFilePath(state.path);
    baselineSession = ownerSession;
    setFileBaseline(snapshot.fileContent);
    setDirty(state.content !== savedContent);
    return !state.dirty;
  }

  function fileName() {
    return state.path ? state.path.split('/').pop() : 'main.m';
  }
  return {
    state,
    init,
    supportsDocuments,
    undo:()=>moveUndo(-1),
    redo:()=>moveUndo(1),
    activateDocument,
    revealDocument,
    activateFile,
    closeDocument,
    hasUnsavedDocuments,
    completeDocumentSave,
    refreshTabs: renderTabs,
    hasDocument: id => id === documentId || documents.has(id),
    documentList: () => {storeActive();return [...documents.values()].map(r=>({id:r.id,path:r.state.path,dirty:r.state.dirty,owner:r.baselineSession}));},
    open,
    newFile,
    markSaved,
    markSavedIfUnchanged,
    saveSnapshot,
    saveSnapshotFor,
    saveCondition,
    sourceFileSnapshot,
    isFileBoundToSession,
    documentId: () => documentId,
    fileName,
    commentSelection,
    runSelection,
    revealLine,
    goToLine,
    showGoToLine,
    refreshGutter,
    markDebugLocation,
    clearDebugLocation,
    toggleBreakpoint,
    breakpointLines,
    breakpointDescriptors,
    setBreakpointCondition,
  };
})();
