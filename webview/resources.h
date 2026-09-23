#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Serves the packaged web UI folder. Nothing here knows about any plugin.
//
// Where the folder lives:
//   macOS bundles, VST3 bundles     <bundle>/Contents/Resources/webUI/
//   loose binaries (Win/Linux .clap) <binary>.resources/webUI/
//   WCLAP                           <bundle>/webUI/
namespace webview {

// Call from clap_entry.init. Native builds find the binary that contains this
// code (wrappers pass different paths to init); WCLAP uses the bundle path.
void initResources(const char *entryPath);
void setResourceRoot(std::filesystem::path folder);

struct Resource
{
    std::string bytes;
    std::string mime;
};

// urlPath is an absolute URL path such as "/index.html" or
// "/assets/font%20a.woff2?v=2". Returns nothing for anything outside the UI
// folder. UI file names must not contain '%', '?' or '#'.
std::optional<Resource> readResource(std::string_view urlPath);

}
