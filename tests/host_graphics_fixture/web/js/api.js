window.MExec = window.MExec || {};

MExec.api = (() => {
  const base = location.origin;

  async function request(method, path, body, timeoutMs) {
    const options = { method, headers: {} };
    if (body !== undefined) {
      options.headers['Content-Type'] = 'application/json';
      options.body = MExec.value?.stringify ? MExec.value.stringify(body) : JSON.stringify(body);
    }
    const controller = new AbortController();
    const timer = setTimeout(
      () => controller.abort(),
      timeoutMs || 10000
    );
    let response;
    let text;
    try {
      options.signal = controller.signal;
      response = await fetch(base + path, options);
      text = await response.text();
    } catch (error) {
      if (error.name === 'AbortError') {
        throw new Error(`请求超时（${Math.round((timeoutMs || 10000) / 1000)} 秒），请检查连接及执行状态`);
      }
      throw error;
    } finally {
      clearTimeout(timer);
    }
    let data = null;
    if (text) {
      try {
        data = JSON.parse(text);
      } catch (error) {
        data = { raw: text };
      }
    }
    if (!response.ok) {
      const message =
        (data && data.error && data.error.message) ||
        `HTTP ${response.status}`;
      const error = new Error(message);
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  const encode = encodeURIComponent;
  const directoryRequests = new Map();

  const fileWriteRequests = new Map();

  function sessionFileWrite(id, path, content, condition) {
    // Paths can alias; serialize writes for each virtual session.
    const previous = fileWriteRequests.get(id) || Promise.resolve();
    const payload = Object.assign({ path, content }, condition || {});
    // A separate endpoint makes older servers fail closed instead of ignoring conditions.
    const endpoint = condition ? '/conditional-file' : '/files';
    const operation = () => request('PUT', '/api/v1/sessions/' + encode(id) + endpoint, payload);
    const pending = previous.then(operation, operation);
    fileWriteRequests.set(id, pending);
    const cleanup = () => {
      if (fileWriteRequests.get(id) === pending) fileWriteRequests.delete(id);
    };
    pending.then(cleanup, cleanup);
    return pending;
  }

  function sessionCd(id, path) {
    // Both the command window and file panel use this queue. Relative paths
    // must reach the server after the preceding directory change completes.
    const previous = directoryRequests.get(id) || Promise.resolve();
    const operation = () => request('POST', `/api/v1/sessions/${encode(id)}/cwd`, { path });
    const pending = previous.then(operation, operation);
    directoryRequests.set(id, pending);
    const cleanup = () => {
      if (directoryRequests.get(id) === pending) directoryRequests.delete(id);
    };
    pending.then(cleanup, cleanup);
    return pending;
  }

  return {
    health: () => request('GET', '/api/v1/health'),
    info: () => request('GET', '/api/v1/info'),
    createSession: (body) => request('POST', '/api/v1/sessions', body || {}),
    sessions: () => request('GET', '/api/v1/sessions'),
    session: (id) => request('GET', `/api/v1/sessions/${encode(id)}`),
    deleteSession: (id) => request('DELETE', `/api/v1/sessions/${encode(id)}`),
    execute: (id, body) => {
      const payload = Object.assign(
        { interactive_input: true },
        body || {}
      );
      const execute = () =>
        request(
          'POST',
          `/api/v1/sessions/${encode(id)}/${Object.prototype.hasOwnProperty.call(payload, 'expected_source_content') ? 'conditional-execute' : 'execute'}`,
          payload,
          120000
        );
      if (MExec.input && MExec.input.withWatch) {
        return MExec.input.withWatch(id, execute);
      }
      return execute();
    },
    inputStatus: (id) =>
      request('GET', `/api/v1/sessions/${encode(id)}/input`),
    inputSubmit: (id, text) =>
      request('POST', `/api/v1/sessions/${encode(id)}/input`, { text }),
    inputCancel: (id) =>
      request('DELETE', `/api/v1/sessions/${encode(id)}/input`),
    cancelExecution: (id) => request('DELETE', `/api/v1/sessions/${encode(id)}/execution`),
    executionState: (id) => request('GET', `/api/v1/sessions/${encode(id)}/execution`),
    reset: (id) => request('POST', `/api/v1/sessions/${encode(id)}/reset`),
    graphics: (id) => request('GET', '/api/v1/sessions/' + encode(id) + '/graphics'),
    workspace: (id, includeValues, includePreviews) =>
      request('GET', '/api/v1/sessions/' + encode(id) + '/workspace' +
        ((includeValues || includePreviews) ? '?' +
          [includeValues ? 'include_values=true' : '', includePreviews ? 'include_previews=true' : ''].filter(Boolean).join('&') : '')),
    workspaceNumericSlice: (id, name, range) =>
      request('POST', `/api/v1/sessions/${encode(id)}/workspace/${encode(name)}/numeric-slice`, range),
    workspaceValue: (id, name) =>
      request('GET', `/api/v1/sessions/${encode(id)}/workspace/${encode(name)}`),
    setWorkspaceValue: (id, name, value, revision) =>
      request(
        'PUT',
        `/api/v1/sessions/${encode(id)}/workspace/${encode(name)}` +
          (revision === undefined ? '' : `?expected_revision=${encode(revision)}`),
        value
      ),
    deleteWorkspaceValueConditional: (id, name, revision) =>
      request('POST', '/api/v1/sessions/' + encode(id) + '/workspace/' + encode(name) + '/conditional-delete', {expected_revision: revision}),
    deleteWorkspaceValue: (id, name) =>
      request(
        'DELETE',
        `/api/v1/sessions/${encode(id)}/workspace/${encode(name)}`
      ),
    filesList: (dir) => request('GET', `/api/v1/files?dir=${encode(dir || '')}`),
    fileRead: (path) => request('GET', `/api/v1/files?path=${encode(path)}`),
    fileWrite: (path, content) =>
      request('PUT', '/api/v1/files', { path, content }),
    sessionCwd: (id) => request('GET', `/api/v1/sessions/${encode(id)}/cwd`),
    sessionCd,
    sessionPath: (id) =>
      request('GET', `/api/v1/sessions/${encode(id)}/path`),
    sessionSnapshot: (id) =>
      request('GET', `/api/v1/sessions/${encode(id)}/snapshot`),
    sessionPathUpdate: (id, action, path) =>
      request('POST', `/api/v1/sessions/${encode(id)}/path`, {
        action,
        path,
      }),
    sessionFilesList: (id, dir) =>
      request(
        'GET',
        `/api/v1/sessions/${encode(id)}/files?dir=${encode(dir || '')}`
      ),
    sessionFileRead: (id, path) =>
      request(
        'GET',
        `/api/v1/sessions/${encode(id)}/files?path=${encode(path)}`
      ),
    sessionFileWrite,
    sessionFileDelete: (id, path) =>
      request(
        'DELETE',
        `/api/v1/sessions/${encode(id)}/files?path=${encode(path)}`
      ),
    compile: (body) => request('POST', '/api/v1/compile', body),
    debugStart: (id, body) => {
      const start = () =>
        request(
          'POST',
          `/api/v1/sessions/${encode(id)}/debug/${body && Object.prototype.hasOwnProperty.call(body, 'expected_source_content') ? 'conditional-start' : 'start'}`,
          Object.assign({ interactive_input: true }, body || {}),
          60000
        );
      if (MExec.input && MExec.input.withWatch) {
        return MExec.input.withWatch(id, start);
      }
      return start();
    },
    debugState: (id, runId) =>
      request(
        'GET',
        `/api/v1/sessions/${encode(id)}/debug/${encode(runId)}`,
        undefined,
        10000
      ),
    debugAction: (id, runId, action) =>
      request(
        'POST',
        `/api/v1/sessions/${encode(id)}/debug/${encode(runId)}/action`,
        { action },
        10000
      ),
    debugEvaluate: (id, runId, source, frame, requestedOutputCount) =>
      request(
        'POST',
        `/api/v1/sessions/${encode(id)}/debug/${encode(runId)}/action`,
        {
          action: 'evaluate',
          source,
          frame: frame === undefined ? -1 : frame,
          requested_output_count:
            requestedOutputCount === undefined ? 1 : requestedOutputCount,
        },
        30000
      ),
    debugDelete: (id, runId) =>
      request(
        'DELETE',
        `/api/v1/sessions/${encode(id)}/debug/${encode(runId)}`,
        undefined,
        10000
      ),
  };
})();
