#define CLAY_IMPLEMENTATION
#include "layout.h"
#include <cstdio>

namespace gpu {

Layout::Layout()
{
    // Clay_MinMemorySize and Clay_Initialize read Clay's global current
    // context, which may belong to another instance: size from the defaults.
    Clay_SetCurrentContext(nullptr);
    memory.resize(Clay_MinMemorySize());
    const Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(memory.size(), memory.data());
    context = Clay_Initialize(arena, {1, 1}, {[](Clay_ErrorData error) {
        std::fprintf(stderr, "Clay: %.*s\n", int(error.errorText.length), error.errorText.chars);
    }, nullptr});
}

Layout::~Layout()
{
    // Don't leave Clay pointing at memory that is about to be freed.
    if (Clay_GetCurrentContext() == context)
        Clay_SetCurrentContext(nullptr);
}

void Layout::begin(Draw2D &newDraw, float width, float height, float newScale)
{
    draw = &newDraw;
    scale = newScale;
    strings.clear();
    customs.clear();
    Clay_SetCurrentContext(context); // each plugin instance has its own
    Clay_SetMeasureTextFunction([](Clay_StringSlice text, Clay_TextElementConfig *config, void *self) -> Clay_Dimensions {
        const auto *layout = static_cast<Layout *>(self);
        const std::string_view characters{text.chars, size_t(text.length)};
        return {layout->draw->measure(config->fontSize, characters, config->fontId), config->lineHeight ? float(config->lineHeight) : config->fontSize * 1.25f};
    }, this);
    Clay_SetLayoutDimensions({width / scale, height / scale});
    Clay_SetPointerState({pointerX / scale, pointerY / scale}, pointerDown);
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::min(std::chrono::duration<float>(now - lastFrame).count(), 0.1f);
    lastFrame = now;
    Clay_UpdateScrollContainers(false, {0, wheelNotches * 40}, seconds); // a notch scrolls 40 points
    wheelNotches = 0;
    Clay_BeginLayout();
}

void Layout::pointer(float x, float y, bool down)
{
    pointerX = x, pointerY = y, pointerDown = down;
}

void Layout::wheel(float notches)
{
    wheelNotches += notches;
}

Clay_String Layout::text(std::string characters)
{
    const auto &kept = strings.emplace_back(std::move(characters));
    return {false, int32_t(kept.size()), kept.data()};
}

void *Layout::custom(int kind, void *data)
{
    return &customs.emplace_back(Custom{kind, data});
}

void Layout::end(const std::function<void(int, void *, Rect)> &onCustom)
{
    const Clay_RenderCommandArray commands = Clay_EndLayout();
    const float s = scale;
    auto color = [](Clay_Color c) { return Color{c.r / 255, c.g / 255, c.b / 255, c.a / 255}; };
    auto corners = [s](Clay_CornerRadius r) {
        return Draw2D::Corners{r.topLeft * s, r.topRight * s, r.bottomRight * s, r.bottomLeft * s};
    };
    for (int32_t i = 0; i < commands.length; ++i)
    {
        const Clay_RenderCommand &command = commands.internalArray[i];
        const Clay_BoundingBox &b = command.boundingBox;
        const Rect r{b.x * s, b.y * s, b.width * s, b.height * s};
        switch (command.commandType)
        {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            const auto &data = command.renderData.rectangle;
            draw->rect(r.x, r.y, r.w, r.h, corners(data.cornerRadius), color(data.backgroundColor));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const auto &data = command.renderData.border;
            const auto &w = data.width;
            draw->rect(r.x, r.y, r.w, r.h, corners(data.cornerRadius), {0, 0, 0, 0},
                       {w.left * s, w.right * s, w.top * s, w.bottom * s}, color(data.color));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            const auto &data = command.renderData.text;
            // Clay gives the line's box; Draw2D wants its baseline, which sits
            // about 0.95 of the font size below the top in a 1.25 line.
            const float lineHeight = data.lineHeight ? float(data.lineHeight) : data.fontSize * 1.25f;
            const float baseline = r.y + (lineHeight - data.fontSize * 1.25f) / 2 * s + data.fontSize * 0.95f * s;
            draw->text(r.x, baseline, data.fontSize * s, {data.stringContents.chars, size_t(data.stringContents.length)},
                       color(data.textColor), Draw2D::Align::left, data.fontId);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: draw->pushClip(r.x, r.y, r.w, r.h); break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: draw->popClip(); break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
            const auto *custom = static_cast<const Custom *>(command.renderData.custom.customData);
            onCustom(custom->kind, custom->data, r);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
            const auto &data = command.renderData.image;
            const Clay_Color tint = data.backgroundColor; // 0, 0, 0, 0 (the default) means untinted
            draw->image(int(reinterpret_cast<intptr_t>(data.imageData)) - 1, r.x, r.y, r.w, r.h, corners(data.cornerRadius),
                        tint.a > 0 ? tint.a / 255 : 1);
            break;
        }
        default: break;
        }
    }
}

}
