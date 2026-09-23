// Serving the UI folder: nested files, binary data, and nothing outside it.
#include "../webview/resources.h"
#include <cassert>
#include <fstream>

int main(int argc, char **argv)
{
    assert(argc == 2); // a fresh, empty directory
    const std::filesystem::path base = argv[1];
    const auto root = base / "webUI";
    std::filesystem::create_directories(root / "assets/fonts");
    std::ofstream(root / "index.html") << "<html>";
    std::ofstream(root / "assets/app.mjs") << "export {}";
    std::ofstream(root / "assets/fonts/Font.WOFF2") << "font";
    std::ofstream(root / "assets/empty.css");
    std::ofstream(root / "assets/with space.bin", std::ios::binary).write("\0\1\xff", 3);
    std::ofstream(base / "secret.txt") << "outside";
    webview::setResourceRoot(root);
    using webview::readResource;

    assert(readResource("/index.html")->bytes == "<html>");
    assert(readResource("/index.html")->mime == "text/html");
    assert(readResource("/")->bytes == "<html>");
    assert(readResource("/assets/app.mjs?v=2#top")->mime == "text/javascript");
    assert(readResource("/assets/fonts/Font.WOFF2")->mime == "font/woff2");
    assert(readResource("/assets/empty.css")->bytes.empty());
    const auto binary = readResource("/assets/with%20space.bin");
    assert(binary && binary->bytes == std::string("\0\1\xff", 3));
    assert(binary->mime == "application/octet-stream");
    assert(readResource("/assets/with space.bin")); // macOS CHOC passes decoded paths

    for (const char *path : {"", "index.html", "/missing.js", "/assets", "/assets//app.mjs",
                             "/../secret.txt", "/assets/../../secret.txt", "/%2e%2e/secret.txt",
                             "/./index.html", "/%00", "/%", "/%4", "/%zz", "/C:/secret.txt",
                             "/..\\secret.txt", "/assets%2f..%2f..%2fsecret.txt"})
        assert(!readResource(path));

    webview::setResourceRoot({});
    assert(!readResource("/index.html"));
}
