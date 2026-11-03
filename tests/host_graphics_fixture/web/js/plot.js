window.MExec = window.MExec || {};

MExec.plot = (() => {
  let hasFigure = false;
  let hasAxes = false;
  const state = {
    series: [],
    title: '',
    xlabel: '',
    ylabel: '',
    grid: true,
    xlim: null,
    ylim: null,
    xlog: false,
    ylog: false,
    axisVisible: true,
  };
  const colors = ['#4c8bf5', '#e2a03f', '#4caf50', '#e05c5c', '#b06cf5', '#38c6c6'];

  function canvas() {
    return document.querySelector('#plot-canvas');
  }

  function clear(preserveView = false) {
    if (!preserveView) MExec.plotView?.resetAll();
    if (!preserveView) {
      sceneEpoch++;
      lastSnapshot = null;selectedFigure = null;selectedAxes.clear();
      for (const id of ['#plot-figure','#plot-axes']) {
        const selector = document.querySelector(id);
        if (selector) { selector.replaceChildren();selector.disabled=true;selector.onchange=null; }
      }
    }
    hasFigure = false;
    hasAxes = false;
    const exportButton = document.querySelector('#plot-export');
    if (exportButton) exportButton.disabled = true;
    state.series = [];
    state.title = '';
    state.xlabel = '';
    state.ylabel = '';
    state.grid = true;
    state.xlim = null;
    state.ylim = null;
    state.xlog = false;
    state.ylog = false;
    state.axisVisible = true;
    draw();
  }

  function niceRange(min, max) {
    if (!Number.isFinite(min) || !Number.isFinite(max)) return [0, 1];
    if (min === max) {
      const pad = Math.abs(min) * 0.05 || 1;
      return [min - pad, max + pad];
    }
    const pad = (max - min) * 0.08;
    return [min - pad, max + pad];
  }

  function ticks(min, max, count) {
    const step = (max - min) / count;
    const result = [];
    for (let index = 0; index <= count; ++index) {
      const fraction = index / count;
      result.push(index === 0 ? min : index === count ? max :
        Number.isFinite(max - min) ? min + step * index : min * (1 - fraction) + max * fraction);
    }
    return result;
  }

  function draw() {
    const element = canvas();
    MExec.plotView?.updateFrame(null);
    if (!element) return;
    const width = element.clientWidth || 600;
    const height = element.clientHeight || 400;
    const dpr = window.devicePixelRatio || 1;
    element.width = Math.round(width * dpr);
    element.height = Math.round(height * dpr);
    const context = element.getContext('2d');
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    context.clearRect(0, 0, width, height);
    context.fillStyle = "#ffffff";
    context.fillRect(0, 0, width, height);

    const margin = { left: 56, right: 24, top: 34, bottom: 46 };
    const plotWidth = width - margin.left - margin.right;
    const plotHeight = height - margin.top - margin.bottom;
    if (plotWidth <= 0 || plotHeight <= 0) return;

    let minX = Infinity;
    let maxX = -Infinity;
    let minY = Infinity;
    let maxY = -Infinity;
    for (const series of state.series) {
      for (const value of series.x) {
        if (Number.isFinite(value) && (!state.xlog || value > 0)) {
          minX = Math.min(minX, value);
          maxX = Math.max(maxX, value);
        }
      }
      for (const value of series.y) {
        if (Number.isFinite(value) && (!state.ylog || value > 0)) {
          minY = Math.min(minY, value);
          maxY = Math.max(maxY, value);
        }
      }
    }
    if (state.series.length === 0 && !hasAxes) {
      if (hasFigure) return;
      context.fillStyle = '#626970';
      context.font = '13px Segoe UI, Microsoft YaHei';
      context.textAlign = 'center';
      context.fillText('暂无图形：在命令窗口输入 plot(y) 或 plot(x, y)', width / 2, height / 2);
      return;
    }
    let [x0, x1] = state.xlim || niceRange(minX, maxX);
    let [y0, y1] = state.ylim || niceRange(minY, maxY);
    const log10Safe = (value) =>
      Math.log10(Math.max(value, 1e-300));
    let xa = state.xlog ? log10Safe(x0) : x0;
    let xb = state.xlog ? log10Safe(x1) : x1;
    let ya = state.ylog ? log10Safe(y0) : y0;
    let yb = state.ylog ? log10Safe(y1) : y1;

    if (MExec.plotView) {
      ({xa,xb,ya,yb} = MExec.plotView.limits({xa,xb,ya,yb}));
      x0 = state.xlog ? 10 ** xa : xa; x1 = state.xlog ? 10 ** xb : xb;
      y0 = state.ylog ? 10 ** ya : ya; y1 = state.ylog ? 10 ** yb : yb;
      MExec.plotView.updateFrame({xa,xb,ya,yb,left:margin.left,top:margin.top,width:plotWidth,height:plotHeight,xlog:state.xlog,ylog:state.ylog});
    }

    function fraction(value, lower, upper) {
      const span = upper - lower, offset = value - lower;
      if (Number.isFinite(span) && Number.isFinite(offset)) return offset / span;
      const scale = Math.max(Math.abs(lower), Math.abs(upper));
      return (value / scale - lower / scale) / (upper / scale - lower / scale);
    }
    const toCanvasX = (value) =>
      margin.left +
      fraction(state.xlog ? log10Safe(value) : value, xa, xb) *
        plotWidth;
    const toCanvasY = (value) =>
      margin.top +
      plotHeight -
      fraction(state.ylog ? log10Safe(value) : value, ya, yb) *
        plotHeight;

    const logTicks = (from, to) => {
      const result = [];
      for (let decade = Math.ceil(from); decade <= Math.floor(to); ++decade) {
        result.push(decade);
      }
      return result;
    };
    const xTicks = state.xlog
      ? logTicks(xa, xb).map((decade) => Math.pow(10, decade))
      : ticks(x0, x1, 6);
    const yTicks = state.ylog
      ? logTicks(ya, yb).map((decade) => Math.pow(10, decade))
      : ticks(y0, y1, 5);

    if (state.axisVisible) {
      context.strokeStyle = state.grid ? '#33373d' : 'transparent';
      context.lineWidth = 1;
      for (const tick of xTicks) {
        const x = toCanvasX(tick);
        context.beginPath();
        context.moveTo(x, margin.top);
        context.lineTo(x, margin.top + plotHeight);
        context.stroke();
      }
      for (const tick of yTicks) {
        const y = toCanvasY(tick);
        context.beginPath();
        context.moveTo(margin.left, y);
        context.lineTo(margin.left + plotWidth, y);
        context.stroke();
      }

      context.strokeStyle = '#626970';
      context.beginPath();
      context.moveTo(margin.left, margin.top);
      context.lineTo(margin.left, margin.top + plotHeight);
      context.lineTo(margin.left + plotWidth, margin.top + plotHeight);
      context.stroke();

      context.fillStyle = '#4c545b';
      context.font = '11px Consolas, monospace';
      context.textAlign = 'center';
      for (const tick of xTicks) {
        context.fillText(
          state.xlog
            ? `10^${Math.round(Math.log10(tick))}`
            : MExec.value.formatNumber(tick),
          toCanvasX(tick),
          margin.top + plotHeight + 16
        );
      }
      context.textAlign = 'right';
      for (const tick of yTicks) {
        context.fillText(
          state.ylog
            ? `10^${Math.round(Math.log10(tick))}`
            : MExec.value.formatNumber(tick),
          margin.left - 8,
          toCanvasY(tick) + 4,
          margin.left - 12
        );
      }
    }

    // Clip strokes to the axes viewport without changing the kernel's data.
    context.save();
    context.beginPath();
    context.rect(margin.left, margin.top, plotWidth, plotHeight);
    context.clip();
    state.series.forEach((series, index) => {
      if (series.style.hidden) return;
      context.strokeStyle = series.style.color || colors[index % colors.length];
      context.lineWidth = 2;
      context.setLineDash(series.style.dash || []);
      context.beginPath();
      let started = false;
      series.x.forEach((x, pointIndex) => {
        const y = series.y[pointIndex];
        if (!Number.isFinite(x) || !Number.isFinite(y)) {
          started = false;
          return;
        }
        const px = toCanvasX(x);
        const py = toCanvasY(y);
        if (!Number.isFinite(px) || !Number.isFinite(py)) { started = false; return; }
        if (started) context.lineTo(px, py);
        else { context.moveTo(px, py); started = true; }
      });
      context.stroke();
      context.setLineDash([]);
    });
    context.restore();

    if (state.title) {
      context.fillStyle = '#202124';
      context.font = '13px Segoe UI, Microsoft YaHei';
      context.textAlign = 'center';
      context.fillText(state.title, width / 2, 18);
    }
    if (state.xlabel) {
      context.fillStyle = '#4c545b';
      context.font = '12px Segoe UI, Microsoft YaHei';
      context.textAlign = 'center';
      context.fillText(state.xlabel, width / 2, height - 8);
    }
    if (state.ylabel) {
      context.save();
      context.fillStyle = '#4c545b';
      context.font = '12px Segoe UI, Microsoft YaHei';
      context.translate(14, height / 2);
      context.rotate(-Math.PI / 2);
      context.textAlign = 'center';
      context.fillText(state.ylabel, 0, 0);
      context.restore();
    }

    // DisplayName alone does not create a legend. Render one only when the
    // kernel graphics schema can represent a legend and its visibility.
  }

  window.addEventListener('resize', () => draw());

  function colorFromGraphics(value) {
    if (Array.isArray(value) && value.length >= 3) {
      const channel = (item) =>
        Math.max(0, Math.min(255, Math.round(Number(item) * 255)));
      return `rgb(${channel(value[0])}, ${channel(value[1])}, ${channel(
        value[2]
      )})`;
    }
    if (typeof value === 'string' && value.length === 1) {
      const map = {
        r: '#e05c5c',
        g: '#4caf50',
        b: '#4c8bf5',
        c: '#38c6c6',
        m: '#b06cf5',
        y: '#e2a03f',
        k: '#777777',
        w: '#ffffff',
      };
      return map[value] || null;
    }
    return null;
  }

  function dashFromGraphics(style) {
    if (style === '--') return [6, 4];
    if (style === ':') return [2, 3];
    if (style === '-.') return [8, 4, 2, 4];
    return [];
  }

  // Render an mparser.graphics v1 snapshot emitted by the kernel (plot and
  // future graphics builtins). The kernel owns the object graph; this client
  // only translates the deterministic snapshot to Canvas state.
  let lastSnapshot = null;
  let selectedFigure = null;
  const selectedAxes = new Map();
  let sceneEpoch = 0;
  function renderGraphicsSnapshot(snapshot, selectionOnly = false, activate = true) {
    if (!snapshot || snapshot.schema !== 'mparser.graphics' || snapshot.version !== 1 || !Array.isArray(snapshot.objects)) return false;
    const changed = JSON.stringify(snapshot) !== JSON.stringify(lastSnapshot);
    if (changed) {
      // Serialized IDs are graph-local references, not persistent object identities.
      MExec.plotView?.resetAll();
      selectedFigure=null;selectedAxes.clear();sceneEpoch++;
    }
    lastSnapshot = snapshot;
    const epoch=sceneEpoch;
    const objects = snapshot.objects;
    const figures = objects.filter(o => o.type === 'Figure' && (o.properties || {}).Visible !== 'off');
    const selector = document.querySelector('#plot-figure');
    if (!figures.some(o => o.id === selectedFigure)) {
      selectedFigure = figures.length ? figures[figures.length - 1].id : null;
    }
    if (selector) {
      selector.replaceChildren();
      for (const figure of figures) {
        const option = document.createElement('option');
        option.value = String(figure.id);
        option.textContent = (figure.properties || {}).Name || 'Figure ' + (figures.indexOf(figure) + 1);
        option.selected = figure.id === selectedFigure;
        selector.appendChild(option);
      }
      selector.disabled = !figures.length;
      selector.onchange = () => {
        if (epoch !== sceneEpoch || !figures.some(item => String(item.id) === selector.value)) return;
        selectedFigure = Number(selector.value);
        renderGraphicsSnapshot(lastSnapshot, true);
      };
    }
    const figure = figures.find(o => o.id === selectedFigure);
    for (const id of selectedAxes.keys()) if (!objects.some(o => o.type === 'Figure' && o.id === id)) selectedAxes.delete(id);
    const axesList = figure ? objects.filter(o => o.type === 'Axes' && o.parent === figure.id) : [];
    let axesId = figure && selectedAxes.get(figure.id);
    if (!axesList.some(o => o.id === axesId)) axesId = axesList[0]?.id;
    if (figure) selectedAxes.set(figure.id, axesId);
    const axes = axesList.find(o => o.id === axesId);
    const axesSelector = document.querySelector('#plot-axes');
    if (axesSelector) {
      axesSelector.replaceChildren();
      axesList.forEach((item,index) => {
        const option = document.createElement('option');
        option.value = String(item.id);option.textContent = 'Axes ' + (index + 1);
        option.selected = item.id === axesId;axesSelector.appendChild(option);
      });
      axesSelector.disabled = axesList.length < 2;
      axesSelector.onchange = () => {
        if (epoch !== sceneEpoch || !figure || selectedFigure !== figure.id || !axesList.some(item => String(item.id) === axesSelector.value)) return;
        selectedAxes.set(figure.id,Number(axesSelector.value));
        renderGraphicsSnapshot(lastSnapshot,true);
      };
    }
    clear(true);
    MExec.plotView?.select(figure && axes ? String(figure.id) + ':' + String(axes.id) : null);
    hasFigure = !!figure;
    hasAxes = !!axes;
    const exportButton = document.querySelector('#plot-export');
    if (exportButton) exportButton.disabled = !hasFigure;
    if (!axes) { draw(); return true; }
    const lines = (axes.children || [])
      .map((id) => objects.find((object) => object.id === id))
      .filter((object) => object && object.type === 'Line');


    state.series = [];
    state.xlog = false;
    state.ylog = false;
    state.axisVisible = true;
    state.grid = false;
    state.xlim = null;
    state.ylim = null;
    const axesProperties = axes.properties || {};
    state.title = axesProperties.Title || '';
    state.xlabel = axesProperties.XLabel || '';
    state.ylabel = axesProperties.YLabel || '';
    if (Array.isArray(axesProperties.XLim) &&
        axesProperties.XLim.length >= 2) {
      state.xlim = axesProperties.XLim.slice(0, 2).map(Number);
    }
    if (Array.isArray(axesProperties.YLim) &&
        axesProperties.YLim.length >= 2) {
      state.ylim = axesProperties.YLim.slice(0, 2).map(Number);
    }
    for (const line of lines) {
      const properties = line.properties || {};
      const x = (properties.XData || []).map(v => v === null ? NaN : Number(v));
      const y = (properties.YData || []).map(v => v === null ? NaN : Number(v));
      if (!x.length || x.length !== y.length) continue;
      state.series.push({
        x,
        y,
        name:
          properties.DisplayName ||
          `series${state.series.length + 1}`,
        style: {
          color: colorFromGraphics(properties.Color),
          dash: dashFromGraphics(properties.LineStyle),
          hidden: properties.LineStyle === 'none',
        },
      });
    }

    draw();
    if (activate && (changed || selectionOnly) && figures.length && MExec.app && MExec.app.showTab) MExec.app.showTab('plot');
    return true;
  }

  function exportPng() {
    if (!hasFigure || !canvas()) return false;
    try {
      draw();
      const link = document.createElement('a');
      link.href = canvas().toDataURL('image/png');
      link.download = 'figure.png';
      link.click();
      return true;
    } catch (error) {
      MExec.command?.print('图形导出失败：' + error.message, 'err');
      return false;
    }
  }
  const exportButton = document.querySelector('#plot-export');
  if (exportButton) exportButton.onclick = exportPng;

  MExec.plotView?.attach(draw);

  return {
    exportPng,
    clear,
    draw,
    state,
    renderGraphicsSnapshot,
  };
})();
