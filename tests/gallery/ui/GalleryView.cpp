// GUI Gallery's interface: tabs, a theme switch, and one page per area of the
// framework. Every cell draws one feature and says underneath what it should
// look like, so a wrong drawing is visible at a glance.
#include "draw2d/ui.h"
#include "draw2d/views.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr float pi = 3.14159265f;

// A pipeline of the gallery's own, drawn in order with Draw2D's shapes through
// Draw2D::custom: diagonal stripes over the whole target, which the clip
// around the custom draw cuts to its cell.
constexpr const char *stripes = R"(
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let on = step(0.5, fract((pos.x + pos.y) / 16.0));
    return vec4f(vec3f(0.3, 0.5, 1.0) * on, 1.0);
}
)";

class GalleryView : public gpu::View
{
public:
    void prepare(const wgpu::Device &device, wgpu::TextureFormat format) override
    {
        ui.draw.prepare(device, format, gpu::loadAsset("/fonts/BarlowSemiCondensed-Medium.ttf"));
        mono = ui.draw.addFont(gpu::loadAsset("/fonts/IBMPlexMono-Regular.ttf"));
        wheel = ui.draw.addImage(gpu::loadAsset("/images/colour-wheel.png"));
        strip = ui.draw.addImage(gpu::loadAsset("/images/knob-strip.png"));
        // An icon, rasterized at 2x its 64-point size (sharp on 2x screens), in
        // white so the theme can tint it.
        speaker = ui.draw.addSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
            <path d="M4 9h4l5-4v14l-5-4H4z" fill="#fff"/>
            <path d="M16 8.5a5 5 0 0 1 0 7M18.5 6a8.5 8.5 0 0 1 0 12" fill="none" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>
        </svg>)", 128, 128);
        wgpu::ShaderSourceWGSL source;
        source.code = stripes;
        wgpu::ShaderModuleDescriptor moduleDescriptor;
        moduleDescriptor.nextInChain = &source;
        const wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);
        wgpu::ColorTargetState target;
        target.format = format;
        wgpu::FragmentState fragment;
        fragment.module = module;
        fragment.targetCount = 1;
        fragment.targets = &target;
        wgpu::RenderPipelineDescriptor pipelineDescriptor;
        pipelineDescriptor.vertex.module = module;
        pipelineDescriptor.fragment = &fragment;
        stripePipeline = device.CreateRenderPipeline(&pipelineDescriptor);
        params.send = send;
        ui.lockPointer = lockPointer;
        ui.setCursor = setCursor;
        ui.copyText = copyText;
        ui.dragFiles = dragFiles;
        ui.resizeTo = resizeTo;
        send(core::Value::Map{{"type", "ready"}});
    }

    void receive(const core::Value &message) override
    {
        if (params.receive(message)) return;
        if (message["type"].text() == "data") setData(message["data"]);
    }
    void pointer(const PointerEvent &event) override { ui.pointer(event); }
    bool key(Key key, unsigned modifiers) override { return ui.key(key, modifiers); }
    bool text(const char *utf8) override { return ui.text(utf8); }
    bool animating() const override { return ui.animating() || page >= 4; } // the meters and the views move
    bool editingText() const override { return ui.editingText(); }
    Caret textCaret() const override { return ui.textCaret(); }
    const gpu::Accessibility *accessibility() const override { return ui.accessibility(); }
    void accessAction(uint64_t id, gpu::Accessible::Action action, double value) override { ui.accessAction(id, action, value); }
    bool files(FileDrag type, float x, float y, const std::vector<std::string> &paths) override { return ui.files(type, x, y, paths); }

    // The frame is built in record, so the presenter can skip one that looks
    // like the last (most pointer moves); draw only flushes it.
    void draw(const wgpu::RenderPassEncoder &pass, float, float, float) override { ui.flush(pass); }
    bool record(float width, float height, float scale) override
    {
        gpu::Theme theme = ui.theme.withColors(themeMode == 0 ? darkMode : themeMode == 1);
        theme.valueFont = mono > 0 ? mono : 0;
        theme.whiteKey = keys == 0 ? gpu::Theme{}.whiteKey : theme.text; // a piano's, or the palette's inverted
        theme.blackKey = keys == 0 ? gpu::Theme{}.blackKey : theme.background;
        ui.theme = theme;
        ui.begin(width, height, scale);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(12),
                         .childGap = 12, .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = gpu::clayColor(theme.background)})
        {
            CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 12, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
            {
                static const char *const pages[] = {"Shapes", "Text", "Controls", "Layout", "More", "Views"};
                ui.segmented(pages, 6, page, 0);
                ui.label("Page");
                if (ui.toggle("Windows", windowsOpen)) mixerOpen = notesOpen = windowsOpen;
                ui.tooltip("Two windows over the page: drag, resize, Tab to a header and use the arrows");
                CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                static const char *const themes[] = {"System", "Dark", "Light"};
                ui.segmented(themes, 3, themeMode, 1);
                ui.label("Theme");
                ui.tooltip("System follows the OS's light or dark mode");
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 10,
                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (page == 0) shapesPage();
                if (page == 1) textPage();
                if (page == 2) controlsPage();
                if (page == 3) layoutPage();
                if (page == 4) morePage();
                if (page == 5) viewsPage();
            }
        }
        // Windows over the page, as compost's.
        ui.window("Settings", mixerFrame, mixerOpen, [&] {
            static const char *const rates[] = {"44.1", "48", "96"};
            ui.segmented(rates, 3, windowRate, 5);
            ui.label("Sample rate (kHz)");
            ui.toggle("Sync", sync, 1);
            if (ui.button("Reset", 1)) windowRate = 1, sync = false;
        });
        ui.window("Notes", notesFrame, notesOpen, [&] {
            label("A window keeps inside the view and comes to the front when pressed. Its header takes Tab: "
                  "the arrows move it, Alt and the arrows resize it, Escape closes it.",
                  ui.theme.text);
        }, {.minWidth = 160, .minHeight = 80});
        if (!mixerOpen && !notesOpen) windowsOpen = false;
        ui.resizeGrip(width, height);
        const bool changed = ui.record([&](int kind, void *data, gpu::Rect r) { drawCell(kind, data, r); });
        sendEdit();
        return changed;
    }

    // The state other than parameters, which the plugin keeps (and the host's
    // undo restores): one map, sent whole after each finished edit.
    core::Value::Map data() const
    {
        core::Value::Array points, steps;
        for (auto p : envelope) points.push_back(core::Value::Array{double(p.x), double(p.y)});
        for (float v : stepValues) steps.push_back(double(v));
        return {{"Envelope", points}, {"Steps", steps}, {"Name", name}, {"Sync", sync}, {"Mode", mode},
                {"Shape", shape}, {"Oversample", oversample}, {"Linear phase", linearPhase}};
    }
    void setData(const core::Value &value)
    {
        const auto *map = std::get_if<core::Value::Map>(&value.data);
        if (!map) return;
        auto flag = [&](const char *key, bool &out) {
            if (auto *b = std::get_if<bool>(&value[key].data)) out = *b;
        };
        if (auto *list = std::get_if<core::Value::Array>(&value["Envelope"].data); list && list->size() >= 2)
        {
            envelope.clear();
            for (const auto &p : *list)
                if (auto *xy = std::get_if<core::Value::Array>(&p.data); xy && xy->size() == 2)
                    envelope.push_back({std::clamp(float((*xy)[0].number()), 0.0f, 1.0f), std::clamp(float((*xy)[1].number()), 0.0f, 1.0f)});
            std::sort(envelope.begin(), envelope.end(), [](auto a, auto b) { return a.x < b.x; });
        }
        if (auto *list = std::get_if<core::Value::Array>(&value["Steps"].data); list && !list->empty())
        {
            stepValues.clear();
            for (const auto &v : *list) stepValues.push_back(std::clamp(float(v.number()), 0.0f, 1.0f));
        }
        if (auto *text = std::get_if<std::string>(&value["Name"].data)) name = *text;
        flag("Sync", sync), flag("Oversample", oversample), flag("Linear phase", linearPhase);
        mode = std::clamp(int(value["Mode"].number(mode)), 0, 1);
        shape = std::clamp(int(value["Shape"].number(shape)), 0, 4);
        sent = data(); // what the plugin has, so not an edit
    }
    // Once nothing is held, any difference from what the plugin has is an
    // edit, named after the first field that differs.
    void sendEdit()
    {
        if (ui.dragging()) return;
        core::Value::Map now = data();
        const char *changed = nullptr;
        for (const auto &[key, value] : now)
            if (core::encode(value) != core::encode(sent[key])) { changed = key.c_str(); break; }
        if (!changed) return;
        send(core::Value::Map{{"type", "edit"}, {"name", changed}, {"data", now}});
        sent = std::move(now);
    }

private:
    // Custom cells: what drawCell draws in each.
    enum Cell { radii, borders, circles, arcs, widths, polyline, lineUnderBox, alpha, sizes, aligned, clipped, customPass, gradients, shadows, fills };

    template <typename F> void row(F &&cells)
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 10}}) { cells(); }
    }

    // A cell: a panel with the feature in it and, under it, what it should look like.
    template <typename F> void cell(const char *expect, F &&content)
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(8),
                         .childGap = 6, .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = gpu::clayColor(ui.theme.panel), .cornerRadius = CLAY_CORNER_RADIUS(ui.theme.radius)})
        {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(30)}, .childGap = 8,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) { content(); }
            label(expect, ui.theme.dim);
        }
    }
    void custom(const char *expect, Cell kind)
    {
        cell(expect, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .custom = {.customData = ui.layout.custom(kind)}}) {}
        });
    }
    void label(const char *text, gpu::Color color, int size = 0, int font = 0) // size 0: the theme's
    {
        const auto points = uint16_t(size ? size : int(ui.theme.textSize));
        CLAY_TEXT(ui.layout.text(text), CLAY_TEXT_CONFIG({.textColor = gpu::clayColor(color), .fontId = uint16_t(font), .fontSize = points}));
    }

    void shapesPage()
    {
        row([&] {
            custom("Radius 0, 4, 12, pill", radii);
            custom("Borders 1, 2, 4 px; edges crisp", borders);
            custom("Circle, ring, filled ring", circles);
            custom("Arcs 90, 180, 270, 360 deg", arcs);
        });
        row([&] {
            custom("Lines 0.5, 1, 2, 4 px; even, no gaps", widths);
            custom("Polyline: round joins, no spikes", polyline);
            custom("Box drawn after line: line hidden under it", lineUnderBox);
            custom("Overlap blends: purple centre", alpha);
        });
        row([&] {
            const gpu::Color a = ui.theme.accent;
            cell("Clay corners 0, 8, 16, 24 (TL TR BR BL)", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .backgroundColor = gpu::clayColor(a),
                      .cornerRadius = {0, 8, 24, 16}}) {}
            });
            cell("Clay borders L1 R2 T4 B8", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                      .border = {.color = gpu::clayColor(a), .width = {1, 2, 4, 8, 0}}}) {}
            });
            cell("Clay rounded border: same radius inside", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                      .cornerRadius = CLAY_CORNER_RADIUS(12), .border = {.color = gpu::clayColor(a), .width = CLAY_BORDER_ALL(3)}}) {}
            });
            custom("Custom pipeline between shapes: stripes under the dot", customPass);
        });
        row([&] {
            custom("Gradients: linear, radial; even, no banding", gradients);
            custom("Shadows: soft, offset, under the box", shadows);
            custom("Fills: a concave L; the area under a curve", fills);
            cell("Clay box with Layout::shadow", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .backgroundColor = gpu::clayColor(ui.theme.background),
                      .cornerRadius = CLAY_CORNER_RADIUS(6), .userData = ui.layout.shadow(ui.theme.shadow, 12, 0, 4)}) {}
            });
        });
    }

    void textPage()
    {
        row([&] { custom("Names (Barlow) and numbers (Plex Mono), 9 to 32 px: readable, none blurry", sizes); });
        row([&] {
            custom("Left, centre, right on the blue line", aligned);
            cell("Clay wraps at word breaks", [&] {
                CLAY_TEXT(CLAY_STRING("Clay measures each word with Draw2D and breaks lines between words, "
                                      "so this paragraph fills the box and wraps cleanly."),
                          CLAY_TEXT_CONFIG({.textColor = gpu::clayColor(ui.theme.text), .fontSize = 13}));
            });
            cell("Unicode: a box only where the font has no glyph (Barlow lacks the arrow)", [&] {
                label("Café · 20 µs · −6 dB · 90° · ±3 · ½ · “quotes” · → ∞", ui.theme.text, 14);
            });
        });
        row([&] {
            cell("Clip: the text stops at the box edge", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(180), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(4)},
                      .backgroundColor = gpu::clayColor(ui.theme.background), .clip = {.horizontal = true, .vertical = true}})
                {
                    label("This line is far too long to fit in its box and should be cut off at the edge", ui.theme.text, 14);
                }
            });
            custom("Draw2D clip: circle cut to the square", clipped);
        });
    }

    void controlsPage()
    {
        static const char *const names[] = {"Gain", "Mix", "Cutoff", "Drive"};
        static const char *const formats[] = {"%.1f dB", "%.0f %%", "%.0f Hz", "%.1f"};
        row([&] {
            cell("Knobs: drag, Shift fine, wheel, double-click resets, Tab + arrows", [&] {
                for (int i = 0; i < 4; ++i)
                {
                    ui.knob(params, params.find(names[i]), formats[i], i == 2);
                    ui.tooltip(names[i]);
                }
            });
        });
        row([&] {
            cell("Sliders (compost style): fill to the value, thumb thickens on hover", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 6,
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    for (int i = 0; i < 4; ++i) ui.slider(params, params.find(names[i]), formats[i], i == 2);
                }
            });
        });
        row([&] {
            cell("Button: counts clicks; release outside cancels", [&] {
                if (ui.button("Click me")) ++clicks;
                ui.tooltip("Press and release on the button");
                char count[32];
                std::snprintf(count, sizeof(count), "%d clicks", clicks);
                label(count, ui.theme.text, 0, ui.theme.valueFont);
            });
            cell("Toggle and segmented", [&] {
                ui.toggle("Sync", sync);
                static const char *const modes[] = {"Ratio", "Diff"};
                ui.segmented(modes, 2, mode, 2);
                ui.label("Mode");
            });
            cell("Dropdown: opens over the knobs, blocks them", [&] {
                static const char *const shapes[] = {"Sine", "Triangle", "Saw", "Square", "Noise"};
                ui.dropdown(shapes, 5, shape);
                ui.label("Waveform");
            });
            cell("Text field: type, arrows, Enter keeps, Esc drops", [&] {
                if (ui.textField(name, 110)) ++edits;
                ui.label("Preset name");
                char count[32];
                std::snprintf(count, sizeof(count), "%d edits", edits);
                label(count, ui.theme.dim, 0, ui.theme.valueFont);
            });
        });
    }

    // A second of 440 Hz at -18 dB, as a 16-bit WAV in the temp folder: what
    // the chip drags out.
    static std::string writeTone()
    {
        const uint32_t rate = 48000, bytes = rate * 2;
        std::vector<int16_t> samples(rate);
        for (uint32_t i = 0; i < rate; ++i) samples[i] = int16_t(std::lround(4096 * std::sin(6.283185307179586 * 440 * i / rate)));
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "GUI Gallery tone.wav";
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); }; // little-endian hosts
        auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
        out.write("RIFF", 4), u32(36 + bytes), out.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
        out.write("data", 4), u32(bytes), out.write(reinterpret_cast<const char *>(samples.data()), bytes);
        return out ? path.string() : std::string();
    }

    void layoutPage()
    {
        row([&] {
            cell("Scroll: wheel scrolls the list, clipped to the box", [&] {
                CLAY({.id = CLAY_ID("List"), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                      .backgroundColor = gpu::clayColor(ui.theme.background),
                      .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    for (int i = 0; i < 40; ++i)
                    {
                        const Clay_ElementId rowId = CLAY_IDI("ListRow", i);
                        CLAY({.id = rowId, .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {8, 8, 4, 4}},
                              .backgroundColor = gpu::clayColor(Clay_PointerOver(rowId) ? ui.theme.panel : gpu::Color{0, 0, 0, 0})})
                        {
                            label(("Preset " + std::to_string(i + 1)).c_str(), ui.theme.text);
                        }
                    }
                }
            });
            cell("Images: PNG square, rounded by Clay, faded; SVG icon tinted by the theme", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(64)}}, .image = {.imageData = gpu::Layout::image(wheel)}}) {}
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(64)}}, .cornerRadius = CLAY_CORNER_RADIUS(32),
                      .image = {.imageData = gpu::Layout::image(wheel)}}) {}
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(64)}}, .backgroundColor = {255, 255, 255, 128},
                      .cornerRadius = CLAY_CORNER_RADIUS(10), .image = {.imageData = gpu::Layout::image(wheel)}}) {}
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(64)}}, .backgroundColor = gpu::clayColor(ui.theme.text),
                      .image = {.imageData = gpu::Layout::image(speaker)}}) {}
            });
        });
        row([&] {
            cell("Piano: press and slide; held notes light (C4 lit)", [&] {
                static const char *const styles[] = {"Ivory", "Palette"};
                ui.segmented(styles, 2, keys, 3);
                ui.label("Key colours");
                const auto notes = ui.piano(48, 83, 126, [&](int n) { return n == 60 || n == held; });
                if (notes.on >= 0) held = notes.on;
                if (notes.off >= 0 && notes.off == held) held = -1;
            });
        });
        row([&] {
            cell("Files: drop on the box; drag the chip out", [&] {
                const Clay_ElementId box = CLAY_ID("DropBox"), chip = CLAY_ID("ToneChip");
                std::vector<std::string> paths;
                if (ui.dropTarget(box, paths))
                {
                    dropped.clear();
                    for (const std::string &path : paths) dropped += (dropped.empty() ? "" : ", ") + std::filesystem::path(path).filename().string();
                }
                CLAY({.id = box, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40)}, .padding = {8, 8, 0, 0},
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                      .backgroundColor = gpu::clayColor(ui.theme.background), .cornerRadius = CLAY_CORNER_RADIUS(ui.theme.radius),
                      .clip = {.horizontal = true},
                      .border = {.color = gpu::clayColor(ui.fileOver(box) ? ui.theme.accent : ui.theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
                {
                    label(dropped.empty() ? "Drop files here" : dropped.c_str(), dropped.empty() ? ui.theme.dim : ui.theme.text);
                }
                ui.dragSource(chip, [] {
                    const std::string tone = writeTone();
                    return tone.empty() ? std::vector<std::string>{} : std::vector<std::string>{tone};
                });
                CLAY({.id = chip, .layout = {.padding = {8, 8, 6, 6}}, .backgroundColor = gpu::clayColor(ui.theme.accent),
                      .cornerRadius = CLAY_CORNER_RADIUS(ui.theme.radius)})
                {
                    label("tone.wav", ui.theme.background);
                }
            });
            cell("Grow, fit and fixed sizes share a row", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(40), CLAY_SIZING_FIXED(40)}}, .backgroundColor = gpu::clayColor(ui.theme.accent)}) {}
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40)}}, .backgroundColor = gpu::clayColor(ui.theme.edge)}) {}
                CLAY({.layout = {.padding = CLAY_PADDING_ALL(6)}, .backgroundColor = gpu::clayColor(ui.theme.background)})
                {
                    label("fit", ui.theme.text);
                }
            });
            cell("Split: drag a divider; focused: arrows, Home, End, Enter collapses", [&] {
                auto pane = [&](const char *name) {
                    return [this, name] {
                        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(6)},
                              .backgroundColor = gpu::clayColor(ui.theme.background)}) { label(name, ui.theme.dim); }
                    };
                };
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(120)}}})
                {
                    ui.split("Sides", sides, pane("Left: a pane's text wraps to its width as the divider moves"), [&] {
                        ui.split("Stack", stack, pane("Top"), pane("Bottom"), {.stacked = true, .min = 40, .secondMin = 40});
                    });
                }
            });
        });
    }

    // Views of a signal, fed made-up data: drawing only, as the views are.
    void viewsPage()
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        if (stereoPeaks.empty()) // a plucked tone, the right a little later
            for (int c = 0; c < 2; ++c)
            {
                stereoPeaks.emplace_back();
                for (int i = 0; i < 600; ++i)
                {
                    const float x = float(i) / 600 - 0.1f * float(c), level = x < 0 ? 0 : std::exp(-5 * x) * (0.9f - 0.3f * float(c));
                    const float wobble = 0.8f + 0.2f * std::sin(float(i) * 0.37f);
                    stereoPeaks.back().push_back({-level * wobble, level});
                }
            }
        for (int i = 0; i < 256; ++i) // two cycles and a harmonic, drifting
        {
            const float phase = float(i) / 256 * 4 * 3.14159265f + t * 2;
            scopeSamples[size_t(i)] = 0.7f * std::sin(phase) + 0.2f * std::sin(3 * phase);
        }
        // A tone sweeping up and down, with its harmonics, over a noise floor.
        const float sweep = 200 * std::pow(20.0f, 0.5f - 0.5f * std::cos(t * 0.8f));
        for (size_t i = 0; i < binHz.size(); ++i)
        {
            binHz[i] = float(i + 1) * 24000 / float(binHz.size());
            float level = -95 + 6 * std::sin(float(i) * 12.9898f + t * 78.233f);
            for (int h = 1; h <= 4; ++h)
            {
                const float octaves = std::log2(binHz[i] / (sweep * float(h)));
                level = std::max(level, -6 * float(h) - 400 * octaves * octaves);
            }
            binDb[i] = level;
        }
        spectrogram.add(binDb.data(), binHz.data(), binDb.size());
        row([&] {
            cell("Waveform: a min and max per bucket, left above right", [&] { gpu::waveform(ui, "Pluck", stereoPeaks, {}, 110); });
            cell("Scope: samples over a grid; 1 at the top, -1 at the bottom", [&] { gpu::scope(ui, "Oscillator", scopeSamples.data(), scopeSamples.size(), {.range = 1}, 110); });
        });
        row([&] {
            cell("Spectrogram: one column a frame, newest on the right; the analysis is the caller's", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 8, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY({.layout = {.childGap = 12, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
                    {
                        static const char *const palettes[] = {"System", "Bone", "Inferno", "Viridis", "Magma"};
                        static const char *const scales[] = {"Linear", "Log", "Mel"};
                        gpu::Spectrogram::Options o = spectrogram.settings();
                        int palette = int(o.palette), scale = int(o.scale);
                        if (ui.segmented(palettes, 5, palette, 7)) o.palette = gpu::Spectrogram::Palette(palette), spectrogram.configure(o);
                        ui.label("Palette");
                        if (ui.segmented(scales, 3, scale, 8)) o.scale = gpu::Spectrogram::Scale(scale), spectrogram.configure(o);
                        ui.label("Frequency scale");
                    }
                    spectrogram.draw(ui, "Sweep", 180);
                }
            });
        });
    }

    void morePage()
    {
        static const char *const names[] = {"Gain", "Mix", "Cutoff", "Drive"};
        static const char *const formats[] = {"%.1f dB", "%.0f %%", "%.0f Hz", "%.1f"};
        row([&] {
            cell("Number boxes: drag right or up, Shift fine, double-click resets, type a number or Enter to edit", [&] {
                for (int i = 0; i < 4; ++i) ui.numberBox(params, params.find(names[i]), formats[i], i == 2);
            });
            cell("Envelope: drag points; double-click adds or removes", [&] { ui.envelope(envelope); });
            cell("Steps: drag across to paint", [&] { ui.steps(stepValues); });
            cell("Drawer: the header opens and closes it; drag its edge, or focus it, to resize", [&] {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    ui.drawer("Advanced", advancedOpen, advancedSize, [&] {
                        ui.toggle("Oversample", oversample);
                        ui.toggle("Linear phase", linearPhase);
                    }, {.min = 40, .max = 140});
                }
            });
        });
        row([&] {
            cell("Vertical sliders", [&] {
                for (int i = 0; i < 4; ++i) ui.vslider(params, params.find(names[i]), formats[i], i == 2);
            });
            cell("XY pad: drag the point; double-click resets", [&] {
                ui.xyPad(params, params.find("Mix"), params.find("Gain"));
            });
            cell("Filmstrip knobs (64 frames): drag like the others", [&] {
                for (int i = 0; i < 2; ++i)
                {
                    ui.imageKnob(params, params.find(names[i]), strip, 64);
                    ui.tooltip(names[i]);
                }
            });
            cell("Meters: level and peak (animated)", [&] {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                for (int i = 0; i < 6; ++i)
                {
                    const float level = 0.5f + 0.45f * float(std::sin(t * (1.3 + 0.4 * i) + i));
                    peaks[i] = std::max(level, peaks[i] - 0.005f);
                    ui.meter(level, peaks[i], 8);
                }
            });
        });
    }

    void drawCell(int kind, void *, gpu::Rect r)
    {
        auto &d = ui.draw;
        const float s = ui.s, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        const gpu::Color a = ui.theme.accent, t = ui.theme.text;
        const float w = (r.w - 30 * s) / 4; // four samples across, 10 points apart
        switch (kind)
        {
        case radii:
            for (int i = 0; i < 4; ++i)
            {
                const float h = std::min(r.h, w), radius[] = {0, 4 * s, 12 * s, h / 2};
                d.rect(r.x + i * (w + 10 * s), cy - h / 2, w, h, radius[i], a);
            }
            break;
        case borders:
            for (int i = 0; i < 3; ++i)
            {
                const float bw = (r.w - 20 * s) / 3, h = std::min(r.h, bw), widths[] = {1, 2, 4};
                d.rect(r.x + i * (bw + 10 * s), cy - h / 2, bw, h, 6 * s, {0, 0, 0, 0}, widths[i] * s, t);
            }
            break;
        case circles:
        {
            const float radius = std::min(r.h / 2, (r.w - 20 * s) / 6);
            d.circle(r.x + radius, cy, radius, a);
            d.circle(cx, cy, radius, {0, 0, 0, 0}, 2 * s, t);
            d.circle(r.x + r.w - radius, cy, radius, a, 3 * s, t);
            break;
        }
        case arcs:
            for (int i = 0; i < 4; ++i)
            {
                const float radius = std::min(r.h / 2, w / 2);
                d.arc(r.x + i * (w + 10 * s) + w / 2, cy, radius - 3 * s, 4 * s, 0, (i + 1) * pi / 2, a);
            }
            break;
        case widths:
        {
            const float lw[] = {0.5f, 1, 2, 4};
            for (int i = 0; i < 4; ++i)
            {
                const float y = r.y + (i + 0.5f) * r.h / 4;
                d.line(r.x, y, r.x + r.w, y - r.h / 8, lw[i] * s, t);
            }
            break;
        }
        case polyline:
        {
            float xy[20];
            for (int i = 0; i < 10; ++i)
                xy[2 * i] = r.x + r.w * i / 9, xy[2 * i + 1] = i % 2 ? r.y + 4 * s : r.y + r.h - 4 * s;
            d.polyline(xy, 10, 3 * s, a);
            break;
        }
        case lineUnderBox:
            d.line(r.x, r.y, r.x + r.w, r.y + r.h, 4 * s, t);
            d.rect(cx - r.w / 4, r.y, r.w / 2, r.h, 6 * s, ui.theme.edge);
            break;
        case alpha:
        {
            const float radius = std::min(r.h / 2, r.w / 3);
            d.circle(cx - radius / 2, cy, radius, {0.9f, 0.2f, 0.2f, 0.6f});
            d.circle(cx + radius / 2, cy, radius, {0.2f, 0.3f, 0.95f, 0.6f});
            break;
        }
        case sizes:
        {
            float y = r.y;
            for (float size : {9.0f, 11.0f, 13.0f, 16.0f, 20.0f, 32.0f})
            {
                y += size * 1.2f * s;
                if (y > r.y + r.h) break;
                char line[64];
                std::snprintf(line, sizeof(line), "%.0f px  Cutoff  Resonance  ", size);
                const float x = r.x + d.text(r.x, y, size * s, line, t, gpu::Draw2D::Align::left, ui.theme.labelFont);
                d.text(x, y, size * s, "-12.5 dB  440 Hz  1:3", t, gpu::Draw2D::Align::left, ui.theme.valueFont);
            }
            break;
        }
        case aligned:
            d.line(cx, r.y, cx, r.y + r.h, 1 * s, {0.3f, 0.5f, 1});
            d.text(cx, r.y + r.h * 0.3f, 13 * s, "Left", t, gpu::Draw2D::Align::left);
            d.text(cx, r.y + r.h * 0.6f, 13 * s, "Centre", t, gpu::Draw2D::Align::center);
            d.text(cx, r.y + r.h * 0.9f, 13 * s, "Right", t, gpu::Draw2D::Align::right);
            break;
        case clipped:
        {
            const float side = std::min(r.w, r.h) * 0.6f;
            d.rect(cx - side / 2, cy - side / 2, side, side, 0, {0, 0, 0, 0}, 1 * s, ui.theme.edge);
            d.pushClip(cx - side / 2, cy - side / 2, side, side);
            d.circle(cx + side / 2, cy + side / 2, side * 0.6f, a);
            d.popClip();
            break;
        }
        case gradients:
        {
            const float half = (r.w - 10 * s) / 2;
            d.rect(r.x, r.y, half, r.h, gpu::Draw2D::Corners{6 * s, 6 * s, 6 * s, 6 * s},
                   gpu::Draw2D::Gradient{ui.theme.background, a, r.x, r.y, r.x + half, r.y + r.h});
            const float cxr = r.x + half + 10 * s + half / 2;
            d.rect(r.x + half + 10 * s, r.y, half, r.h, gpu::Draw2D::Corners{6 * s, 6 * s, 6 * s, 6 * s},
                   gpu::Draw2D::Gradient{a, ui.theme.background, cxr, cy, cxr + half / 2, cy, true});
            break;
        }
        case shadows:
        {
            const float side = std::min(r.h, r.w / 2) * 0.7f;
            d.shadow(r.x + r.w * 0.25f - side / 2, cy - side / 2 + 4 * s, side, side, 6 * s, 14 * s, ui.theme.shadow);
            d.rect(r.x + r.w * 0.25f - side / 2, cy - side / 2, side, side, 6 * s, ui.theme.panel);
            d.shadow(r.x + r.w * 0.75f - side / 2, cy - side / 2, side, side, side / 2, 6 * s, {0, 0, 0, 0.8f});
            d.circle(r.x + r.w * 0.75f, cy, side / 2, a);
            break;
        }
        case fills:
        {
            const float side = std::min(r.h, r.w / 2.2f);
            const float l[] = {r.x, r.y, r.x + side, r.y, r.x + side, r.y + side * 0.35f, r.x + side * 0.35f, r.y + side * 0.35f,
                               r.x + side * 0.35f, r.y + side, r.x, r.y + side};
            d.fill(l, 6, a);
            std::vector<float> curve;
            const float x0 = r.x + side + 10 * s, span = r.x + r.w - x0;
            for (int i = 0; i <= 48; ++i)
            {
                const float u = i / 48.0f;
                curve.push_back(x0 + span * u);
                curve.push_back(r.y + r.h * (0.55f - 0.4f * std::sin(u * 6.2831853f) * std::exp(-2 * u)));
            }
            curve.insert(curve.end(), {x0 + span, r.y + r.h, x0, r.y + r.h});
            d.fill(curve.data(), curve.size() / 2, {a.r, a.g, a.b, 0.35f});
            d.polyline(curve.data(), 49, 1.5f * s, a);
            break;
        }
        case customPass:
            d.pushClip(r.x, r.y, r.w, r.h);
            d.custom([this](const wgpu::RenderPassEncoder &pass) {
                pass.SetPipeline(stripePipeline);
                pass.Draw(3);
            }, 1); // version 1 for good: the stripes never change

            d.popClip();
            d.circle(cx, cy, std::min(r.w, r.h) / 4, a);
            break;
        }
    }

    std::string dropped; // names of the files last dropped on the box
    int page = 0, themeMode = 0, keys = 0, mode = 0, shape = 0, clicks = 0, edits = 0, mono = 0, wheel = -1, strip = -1, speaker = -1;
    int held = -1; // the piano note held down
    std::vector<gpu::Ui::Point> envelope = {{0, 0}, {0.08f, 1}, {0.3f, 0.6f}, {0.8f, 0.6f}, {1, 0}};
    float sides = 220, stack = 50; // the split's sizes (points)
    float advancedSize = 70;       // the drawer's
    bool windowsOpen = false, mixerOpen = false, notesOpen = false;
    int windowRate = 1;
    gpu::Rect mixerFrame{60, 90, 220, 190}, notesFrame{320, 140, 260, 150}; // points
    std::vector<float> stepValues = {0.8f, 0.2f, 0.5f, 0.2f, 1.0f, 0.2f, 0.5f, 0.3f, 0.7f, 0.2f, 0.5f, 0.2f, 0.9f, 0.4f, 0.6f, 0.3f};
    bool sync = false, advancedOpen = true, oversample = false, linearPhase = true;
    float peaks[6] = {};
    std::vector<std::vector<gpu::Peak>> stereoPeaks;
    std::vector<float> scopeSamples = std::vector<float>(256), binHz = std::vector<float>(512), binDb = std::vector<float>(512);
    gpu::Spectrogram spectrogram;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::string name = "Preset 1";
    core::Value::Map sent = data(); // the data the plugin has
    gpu::Params params;
    gpu::Ui ui;
    wgpu::RenderPipeline stripePipeline;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<GalleryView>(); }
