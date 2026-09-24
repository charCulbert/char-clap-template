// C++ side of the message codec. With "<in> <out>" arguments it decodes a
// packet and writes it re-encoded, so messages.test.mjs can cross-check JS.
#include "../libs/core/messages.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <random>

using core::Value;

static std::optional<Value> decode(const std::vector<unsigned char> &bytes)
{
    return core::decode(bytes.data(), bytes.size());
}

int main(int argc, char **argv)
{
    if (argc == 3)
    {
        std::ifstream in(argv[1], std::ios::binary);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), {}};
        auto value = decode(bytes);
        if (!value) return 1;
        const auto out = core::encode(*value);
        std::ofstream(argv[2], std::ios::binary).write((const char *)out.data(), out.size());
        return 0;
    }

    Value::Map big;
    for (int i = 0; i < 30; ++i) big["key" + std::to_string(i)] = std::string(size_t(i) * 10, 'v');
    const Value message = Value::Map{{"type", "param"}, {"id", 1}, {"value", 1234.5},
                                     {"list", Value::Array{Value(), true, false, -7, "text"}},
                                     {"big", big}};
    const auto bytes = core::encode(message);
    const auto decoded = decode(bytes);
    assert(decoded && core::encode(*decoded) == bytes);
    assert((*decoded)["type"].text() == "param");
    assert((*decoded)["value"].number() == 1234.5);
    assert((*decoded)["big"]["key29"].text().size() == 290);
    assert((*decoded)["missing"]["deeper"].number(-1) == -1);
    assert((*decoded)["type"].number(-1) == -1);

    // Exactly one well-formed value, nothing more or less.
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(!decode(std::vector<unsigned char>(bytes.begin(), bytes.begin() + i)));
    auto extra = bytes;
    extra.push_back(0xf6);
    assert(!decode(extra));
    assert(!core::decode(nullptr, 0));

    // Lengths larger than the input, and the size/depth limits.
    assert(!decode({0x78, 100, 'a', 'b', 'c'}));                 // text claims 100 bytes
    assert(!decode({0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})); // huge array
    assert(!decode({0xb9, 0x03, 0xe8, 0x61}));                    // map claims 1000 pairs
    assert(!decode({0xa1, 0x01, 0xf6}));                          // non-text key
    assert(!decode({0xa2, 0x61, 'a', 0xf6, 0x61, 'a', 0xf6}));    // duplicate key
    assert(!decode({0x41, 0x00}));                                // byte strings not accepted
    assert(!decode({0x7f, 0xff}));                                // indefinite length
    std::vector<unsigned char> deep(40, 0x81);
    deep.push_back(0xf6);
    assert(!decode(deep));
    std::vector<unsigned char> tooBig(core::maxMessageBytes + 1, 0);
    assert(!decode(tooBig));

    // Random input must be rejected or accepted without reading out of bounds.
    std::mt19937 random(1);
    for (int i = 0; i < 20000; ++i)
    {
        std::vector<unsigned char> noise(random() % 48);
        for (auto &b : noise) b = (unsigned char)random();
        if (auto v = decode(noise)) assert(decode(core::encode(*v)));
    }
}
