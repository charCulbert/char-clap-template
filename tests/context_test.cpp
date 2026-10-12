// gpu::Context notices a lost device (a driver reset, a GPU removed), which
// gpu::Gui then replaces, and not its own teardown.
#include "../libs/gpu/context.h"
#include <cstdio>

static int failures = 0;
#define CHECK(condition)                                                                      \
    do                                                                                        \
        if (!(condition))                                                                     \
            std::fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), ++failures; \
    while (0)

int main()
{
    gpu::Context context;
    if (!context.createDevice())
    {
        std::fprintf (stderr, "no WebGPU adapter: skipped\n");
        return 77;
    }
    CHECK (!context.lost->load());
    context.device.ForceLoss (wgpu::DeviceLostReason::Unknown, "a test");
    context.instance.ProcessEvents();
    CHECK (context.lost->load());

    gpu::Context other;
    if (other.createDevice())
    {
        const auto lost = other.lost;
        other.device.Destroy(); // ours to end: not a loss
        other.instance.ProcessEvents();
        CHECK (!lost->load());
    }
    if (failures)
        std::fprintf (stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
