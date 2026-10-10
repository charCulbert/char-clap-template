// Ui text editing: stb_textedit setup, text fields, number boxes and typed values.
#include "ui_internal.h"

// stb_textedit does the editing (caret, selection, word moves, undo) on the
// text as codepoints, one line; Ui keeps draft and its offsets in step.
namespace gpu {
struct TextBuffer
{
    std::u32string text;
    std::function<float(char32_t)> width; // in points
};
}
#define STB_TEXTEDIT_CHARTYPE char32_t
#define STB_TEXTEDIT_STRING gpu::TextBuffer
#define STB_TEXTEDIT_UNDOSTATECOUNT 64
#define STB_TEXTEDIT_UNDOCHARCOUNT 1024
#include "stb_textedit.h"

static void layoutRow(StbTexteditRow *row, gpu::TextBuffer *buffer, int start)
{
    float width = 0;
    for (size_t i = size_t(start); i < buffer->text.size(); ++i) width += buffer->width(buffer->text[i]);
    *row = {};
    row->x1 = width, row->ymax = row->baseline_y_delta = 1;
    row->num_chars = int(buffer->text.size()) - start;
}
#define STB_TEXTEDIT_STRINGLEN(b) int((b)->text.size())
#define STB_TEXTEDIT_LAYOUTROW(row, b, start) layoutRow(row, b, start)
#define STB_TEXTEDIT_GETWIDTH(b, start, i) ((b)->width((b)->text[size_t((start) + (i))]))
#define STB_TEXTEDIT_KEYTOTEXT(key) -1 // text comes in through stb_textedit_paste
#define STB_TEXTEDIT_GETCHAR(b, i) ((b)->text[size_t(i)])
#define STB_TEXTEDIT_NEWLINE U'\n'
#define STB_TEXTEDIT_IS_SPACE(c) ((c) == U' ' || (c) == U'\t')
#define STB_TEXTEDIT_DELETECHARS(b, i, n) ((b)->text.erase(size_t(i), size_t(n)))
#define STB_TEXTEDIT_INSERTCHARS(b, i, chars, n) ((b)->text.insert(size_t(i), (chars), size_t(n)), 1)
#define STB_TEXTEDIT_K_LEFT 0x200000
#define STB_TEXTEDIT_K_RIGHT 0x200001
#define STB_TEXTEDIT_K_UP 0x200002
#define STB_TEXTEDIT_K_DOWN 0x200003
#define STB_TEXTEDIT_K_PGUP 0x200004
#define STB_TEXTEDIT_K_PGDOWN 0x200005
#define STB_TEXTEDIT_K_LINESTART 0x200006
#define STB_TEXTEDIT_K_LINEEND 0x200007
#define STB_TEXTEDIT_K_TEXTSTART 0x200008
#define STB_TEXTEDIT_K_TEXTEND 0x200009
#define STB_TEXTEDIT_K_DELETE 0x20000A
#define STB_TEXTEDIT_K_BACKSPACE 0x20000B
#define STB_TEXTEDIT_K_UNDO 0x20000C
#define STB_TEXTEDIT_K_REDO 0x20000D
#define STB_TEXTEDIT_K_WORDLEFT 0x20000E
#define STB_TEXTEDIT_K_WORDRIGHT 0x20000F
#define STB_TEXTEDIT_K_SHIFT 0x400000
#define STB_TEXTEDIT_IMPLEMENTATION
#include "stb_textedit.h"

namespace gpu {

struct TextEdit
{
    TextBuffer buffer;
    STB_TexteditState state;
    bool selecting = false; // a press in the field is dragging out a selection
    int typing = -1;        // the undo record a run of typing started, so the run undoes as one step
};

// Folds the undo records from `first` on into one, while each continues where the last ended.
static void joinUndo(StbUndoState &u, int first)
{
    while (first >= 0 && u.undo_point - 1 > first)
    {
        StbUndoRecord &a = u.undo_rec[u.undo_point - 2], &b = u.undo_rec[u.undo_point - 1];
        if (b.insert_length != 0 || a.where + a.delete_length != b.where) return;
        a.delete_length += b.delete_length;
        --u.undo_point;
    }
}

Ui::Ui() : textEdit(std::make_unique<TextEdit>()) {}
Ui::~Ui() = default;

static std::u32string toCodepoints(std::string_view utf8)
{
    std::u32string out;
    for (size_t i = 0; i < utf8.size();)
    {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        char32_t cp = extra ? c & (0x3F >> extra) : c;
        size_t j = i + 1;
        for (; j < utf8.size() && j <= i + size_t(extra) && (utf8[j] & 0xC0) == 0x80; ++j) cp = cp << 6 | (utf8[j] & 0x3F);
        out.push_back(cp);
        i = j;
    }
    return out;
}
static std::string toUtf8(std::u32string_view text)
{
    std::string out;
    for (char32_t c : text)
    {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        else out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

// Applies this frame's typing to the field being edited, if it's this one.
// Returns true when the edit is committed with a changed value.
bool Ui::editKeys(uint32_t field, std::string &value, bool overField, float textX)
{
    if (editing != field) return false;
    TextEdit &t = *textEdit;
    TextBuffer &b = t.buffer;
    b.width = [this](char32_t c) {
        const std::string one = toUtf8(std::u32string_view(&c, 1));
        return draw.measure(theme.textSize, one, theme.valueFont);
    };
    bool committed = false;
    auto finish = [&](bool keep) {
        draft = toUtf8(b.text);
        if (keep && draft != value) value = draft, committed = true;
        editing = 0;
        // This frame's keys were the field's (Enter or Escape among them): not
        // also its control's, which may have focus again. Tab still moves on.
        std::erase_if(keys, [](auto k) { return k.first != View::Key::tab; });
    };
    auto selected = [&] {
        const int from = std::min(t.state.select_start, t.state.select_end), to = std::max(t.state.select_start, t.state.select_end);
        return toUtf8(std::u32string_view(b.text).substr(size_t(from), size_t(to - from)));
    };
    if (mouse.pressed && !overField) finish(true);
    else if (!std::isnan(textX))
    {
        const float x = mouse.x / s - textX;
        if (mouse.pressed && mouse.doubleClicked) // the whole text, as a word would be one in a short field
            t.state.select_start = 0, t.state.select_end = t.state.cursor = int(b.text.size()), t.selecting = false;
        else if (mouse.pressed) stb_textedit_click(&b, &t.state, x, 0), t.selecting = true;
        if (mouse.pressed) t.typing = -1;
        else if (mouse.down && t.selecting) stb_textedit_drag(&b, &t.state, x, 0);
        if (!mouse.down) t.selecting = false;
    }
    for (const auto &[text, key, modifiers] : edits) // in the order they came
    {
        if (editing != field) break;
        if (!text.empty())
        {
            std::u32string typed = toCodepoints(text);
            std::erase_if(typed, [](char32_t c) { return c < 0x20 || c == 0x7F; }); // one line
            if (typed.empty()) continue;
            const int before = t.state.undostate.undo_point;
            if (t.typing < 0 || t.typing >= before) t.typing = before;
            stb_textedit_paste(&b, &t.state, typed.data(), int(typed.size()));
            joinUndo(t.state.undostate, t.typing);
            continue;
        }
        t.typing = -1;
        const int shift = modifiers & View::shift ? STB_TEXTEDIT_K_SHIFT : 0;
        const bool word = modifiers & (View::alt | View::control), line = modifiers & View::command;
        switch (key)
        {
        case View::Key::left: stb_textedit_key(&b, &t.state, (line ? STB_TEXTEDIT_K_LINESTART : word ? STB_TEXTEDIT_K_WORDLEFT : STB_TEXTEDIT_K_LEFT) | shift); break;
        case View::Key::right: stb_textedit_key(&b, &t.state, (line ? STB_TEXTEDIT_K_LINEEND : word ? STB_TEXTEDIT_K_WORDRIGHT : STB_TEXTEDIT_K_RIGHT) | shift); break;
        case View::Key::up: case View::Key::home: case View::Key::pageUp: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_TEXTSTART | shift); break;
        case View::Key::down: case View::Key::end: case View::Key::pageDown: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_TEXTEND | shift); break;
        case View::Key::backspace: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_BACKSPACE); break;
        case View::Key::deleteForward: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_DELETE); break;
        case View::Key::undo: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_UNDO); break;
        case View::Key::redo: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_REDO); break;
        case View::Key::selectAll: t.state.select_start = 0, t.state.select_end = t.state.cursor = int(b.text.size()); break;
        case View::Key::copy: case View::Key::cut:
            if (t.state.select_start != t.state.select_end)
            {
                if (copyText) copyText(selected());
                if (key == View::Key::cut) stb_textedit_cut(&b, &t.state);
            }
            break;
        case View::Key::enter: finish(true); break;
        case View::Key::escape: finish(false); break;
        default: break;
        }
    }
    if (editing == field)
    {
        draft = toUtf8(b.text);
        auto bytes = [&](int i) { return toUtf8(std::u32string_view(b.text).substr(0, size_t(std::clamp(i, 0, int(b.text.size()))))).size(); };
        caret = bytes(t.state.cursor);
        selectFrom = bytes(std::min(t.state.select_start, t.state.select_end));
        selectTo = bytes(std::max(t.state.select_start, t.state.select_end));
    }
    return committed;
}

// Typing that started an edit: the keys that came with it are the edit's
// (Enter commits it), not the control's that has just let them go.
void Ui::keysToEdit()
{
    for (auto [key, modifiers] : keys)
        if (key != View::Key::tab) edits.push_back({{}, key, modifiers});
    std::erase_if(keys, [](auto k) { return k.first != View::Key::tab; });
}

void Ui::startEditing(uint32_t field, std::string text, bool selectAll)
{
    if (editing && editing != field) pendingField = editing, pendingDraft = draft; // see textField
    editing = field, draft = std::move(text), focus = nullptr, fieldScroll = 0;
    TextEdit &t = *textEdit;
    t.buffer.text = toCodepoints(draft);
    t.typing = -1;
    stb_textedit_initialize_state(&t.state, 1);
    const int length = int(t.buffer.text.size());
    t.state.cursor = length;
    if (selectAll) t.state.select_start = 0, t.state.select_end = length;
    t.selecting = false;
    caret = draft.size(), selectFrom = selectAll ? 0 : caret, selectTo = caret;
}

// A value as it's typed: 2.5, not 2.500000.
// Typed and shown values use '.' whatever locale the host set (a decimal
// comma would show "2,5" and read "2.5" as 2); a typed ',' counts as '.'.
static char decimalPoint() { return *std::localeconv()->decimal_point; }

static bool parseNumber(std::string text, double &value)
{
    for (char &c : text)
        if (c == '.' || c == ',') c = decimalPoint();
    char *end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str();
}

std::string Ui::plainNumber(double value)
{
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%.6f", value);
    std::string text = formatted;
    std::replace(text.begin(), text.end(), decimalPoint(), '.');
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}

// A knob's or slider's value, which a click opens for typing (as compost's
// editable values): returns true while it's being edited, drawn here.
// id tells apart controls of the same parameter (a knob and a slider of it).
// A focused knob or slider: Enter opens its value for typing, and a digit
// starts typing a new one, as a number box does.
void Ui::typeInto(const void *me, const void *entry, const Param &p)
{
    if (focus != me) return;
    const uint32_t field = uint32_t(reinterpret_cast<uintptr_t>(entry)) | 1u;
    if (editing == field) return;
    if (!typedToFocus.empty()) return startEditing(field, std::exchange(typedToFocus, {})), keysToEdit();
    const auto enter = std::find_if(keys.begin(), keys.end(), [](auto k) { return k.first == View::Key::enter; });
    if (enter != keys.end()) keys.erase(enter), startEditing(field, plainNumber(p.value), true); // not also its commit
}

bool Ui::valueEntry(Params &params, Param &p, const void *control, const void *id, Rect r, Draw2D::Align align)
{
    const uint32_t field = uint32_t(reinterpret_cast<uintptr_t>(id)) | 1u;
    const bool isOver = over(r.x, r.y, r.w, r.h);
    if (press(id, isOver, doubleClicks).pressed && editing != field) startEditing(field, plainNumber(p.value), true);
    std::string typed = plainNumber(p.value);
    const bool wasEditing = editing == field;
    if (editKeys(field, typed, isOver))
    {
        if (double value; parseNumber(typed, value)) set(params, p, value);
    }
    // Typing ended with Enter or Escape: the keys are the control's again
    // (a click elsewhere takes them away as usual).
    if (wasEditing && editing != field && !mouse.pressed) focus = control;
    if (editing != field) return false;
    const float size = theme.textSize * s, width = draw.measure(size, draft, theme.valueFont);
    const float x = align == Draw2D::Align::center ? r.x + (r.w - width) / 2 : align == Draw2D::Align::right ? r.x + r.w - width : r.x;
    draw.rect(std::min(x, r.x + r.w / 2) - 3 * s, r.y, std::max(width, r.w / 2) + 6 * s, r.h, theme.radius * s, theme.background,
              1 * s, theme.accent);
    if (selectTo > selectFrom)
    {
        const float from = x + draw.measure(size, std::string_view(draft).substr(0, selectFrom), theme.valueFont);
        const float to = x + draw.measure(size, std::string_view(draft).substr(0, selectTo), theme.valueFont);
        draw.rect(from, r.y + 2 * s, to - from, r.h - 4 * s, 0, {theme.accent.r, theme.accent.g, theme.accent.b, 0.35f});
    }
    draw.text(x, r.y + r.h / 2 + 0.35f * size, size, draft, theme.text, Draw2D::Align::left, theme.valueFont);
    const float caretX = x + draw.measure(size, std::string_view(draft).substr(0, caret), theme.valueFont);
    draw.rect(caretX, r.y + 2 * s, 1 * s, r.h - 4 * s, 0, theme.accent);
    caretAt = {caretX, r.y, r.h}; // for an input method's window
    return true;
}

// The text inside a field: the draft and its caret while editing, else shown.
void Ui::fieldText(const std::string &shown, bool edit)
{
    const float lineHeight = theme.textSize * 1.25f;
    auto run = [&](size_t from, size_t to) {
        if (to > from) CLAY_TEXT(layout.text(draft.substr(from, to - from)), textConfig(theme.text, theme.valueFont));
    };
    auto caretBar = [&] {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(lineHeight)}}, .backgroundColor = clayColor(theme.accent)}) {}
    };
    if (!edit)
    {
        if (!shown.empty()) CLAY_TEXT(layout.text(shown), textConfig(theme.text, theme.valueFont));
        else CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(lineHeight)}}}) {}
        return;
    }
    if (selectFrom == selectTo)
    {
        run(0, caret), caretBar(), run(caret, draft.size());
        return;
    }
    run(0, selectFrom);
    if (caret == selectFrom) caretBar();
    const Color tint = {theme.accent.r, theme.accent.g, theme.accent.b, 0.35f};
    CLAY({.backgroundColor = clayColor(tint)}) { run(selectFrom, selectTo); }
    if (caret == selectTo) caretBar();
    run(selectTo, draft.size());
}

bool Ui::textField(std::string &value, float widthPoints, int index)
{
    const Clay_ElementId fid = id("TextField", index);
    const Area a = element(fid);
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(fid.id)), Accessible::Role::textInput, nullptr);
    d.element = fid.id, d.node.value = value;
    bool committed = false;
    // Clicking from one field to another keeps the first one's edit, which
    // it picks up the next time it's declared.
    if (pendingField == fid.id)
    {
        committed = value != pendingDraft;
        value = std::move(pendingDraft);
        pendingField = 0;
    }
    const void *me = reinterpret_cast<const void *>(uintptr_t(fid.id));
    order.push_back(me); // Tab reaches it, and starts editing it
    // After an edit ends with Enter or Escape it keeps focus without editing,
    // as a page's field keeps it: Enter edits it again, and typing goes on at
    // its end.
    if (focus != me && restingField == fid.id) restingField = 0;
    const bool resting = focus == me && restingField == fid.id;
    if ((a.pressed || a.doubleClicked) && editing != fid.id) startEditing(fid.id, value);
    else if (focus == me && editing != fid.id && !resting) startEditing(fid.id, value, true);
    else if (resting && editing != fid.id)
    {
        focusUses = keyBit(View::Key::enter), focusTakesDigits = false, focusTakesLetters = true;
        const bool enter = std::any_of(keys.begin(), keys.end(), [](auto k) { return k.first == View::Key::enter; });
        if (!typedToFocus.empty()) startEditing(fid.id, value), edits.push_back({std::exchange(typedToFocus, {}), {}, 0}), keysToEdit();
        else if (enter) startEditing(fid.id, value, true);
    }
    const bool wasEditing = editing == fid.id;
    const Clay_BoundingBox box = Clay_GetElementData(fid).boundingBox; // last frame's
    committed |= editKeys(fid.id, value, Clay_PointerOver(fid), box.x + 5 - fieldScroll);
    if (wasEditing && editing != fid.id && !mouse.pressed)
        focus = me, restingField = fid.id, focusUses = keyBit(View::Key::enter), focusTakesDigits = false, focusTakesLetters = true;
    const bool edit = editing == fid.id, hot = Clay_PointerOver(fid);
    if (hot && !active) cursor = View::Cursor::text;
    // Text longer than the field scrolls to keep the caret in view, as far
    // as it must and no further; it shows its start again when not edited.
    float scroll = 0;
    if (edit)
    {
        const float visible = widthPoints - 10 - 1; // less the padding and the caret
        const float caretX = draw.measure(theme.textSize, draft.substr(0, caret), theme.valueFont);
        const float textWidth = draw.measure(theme.textSize, draft, theme.valueFont);
        fieldScroll = std::clamp(fieldScroll, caretX - visible, caretX);
        scroll = fieldScroll = std::clamp(fieldScroll, 0.0f, std::max(0.0f, textWidth - visible));
        caretAt = {(box.x + 5 - scroll + caretX) * s, box.y * s, box.height * s};
    }
    CLAY({.id = fid, .layout = {.sizing = {CLAY_SIZING_FIXED(widthPoints), CLAY_SIZING_FIT(0)}, .padding = {5, 5, 2, 2},
                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius), .clip = {.horizontal = true, .childOffset = {-scroll, 0}},
          .border = {.color = clayColor(edit ? theme.accent : hot ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        fieldText(value, edit);
    }
    lastElement = fid.id;
    return committed;
}

// Drag up or down for the value (Shift for fine), as ImGui's DragFloat;
// double-click to type one.
void Ui::numberBox(Params &params, Param *p, const char *format, bool log, float widthPoints)
{
    if (!p) return;
    const Clay_ElementId nid = id("Number", p->id);
    const void *me = reinterpret_cast<const void *>(uintptr_t(nid.id));
    char shown[32];
    std::snprintf(shown, sizeof(shown), format, p->value);
    Described &self = describeParam(me, params, *p, shown, {});
    self.node.role = Accessible::Role::spinButton, self.element = nid.id;
    std::string typed = plainNumber(p->value);
    const std::string before = typed;
    if (pendingField == nid.id) typed = std::move(pendingDraft), pendingField = 0;
    order.push_back(me);
    const Area a = element(nid, eachPress); // a second press too: a double-click is told by mouse.doubleClicked
    const bool fine = mouse.modifiers & View::shift, edit = editing == nid.id;
    if (a.pressed || a.doubleClicked) focus = me, pressFocused = true;
    if (focus == me)
        focusUses = stepKeys | keyBit(View::Key::enter) | keyBit(View::Key::backspace) | keyBit(View::Key::deleteForward)
                  | (numberDrag == nid.id ? keyBit(View::Key::escape) : 0), // Escape cancels a drag, and only that
        focusTakesDigits = true;
    auto endDrag = [&] {
        numberDrag = 0, held = {};
        params.end(*p);
        if (lockPointer) lockPointer(false);
    };
    // A drag (right or up, past a few points' dead zone) changes it, Shift
    // for fine and Escape to cancel; a double-click resets it, and a
    // double-click that drags is fine (as compost's); clicks do nothing else.
    if (a.pressed && !edit)
    {
        grabSecond = mouse.doubleClicked;
        grabX = mouse.x, grabY = mouse.y, grabUnit = toUnit(*p, p->value, log), grabFine = fine || grabSecond;
        numberDrag = nid.id, numberMoved = false, numberStart = p->value;
        params.begin(*p);
        if (lockPointer) lockPointer(true);
        held = {&params, p, nullptr, true};
    }
    else if (a.held && numberDrag == nid.id)
    {
        const bool slow = fine || grabSecond;
        if (slow != grabFine) grabX = mouse.x, grabY = mouse.y, grabUnit = toUnit(*p, p->value, log), grabFine = slow;
        const float moved = (mouse.x - grabX) + (grabY - mouse.y);
        numberMoved |= std::abs(moved) > 4 * s;
        if (numberMoved)
        {
            const double u = grabUnit + moved / (180 * s) * (slow ? 0.1 : 1);
            if (u > 1) grabUnit -= u - 1; // past an end: turning back moves it at once
            if (u < 0) grabUnit -= u;
            const double value = fromUnit(*p, std::clamp(u, 0.0, 1.0), log);
            if (value != p->value) params.change(*p, value);
        }
    }
    if (a.released && numberDrag == nid.id)
    {
        if (grabSecond && !numberMoved) params.change(*p, p->defaultValue); // a double-click: reset
        endDrag();
    }
    // Keys while it has focus: arrows step (Alt a large step), Page Up and
    // Down step large, Home and End go to the ends, Delete or Backspace
    // reset it, Enter or a typed number edits it.
    if (focus == me && !edit && !typedToFocus.empty()) startEditing(nid.id, std::exchange(typedToFocus, {})), keysToEdit();
    else if (focus == me && !edit)
    {
        for (auto [key, modifiers] : keys)
        {
            // Shift for fine, Alt for a page's step, as a knob's.
            const double u = toUnit(*p, p->value, log), small = modifiers & View::shift ? 0.001 : modifiers & View::alt ? 0.1 : 0.01;
            switch (key)
            {
            case View::Key::up: case View::Key::right: set(params, *p, fromUnit(*p, u + small, log)); break;
            case View::Key::down: case View::Key::left: set(params, *p, fromUnit(*p, u - small, log)); break;
            case View::Key::pageUp: set(params, *p, fromUnit(*p, u + 0.1, log)); break;
            case View::Key::pageDown: set(params, *p, fromUnit(*p, u - 0.1, log)); break;
            case View::Key::home: set(params, *p, p->min); break;
            case View::Key::end: set(params, *p, p->max); break;
            case View::Key::enter: startEditing(nid.id, typed, true); break;
            case View::Key::escape:
                if (numberDrag == nid.id) params.change(*p, numberStart), endDrag(); // cancel the drag
                break;
            case View::Key::backspace: case View::Key::deleteForward: set(params, *p, p->defaultValue); break;
            default: break;
            }
        }
    }
    if (editKeys(nid.id, typed, Clay_PointerOver(nid)) || typed != before)
    {
        if (double value; parseNumber(typed, value)) set(params, *p, value);
    }
    // An edit ended with Enter or Escape leaves the keys with the box (a
    // click elsewhere takes them away as usual).
    if (edit && editing != nid.id && !mouse.pressed) focus = me;
    // The value as a bar filling the box, the number over it (compost's look).
    const bool editNow = editing == nid.id, hot = Clay_PointerOver(nid);
    const float lineHeight = theme.textSize * 1.25f;
    if (editNow) // the caret, for an input method's window: the text and its 1-point caret are centred in the box (last frame's)
    {
        const Clay_BoundingBox box = Clay_GetElementData(nid).boundingBox;
        const float textWidth = draw.measure(theme.textSize, draft, theme.valueFont) + 1;
        const float caretX = box.x + (box.width - textWidth) / 2 + draw.measure(theme.textSize, draft.substr(0, caret), theme.valueFont);
        caretAt = {caretX * s, box.y * s, box.height * s};
    }
    const Color fill = {theme.accent.r, theme.accent.g, theme.accent.b, 0.3f};
    CLAY({.id = nid, .layout = {.sizing = {CLAY_SIZING_FIXED(widthPoints), CLAY_SIZING_FIXED(lineHeight + 4)}},
          .backgroundColor = clayColor(theme.background), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .clip = {.horizontal = true},
          .border = {.color = clayColor(editNow || a.held || focusShown(nid) ? theme.accent : hot ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        if (!editNow)
            CLAY({.layout = {.sizing = {CLAY_SIZING_PERCENT(float(std::clamp(toUnit(*p, p->value, log), 0.0, 1.0))), CLAY_SIZING_GROW(0)}},
                  .backgroundColor = clayColor(fill)}) {}
        CLAY({.layout = {.padding = {5, 5, 0, 0}},
              .floating = {.attachPoints = {CLAY_ATTACH_POINT_CENTER_CENTER, CLAY_ATTACH_POINT_CENTER_CENTER},
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_PARENT, .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT}})
        {
            fieldText(shown, editNow);
        }
    }
    lastElement = nid.id;
}

void Ui::readout(Params &params, Param *p, const char *label, const char *format, bool log, float widthPoints)
{
    if (p) readout(params, *p, label, format, log, nullptr, 0, widthPoints);
}

void Ui::readout(Params &params, Param *p, const char *label, const char *const *items, int count, float widthPoints)
{
    if (p && count > 0) readout(params, *p, label, nullptr, false, items, count, widthPoints);
}

// Text that changes when dragged, as char-spectrum's Smoothing readout; with
// items, an enumeration a click steps through.
void Ui::readout(Params &params, Param &p, const char *label, const char *format, bool log, const char *const *items, int count,
                 float widthPoints)
{
    const Clay_ElementId rid = id("Readout", p.id);
    const void *me = reinterpret_cast<const void *>(uintptr_t(rid.id));
    const int item = items ? std::clamp(int(std::lround(p.value - p.min)), 0, count - 1) : 0;
    char shown[32];
    if (items) std::snprintf(shown, sizeof(shown), "%s", items[item]);
    else std::snprintf(shown, sizeof(shown), format, p.value);
    Described &self = describeParam(me, params, p, shown, {});
    self.node.role = Accessible::Role::spinButton, self.element = rid.id;
    order.push_back(me);
    // A value a step from this one: items, or a fraction of the range.
    auto step = [&](double by) {
        if (items) return p.min + std::clamp(item + int(by), 0, count - 1);
        return fromUnit(p, std::clamp(toUnit(p, p.value, log) + by, 0.0, 1.0), log);
    };
    const Area a = element(rid, eachPress); // a second press too: a double-click is told by mouse.doubleClicked
    if (a.pressed) focus = me, pressFocused = true;
    if (focus == me) // Escape cancels a drag, and only that
        focusUses = stepKeys | keyBit(View::Key::backspace) | keyBit(View::Key::deleteForward)
                  | (numberDrag == rid.id ? keyBit(View::Key::escape) : 0);
    // A drag up (or right), past a few points' dead zone, raises it: the
    // whole range in 200 points, or an item every 16; Shift slows it.
    const bool fine = mouse.modifiers & View::shift;
    auto grabHere = [&] {
        grabX = mouse.x, grabY = mouse.y, grabUnit = items ? double(item) : toUnit(p, p.value, log), grabFine = fine;
    };
    if (a.pressed)
    {
        grabSecond = mouse.doubleClicked;
        grabHere();
        numberDrag = rid.id, numberMoved = false, numberStart = p.value;
        params.begin(p);
        held = {&params, &p, nullptr, false};
    }
    else if (a.held && numberDrag == rid.id)
    {
        if (fine != grabFine) grabHere();
        const float moved = (mouse.x - grabX) + (grabY - mouse.y);
        numberMoved |= std::abs(moved) > 4 * s;
        if (numberMoved)
        {
            const double value = items ? p.min + std::clamp(int(std::lround(grabUnit + moved / (16 * s * (fine ? 4 : 1)))), 0, count - 1)
                                       : fromUnit(p, std::clamp(grabUnit + moved / (200 * s) * (fine ? 0.1 : 1), 0.0, 1.0), log);
            if (value != p.value) params.change(p, value);
        }
    }
    if (a.released && numberDrag == rid.id)
    {
        if (!numberMoved && grabSecond) params.change(p, p.defaultValue);             // a double-click: reset
        else if (!numberMoved && items) params.change(p, p.min + (item + 1) % count); // a click: the next item
        numberDrag = 0, held = {};
        params.end(p);
    }
    if (a.wheel != 0 && !a.held && (!wheelNeedsFocus || focus == me))
    {
        if (items) // an item a notch; a trackpad's small turns add up to one
        {
            itemWheel += a.wheel;
            if (std::abs(itemWheel) >= 1) set(params, p, step(itemWheel > 0 ? 1 : -1)), itemWheel = 0;
        }
        else set(params, p, step(a.wheel * (fine ? 0.002 : 0.02)));
        wheelUsed = true;
    }
    if (focus == me)
        for (auto [key, modifiers] : keys)
        {
            const double small = items ? 1 : modifiers & View::shift ? 0.001 : 0.01, large = items ? 2 : 0.1;
            switch (key)
            {
            case View::Key::up: case View::Key::right: set(params, p, step(small)); break;
            case View::Key::down: case View::Key::left: set(params, p, step(-small)); break;
            case View::Key::pageUp: set(params, p, step(large)); break;
            case View::Key::pageDown: set(params, p, step(-large)); break;
            case View::Key::home: set(params, p, p.min); break;
            case View::Key::end: set(params, p, items ? p.min + count - 1 : p.max); break;
            case View::Key::escape:
                if (numberDrag == rid.id) params.change(p, numberStart), numberDrag = 0, held = {}, params.end(p); // cancel the drag
                break;
            case View::Key::backspace: case View::Key::deleteForward: set(params, p, p.defaultValue); break;
            default: break;
            }
        }
    // The label dim and the value in the text colour (the accent while
    // held), a faint wash under the pointer, an accent outline when focus shows.
    const bool hot = Clay_PointerOver(rid), fixed = widthPoints > 0;
    const Color wash{theme.text.r, theme.text.g, theme.text.b, hot || a.held ? 0.12f : 0};
    // At least 24 points tall: WCAG 2.5.8's least target.
    CLAY({.id = rid, .layout = {.sizing = {fixed ? CLAY_SIZING_FIXED(widthPoints) : CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(24)},
                                .padding = {4, 4, 1, 1}, .childGap = uint16_t(theme.textSize / 2),
                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(wash), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .border = {.color = clayColor(focusShown(rid) ? theme.accent : Color{0, 0, 0, 0}), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        CLAY_TEXT(layout.text(label), textConfig(theme.dim));
        if (fixed) CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
        CLAY_TEXT(layout.text(shown), textConfig(a.held ? theme.accent : theme.text, theme.valueFont));
    }
    lastElement = rid.id;
}

}
