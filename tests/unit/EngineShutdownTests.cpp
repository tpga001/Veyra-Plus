#include "veyra/engine/EngineController.h"
#include <iostream>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << label << '\n';
    }
}

// Match the real native-window teardown order: main joins the dispatcher,
// then the browser controller asks it to stop and waits for idle(). No media,
// HWND, graphics context, SDK, prewarm request, or GPU operation is needed.
void stopAfterShutdown() {
    veyra::engine::EngineController engine;
    engine.shutdown();
    check(engine.idle(), "joined dispatcher starts idle");
    engine.stop();
    check(engine.idle(), "stop after shutdown cannot queue work on a joined dispatcher");
    engine.stop();
    engine.stop();
    check(engine.idle(), "repeated stop after shutdown remains idle");
    engine.shutdown();
    check(engine.idle(), "repeated shutdown after stop remains idle");
}

void disablePrewarmAfterShutdown() {
    veyra::engine::EngineController engine;
    engine.shutdown();
    check(engine.idle(), "prewarm-disable case starts with a joined idle dispatcher");
    engine.setEnhancementPrewarmEnabled(false);
    check(engine.idle(), "prewarm disable after shutdown cannot queue release work");
    engine.setEnhancementPrewarmEnabled(false);
    check(engine.idle(), "repeated prewarm disable after shutdown remains idle");
    engine.stop();
    check(engine.idle(), "browser stop after prewarm disable remains idle");
}

void togglePreferenceAfterShutdown() {
    veyra::engine::EngineController engine;
    engine.shutdown();
    engine.setEnhancementPrewarmEnabled(true);
    check(engine.idle(), "prewarm preference enable alone queues no GPU work after shutdown");
    engine.setEnhancementPrewarmEnabled(false);
    check(engine.idle(), "prewarm preference enable-disable after shutdown remains idle");
    engine.shutdown();
    check(engine.idle(), "shutdown stays idempotent after late preference notifications");
}
} // namespace

int main() {
    stopAfterShutdown();
    disablePrewarmAfterShutdown();
    togglePreferenceAfterShutdown();
    std::cout << "ENGINE_SHUTDOWN checks=" << checks << " failures=" << failures
              << " scope=cpu-lifecycle-no-media-no-gpu-requests\n";
    return failures ? 1 : 0;
}
