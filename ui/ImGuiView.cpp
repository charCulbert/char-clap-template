// simpleCharClap's GPU interface drawn with Dear ImGui instead of gpu/ui.h: the
// same plot and knobs as FilterView.cpp, for comparison. ImGui renders with its
// own WebGPU backend into the pass the presenter gives us, natively (Dawn) and
// in WCLAP (the browser's WebGPU).
//
// Drag the plot (left/right: Cutoff, up/down: the resonant peak, i.e. Q) or the
// knobs. Knobs: Shift for fine, wheel, double-click for the default. The value
// boxes under them are ImGui drags: click (or Ctrl+click) to type a value.
#include "../gpu/params.h"
#include "../gpu/view.h"
#include <imgui.h>
#include <imgui_impl_wgpu.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr float minHz = 20, maxHz = 20000, minDb = -48, maxDb = 24;

// The plot's frequency scale: Signalsmith's hz / (1500 + hz), as a 0..1
// position across the plot and back (as in FilterView.cpp).
float hzToUnit(float hz)
{
    auto warp = [](float f) { return f / (1500 + f); };
    return (warp(hz) - warp(minHz)) / (warp(maxHz) - warp(minHz));
}
float unitToHz(float t)
{
    const float lo = minHz / (1500 + minHz), hi = maxHz / (1500 + maxHz);
    const float w = lo + t * (hi - lo);
    return 1500 * w / (1 - w);
}
// The TPT state-variable lowpass's exact magnitude response in dB.
float responseDb(float hz, float cutoff, float q, float sampleRate)
{
    const float pi = 3.14159265f;
    const float fc = std::clamp(cutoff, 1.0f, sampleRate * 0.48f);
    const float w = std::tan(pi * std::min(hz, sampleRate * 0.499f) / sampleRate) / std::tan(pi * fc / sampleRate);
    const float k = 1 / std::max(q, 1e-3f);
    return -10 * std::log10((1 - w * w) * (1 - w * w) + (w * k) * (w * k));
}
// The spectrum's level scale, Signalsmith's: 0 dBFS at the top, as 0..1 from the bottom.
float levelUnit(float dbfs)
{
    const float v = std::pow(10.0f, 0.012f * dbfs);
    return 0.6f * v / (0.4f + 0.2f * v);
}

double toUnit(const gpu::Param &p, double v, bool log)
{
    if (p.max <= p.min) return 0;
    if (log && p.min > 0) return std::log(v / p.min) / std::log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}
double fromUnit(const gpu::Param &p, double u, bool log)
{
    u = std::clamp(u, 0.0, 1.0);
    if (log && p.min > 0) return p.min * std::pow(p.max / p.min, u);
    return p.min + u * (p.max - p.min);
}

const ImU32 accent = IM_COL32(94, 181, 163, 255), dim = IM_COL32(140, 155, 168, 255), track = IM_COL32(51, 59, 66, 255);

class ImGuiView : public gpu::View
{
public:
    ~ImGuiView() override
    {
        if (!context) return;
        ImGui::SetCurrentContext(context);
        ImGui_ImplWGPU_Shutdown();
        ImGui::DestroyContext(context); // also clears ImGui's current context
    }

    void prepare(const wgpu::Device &device, wgpu::TextureFormat format) override
    {
        // Each plugin instance has its own ImGui context; ImGui's "current
        // context" is global, so every entry point below selects ours first.
        context = ImGui::CreateContext();
        ImGuiIO &io = io_();
        io.IniFilename = nullptr; // no imgui.ini next to the host
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDragClickToInputText = true; // a click without dragging types a value
        font = gpu::loadAsset("/fonts/Inter.ttf");
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false; // `font` keeps the bytes
        if (!font.empty()) io.Fonts->AddFontFromMemoryTTF(font.data(), int(font.size()), 14.0f, &config);

        ImGui::StyleColorsDark();
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowPadding = {0, 0};
        style.WindowBorderSize = 0;
        style.FrameRounding = 4;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.09f, 1);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.13f, 0.15f, 1);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.21f, 1);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.23f, 0.26f, 1);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.37f, 0.71f, 0.64f, 0.5f);
        style.Colors[ImGuiCol_NavCursor] = ImVec4(0.37f, 0.71f, 0.64f, 1);

        ImGui_ImplWGPU_InitInfo init;
        init.Device = device.Get();
        init.RenderTargetFormat = static_cast<WGPUTextureFormat>(format);
        ImGui_ImplWGPU_Init(&init);
        lastFrame = std::chrono::steady_clock::now();

        params.send = send;
        send(webview::Value::Map{{"type", "ready"}});
    }

    void receive(const webview::Value &message) override
    {
        if (params.receive(message) || message["type"].text() != "spectrum") return;
        if (auto db = std::get_if<webview::Value::Array>(&message["db"].data))
            for (size_t i = 0; i < spectrum.size() && i < db->size(); ++i)
                spectrum[i] = float((*db)[i].number(-120));
    }

    void pointer(const PointerEvent &event) override
    {
        if (!context) return;
        ImGuiIO &io = io_();
        modifiers(event.modifiers);
        io.AddMousePosEvent(event.x / scale, event.y / scale);
        if (event.type == Pointer::down) io.AddMouseButtonEvent(0, true);
        if (event.type == Pointer::up) io.AddMouseButtonEvent(0, false);
        if (event.type == Pointer::wheel) io.AddMouseWheelEvent(0, event.wheel);
        settle();
    }

    bool key(Key key, unsigned mods) override
    {
        if (!context) return false;
        static const ImGuiKey keys[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                                        ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_Home, ImGuiKey_End, ImGuiKey_Tab,
                                        ImGuiKey_Enter, ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_Delete};
        ImGuiIO &io = io_();
        modifiers(mods);
        // Presenters report presses only; ImGui spreads the pair over frames.
        io.AddKeyEvent(keys[int(key)], true);
        io.AddKeyEvent(keys[int(key)], false);
        settle();
        return io.WantCaptureKeyboard;
    }

    bool text(const char *utf8) override
    {
        if (!context) return false;
        ImGuiIO &io = io_();
        if (!io.WantTextInput) return false;
        io.AddInputCharactersUTF8(utf8);
        settle();
        return true;
    }

    // ImGui finishes reacting to input over a few frames (queued events,
    // hover); a text field's cursor blinks.
    bool animating() const override { return framesLeft > 0 || typing; }

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float newScale) override
    {
        auto *cutoff = params.find("Cutoff");
        auto *q = params.find("Q");
        if (!context || !cutoff || !q) return;
        scale = newScale;
        ImGuiIO &io = io_();
        io.DisplaySize = {width / scale, height / scale};
        io.DisplayFramebufferScale = {scale, scale};
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - lastFrame).count());
        lastFrame = now;
        if (framesLeft > 0) --framesLeft;

        ImGui_ImplWGPU_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("simpleCharClap", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float side = 112;
        plot(*cutoff, *q, {io.DisplaySize.x - side, io.DisplaySize.y});
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("side", {side, 0}, ImGuiChildFlags_None);
        ImGui::SetCursorPosY(std::max(12.0f, (io.DisplaySize.y - 2 * 128) / 2));
        knob(*cutoff, "%.0f Hz", true);
        knob(*q, "%.2f", false);
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
        typing = io.WantTextInput;
    }

private:
    ImGuiIO &io_() { ImGui::SetCurrentContext(context); return ImGui::GetIO(); }
    void settle() { framesLeft = 4; }
    void modifiers(unsigned mods)
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Shift, mods & shift);
        io.AddKeyEvent(ImGuiMod_Ctrl, mods & (control | command)); // ImGui swaps Cmd for Ctrl on macOS itself
        io.AddKeyEvent(ImGuiMod_Alt, mods & alt);
    }

    // A whole edit at once: begin, change, end (wheel, double-click).
    void set(gpu::Param &p, double value) { params.begin(p), params.change(p, value), params.end(p); }

    // A rotary knob: an invisible button drawn with ImGui's draw list, plus a
    // value box under it that ImGui can edit as text.
    void knob(gpu::Param &p, const char *format, bool log)
    {
        ImGui::PushID(p.id);
        const float r = 30, width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX((width - 2 * r) / 2);
        const ImVec2 at = ImGui::GetCursorScreenPos(), centre = {at.x + r, at.y + r};
        ImGui::InvisibleButton("knob", {2 * r, 2 * r});
        const ImGuiIO &io = ImGui::GetIO();
        const bool fine = io.KeyShift;
        const double unit = toUnit(p, p.value, log);
        if (ImGui::IsItemActivated())
        {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) set(p, p.defaultValue);
            else params.begin(p), dragging = true, grabUnit = unit, grabY = io.MousePos.y, grabFine = fine;
            if (dragging && lockPointer) lockPointer(true);
        }
        if (ImGui::IsItemActive() && dragging)
        {
            if (fine != grabFine) grabUnit = unit, grabY = io.MousePos.y, grabFine = fine; // no jump on Shift
            const double value = fromUnit(p, grabUnit + (grabY - io.MousePos.y) / (200 * (fine ? 10 : 1)), log);
            if (value != p.value) params.change(p, value);
        }
        if (ImGui::IsItemDeactivated() && dragging)
        {
            params.end(p), dragging = false;
            if (lockPointer) lockPointer(false);
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0)
            set(p, fromUnit(p, unit + io.MouseWheel * (fine ? 0.002 : 0.02), log));

        // 270 degrees of travel, from 7:30 to 4:30 (ImGui angles: 0 = right, clockwise).
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const float from = 0.75f * 3.14159265f, sweep = 1.5f * 3.14159265f, u = float(toUnit(p, p.value, log));
        const float thick = 4, arcR = r - thick;
        draw->PathArcTo(centre, arcR, from, from + sweep, 48);
        draw->PathStroke(track, thick);
        if (u > 0)
        {
            draw->PathArcTo(centre, arcR, from, from + sweep * u, 48);
            draw->PathStroke(accent, thick);
        }
        const float angle = from + sweep * u, inner = arcR - 2.6f * thick;
        draw->AddCircleFilled(centre, inner, IM_COL32(28, 33, 38, 255), 48);
        draw->AddCircle(centre, inner, ImGui::IsItemActive() ? accent : track, 48, 1);
        draw->AddLine({centre.x + std::cos(angle) * inner * 0.35f, centre.y + std::sin(angle) * inner * 0.35f},
                      {centre.x + std::cos(angle) * inner * 0.85f, centre.y + std::sin(angle) * inner * 0.85f},
                      IM_COL32_WHITE, 2);

        // The value, typed or dragged; each edit is one gesture.
        double value = p.value;
        ImGui::SetCursorPosX(8);
        ImGui::SetNextItemWidth(width - 16);
        const double speed = (p.max - p.min) / 400;
        const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, float(speed), &p.min, &p.max,
                                               format, ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
        if (ImGui::IsItemActivated()) params.begin(p);
        if (changed) params.change(p, value);
        if (ImGui::IsItemDeactivated()) params.end(p);
        const float nameWidth = ImGui::CalcTextSize(p.name.c_str()).x;
        ImGui::SetCursorPosX((width - nameWidth) / 2);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(dim), "%s", p.name.c_str());
        ImGui::Dummy({0, 10});
        ImGui::PopID();
    }

    // The frequency response over the live spectrum, with axis labels.
    void plot(gpu::Param &cutoff, gpu::Param &q, ImVec2 size)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("plot", size);
        const ImVec2 lo = {at.x + 34, at.y + 10}, hi = {at.x + size.x - 10, at.y + size.y - 22};
        const float w = hi.x - lo.x, h = hi.y - lo.y;
        auto xOf = [&](float hz) { return lo.x + w * hzToUnit(hz); };
        auto yOfGain = [&](float db) { return hi.y - h * (db - minDb) / (maxDb - minDb); };
        auto yOfLevel = [&](float dbfs) { return hi.y - h * levelUnit(dbfs); };

        const ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsItemActivated())
        {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) set(cutoff, cutoff.defaultValue), set(q, q.defaultValue);
            else params.begin(cutoff), params.begin(q), draggingPlot = true;
        }
        if (ImGui::IsItemActive() && draggingPlot)
        {
            const float tx = std::clamp((io.MousePos.x - lo.x) / w, 0.0f, 1.0f);
            const float ty = std::clamp((hi.y - io.MousePos.y) / h, 0.0f, 1.0f);
            params.change(cutoff, unitToHz(tx));
            params.change(q, std::pow(10.0f, (minDb + ty * (maxDb - minDb)) / 20.0f));
        }
        if (ImGui::IsItemDeactivated() && draggingPlot) params.end(cutoff), params.end(q), draggingPlot = false;
        if (ImGui::IsItemHovered() && io.MouseWheel != 0)
        {
            const float db = 20 * std::log10(float(q.value)) + io.MouseWheel * (io.KeyShift ? 0.1f : 1.0f);
            set(q, std::pow(10.0f, db / 20));
        }

        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(at, {at.x + size.x, at.y + size.y}, true);
        // Grid: frequencies, and the spectrum's level scale.
        for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
            draw->AddLine({xOf(hz), lo.y}, {xOf(hz), hi.y}, IM_COL32(255, 255, 255, 14));
        for (float dbfs : {-12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -80.0f})
            draw->AddLine({lo.x, yOfLevel(dbfs)}, {hi.x, yOfLevel(dbfs)}, IM_COL32(255, 255, 255, 14));

        // The spectrum: bands evenly spaced on the plot's scale, filled below.
        points.clear();
        const size_t n = spectrum.size();
        for (size_t i = 0; i < n; ++i)
            points.push_back({lo.x + w * (float(i) + 0.5f) / float(n), std::clamp(yOfLevel(spectrum[i]), lo.y, hi.y)});
        for (size_t i = 0; i + 1 < n; ++i)
            draw->AddQuadFilled(points[i], points[i + 1], {points[i + 1].x, hi.y}, {points[i].x, hi.y}, IM_COL32(115, 153, 184, 40));
        draw->AddPolyline(points.data(), int(n), IM_COL32(115, 153, 184, 140), 1);

        // The filter's response, one point per 2 points of width, and its peak.
        points.clear();
        for (float x = 0; x <= w; x += 2)
        {
            const float db = responseDb(unitToHz(x / w), float(cutoff.value), float(q.value), 48000);
            points.push_back({lo.x + x, std::clamp(yOfGain(db), lo.y - 4, hi.y + 4)});
        }
        draw->AddPolyline(points.data(), int(points.size()), IM_COL32(94, 181, 163, 60), 6);
        draw->AddPolyline(points.data(), int(points.size()), IM_COL32(170, 250, 235, 255), 2);
        const float peakDb = responseDb(float(cutoff.value), float(cutoff.value), float(q.value), 48000);
        draw->AddCircleFilled({xOf(float(cutoff.value)), yOfGain(peakDb)}, draggingPlot ? 8.0f : 6.0f, IM_COL32(170, 250, 235, 255));
        draw->PopClipRect();

        // Labels: frequency below, spectrum level left, filter gain right.
        const float fontSize = 11;
        auto label = [&](ImVec2 p, ImU32 color, const char *text, float align) {
            const float tw = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0, text).x;
            draw->AddText(ImGui::GetFont(), fontSize, {p.x - tw * align, p.y}, color, text);
        };
        for (auto [hz, text] : {std::pair{50.0f, "50"}, {100.0f, "100"}, {200.0f, "200"}, {500.0f, "500"},
                                {1000.0f, "1k"}, {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"}})
            label({xOf(hz), hi.y + 6}, dim, text, 0.5f);
        for (float dbfs : {0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -80.0f})
        {
            char text[8];
            std::snprintf(text, sizeof(text), "%.0f", dbfs);
            label({lo.x - 6, std::max(yOfLevel(dbfs), lo.y + 4) - 6}, dim, text, 1);
        }
        for (float gain : {12.0f, 0.0f, -12.0f})
        {
            char text[12];
            std::snprintf(text, sizeof(text), gain > 0 ? "+%.0f dB" : "%.0f dB", gain);
            label({hi.x - 4, yOfGain(gain) - 12}, accent, text, 1);
        }
    }

    ImGuiContext *context = nullptr;
    std::string font;
    gpu::Params params;
    std::vector<float> spectrum = std::vector<float>(256, -120.0f); // dB per band, from the plugin
    std::vector<ImVec2> points;
    std::chrono::steady_clock::time_point lastFrame;
    float scale = 1;
    int framesLeft = 0;
    bool typing = false, dragging = false, grabFine = false, draggingPlot = false;
    double grabUnit = 0;
    float grabY = 0;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<ImGuiView>(); }
