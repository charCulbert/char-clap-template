// Instrument WebView's page: knobs for Filter cutoff and Release, and a peak meter.
// It only knows the messages plugin.cpp documents:
//   from the plugin  {type: "params", params: [{id, name, min, max, default}]},
//                    {type: "values", values: [...]}, {type: "meter", peaks: [left, right]}
//   to the plugin    {type: "ready"}, {type: "begin" | "end", id}, {type: "change", id, value},
//                    {type: "resize", width, height, scale}: the page's new size in CSS pixels
import {sendMessage, onMessage} from './lib/messages.js';

// Each parameter's control, by its CLAP id (P_* in plugin.cpp), in order: a
// knob or an upright slider, its value as text, and optionally a shorter name
// to show, whether it's logarithmic, and the element it goes in (#controls
// unless it says; before the meter, if that's in there too).
const layout = [
  {id: 0, control: 'knob', label: 'Cutoff', log: true, text: v => v < 1000 ? `${v.toFixed(0)} Hz` : `${(v / 1000).toFixed(2)} kHz`},
  {id: 1, control: 'knob', log: true, text: v => v < 1 ? `${(v * 1000).toFixed(0)} ms` : `${v.toFixed(2)} s`},
];

const controls = [];

// Builds a knob or an upright slider for parameter p. Drag it (Shift for fine),
// double-click it for the default; focused, the arrow keys step it.
function control(p, {control: kind, text, label, log}) {
  const toUnit = v => log ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
  const fromUnit = u => log ? p.min * (p.max / p.min) ** u : p.min + u * (p.max - p.min);
  const element = document.createElement('div');
  element.className = kind;
  element.tabIndex = 0;
  element.setAttribute('role', 'slider');
  element.setAttribute('aria-label', p.name);
  element.innerHTML = kind === 'knob'
    ? `<svg viewBox="-26 -26 52 52"><path class="track" fill="none" stroke-width="5"/><path class="value" fill="none" stroke-width="5"/>
       <circle class="cap" r="18"/><line class="pointer" stroke-width="1.4"/></svg><output></output><span class="name"></span>`
    : `<output></output><div class="rail"><div class="fill"></div><div class="thumb"></div></div><span class="name"></span>`;
  element.querySelector('.name').textContent = label ?? p.name;
  const output = element.querySelector('output');
  let value = p.default, unit = toUnit(value), dragging = false;

  // The knob's ring: 270 degrees, from 7:30 to 4:30.
  const arc = u => {
    const a0 = 3.927, a1 = a0 + 4.712 * u, r = 21;
    const point = a => `${(Math.sin(a) * r).toFixed(2)} ${(-Math.cos(a) * r).toFixed(2)}`;
    return u <= 0 ? '' : `M ${point(a0)} A ${r} ${r} 0 ${a1 - a0 > Math.PI ? 1 : 0} 1 ${point(a1)}`;
  };
  if (kind === 'knob') element.querySelector('.track').setAttribute('d', arc(1));

  function show(v) {
    value = v; unit = Math.min(1, Math.max(0, toUnit(v)));
    output.textContent = text(v);
    element.setAttribute('aria-valuenow', v);
    element.setAttribute('aria-valuetext', text(v));
    if (kind === 'knob') {
      element.querySelector('.value').setAttribute('d', arc(unit));
      const a = 3.927 + 4.712 * unit, pointer = element.querySelector('.pointer');
      pointer.setAttribute('x1', Math.sin(a) * 5.4); pointer.setAttribute('y1', -Math.cos(a) * 5.4);
      pointer.setAttribute('x2', Math.sin(a) * 16.3); pointer.setAttribute('y2', -Math.cos(a) * 16.3);
    } else {
      element.querySelector('.fill').style.height = `${unit * 100}%`;
      element.querySelector('.thumb').style.bottom = `${unit * 100}%`;
    }
  }
  // A whole edit, for a click or a key: begin, the value, end.
  function set(u) {
    sendMessage({type: 'begin', id: p.id});
    change(u);
    sendMessage({type: 'end', id: p.id});
  }
  function change(u) {
    const v = fromUnit(Math.min(1, Math.max(0, u)));
    show(v);
    sendMessage({type: 'change', id: p.id, value: v});
  }

  element.addEventListener('pointerdown', event => {
    element.setPointerCapture(event.pointerId);
    element.focus();
    dragging = true;
    sendMessage({type: 'begin', id: p.id});
    // Up raises it. A slider's value jumps to the press first; a knob's doesn't.
    let last = event.clientY;
    const rail = element.querySelector('.rail')?.getBoundingClientRect();
    if (rail && event.clientY >= rail.top && event.clientY <= rail.bottom) change((rail.bottom - event.clientY) / rail.height);
    const move = e => {
      const span = (rail ? rail.height : 200) * (e.shiftKey ? 10 : 1);
      change(unit + (last - e.clientY) / span);
      last = e.clientY;
    };
    const up = () => {
      element.removeEventListener('pointermove', move);
      dragging = false;
      sendMessage({type: 'end', id: p.id});
    };
    element.addEventListener('pointermove', move);
    element.addEventListener('pointerup', up, {once: true});
  });
  element.addEventListener('dblclick', () => set(toUnit(p.default)));
  element.addEventListener('keydown', event => {
    const step = event.shiftKey ? 0.01 : 0.05;
    const u = {ArrowUp: unit + step, ArrowRight: unit + step, ArrowDown: unit - step, ArrowLeft: unit - step,
               Home: 0, End: 1, Delete: toUnit(p.default), Backspace: toUnit(p.default)}[event.key];
    if (u === undefined) return;
    event.preventDefault();
    set(u);
  });
  element.setAttribute('aria-valuemin', p.min);
  element.setAttribute('aria-valuemax', p.max);
  show(p.default);
  return {element, show: v => { if (!dragging) show(v); }};
}

function build(params) {
  controls.forEach(c => c.element.remove());
  for (const entry of layout) {
    const p = params.find(p => p.id === entry.id);
    if (!p) continue;
    controls[p.id] = control(p, entry);
    const container = document.querySelector(entry.into ?? '#controls');
    container.insertBefore(controls[p.id].element, container.querySelector(':scope > #meter'));
  }
}

// The meter: 0 to 1 is -60 to 0 dBFS. The bar falls back over about half a
// second, the peak line over two.
const bars = [...document.querySelectorAll('#meter .bar')].map(bar => ({bar, shown: 0, held: 0}));
function meter(peaks) {
  bars.forEach((b, c) => {
    const level = Math.min(1, Math.max(0, (20 * Math.log10((peaks[c] ?? 0) + 1e-9) + 60) / 60));
    b.shown = Math.max(level, b.shown - 0.06);
    b.held = Math.max(level, b.held - 0.015);
    b.bar.querySelector('.fill').style.height = `${b.shown * 100}%`;
    const peak = b.bar.querySelector('.peak');
    peak.style.bottom = `calc(${b.held * 100}% - 1px)`;
    peak.hidden = b.held <= 0;
  });
  document.getElementById('meter').setAttribute('aria-valuenow', Math.round(bars[0].shown * 60 - 60));
}

onMessage(message => {
  if (message.type === 'params') build(message.params);
  if (message.type === 'values') message.values.forEach((value, id) => controls[id]?.show(value));
  if (message.type === 'meter') meter(message.peaks);
});

// The corner grip asks the plugin for a new window size while it is dragged.
const grip = document.getElementById('resize');
grip.addEventListener('pointerdown', event => {
  grip.setPointerCapture(event.pointerId);
  const offsetX = innerWidth - event.clientX, offsetY = innerHeight - event.clientY;
  const move = e => sendMessage({type: 'resize', width: e.clientX + offsetX, height: e.clientY + offsetY,
                                 scale: devicePixelRatio});
  grip.addEventListener('pointermove', move);
  grip.addEventListener('pointerup', () => grip.removeEventListener('pointermove', move), {once: true});
});

sendMessage({type: 'ready'});
