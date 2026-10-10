#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What a screen reader sees of a view: a tree of elements, each with a role,
// a name, a value and what can be done with it, as ARIA gives HTML. A View
// fills one while it records a frame (draw2d's Ui does it from its controls);
// the presenter hands it to the system's accessibility API and turns the
// screen reader's requests back into View::accessAction calls.
namespace gpu {

struct Accessible
{
    enum class Role { window, group, button, checkBox, radioGroup, radioButton, comboBox, slider, spinButton,
                      textInput, label, image, listBox, option, splitter, dialog, tabList, tab, tabPanel,
                      menu, menuItem };
    // label: text to read (a caption), its words in value; tab: one of a tab list's, selected when it's shown
    // What a screen reader may ask of it: bits. showMenu opens its context menu.
    enum Action : unsigned { focus = 1, click = 2, increment = 4, decrement = 8, setValue = 16, expand = 32, collapse = 64,
                             showMenu = 128 };

    uint64_t id = 0; // the same element keeps its id from frame to frame
    Role role = Role::group;
    std::string name;  // its label: "Cutoff"
    std::string value; // its value as shown: "1000 Hz"
    std::string description; // more about it, read after the name: a tooltip's text
    bool numeric = false; // number, min, max and step mean something (sliders)
    double number = 0, min = 0, max = 0, step = 0;
    int checked = -1; // check boxes and radio buttons: 0 or 1; -1 for others
    bool selected = false; // a list's chosen option, a tab list's shown tab
    bool expanded = false; // a combo box's list is open
    bool live = false;     // read out whenever its value changes (aria-live): announcements
    int orientation = 0;   // sliders: 1 horizontal, 2 vertical (aria-orientation); 0 not said
    float x = 0, y = 0, width = 0, height = 0; // pixels, in the view
    unsigned actions = 0;
    std::vector<uint64_t> children;
    uint64_t activeDescendant = 0; // a combo box's highlighted option (aria-activedescendant)
};

struct Accessibility
{
    std::vector<Accessible> nodes; // nodes[0] is the root, the view itself
    uint64_t focus = 0;            // the focused node's id, or the root's
    unsigned version = 0;          // changes whenever anything above does
};

}
