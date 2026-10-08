#pragma once
#include "accessibility.h"
#include <accesskit.h>

// A view's accessibility tree in AccessKit's terms, which its platform
// adapters (macOS, Windows) hand to the system's accessibility API.
namespace gpu::accesskit {

// The whole tree as one update (AccessKit takes ownership of it).
accesskit_tree_update *update(const Accessibility &tree);
// A screen reader's request as View::accessAction arguments; false for
// requests the views don't handle. Frees the request.
bool action(accesskit_action_request *request, uint64_t &id, Accessible::Action &kind, double &value);

}
