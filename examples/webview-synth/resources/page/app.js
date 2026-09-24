// WebView Synth's page: a slider per parameter. It only knows the messages
// plugin.cpp documents:
//   from the plugin  {type: "params", params: [{id, name, min, max, default, unit}]},
//                    {type: "values", values: [...]}
//   to the plugin    {type: "ready"}, {type: "begin" | "end", id}, {type: "change", id, value},
//                    {type: "resize", width, height, scale}: the page's new size in CSS pixels
import {sendMessage, onMessage} from './lib/messages.js';

const sliders = [];

// Wide ranges above zero (like Release) feel right on a logarithmic slider.
const isLog = p => p.min > 0 && p.max / p.min >= 100;
const toSlider = (p, v) => isLog(p) ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
const fromSlider = (p, x) => isLog(p) ? p.min * (p.max / p.min) ** x : p.min + x * (p.max - p.min);
const format = (p, v) => `${v.toFixed(Math.abs(v) < 10 ? 2 : 1)} ${p.unit ?? ''}`;

function build(params) {
  const container = document.getElementById('params');
  container.replaceChildren();
  for (const p of params) {
    const row = document.createElement('label');
    row.innerHTML = '<span class="name"></span><input type="range" min="0" max="1" step="0.001"><output></output>';
    row.querySelector('.name').textContent = p.name;
    const input = row.querySelector('input'), output = row.querySelector('output');
    let dragging = false;
    // 'input' fires while moving, 'change' when the edit is committed. A host
    // needs begin/end around the values to record one automation gesture.
    input.addEventListener('input', () => {
      if (!dragging) { dragging = true; sendMessage({type: 'begin', id: p.id}); }
      const value = fromSlider(p, Number(input.value));
      output.textContent = format(p, value);
      sendMessage({type: 'change', id: p.id, value});
    });
    input.addEventListener('change', () => {
      if (dragging) { dragging = false; sendMessage({type: 'end', id: p.id}); }
    });
    input.addEventListener('dblclick', () => {
      sendMessage({type: 'begin', id: p.id});
      sendMessage({type: 'change', id: p.id, value: p.default});
      sendMessage({type: 'end', id: p.id});
      show(p.id, p.default);
    });
    container.append(row);
    sliders[p.id] = {p, input, output, isDragging: () => dragging};
  }
}

function show(id, value) {
  const slider = sliders[id];
  if (!slider) return;
  if (!slider.isDragging()) slider.input.value = toSlider(slider.p, value);
  slider.output.textContent = format(slider.p, value);
}

onMessage(message => {
  if (message.type === 'params') build(message.params);
  if (message.type === 'values') message.values.forEach((value, id) => show(id, value));
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
