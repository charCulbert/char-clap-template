#include "accesskit_tree.h"
#include <utility>

namespace gpu::accesskit {

static accesskit_role role(Accessible::Role role)
{
    switch (role)
    {
    case Accessible::Role::window: return ACCESSKIT_ROLE_WINDOW;
    case Accessible::Role::group: return ACCESSKIT_ROLE_GROUP;
    case Accessible::Role::button: return ACCESSKIT_ROLE_BUTTON;
    case Accessible::Role::checkBox: return ACCESSKIT_ROLE_CHECK_BOX;
    case Accessible::Role::radioGroup: return ACCESSKIT_ROLE_RADIO_GROUP;
    case Accessible::Role::radioButton: return ACCESSKIT_ROLE_RADIO_BUTTON;
    case Accessible::Role::comboBox: return ACCESSKIT_ROLE_COMBO_BOX;
    case Accessible::Role::slider: return ACCESSKIT_ROLE_SLIDER;
    case Accessible::Role::spinButton: return ACCESSKIT_ROLE_SPIN_BUTTON;
    case Accessible::Role::textInput: return ACCESSKIT_ROLE_TEXT_INPUT;
    case Accessible::Role::label: return ACCESSKIT_ROLE_LABEL;
    case Accessible::Role::image: return ACCESSKIT_ROLE_IMAGE;
    case Accessible::Role::listBox: return ACCESSKIT_ROLE_LIST_BOX;
    case Accessible::Role::option: return ACCESSKIT_ROLE_LIST_BOX_OPTION;
    }
    return ACCESSKIT_ROLE_GENERIC_CONTAINER;
}

accesskit_tree_update *update(const Accessibility &tree)
{
    const uint64_t root = tree.nodes.empty() ? 0 : tree.nodes.front().id;
    accesskit_tree_update *result = accesskit_tree_update_with_capacity_and_focus(
        tree.nodes.empty() ? 1 : tree.nodes.size(), tree.focus ? tree.focus : root);
    accesskit_tree_update_set_tree_info(result, accesskit_tree_info_new(root));
    if (tree.nodes.empty()) // nothing recorded yet: just the view
        accesskit_tree_update_push_node(result, root, accesskit_node_new(ACCESSKIT_ROLE_WINDOW));
    for (const Accessible &a : tree.nodes)
    {
        accesskit_node *node = accesskit_node_new(role(a.role));
        if (!a.name.empty()) accesskit_node_set_label(node, a.name.c_str());
        if (!a.value.empty()) accesskit_node_set_value(node, a.value.c_str());
        if (!a.description.empty()) accesskit_node_set_description(node, a.description.c_str());
        if (a.live) accesskit_node_set_live(node, ACCESSKIT_LIVE_POLITE);
        if (a.orientation) accesskit_node_set_orientation(node, a.orientation == 1 ? ACCESSKIT_ORIENTATION_HORIZONTAL : ACCESSKIT_ORIENTATION_VERTICAL);
        if (a.numeric)
        {
            accesskit_node_set_numeric_value(node, a.number);
            accesskit_node_set_min_numeric_value(node, a.min);
            accesskit_node_set_max_numeric_value(node, a.max);
            if (a.step > 0) accesskit_node_set_numeric_value_step(node, a.step);
        }
        if (a.checked >= 0) accesskit_node_set_toggled(node, a.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
        if (a.role == Accessible::Role::comboBox) accesskit_node_set_expanded(node, a.expanded);
        if (a.role == Accessible::Role::option) accesskit_node_set_selected(node, a.selected);
        if (a.width > 0 && a.height > 0)
            accesskit_node_set_bounds(node, {a.x, a.y, a.x + a.width, a.y + a.height});
        for (auto [bit, kind] : {std::pair{Accessible::focus, ACCESSKIT_ACTION_FOCUS}, {Accessible::click, ACCESSKIT_ACTION_CLICK},
                                 {Accessible::increment, ACCESSKIT_ACTION_INCREMENT},
                                 {Accessible::decrement, ACCESSKIT_ACTION_DECREMENT},
                                 {Accessible::setValue, ACCESSKIT_ACTION_SET_VALUE}, {Accessible::expand, ACCESSKIT_ACTION_EXPAND},
                                 {Accessible::collapse, ACCESSKIT_ACTION_COLLAPSE}})
            if (a.actions & bit) accesskit_node_add_action(node, kind);
        for (uint64_t child : a.children) accesskit_node_push_child(node, child);
        accesskit_tree_update_push_node(result, a.id, node);
    }
    return result;
}

bool action(accesskit_action_request *request, uint64_t &id, Accessible::Action &kind, double &value)
{
    id = request->target_node;
    value = request->data.has_value && request->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE
              ? request->data.value.numeric_value : 0;
    bool known = true;
    switch (request->action)
    {
    case ACCESSKIT_ACTION_FOCUS: kind = Accessible::focus; break;
    case ACCESSKIT_ACTION_CLICK: kind = Accessible::click; break;
    case ACCESSKIT_ACTION_INCREMENT: kind = Accessible::increment; break;
    case ACCESSKIT_ACTION_DECREMENT: kind = Accessible::decrement; break;
    case ACCESSKIT_ACTION_EXPAND: kind = Accessible::expand; break;
    case ACCESSKIT_ACTION_COLLAPSE: kind = Accessible::collapse; break;
    case ACCESSKIT_ACTION_SET_VALUE:
        kind = Accessible::setValue;
        known = request->data.has_value && request->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE;
        break;
    default: known = false;
    }
    accesskit_action_request_free(request);
    return known;
}

}
