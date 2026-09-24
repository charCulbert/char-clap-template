// The plugin's page. It only knows the messages plugin.cpp documents:
//   from the plugin  {type: "params", params: [...]}, {type: "values", values: [...]}
//   to the plugin    {type: "ready"}, {type: "begin" | "end", id}, {type: "change", id, value}
import {sendMessage, onMessage} from './lib/messages.js';

const sliders = [];

// Wide ranges above zero (like Cutoff) feel right on a logarithmic slider.
const isLog = p => p.min > 0 && p.max / p.min >= 100;
const toSlider = (p, v) => isLog(p) ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
const fromSlider = (p, x) => isLog(p) ? p.min * (p.max / p.min) ** x : p.min + x * (p.max - p.min);
const format = (p, v) => isLog(p) ? `${v.toFixed(0)} Hz` : v.toFixed(2);

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

// The live spectrum: bands in dB from 20 Hz to 20 kHz, drawn as a filled curve.
const spectrum = document.getElementById('spectrum');
const context = spectrum.getContext('2d');
function drawSpectrum(db) {
  const r = devicePixelRatio, w = spectrum.clientWidth * r, h = spectrum.clientHeight * r;
  if (spectrum.width !== w || spectrum.height !== h) spectrum.width = w, spectrum.height = h;
  const y = d => h * (1 - (Math.max(d, -96) + 96) / 108); // -96 dB .. +12 dB
  const line = new Path2D();
  db.forEach((d, i) => line.lineTo(i / (db.length - 1) * w, y(d)));
  const fill = new Path2D(line);
  fill.lineTo(w, h);
  fill.lineTo(0, h);
  context.clearRect(0, 0, w, h);
  context.fillStyle = 'rgba(95, 180, 162, 0.25)';
  context.fill(fill);
  context.strokeStyle = '#5fb4a2';
  context.lineWidth = 1.5 * r;
  context.stroke(line);
}

onMessage(message => {
  if (message.type === 'params') build(message.params);
  if (message.type === 'values') message.values.forEach((value, id) => show(id, value));
  if (message.type === 'spectrum') drawSpectrum(message.db);
});
sendMessage({type: 'ready'});
