// ImGui Filter's interface: stock Dear ImGui widgets, one per parameter.
// ImGui renders with its
// own WebGPU backend into the pass the presenter gives us: Dawn natively, the
// browser's WebGPU in WCLAP.
//
// Every edit reaches the plugin as a gesture: begin when a widget is grabbed,
// changes while it moves, end when it is let go (Ctrl+click a slider to type).
#include "gpu/params.h"
#include "gpu/view.h"
#include <imgui.h>
#include <imgui_impl_wgpu.h>
#include <algorithm>
#include <chrono>
#include <string>

namespace {

class FilterView : public gpu::View
{
public:
    ~FilterView() override
    {
        if (!context) return;
        ImGui::SetCurrentContext(context);
        ImGui_ImplWGPU_Shutdown();
        ImGui::DestroyContext(context); // also clears ImGui's current context
    }

    void prepare(const wgpu::Device &device, wgpu::TextureFormat format) override
    {
        // Each plugin instance has its own ImGui context. ImGui's current
        // context is global, so every entry point below selects ours first.
        context = ImGui::CreateContext();
        ImGuiIO &io = io_();
        io.IniFilename = nullptr; // no imgui.ini beside the host
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        font = gpu::loadAsset("/fonts/Inter.ttf");
        if (!font.empty())
        {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false; // `font` keeps the bytes
            io.Fonts->AddFontFromMemoryTTF(font.data(), int(font.size()), 15.0f, &config);
        }
        ImGui::StyleColorsDark();
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowPadding = {16, 16};
        style.FrameRounding = 4;
        style.GrabRounding = 4;

        ImGui_ImplWGPU_InitInfo init;
        init.Device = device.Get();
        init.RenderTargetFormat = static_cast<WGPUTextureFormat>(format);
        ImGui_ImplWGPU_Init(&init);
        lastFrame = std::chrono::steady_clock::now();

        params.send = send;
        send(core::Value::Map{{"type", "ready"}});
    }

    void receive(const core::Value &message) override { params.receive(message); }

    void pointer(const PointerEvent &event) override
    {
        if (!context) return;
        ImGuiIO &io = io_();
        modifiers(event.modifiers);
        io.AddMousePosEvent(event.x / scale, event.y / scale);
        if (event.type == Pointer::down) io.AddMouseButtonEvent(0, true);
        if (event.type == Pointer::up) io.AddMouseButtonEvent(0, false);
        if (event.type == Pointer::wheel) io.AddMouseWheelEvent(0, event.wheel);
        framesLeft = 4;
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
        framesLeft = 4;
        return io.WantCaptureKeyboard;
    }

    bool text(const char *utf8) override
    {
        if (!context) return false;
        ImGuiIO &io = io_();
        if (!io.WantTextInput) return false;
        io.AddInputCharactersUTF8(utf8);
        framesLeft = 4;
        return true;
    }

    // ImGui finishes reacting to input over a few frames; a text cursor blinks.
    bool animating() const override { return framesLeft > 0 || typing; }

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float newScale) override
    {
        gpu::Param *type = params.find("Type"), *cutoff = params.find("Cutoff"), *resonance = params.find("Resonance");
        if (!context || !type || !cutoff || !resonance) return; // until the plugin has sent its parameters
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
        ImGui::Begin("ImGui Filter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushItemWidth(-90); // leave room for the labels on the right

        int current = int(type->value);
        if (ImGui::Combo("Type", &current, "Lowpass\0Bandpass\0Highpass\0"))
            params.begin(*type), params.change(*type, current), params.end(*type);

        slider(*cutoff, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        slider(*resonance, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::PopItemWidth();
        resizeGrip(io);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
        typing = io.WantTextInput;
    }

private:
    ImGuiIO &io_() { ImGui::SetCurrentContext(context); return ImGui::GetIO(); }

    void modifiers(unsigned mods)
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Shift, mods & shift);
        io.AddKeyEvent(ImGuiMod_Ctrl, mods & (control | command)); // ImGui swaps Cmd for Ctrl on macOS itself
        io.AddKeyEvent(ImGuiMod_Alt, mods & alt);
    }

    // A grip in the bottom-right corner: dragging it asks the host for a new
    // window size (View::resizeTo). Hosts without resizing of their own, such
    // as AU hosts, need one.
    void resizeGrip(const ImGuiIO &io)
    {
        if (!resizeTo) return;
        const float size = 14;
        const ImVec2 corner = io.DisplaySize;
        ImGui::PushClipRect({0, 0}, corner, false); // the corner is outside the window's padded area
        ImGui::SetCursorScreenPos({corner.x - size, corner.y - size});
        ImGui::InvisibleButton("##resize", {size, size});
        if (ImGui::IsItemActivated()) gripOffset = {corner.x - io.MousePos.x, corner.y - io.MousePos.y};
        if (ImGui::IsItemActive()) resizeTo((io.MousePos.x + gripOffset.x) * scale, (io.MousePos.y + gripOffset.y) * scale);
        const ImU32 color = ImGui::GetColorU32(ImGui::IsItemActive() || ImGui::IsItemHovered() ? ImGuiCol_ResizeGripActive : ImGuiCol_TextDisabled);
        for (float d : {4.0f, 8.0f, 12.0f})
            ImGui::GetWindowDrawList()->AddLine({corner.x - 3 - d, corner.y - 3}, {corner.x - 3, corner.y - 3 - d}, color, 1.2f);
        ImGui::PopClipRect();
    }

    // A stock slider bound to a parameter, reporting its gesture.
    void slider(gpu::Param &p, const char *format, ImGuiSliderFlags flags)
    {
        double value = p.value;
        const bool changed = ImGui::SliderScalar(p.name.c_str(), ImGuiDataType_Double, &value, &p.min, &p.max,
                                                 format, flags | ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) params.begin(p);
        if (changed) params.change(p, value);
        if (ImGui::IsItemDeactivated()) params.end(p);
    }

    ImGuiContext *context = nullptr;
    std::string font;
    gpu::Params params;
    std::chrono::steady_clock::time_point lastFrame;
    float scale = 1;
    ImVec2 gripOffset;
    int framesLeft = 0;
    bool typing = false;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<FilterView>(); }
