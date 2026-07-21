#include "RenderEngine.h"

int main(int argc, char* argv[]) {
    RenderEngine engine;

    if (!engine.Initialize("Potato Motion Graphics Editor - x64", 1600, 900)) {
        return -1;
    }

    bool running = true;
    while (running) {
        engine.HandleEvents(running);
        engine.BeginFrame();
        engine.RenderUI();
        engine.EndFrame();
    }

    engine.Shutdown();
    return 0;
}
