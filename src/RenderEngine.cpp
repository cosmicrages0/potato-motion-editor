#include "RenderEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <type_traits>
#include <fstream>
#include <sstream>

// Task 5.2: save/load
#include "Serialization.h"

// Task 5.9: DirectWrite text sprite renderer + Win32 known-folder path for
// the favorites JSON file. ShlObj_core.h brings SHGetKnownFolderPath.
#include "TextRenderer.h"
#include <ShlObj_core.h>
#include <KnownFolders.h>

// =============================================================================
// Lifecycle
// =============================================================================
RenderEngine::RenderEngine() {
    lastTime = SDL_GetPerformanceCounter();
}

RenderEngine::~RenderEngine() {
    Shutdown();
}

bool RenderEngine::Initialize(const char* title, int width, int height) {
    windowWidth  = width;
    windowHeight = height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "Error initializing SDL: " << SDL_GetError() << std::endl;
        return false;
    }

    // Task 4.5: size the initial window to ~90% of the primary display so it
    // never launches larger than the user's screen, and immediately maximize.
    SDL_Rect displayBounds{ 0, 0, windowWidth, windowHeight };
    if (SDL_GetDisplayUsableBounds(0, &displayBounds) == 0) {
        windowWidth  = std::max(800, (int)(displayBounds.w * 0.9f));
        windowHeight = std::max(600, (int)(displayBounds.h * 0.9f));
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
    window = SDL_CreateWindow(
        title ? title : "Potato Motion Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        windowWidth, windowHeight, window_flags);

    if (!window) {
        std::cerr << "Error creating SDL Window: " << SDL_GetError() << std::endl;
        return false;
    }

    // Belt-and-braces: some window managers ignore the MAXIMIZED flag at
    // creation; explicitly maximize after the window exists.
    SDL_MaximizeWindow(window);

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        std::cerr << "Error getting SDL Window Info" << std::endl;
        return false;
    }
    hwnd = wmInfo.info.win.window;

    if (!InitDirectX()) return false;
    if (!InitImGui())   return false;

    // Task 5.0: create the fixed-size composition render target BEFORE the
    // effect manager (which sizes its ping-pong buffers to match).
    // Task 5.8: RT dims = comp dims * previewScale (default 1.0 => equal).
    if (!CreateCompositionRT(RtWidth(), RtHeight())) {
        std::cerr << "[RenderEngine] Composition RT allocation failed" << std::endl;
        return false;
    }

    // Task 5: initialise the HLSL effect pipeline. Failure is logged but
    // NON-FATAL — the editor still runs; effects panels just show "not ready".
    if (!effectManager.Initialize(device, context,
                                   (UINT)compositionWidth,
                                   (UINT)compositionHeight)) {
        std::cerr << "[RenderEngine] EffectManager init failed; effects disabled" << std::endl;
    }

    // Task 5.0: the shape rasterizer that turns Layers into compRTV pixels.
    if (!compRenderer.Initialize(device, context)) {
        std::cerr << "[RenderEngine] CompositionRenderer init failed" << std::endl;
    }

    // Task 5.9: DirectWrite text sprite renderer. Non-fatal on failure —
    // Text layers just won't render, but everything else still works.
    textRenderer = new TextRenderer();
    if (!textRenderer->Initialize()) {
        std::cerr << "[RenderEngine] TextRenderer init failed; text layers disabled" << std::endl;
    } else {
        textRenderer->EnumerateSystemFonts(systemFonts);
        LoadFontFavorites();
    }

    // Seed a couple of demo layers so the app is immediately editable.
    // Task 5.0: use canvas center (960, 540) for a 1920x1080 composition, not
    // the old (640, 360) which was the 720p center.
    const float cx = (float)compositionWidth  * 0.5f;
    const float cy = (float)compositionHeight * 0.5f;
    layerManager.AddLayer(ShapeType::Rectangle, "Background Rect");
    if (Layer* bg = layerManager.GetSelectedLayer()) {
        // Task 5.1: seed layer defaults go directly into .staticValue since
        // the stopwatch is off at creation time.
        bg->transform.sizePixels.staticValue = Vec2(800.0f, 480.0f);
        bg->transform.position  .staticValue = Vec3(cx, cy, 0.0f);
        bg->fillColor = 0xFF3A3A55; // dark violet-ish (ABGR)
    }
    layerManager.AddLayer(ShapeType::Ellipse, "Bouncing Ball");
    if (Layer* ball = layerManager.GetSelectedLayer()) {
        ball->transform.sizePixels.staticValue = Vec2(200.0f, 200.0f);
        ball->transform.position  .staticValue = Vec3(cx, cy, 0.0f);
        ball->fillColor = 0xFF00B4FF;
    }
    // User feedback from Task 4.5 screenshot review: the slingshot demo was
    // silently overwriting whatever the artist typed into the Scale field,
    // making it look broken. Default OFF; the checkbox in the Timeline still
    // exists as a debug/demo toggle for showing the Bezier curve in action.
    applySlingshotToSelected = false;

    // Seed the 3D camera with a sensible default framing (Task 4).
    camera.ResetToDefault();

    lastTime = SDL_GetPerformanceCounter();
    return true;
}

// =============================================================================
// DirectX 11 device + swap chain (potato-PC feature-level fallback chain)
// =============================================================================
bool RenderEngine::InitDirectX() {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed    = TRUE;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[4] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 4,
        D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context);

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 4,
            D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context);
    }

    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDeviceAndSwapChain failed. HRESULT=0x"
                  << std::hex << hr << std::endl;
        return false;
    }

    CreateRenderTarget();
    return mainRenderTargetView != nullptr;
}

void RenderEngine::CreateRenderTarget() {
    if (!swapChain || !device) return;
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr) || !pBackBuffer) return;

    hr = device->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) mainRenderTargetView = nullptr;
}

void RenderEngine::CleanupRenderTarget() {
    if (mainRenderTargetView) { mainRenderTargetView->Release(); mainRenderTargetView = nullptr; }
}

bool RenderEngine::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplSDL2_InitForD3D(window)) return false;
    if (!ImGui_ImplDX11_Init(device, context)) return false;

    imguiInitialized = true;
    return true;
}

// =============================================================================
// Event loop
// =============================================================================
void RenderEngine::HandleEvents(bool& running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_QUIT) running = false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(window)) {
            running = false;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_RESIZED) {
            if (swapChain) {
                CleanupRenderTarget();
                swapChain->ResizeBuffers(0,
                    (UINT)event.window.data1, (UINT)event.window.data2,
                    DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
                windowWidth  = event.window.data1;
                windowHeight = event.window.data2;
            }
        }
        // Keyboard shortcuts.
        // Delete / Backspace:
        //   * If the playhead is snapped to at least one keyframe on the
        //     selected layer (matching the timeline strip's near-playhead
        //     highlight), delete THOSE keyframes only.
        //   * Otherwise, delete the whole layer (original behavior).
        // The threshold used here mirrors the visual highlight in
        // DrawTimelineStrip (`kNearPlayheadPixels = 10.0f`). We can't reuse
        // the pixel-space math from that function (the panel geometry isn't
        // known here), so we convert to a time epsilon: 1% of the comp
        // duration, clamped to a sane [0.02s, 0.5s] range. That's tight
        // enough that only visibly-highlighted diamonds get hit at typical
        // timeline widths.
        if (event.type == SDL_KEYDOWN && !ImGui::GetIO().WantTextInput) {
            const SDL_Keycode k = event.key.keysym.sym;
            // Task 5.10: Ctrl+D duplicates the selected layer (AE convention).
            // Snapshot BEFORE the mutation per the Task 5.6 sync-snapshot
            // rule so Ctrl+Z returns to pre-duplicate state.
            const bool ctrlHeld = (event.key.keysym.mod & KMOD_CTRL) != 0;
            if (ctrlHeld && k == SDLK_d) {
                const int selId = layerManager.GetSelectedId();
                if (selId != -1) {
                    MarkForSnapshot();
                    layerManager.DuplicateLayer(selId);
                }
                continue; // consume — don't fall through to Delete
            }
            if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                const int selId = layerManager.GetSelectedId();
                if (selId != -1) {
                    Layer* sel = layerManager.GetLayerById(selId);
                    const float t = animEngine.currentTime;
                    const float dur = (animEngine.duration > 0.001f)
                                          ? animEngine.duration : 1.0f;
                    const float eps = std::clamp(dur * 0.01f, 0.02f, 0.5f);

                    // Helper: return true if any key in `prop` sits within
                    // `eps` seconds of the playhead. Delete every such key
                    // (multiple keys can share a time if the user really
                    // wants it, though the same-time guard usually prevents
                    // that from happening).
                    auto deleteNearby = [&](auto& prop) -> int {
                        int removed = 0;
                        for (auto it = prop.keyframes.begin();
                             it != prop.keyframes.end(); ) {
                            if (std::fabs(it->time - t) <= eps) {
                                it = prop.keyframes.erase(it);
                                ++removed;
                            } else {
                                ++it;
                            }
                        }
                        return removed;
                    };

                    // First pass: count matches without deleting so we know
                    // whether to route to key-delete or layer-delete.
                    int nearKeys = 0;
                    if (sel) {
                        auto countNearby = [&](const auto& prop) {
                            for (const auto& kk : prop.keyframes) {
                                if (std::fabs(kk.time - t) <= eps) ++nearKeys;
                            }
                        };
                        countNearby(sel->transform.position);
                        countNearby(sel->transform.rotation);
                        countNearby(sel->transform.scale);
                        countNearby(sel->transform.anchorPoint);
                        countNearby(sel->transform.sizePixels);
                        countNearby(sel->transform.opacity);
                    }

                    if (nearKeys > 0 && sel) {
                        // Key-delete path. Snapshot BEFORE the mutation so
                        // Ctrl+Z restores the removed keys.
                        MarkForSnapshot();
                        int total = 0;
                        total += deleteNearby(sel->transform.position);
                        total += deleteNearby(sel->transform.rotation);
                        total += deleteNearby(sel->transform.scale);
                        total += deleteNearby(sel->transform.anchorPoint);
                        total += deleteNearby(sel->transform.sizePixels);
                        total += deleteNearby(sel->transform.opacity);
                        char msg[96];
                        std::snprintf(msg, sizeof(msg),
                                      "Deleted %d keyframe%s at %.3fs.",
                                      total, (total == 1 ? "" : "s"), t);
                        SetStatus(msg);
                    } else {
                        // Layer-delete path (original behavior).
                        MarkForSnapshot();
                        layerManager.DeleteLayerById(selId);
                    }
                }
            }
        }
    }
}

void RenderEngine::BeginFrame() {
    Uint64 now  = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    float deltaTime = (freq > 0) ? (float)((now - lastTime) / (double)freq) : 0.0f;
    lastTime = now;
    if (deltaTime > 0.25f) deltaTime = 0.25f;

    // Task 5.8: feed the fps ring for the viewport toolbar readout. Skip
    // pathological zero-delta frames so we don't accidentally publish inf.
    if (deltaTime > 1e-4f) {
        fpsRing[fpsRingHead] = 1.0f / deltaTime;
        fpsRingHead = (fpsRingHead + 1) % kFpsRingCap;
        if (fpsRingCount < kFpsRingCap) ++fpsRingCount;
    }

    // Task 5.6: bump the per-frame counter FIRST so any MarkForSnapshot()
    // call made during this frame's UI + input pass can coalesce correctly.
    // Wrap at max uint64 is a non-issue (billions of years of 60 FPS use).
    ++currentFrameNumber;
    if (currentFrameNumber == 0) currentFrameNumber = 1; // avoid the "0 = always push" sentinel

    // Task 5.3 -> Task 5.6: FlushPendingSnapshot is now a no-op stub. Kept
    // as a call for one commit for source compatibility; will be removed.
    FlushPendingSnapshot();

    // Task 5.8-fix: freeze the animation clock while ANY interactive drag is
    // in flight. Without this guard, if the user grabs a shape (gizmo drag)
    // or a timeline diamond while playback is running with loop on, every
    // per-frame SetValue(currentTime, ...) call stamps a NEW keyframe at a
    // NEW time (currentTime advances ~16ms/frame at 60 fps), producing
    // hundreds of spurious keys and a corrupted Value graph. Matches
    // Alight behavior: dragging silently suspends the clock; releasing
    // resumes playback where it left off. isPlaying is left untouched
    // so no explicit re-Play is needed.
    const bool anyDragActive =
        (activeGizmo != GizmoMode::None) ||
        diamondDragActive;
    if (!anyDragActive) {
        animEngine.Update(deltaTime);
    }

    // Task 5.1: LayerManager now publishes composition time so every downstream
    // read (matrix build, opacity chain, camera sync, Inspector) samples at
    // exactly the same instant. No more per-layer SampleTracks pre-pass — the
    // AnimatedProperty<T>::Evaluate() call at each read site IS the sampling.
    layerManager.BeginFrame(animEngine.currentTime);

    SyncCameraFromLayerIfAny(); // Task 4: let a Camera layer drive the view

    // Task 2 demo (slingshot -> selected scale).
    // Task 5.3-fix-3: previously this used to overwrite scale.staticValue
    // every frame while active, and when the user toggled OFF we just
    // stopped writing — leaving the last-computed scale (usually ~0.9 or
    // whatever mul was at that instant) baked in permanently. Now we
    // cache the layer's ORIGINAL scale when the toggle first turns on
    // (or when the selected layer changes while active) and RESTORE it
    // when the toggle turns off.
    {
        const int selId = layerManager.GetSelectedId();
        const bool nowActive = applySlingshotToSelected;
        Layer* sel = layerManager.GetSelectedLayer();

        // Transition OFF, or selection changed while active -> restore.
        const bool needRestore =
            slingshotWasActiveLastFrame &&
            (!nowActive || selId != slingshotOriginalLayerId);
        if (needRestore && slingshotOriginalLayerId >= 0) {
            if (Layer* prev = layerManager.GetLayerById(slingshotOriginalLayerId)) {
                if (!prev->transform.scale.IsAnimated()) {
                    prev->transform.scale.staticValue = slingshotOriginalScale;
                }
            }
            slingshotOriginalLayerId = -1;
        }

        // Transition ON, or selection changed to a new layer while active:
        // capture the pre-demo scale so we can restore later.
        if (nowActive && sel &&
            (!slingshotWasActiveLastFrame || selId != slingshotOriginalLayerId)) {
            if (!sel->transform.scale.IsAnimated()) {
                slingshotOriginalScale   = sel->transform.scale.staticValue;
                slingshotOriginalLayerId = selId;
            }
        }

        // Live: drive scale from the slingshot curve. Only kicks in for
        // layers whose scale stopwatch is off — real keyframes always win.
        if (nowActive && sel && !sel->transform.scale.IsAnimated()) {
            const float safeDur = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
            const float t = std::clamp(animEngine.currentTime / safeDur, 0.0f, 1.0f);
            const float k = animEngine.currentCurve.Evaluate(t);
            const float mul = std::max(k, 0.0f);
            // Multiply against the ORIGINAL scale so a shape that started
            // at scale (2,2) slingshots between (0, 2*peak) not between (0, peak).
            sel->transform.scale.staticValue = Vec3(
                slingshotOriginalScale.x * mul,
                slingshotOriginalScale.y * mul,
                slingshotOriginalScale.z);
        }

        slingshotWasActiveLastFrame = nowActive;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

// =============================================================================
// Docking layout + main menu
// =============================================================================
void RenderEngine::RenderUI() {
    // Task 5.2/5.3: global keyboard shortcuts. Checked once per frame before
    // any panel consumes the keypress. WantTextInput guard means Ctrl+S while
    // typing in a text field falls through to the field's own binding.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl) {
        // Ctrl+S = Save (or Save As if no path yet); Ctrl+Shift+S = Save As
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            std::string p = (io.KeyShift || lastSavePath.empty())
                                ? OpenSaveFileDialog("scene.pmge", true)
                                : lastSavePath;
            if (!p.empty()) {
                AppState st{}; BuildAppState(st);
                std::string err;
                if (SaveProject(st, p, &err)) {
                    lastSavePath = p;
                    SetStatus("Saved: " + p, false);
                } else {
                    SetStatus("Save failed: " + err, true);
                }
            }
        }
        // Ctrl+O = Open
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            const std::string p = OpenSaveFileDialog("scene.pmge", false);
            if (!p.empty()) {
                AppState st{}; BuildAppState(st);
                std::string err;
                if (LoadProject(st, p, &err)) {
                    ApplyLoadedScalars(st);
                    lastSavePath = p;
                    // Loading a new file is not undoable back into whatever
                    // was in memory before -- clear both stacks to avoid a
                    // confusing "undo brought me to a totally different file"
                    // experience.
                    undoStack.Clear();
                    SetStatus("Loaded: " + p, false);
                } else {
                    SetStatus("Load failed: " + err, true);
                }
            }
        }
        // Task 5.3: Ctrl+Z = Undo, Ctrl+Y or Ctrl+Shift+Z = Redo
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyShift) {
            AppState st{}; BuildAppState(st);
            if (undoStack.Undo(st)) {
                ApplyLoadedScalars(st);
                SetStatus("Undo (" + std::to_string(undoStack.PastCount()) +
                          " more available)", false, 1.5f);
            } else {
                SetStatus("Nothing to undo", false, 1.0f);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
            (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyShift)) {
            AppState st{}; BuildAppState(st);
            if (undoStack.Redo(st)) {
                ApplyLoadedScalars(st);
                SetStatus("Redo (" + std::to_string(undoStack.FutureCount()) +
                          " more available)", false, 1.5f);
            } else {
                SetStatus("Nothing to redo", false, 1.0f);
            }
        }
    }
    RenderAEDockingLayout();
}

void RenderEngine::RenderAEDockingLayout() {
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                 |  ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    // Task 5.6: View -> Reset Layout consumer. Nuking the node makes the
    // null-check below fall through to the initial-layout builder.
    if (pendingResetLayout) {
        pendingResetLayout = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
    }
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID dock_main_id = dockspace_id;
        // Task 5.12: unified bottom dock — Timeline now spans the full
        // width. Graph Editor is no longer a separate dock; it's a mode
        // toggle inside the Timeline panel's right pane (Shift+F3).
        ImGuiID dock_bottom_id       = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Down,  0.35f, nullptr, &dock_main_id);
        ImGuiID dock_left_id         = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Left,  0.20f, nullptr, &dock_main_id);
        ImGuiID dock_right_id        = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);

        ImGui::DockBuilderDockWindow("Project Assets",        dock_left_id);
        ImGui::DockBuilderDockWindow("Effects Palette",       dock_left_id);   // tabbed under Project Assets
        ImGui::DockBuilderDockWindow("Composition Viewport",  dock_main_id);
        ImGui::DockBuilderDockWindow("Inspector & Effects",   dock_right_id);
        ImGui::DockBuilderDockWindow("Effect Controls",       dock_right_id);  // tabbed under Inspector
        ImGui::DockBuilderDockWindow("Timeline",              dock_bottom_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("New Composition");   // still a stub; wired in a later commit
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                const std::string p = OpenSaveFileDialog("scene.pmge", false);
                if (!p.empty()) {
                    AppState st{}; BuildAppState(st);
                    std::string err;
                    if (LoadProject(st, p, &err)) {
                        ApplyLoadedScalars(st);
                        lastSavePath = p;
                        undoStack.Clear();   // see Ctrl+O handler for rationale
                        SetStatus("Loaded: " + p, false);
                    } else {
                        SetStatus("Load failed: " + err, true);
                    }
                }
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                std::string p = lastSavePath;
                if (p.empty()) p = OpenSaveFileDialog("scene.pmge", true);
                if (!p.empty()) {
                    AppState st{}; BuildAppState(st);
                    std::string err;
                    if (SaveProject(st, p, &err)) {
                        lastSavePath = p;
                        SetStatus("Saved: " + p, false);
                    } else {
                        SetStatus("Save failed: " + err, true);
                    }
                }
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                const std::string p = OpenSaveFileDialog("scene.pmge", true);
                if (!p.empty()) {
                    AppState st{}; BuildAppState(st);
                    std::string err;
                    if (SaveProject(st, p, &err)) {
                        lastSavePath = p;
                        SetStatus("Saved: " + p, false);
                    } else {
                        SetStatus("Save failed: " + err, true);
                    }
                }
            }
            ImGui::Separator();
            // Task 6: Alight Motion XML curve import. Reads the FIRST curve's
            // (p1, p2) segment and drops it into the global slingshot Bezier
            // so the imported easing shows up immediately in the Graph Editor.
            if (ImGui::MenuItem("Import Alight Motion .xml (default path)")) {
                auto keys = xmlImporter.ImportKeyframesFromFile("import.xml");
                if (!keys.empty() && keys[0].hasCurve) {
                    animEngine.currentCurve.P1 = keys[0].p1;
                    animEngine.currentCurve.P2 = keys[0].p2;
                    std::cerr << "[Import] Loaded " << keys.size()
                              << " keys from import.xml" << std::endl;
                    SetStatus("Imported " + std::to_string(keys.size()) +
                              " keyframes from import.xml", false);
                } else {
                    std::cerr << "[Import] " << xmlImporter.LastError() << std::endl;
                    SetStatus("Import failed: " + xmlImporter.LastError(), true);
                }
            }
            ImGui::EndMenu();
        }
        // Task 5.3: real Undo/Redo menu items backed by UndoStack. The
        // in-item Ctrl+Z / Ctrl+Y strings are cosmetic; the actual key
        // handling is in RenderUI's shortcut block above so it fires even
        // when the menu isn't open.
        if (ImGui::BeginMenu("Edit")) {
            const bool canU = undoStack.CanUndo();
            const bool canR = undoStack.CanRedo();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canU)) {
                AppState st{}; BuildAppState(st);
                if (undoStack.Undo(st)) {
                    ApplyLoadedScalars(st);
                    SetStatus("Undo", false, 1.5f);
                }
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canR)) {
                AppState st{}; BuildAppState(st);
                if (undoStack.Redo(st)) {
                    ApplyLoadedScalars(st);
                    SetStatus("Redo", false, 1.5f);
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Undo stack: %zu past / %zu redo",
                                undoStack.PastCount(), undoStack.FutureCount());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layer")) {
            if (ImGui::MenuItem("New Rectangle")) SpawnShapeAtViewportCenter(ShapeType::Rectangle);
            if (ImGui::MenuItem("New Ellipse"))   SpawnShapeAtViewportCenter(ShapeType::Ellipse);
            // Task 5.9: DirectWrite text layer.
            if (ImGui::MenuItem("New Text"))      SpawnShapeAtViewportCenter(ShapeType::Text, "Text");
            if (ImGui::MenuItem("New Null Object")) SpawnShapeAtViewportCenter(ShapeType::Null, "Null");
            if (show3DFeatures) {
                if (ImGui::MenuItem("New Camera"))    SpawnShapeAtViewportCenter(ShapeType::Camera,  "Camera");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Selected", "Del")) {
                if (layerManager.GetSelectedId() != -1) {
                    MarkForSnapshot();
                    layerManager.DeleteLayerById(layerManager.GetSelectedId());
                }
            }
            ImGui::EndMenu();
        }
        // Task 5.0-b: View menu with 2D/3D visibility toggle + debug panel.
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show 3D Features", nullptr, &show3DFeatures);
            ImGui::MenuItem("Show Debug Panel", nullptr, &showDebugPanel);
            ImGui::Separator();
            // Task 5.6: recover the default AE dock. Fire-and-forget per
            // design section 11 — no confirmation popup (Blender convention).
            // Consumed at the top of the next RenderAEDockingLayout() call.
            if (ImGui::MenuItem("Reset Layout")) {
                pendingResetLayout = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Composition")) {
            // Task 5.6: wired to open the settings modal (was a stub).
            if (ImGui::MenuItem("Composition Settings...")) {
                OpenCompSettingsModal();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Camera Style");
            bool ae     = (cameraStyle == CameraStyle::AfterEffects);
            bool alight = (cameraStyle == CameraStyle::AlightMotion);
            if (ImGui::MenuItem("After Effects (free parenting)", nullptr, ae)) {
                cameraStyle = CameraStyle::AfterEffects;
            }
            if (ImGui::MenuItem("Alight Motion (layers attach to camera)", nullptr, alight)) {
                cameraStyle = CameraStyle::AlightMotion;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Effect")) {
            Layer* selForFx = layerManager.GetSelectedLayer();
            const bool haveSel = (selForFx != nullptr);
            if (!haveSel) ImGui::BeginDisabled();
            // Task 5.3: every effect-add snapshots first for undo.
            auto addFx = [&](const Effect& e) { if (selForFx) { MarkForSnapshot(); selForFx->AddEffect(e); } };
            if (ImGui::MenuItem("Add Motion Tile"))         addFx(Effect::MakeMotionTile());
            if (ImGui::MenuItem("Add Directional Motion Blur")) addFx(Effect::MakeMotionBlur());
            if (ImGui::MenuItem("Add Chromatic Aberration")) addFx(Effect::MakeChromaticAberration());
            // Task 5.13: DropShadow — the per-layer isolation demonstrator.
            if (ImGui::MenuItem("Add Drop Shadow"))         addFx(Effect::MakeDropShadow());
            ImGui::Separator();
            if (ImGui::BeginMenu("Add Blend Mode")) {
                if (ImGui::MenuItem("Normal"))     addFx(Effect::MakeBlendMode(BlendMode::Normal));
                if (ImGui::MenuItem("Additive"))   addFx(Effect::MakeBlendMode(BlendMode::Additive));
                if (ImGui::MenuItem("Multiply"))   addFx(Effect::MakeBlendMode(BlendMode::Multiply));
                if (ImGui::MenuItem("Screen"))     addFx(Effect::MakeBlendMode(BlendMode::Screen));
                if (ImGui::MenuItem("Overlay"))    addFx(Effect::MakeBlendMode(BlendMode::Overlay));
                if (ImGui::MenuItem("Color Dodge"))addFx(Effect::MakeBlendMode(BlendMode::ColorDodge));
                ImGui::EndMenu();
            }
            if (!haveSel) ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("Render Queue...")) showRenderQueue = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();

    // Panels
    ImGui::Begin("Project Assets");        DrawProjectAssetsPanel();     ImGui::End();
    ImGui::Begin("Effects Palette");       DrawEffectsPalettePanel();    ImGui::End();
    ImGui::Begin("Composition Viewport");  DrawViewportCanvas();         ImGui::End();
    ImGui::Begin("Inspector & Effects");   DrawInspectorPanel();         ImGui::End();
    ImGui::Begin("Effect Controls");       DrawEffectControlsPanel();    ImGui::End();
    // Task 5.12: unified bottom dock — Graph Editor is now a mode inside
    // Timeline (right pane switches between bars and graph curves via a
    // toolbar toggle + Shift+F3 shortcut). No separate window.
    ImGui::Begin("Timeline");              DrawTimelinePanel();          ImGui::End();

    // Task 6: Render Queue window (only shown after Export -> Render Queue).
    if (showRenderQueue) {
        DrawRenderQueuePanel();
    }

    // Task 5.6: Composition Settings modal. Body is a no-op unless
    // `showCompSettingsModal` is true; opened via Composition -> Settings...
    DrawCompSettingsModal();

    // Task 5.0-b: Debug diagnostic panel. Off by default; toggled via
    // View -> Show Debug Panel. Prints live mouse-canvas coords, selected
    // layer transform values, and drag state so the user can screenshot any
    // odd behavior and the maintainer can debug from the numbers.
    if (showDebugPanel) {
        DrawDebugPanel();
    }
}

// =============================================================================
// Panel: Project Assets (still a stub in Task 3 — will grow in Task 6)
// =============================================================================
void RenderEngine::DrawProjectAssetsPanel() {
    ImGui::Text("Media Library");
    ImGui::Separator();
    ImGui::BulletText("(Media import lands in Task 6)");
    ImGui::Spacing();
    ImGui::Text("Quick Add");
    if (ImGui::Button("+ Rectangle"))   layerManager.AddLayer(ShapeType::Rectangle);
    ImGui::SameLine();
    if (ImGui::Button("+ Ellipse"))     layerManager.AddLayer(ShapeType::Ellipse);
}

// =============================================================================
// Panel: Timeline (layer list with visibility + selection)
// =============================================================================
void RenderEngine::DrawTimelinePanel() {
    // Task 5.12: install imgui.ini SettingsHandler for the bottom-dock
    // state (mode + splitFrac) on first call. Idempotent — the guard
    // ensures we only register once even if the panel is torn down and
    // rebuilt (e.g. after View -> Reset Layout).
    RegisterBottomDockSettings();

    // -------------------- Top toolbar row 1: add / delete / demo -----------
    if (ImGui::Button("+ Rect"))    SpawnShapeAtViewportCenter(ShapeType::Rectangle);
    ImGui::SameLine();
    if (ImGui::Button("+ Ellipse")) SpawnShapeAtViewportCenter(ShapeType::Ellipse);
    ImGui::SameLine();
    if (ImGui::Button("+ Text"))    SpawnShapeAtViewportCenter(ShapeType::Text,   "Text");
    ImGui::SameLine();
    if (ImGui::Button("+ Null"))    SpawnShapeAtViewportCenter(ShapeType::Null,   "Null");
    if (show3DFeatures) {
        ImGui::SameLine();
        if (ImGui::Button("+ Camera"))  SpawnShapeAtViewportCenter(ShapeType::Camera, "Camera");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Selected")) {
        if (layerManager.GetSelectedId() != -1) {
            MarkForSnapshot();
            layerManager.DeleteLayerById(layerManager.GetSelectedId());
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Slingshot -> Selected Scale", &applySlingshotToSelected);

    // -------------------- Task 5.12: mode toggle + shortcut ---------------
    // Bars mode = existing timeline strip. Graph mode = graph editor
    // replaces the strip in the same panel body (full-width).
    // Shift+F3 also toggles (AE-standard).
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
    {
        const bool barsSel = (bottomPaneMode == BottomPaneMode::Bars);
        if (barsSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.52f, 0.85f, 1.0f));
        if (ImGui::Button("Bars##paneMode")) bottomPaneMode = BottomPaneMode::Bars;
        if (barsSel) ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool graphSel = (bottomPaneMode == BottomPaneMode::Graph);
        if (graphSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.52f, 0.85f, 1.0f));
        if (ImGui::Button("Graph##paneMode")) bottomPaneMode = BottomPaneMode::Graph;
        if (graphSel) ImGui::PopStyleColor();
    }
    // Global Shift+F3 shortcut — only fires when the Timeline is focused
    // OR nobody's typing text (so it doesn't fire mid-input).
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_F3) &&
        (ImGui::GetIO().KeyShift)) {
        bottomPaneMode = (bottomPaneMode == BottomPaneMode::Bars)
                            ? BottomPaneMode::Graph : BottomPaneMode::Bars;
    }

    // Task 5.0-b: composition duration is now editable directly from the
    // timeline panel (was previously buried in the Global tab). Users hit
    // the end of the strip almost immediately at the 1-second default and
    // couldn't extend it.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##durSlider_tl", &animEngine.duration, 0.5f, 60.0f, "%.2f s");
    if (ImGui::IsItemActivated()) MarkForSnapshot();
    // Task 5.10 (user request #7a): pair the slider with a numeric input
    // so users can click-to-type instead of drag. Both bind to the same
    // float. Ctrl+click on the slider ALSO switches to text input (ImGui
    // native behavior) but the extra visible input makes it discoverable.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputFloat("Duration (s)##durInput_tl", &animEngine.duration,
                          0.1f, 1.0f, "%.3f")) {
        MarkForSnapshot();
        if (animEngine.duration < 0.1f)  animEngine.duration = 0.1f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(scrub playhead or drag it below)");

    ImGui::Separator();

    // Task 5.12: right-pane content selector.
    //   Bars mode  -> timeline strip (label column with Vis/Name/Parent
    //                 + track column with trim bars + keyframe diamonds
    //                 + playhead). This IS the layer list — no separate
    //                 table below anymore (Task 5.12b removed it).
    //   Graph mode -> graph editor (full-width now that it isn't sharing
    //                 the bottom dock with the Timeline anymore).
    if (bottomPaneMode == BottomPaneMode::Bars) {
        DrawTimelineStrip();
    } else {
        DrawGraphEditor();
    }
}

// =============================================================================
// Panel: Inspector (transform + Bezier handles + comp-clock playback)
// =============================================================================
// =============================================================================
// Timeline strip (Task 4.5): time ruler, draggable playhead, per-layer rows
// with keyframe diamonds. Kept simple — one horizontal band per layer showing
// all four track types combined (position/scale/rotation/opacity keys).
// =============================================================================
void RenderEngine::DrawTimelineStrip() {
    const float duration = (animEngine.duration > 0.001f) ? animEngine.duration : 1.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    const float stripW = std::max(200.0f, avail.x);

    // Task 5.12: label column width is now driven by the user-adjustable
    // bottomPaneSplitFrac (persisted to imgui.ini via SettingsHandler).
    // Clamped [0.15, 0.60] of the strip width; 100 px floor as a safety.
    // The auto-fit-to-longest-name behavior from Task 5.0 is preserved as
    // a MINIMUM — if the user shrinks the splitter below what names need
    // for display, we bump it up so names never clip.
    float labelW = stripW * bottomPaneSplitFrac;
    float autoMin = 100.0f;
    for (const auto& layer : layerManager.Layers()) {
        const ImVec2 sz = ImGui::CalcTextSize(layer.name.c_str());
        if (sz.x + 24.0f > autoMin) autoMin = sz.x + 24.0f;
    }
    if (labelW < autoMin)         labelW = autoMin;
    if (labelW > stripW * 0.60f)  labelW = stripW * 0.60f;
    if (labelW < 100.0f)          labelW = 100.0f;
    const float rulerH = 22.0f;
    const float rowH   = 18.0f;
    // Task 5.10: mutable ref so trim-bar drag can mutate in/out points
    // directly. Diamond drag path already relies on mutation via
    // property accessors, so this doesn't regress anything.
    auto& layers = layerManager.Layers();
    const float bodyH  = rowH * std::max<int>(1, (int)layers.size());
    const float totalH = rulerH + bodyH + 4.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) { ImGui::Dummy(ImVec2(stripW, totalH)); return; }

    // Backdrop
    dl->AddRectFilled(origin, ImVec2(origin.x + stripW, origin.y + totalH),
                      IM_COL32(24, 24, 30, 255));
    dl->AddLine(ImVec2(origin.x + labelW, origin.y),
                ImVec2(origin.x + labelW, origin.y + totalH),
                IM_COL32(60, 60, 70, 255), 1.0f);

    // Task 5.12: 6-pixel splitter handle over the label|track divider.
    // Drag = updates bottomPaneSplitFrac. Clamped [0.15, 0.60]. Value
    // persists to imgui.ini via the SettingsHandler installed in
    // DrawTimelinePanel::RegisterBottomDockSettings.
    {
        const float splitterW = 6.0f;
        ImGui::PushID("##bottomDockSplitter");
        ImGui::SetCursorScreenPos(ImVec2(origin.x + labelW - splitterW * 0.5f, origin.y));
        ImGui::InvisibleButton("##splitBar", ImVec2(splitterW, totalH));
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();
        if (hov || act) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            // Bright hover / active tint so users can see it.
            dl->AddRectFilled(
                ImVec2(origin.x + labelW - splitterW * 0.5f, origin.y),
                ImVec2(origin.x + labelW + splitterW * 0.5f, origin.y + totalH),
                IM_COL32(120, 140, 200, act ? 180 : 90));
        }
        if (act) {
            const float dx = ImGui::GetIO().MouseDelta.x;
            if (dx != 0.0f && stripW > 1.0f) {
                float f = bottomPaneSplitFrac + dx / stripW;
                if (f < 0.15f) f = 0.15f;
                if (f > 0.60f) f = 0.60f;
                bottomPaneSplitFrac = f;
            }
        }
        ImGui::PopID();
    }

    const float trackX0 = origin.x + labelW + 4.0f;
    const float trackX1 = origin.x + stripW - 6.0f;
    const float trackW  = std::max(10.0f, trackX1 - trackX0);
    auto TimeToX = [&](float t) {
        const float u = std::clamp(t / duration, 0.0f, 1.0f);
        return trackX0 + u * trackW;
    };
    auto XToTime = [&](float x) {
        const float u = std::clamp((x - trackX0) / trackW, 0.0f, 1.0f);
        return u * duration;
    };

    // Ruler ticks (10 divisions)
    for (int i = 0; i <= 10; ++i) {
        const float u = (float)i / 10.0f;
        const float x = trackX0 + u * trackW;
        const bool major = (i % 5 == 0);
        dl->AddLine(ImVec2(x, origin.y + rulerH - (major ? 10.0f : 6.0f)),
                    ImVec2(x, origin.y + rulerH),
                    IM_COL32(120, 120, 130, 255), 1.0f);
        if (major) {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.2fs", u * duration);
            dl->AddText(ImVec2(x + 2.0f, origin.y + 2.0f),
                        IM_COL32(200, 200, 210, 255), buf);
        }
    }

    // Per-layer rows with keyframe diamonds.
    // Task 5.11 (AE-order): row 0 (top of strip) maps to the LAST layer in
    // the vector — matches AE where the top row is the front-most layer.
    // CompositionRenderer keeps its forward-vector iteration (later index =
    // drawn last = on top), which is what makes the front-most layer
    // visually on top of the canvas AND at the top of the timeline strip.
    const int selectedId = layerManager.GetSelectedId();
    // Task 5.3-fix: precompute playhead X once so per-diamond render code can
    // highlight keys that are near the playhead.
    const float playheadX = TimeToX(animEngine.currentTime);
    constexpr float kNearPlayheadPixels = 10.0f;
    const size_t nLayers = layers.size();
    for (size_t rowI = 0; rowI < nLayers; ++rowI) {
        // Vector index that this row displays (top row = last vector element).
        const size_t i = nLayers - 1 - rowI;
        Layer& layer = layers[i];    // Task 5.10: mutable for trim-bar drag
        const float rowY0 = origin.y + rulerH + rowH * (float)rowI;
        const float rowYc = rowY0 + rowH * 0.5f;

        // Row background: highlight selected. Slightly brighter tint when
        // this row is the drag-in-flight source so the user sees what's
        // being moved.
        if (layer.id == selectedId) {
            dl->AddRectFilled(ImVec2(origin.x, rowY0),
                              ImVec2(origin.x + stripW, rowY0 + rowH),
                              IM_COL32(50, 50, 80, 200));
        }
        if (layer.id == layerReorderDragId) {
            dl->AddRectFilled(ImVec2(origin.x, rowY0),
                              ImVec2(origin.x + stripW, rowY0 + rowH),
                              IM_COL32(90, 90, 140, 180));
        }
        // Task 5.12b: label column now hosts the Vis toggle + Name + Parent
        // combo directly, so the redundant ImGui table below can go away.
        // Widget order matters for click routing: the interactive widgets
        // (Vis checkbox, Parent combo) are drawn BEFORE the reorder
        // InvisibleButton so they win the hit-test where they overlap.
        //
        // Layout inside labelW:
        //   [Vis 18px] [padding 4px] [Name flex] [Parent 90px if room]
        const float visW    = 18.0f;
        const float parentW = 90.0f;
        // Skip parent combo when the label column is too tight — user can
        // still parent via the Inspector.
        const bool  showParentCol = (trackX0 - origin.x) > 260.0f;
        const float nameX0    = origin.x + visW + 6.0f;
        const float parentX0  = origin.x + (trackX0 - origin.x) - parentW - 6.0f;
        const float nameX1    = showParentCol ? parentX0 - 4.0f
                                              : (trackX0 - 4.0f);

        // --- Vis toggle (eye checkbox, small) --------------------------
        {
            ImGui::PushID((int)(0x7B000000 | layer.id));
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 2.0f, rowY0 + 2.0f));
            bool vis = layer.isVisible;
            if (ImGui::Checkbox("##vis", &vis)) {
                MarkForSnapshot();
                layer.isVisible = vis;
            }
            ImGui::PopID();
        }

        // --- Layer name text -------------------------------------------
        // Clip to available width so long names don't spill over the parent
        // combo or the track area. Cheap manual clip: measure and truncate.
        {
            const float nameMaxW = std::max(20.0f, nameX1 - nameX0);
            const char* nm = layer.name.c_str();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", nm);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            if (sz.x > nameMaxW) {
                // Truncate with ellipsis. Cheap loop; label lengths are
                // ~20 chars typical so this is O(N) per row per frame.
                size_t len = std::strlen(buf);
                while (len > 1 && sz.x > nameMaxW - 12.0f) {
                    buf[--len] = '\0';
                    if (len < sizeof(buf) - 3) {
                        buf[len] = '\0';
                    }
                    sz = ImGui::CalcTextSize(buf);
                }
                // Append "..." if we actually truncated.
                if (len < std::strlen(nm)) {
                    if (len + 3 < sizeof(buf)) std::strcat(buf, "...");
                }
            }
            dl->AddText(ImVec2(nameX0, rowY0 + 2.0f),
                        IM_COL32(220, 220, 230, 255), buf);
        }

        // --- Parent combo (only when there's room) ---------------------
        if (showParentCol) {
            ImGui::PushID((int)(0x7C000000 | layer.id));
            ImGui::SetCursorScreenPos(ImVec2(parentX0, rowY0 + 1.0f));
            ImGui::SetNextItemWidth(parentW);
            const Layer* parent = layerManager.GetLayerById(layer.parentId);
            const char* preview = parent ? parent->name.c_str() : "(none)";
            if (ImGui::BeginCombo("##parent", preview,
                                   ImGuiComboFlags_HeightSmall |
                                   ImGuiComboFlags_NoArrowButton)) {
                const float ct = animEngine.currentTime;
                if (ImGui::Selectable("(none)", layer.parentId == -1)) {
                    MarkForSnapshot();
                    layerManager.SetParentPreservingWorld(layer.id, -1, ct);
                }
                for (const auto& candidate : layers) {
                    if (candidate.id == layer.id) continue;
                    const bool wouldCycle =
                        layerManager.WouldCreateCycle(layer.id, candidate.id);
                    const ImGuiSelectableFlags flags =
                        wouldCycle ? ImGuiSelectableFlags_Disabled : 0;
                    const bool sel = (layer.parentId == candidate.id);
                    if (ImGui::Selectable(candidate.name.c_str(), sel, flags)) {
                        MarkForSnapshot();
                        layerManager.SetParentPreservingWorld(layer.id, candidate.id, ct);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }

        // Task 5.11: drag-to-reorder hit-region.
        // Task 5.12b: EXTENDED to the full row width (not just the label
        // column) so users can grab any part of the row — matching AE.
        // Sits AFTER the Vis + Parent widgets so those claim their hit-
        // rects first; the trim-bar handles + keyframe diamonds draw
        // later in the loop and win on their sub-rects too.
        //
        // Task 5.11-fix: we CAN'T rely on ImGui::IsItemActive() to keep
        // the drag alive across frames — MoveLayerToIndex reorders the
        // vector mid-drag, moving this row's InvisibleButton to a
        // different screen Y. ImGui interprets 'widget moved' = drag
        // lost. Detect mouse-down via IsItemHovered + IsMouseClicked,
        // then track lifetime via the global IsMouseDown check in the
        // post-loop block.
        if (!diamondDragActive) {
            ImGui::PushID((int)(0x7A000000 | layer.id));
            ImGui::SetCursorScreenPos(ImVec2(origin.x, rowY0));
            ImGui::InvisibleButton("##layerReorder", ImVec2(stripW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            if (hovered || layer.id == layerReorderDragId) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                (layerReorderDragId < 0)) {
                MarkForSnapshot();
                layerReorderDragId       = layer.id;
                layerReorderSnapshotDone = true;
                layerManager.SetSelectedId(layer.id);
            }
            ImGui::PopID();
        }

        // Track baseline
        dl->AddLine(ImVec2(trackX0, rowYc), ImVec2(trackX1, rowYc),
                    IM_COL32(60, 60, 70, 255), 1.0f);

        // Task 5.10: trim bar. Sits BEHIND the keyframe diamonds so a
        // diamond click-drag still wins the hit-test. Three interaction
        // zones: left 6px = trim inPoint, right 6px = trim outPoint,
        // middle = slip both (drag whole bar without changing length).
        {
            const float durSafe = (animEngine.duration > 0.001f) ? animEngine.duration : 1.0f;
            const float outResolved = (layer.outPoint < 0.0f) ? durSafe : layer.outPoint;
            const float barX0 = TimeToX(std::clamp(layer.inPoint,  0.0f, durSafe));
            const float barX1 = TimeToX(std::clamp(outResolved,    0.0f, durSafe));
            const float barY0 = rowYc - rowH * 0.35f;
            const float barY1 = rowYc + rowH * 0.35f;

            // Bar body (layer's fillColor at 40% alpha) + selection border.
            const unsigned int fc = layer.fillColor;
            const ImU32 barCol = ((unsigned int)std::clamp((int)(((fc >> 24) & 0xFFu) * 0.4f), 0, 255) << 24)
                                 | (fc & 0x00FFFFFFu);
            dl->AddRectFilled(ImVec2(barX0, barY0), ImVec2(barX1, barY1), barCol, 3.0f);
            if (layer.id == selectedId) {
                dl->AddRect(ImVec2(barX0, barY0), ImVec2(barX1, barY1),
                            IM_COL32(255, 255, 255, 200), 3.0f, 0, 1.0f);
            }

            // Interaction zones (invisible buttons). Suppressed while a
            // keyframe diamond drag is in flight so trim doesn't hijack it.
            if (!diamondDragActive) {
                const float handleW = 6.0f;
                // Unique ID per layer + zone via ImGui id stack.
                ImGui::PushID(layer.id * 100 + 91);
                // Left handle (trim inPoint)
                {
                    ImGui::SetCursorScreenPos(ImVec2(barX0 - handleW*0.5f, barY0));
                    ImGui::InvisibleButton("##trimL", ImVec2(handleW, barY1 - barY0));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    }
                    if (ImGui::IsItemActivated()) MarkForSnapshot();
                    if (ImGui::IsItemActive()) {
                        const float dt = ImGui::GetIO().MouseDelta.x / trackW * durSafe;
                        float nv = layer.inPoint + dt;
                        if (nv < 0.0f) nv = 0.0f;
                        if (nv > outResolved - 0.01f) nv = outResolved - 0.01f;
                        layer.inPoint = nv;
                    }
                }
                // Right handle (trim outPoint) — materializes -1 sentinel
                // to the resolved comp end so drag has a start value.
                {
                    ImGui::SetCursorScreenPos(ImVec2(barX1 - handleW*0.5f, barY0));
                    ImGui::InvisibleButton("##trimR", ImVec2(handleW, barY1 - barY0));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    }
                    if (ImGui::IsItemActivated()) {
                        MarkForSnapshot();
                        if (layer.outPoint < 0.0f) layer.outPoint = outResolved;
                    }
                    if (ImGui::IsItemActive()) {
                        const float dt = ImGui::GetIO().MouseDelta.x / trackW * durSafe;
                        float nv = layer.outPoint + dt;
                        if (nv < layer.inPoint + 0.01f) nv = layer.inPoint + 0.01f;
                        if (nv > durSafe)               nv = durSafe;
                        layer.outPoint = nv;
                    }
                }
                // Middle (slip both together, preserving bar length).
                {
                    const float midW = std::max(0.0f, (barX1 - handleW) - (barX0 + handleW));
                    if (midW > 0.0f) {
                        ImGui::SetCursorScreenPos(ImVec2(barX0 + handleW*0.5f, barY0));
                        ImGui::InvisibleButton("##trimMid", ImVec2(midW, barY1 - barY0));
                        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                        }
                        if (ImGui::IsItemActivated()) {
                            MarkForSnapshot();
                            if (layer.outPoint < 0.0f) layer.outPoint = outResolved;
                        }
                        if (ImGui::IsItemActive()) {
                            const float dt = ImGui::GetIO().MouseDelta.x / trackW * durSafe;
                            const float span = layer.outPoint - layer.inPoint;
                            float ni = layer.inPoint + dt;
                            // Clamp so the whole span stays in [0, durSafe].
                            if (ni < 0.0f)                ni = 0.0f;
                            if (ni + span > durSafe)      ni = durSafe - span;
                            layer.inPoint  = ni;
                            layer.outPoint = ni + span;
                        }
                    }
                }
                ImGui::PopID();
            }
        }

        // Draw + hit-test each AnimatedProperty's keyframe diamonds.
        // Task 5.3: diamonds are now interactive — left-click to start drag,
        // right-click to open a context menu, drag to slide along the ruler.
        // A drag in flight suppresses the strip's scrub button below.
        //
        // Templated lambda so one body handles Vec2/Vec3/float property types.
        const ImVec2 mp = ImGui::GetIO().MousePos;
        auto drawAndHitKeys = [&](auto& prop, DiamondProperty which, ImU32 col) {
            const float r = 5.0f;
            for (size_t ki = 0; ki < prop.keyframes.size(); ++ki) {
                const auto& k = prop.keyframes[ki];
                const float x = TimeToX(k.time);
                const ImVec2 p(x, rowYc);
                const ImVec2 tri[4] = {
                    ImVec2(p.x,     p.y - r),
                    ImVec2(p.x + r, p.y),
                    ImVec2(p.x,     p.y + r),
                    ImVec2(p.x - r, p.y),
                };
                // Highlight the diamond being dragged or opened in a menu.
                const bool isTarget =
                    ((draggedDiamond.layerId == layer.id &&
                      (int)draggedDiamond.which == (int)which &&
                      draggedDiamond.keyIndex == (int)ki) ||
                     (contextDiamond.layerId == layer.id &&
                      (int)contextDiamond.which == (int)which &&
                      contextDiamond.keyIndex == (int)ki));
                // Task 5.3-fix: also highlight when the playhead is within
                // ~10px. Combined with the scrub-snap below, this makes it
                // obvious which key you'd be editing if you scrubbed to here.
                const bool nearPlayhead = std::fabs(p.x - playheadX) < kNearPlayheadPixels;
                ImU32 diamondCol = col;
                if (isTarget)          diamondCol = IM_COL32(255, 255, 255, 255);
                else if (nearPlayhead) diamondCol = IM_COL32(255, 220, 100, 255);   // warm yellow-glow
                dl->AddConvexPolyFilled(tri, 4, diamondCol);
                if (nearPlayhead) {
                    // Small halo ring around the near-playhead diamond so it
                    // stands out even against a busy row.
                    dl->AddCircle(p, r + 3.0f, IM_COL32(255, 220, 100, 180), 12, 1.2f);
                }

                // Hit-test: 6px square around the diamond center.
                if (std::fabs(mp.x - p.x) <= r + 1.0f &&
                    std::fabs(mp.y - p.y) <= r + 1.0f) {
                    // Left-click begins a drag. Task 5.3-fix: snapshot BEFORE
                    // the drag starts so Ctrl+Z rewinds to the pre-drag time.
                    if (!diamondDragActive &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        MarkForSnapshot();
                        diamondDragActive = true;
                        draggedDiamond.layerId  = layer.id;
                        draggedDiamond.which    = which;
                        draggedDiamond.keyIndex = (int)ki;
                        draggedDiamond.origTime = k.time;
                    }
                    // Right-click stages a context menu (opened below).
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        contextDiamond.layerId  = layer.id;
                        contextDiamond.which    = which;
                        contextDiamond.keyIndex = (int)ki;
                        contextDiamond.origTime = k.time;
                        ImGui::OpenPopup("##kfContext");
                    }
                }
            }
        };
        drawAndHitKeys(layer.transform.position, DiamondProperty::Position, IM_COL32(120, 200, 255, 255));
        drawAndHitKeys(layer.transform.scale,    DiamondProperty::Scale,    IM_COL32(255, 200, 120, 255));
        drawAndHitKeys(layer.transform.rotation, DiamondProperty::Rotation, IM_COL32(200, 255, 120, 255));
        drawAndHitKeys(layer.transform.opacity,  DiamondProperty::Opacity,  IM_COL32(255, 120, 200, 255));
    }

    // Task 5.11: reorder-drag position tracking. Runs AFTER the per-row
    // loop so we can consult mouse Y against known row bounds. On each
    // frame the drag is active, work out which row the mouse hovers and
    // if it's different from the dragged layer's current row, call
    // MoveLayerToIndex. Ends cleanly on mouse-up.
    if (layerReorderDragId >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float mY   = ImGui::GetIO().MousePos.y;
            const float body0 = origin.y + rulerH;
            // Task 5.11-fix-2: CLAMP dragFromRow into the valid strip range
            // instead of early-returning when the mouse leaves the strip.
            // Old behavior: dragging above the strip did nothing (bug — user
            // wanted "move to top"). Clamp semantics: mouse above strip =
            // row 0 (top / front-most), below strip = last row (back-most).
            int dragFromRow = (int)((mY - body0) / rowH);
            if (dragFromRow < 0)               dragFromRow = 0;
            if (dragFromRow > (int)nLayers - 1) dragFromRow = (int)nLayers - 1;
            // Row -> FINAL vector index (reverse mapping: top row = last vec).
            const int targetVecIdx = (int)nLayers - 1 - dragFromRow;
            int curVecIdx = -1;
            for (size_t k = 0; k < nLayers; ++k) {
                if (layers[k].id == layerReorderDragId) { curVecIdx = (int)k; break; }
            }
            if (curVecIdx >= 0 && curVecIdx != targetVecIdx) {
                layerManager.MoveLayerToIndex(layerReorderDragId, targetVecIdx);
            }
        } else {
            // Mouse-up: end the drag. Snapshot was already fired on
            // activation so no post-drag snapshot needed.
            layerReorderDragId       = -1;
            layerReorderSnapshotDone = false;
        }
    }

    // Right-click context menu for a keyframe. contextDiamond is set by the
    // diamond hit-test above (which also calls OpenPopup on the click).
    // Kept OUTSIDE the per-layer loop so the popup opens once per frame.
    if (ImGui::BeginPopup("##kfContext")) {
        if (contextDiamond.valid()) {
            Layer* cl = layerManager.GetLayerById(contextDiamond.layerId);
            if (cl) {
                ImGui::TextDisabled("Keyframe at t=%.3fs", contextDiamond.origTime);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Keyframe")) {
                    MarkForSnapshot();
                    switch (contextDiamond.which) {
                    case DiamondProperty::Position: cl->transform.position.RemoveKeyAt(contextDiamond.origTime); break;
                    case DiamondProperty::Rotation: cl->transform.rotation.RemoveKeyAt(contextDiamond.origTime); break;
                    case DiamondProperty::Scale:    cl->transform.scale   .RemoveKeyAt(contextDiamond.origTime); break;
                    case DiamondProperty::Opacity:  cl->transform.opacity .RemoveKeyAt(contextDiamond.origTime); break;
                    }
                    contextDiamond.clear();
                }
                if (ImGui::MenuItem("Go to Keyframe")) {
                    animEngine.currentTime = contextDiamond.origTime;
                    animEngine.isPlaying   = false;
                }
            }
        }
        ImGui::EndPopup();
    }

    // Handle in-flight diamond drag: slide the keyframe's time as the mouse moves.
    // On mouse-up: commit + snapshot.
    if (diamondDragActive) {
        Layer* dl_layer = layerManager.GetLayerById(draggedDiamond.layerId);
        if (dl_layer && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mmp = ImGui::GetIO().MousePos;
            const float newTime = std::clamp(XToTime(mmp.x), 0.0f,
                                             (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f);
            // Move the keyframe by re-inserting at the new time. RemoveKeyAt
            // + SetValue on the same AnimatedProperty preserves the value.
            auto moveKey = [&](auto& prop) {
                if (draggedDiamond.keyIndex < 0 ||
                    draggedDiamond.keyIndex >= (int)prop.keyframes.size()) return;
                auto oldVal = prop.keyframes[draggedDiamond.keyIndex].value;
                // Force the write path via a temporarily-enabled stopwatch so
                // SetValue actually keys (it may already be enabled; toggle is
                // a no-op then).
                const bool wasOn = prop.stopwatchEnabled;
                prop.stopwatchEnabled = true;
                prop.RemoveKeyAt(draggedDiamond.origTime);
                prop.SetValue(newTime, oldVal);
                prop.stopwatchEnabled = wasOn || true;   // keep on; drag implies animation
                draggedDiamond.origTime = newTime;
                // keyIndex may have shifted after re-sort; find it again next frame.
                for (size_t i = 0; i < prop.keyframes.size(); ++i) {
                    if (std::fabs(prop.keyframes[i].time - newTime) < 1e-3f) {
                        draggedDiamond.keyIndex = (int)i;
                        break;
                    }
                }
            };
            switch (draggedDiamond.which) {
            case DiamondProperty::Position: moveKey(dl_layer->transform.position); break;
            case DiamondProperty::Rotation: moveKey(dl_layer->transform.rotation); break;
            case DiamondProperty::Scale:    moveKey(dl_layer->transform.scale);    break;
            case DiamondProperty::Opacity:  moveKey(dl_layer->transform.opacity);  break;
            }
        } else {
            // Mouse released -> end drag. No snapshot here — we already
            // snapshotted BEFORE the drag started (see IsMouseClicked block
            // above), so Ctrl+Z rewinds to the pre-drag keyframe time.
            diamondDragActive = false;
            draggedDiamond.clear();
        }
    }

    // Playhead
    // playheadX already computed at the top of this function.
    dl->AddLine(ImVec2(playheadX, origin.y),
                ImVec2(playheadX, origin.y + totalH),
                IM_COL32(255, 60, 60, 255), 2.0f);
    dl->AddTriangleFilled(
        ImVec2(playheadX - 5.0f, origin.y),
        ImVec2(playheadX + 5.0f, origin.y),
        ImVec2(playheadX,        origin.y + 8.0f),
        IM_COL32(255, 60, 60, 255));

    // Interaction: click / drag anywhere in the strip below the ruler to
    // scrub. Suppressed while a keyframe diamond is being dragged so the
    // two interactions don't fight over the same mouse events.
    //
    // Task 5.3-fix: playhead SNAPS to nearby keyframes on scrub. When the
    // mouse is within 8px of any diamond's X, the playhead's time snaps to
    // that diamond's exact time. Makes it easy to land ON a keyframe when
    // editing rather than at some arbitrary time next to it.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("TimelineStripHit", ImVec2(stripW, totalH));
    if (!diamondDragActive && ImGui::IsItemActive()) {
        const ImVec2 mp2 = ImGui::GetIO().MousePos;
        if (mp2.x >= trackX0 && mp2.x <= trackX1) {
            float snappedTime = XToTime(mp2.x);
            constexpr float kSnapPixels = 8.0f;
            float bestDx = kSnapPixels + 1.0f;
            // Scan all keyframes across all visible layers for a close match.
            for (const auto& L : layerManager.Layers()) {
                if (!L.isVisible) continue;
                auto scan = [&](const auto& prop) {
                    for (const auto& k : prop.keyframes) {
                        const float dx = std::fabs(TimeToX(k.time) - mp2.x);
                        if (dx < bestDx) { bestDx = dx; snappedTime = k.time; }
                    }
                };
                scan(L.transform.position);
                scan(L.transform.rotation);
                scan(L.transform.scale);
                scan(L.transform.opacity);
            }
            animEngine.currentTime = snappedTime;
            animEngine.isPlaying   = false; // scrubbing pauses playback
        }
    }
}

void RenderEngine::DrawInspectorPanel() {
    Layer* sel = layerManager.GetSelectedLayer();
    if (!sel) {
        ImGui::TextDisabled("No layer selected. Add one from the Timeline.");
        return;
    }

    // Editable layer name (always visible above the tab bar so it's obvious
    // which layer you're editing regardless of which tab is active).
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        sel->name = nameBuf;
    }
    ImGui::Text("Layer ID: %d   Parent ID: %d", sel->id, sel->parentId);
    ImGui::Separator();

    // Task 5.0: Inspector is tabbed so per-layer and global-clock properties
    // stop mixing together in one long scroll.
    if (!ImGui::BeginTabBar("InspectorTabs")) return;

    if (ImGui::BeginTabItem("Transform")) {
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Task 5.0: AE-style stopwatch UX. Each property has a small orange
        // circle button ("stopwatch"). Click it -> keyframing turns ON and a
        // first keyframe is dropped at the current time. Any subsequent
        // value change (drag, type) auto-adds a keyframe at the current time.
        // Click stopwatch again -> all keys for that property are removed,
        // the property becomes static.
        //
        // Colors: lit ORANGE dot when the stopwatch is on; DIM gray when off.
        const float t = animEngine.currentTime;
        auto stopwatch = [&](const char* id, bool lit) {
            // Task 5.0-c: the 'id' parameter used to be discarded, so all four
            // stopwatch buttons ended up with the same ImGui ID (derived from
            // the visible label). ImGui debug mode caught this and printed a
            // "4 visible items with conflicting ID!" popup on hover, and worse,
            // clicking any stopwatch could target the wrong track.
            //
            // Build the label as "<glyph>##<id>" so the visible text is just
            // the glyph but the ID hash includes the caller's unique id string.
            char label[16];
            std::snprintf(label, sizeof(label), "%s##%s", lit ? "(*)" : "( )", id);
            ImGui::PushStyleColor(ImGuiCol_Button,
                lit ? IM_COL32(240, 130, 30, 255) : IM_COL32(60, 60, 70, 255));
            const bool clicked = ImGui::Button(label, ImVec2(28, 0));
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", lit
                    ? "Stopwatch ON: value changes auto-key. Click to disable + clear."
                    : "Stopwatch OFF: property is static. Click to enable keyframing.");
            }
            return clicked;
        };

        // Task 5.1 (AE / Lottie model): each property is an AnimatedProperty<T>.
        // The pattern for every widget is now identical:
        //   1. Read current value via Evaluate(t) into a local temp
        //   2. Present the widget bound to the temp
        //   3. If the widget returned true, call SetValue(t, temp) — that
        //      routes the write into either a keyframe (stopwatch ON) or
        //      the staticValue (stopwatch OFF). Zero ambiguity.
        //
        // Because Evaluate() returns the CURRENT canvas value (respecting
        // keyframe interpolation), the Inspector field ALWAYS shows what
        // the user sees on screen — killing the "field says 423 but I
        // typed 500" confusion of the previous architecture.

        // Task 5.3-fix: snapshot when the slider is ACTIVATED (first click),
        // not when it's deactivated. Snapshotting after-edit recorded the
        // post-edit state so the first Ctrl+Z looked like a no-op ("nothing
        // changed") and the user had to press it twice. IsItemActivated
        // fires the single frame the widget goes from inactive to active,
        // which is when the "before" state is still intact.

        // POSITION (Vec3)
        if (stopwatch("pos", sel->transform.position.HasStopwatch())) {
            MarkForSnapshot();
            sel->transform.position.ToggleStopwatch(t);
        }
        ImGui::SameLine();
        Vec3 pos = sel->transform.position.Evaluate(t);
        if (ImGui::DragFloat3("Position (x,y,z)", &pos.x, 1.0f))
            sel->transform.position.SetValue(t, pos);
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // ROTATION (Vec3, degrees)
        if (stopwatch("rot", sel->transform.rotation.HasStopwatch())) {
            MarkForSnapshot();
            sel->transform.rotation.ToggleStopwatch(t);
        }
        ImGui::SameLine();
        Vec3 rot = sel->transform.rotation.Evaluate(t);
        if (ImGui::DragFloat3("Rotation (deg)", &rot.x, 0.5f))
            sel->transform.rotation.SetValue(t, rot);
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // SCALE (Vec3) + linked-scale chain button (Task 5.3-fix-3).
        // The chain button between the stopwatch and the DragFloat3 toggles
        // uniform scaling. When linked, editing scale.x auto-updates scale.y
        // and scale.z to preserve the current X:Y:Z ratio (matches AE's
        // classic chain icon behavior).
        if (stopwatch("scl", sel->transform.scale.HasStopwatch())) {
            MarkForSnapshot();
            sel->transform.scale.ToggleStopwatch(t);
        }
        ImGui::SameLine();
        // Chain-link button. Lit orange when linked, dim gray when unlinked.
        // Text label is a simple ASCII glyph so we don't depend on a font.
        {
            ImGui::PushStyleColor(ImGuiCol_Button,
                linkedScale ? IM_COL32(240, 130, 30, 255)
                            : IM_COL32(60, 60, 70, 255));
            if (ImGui::Button(linkedScale ? "8##link" : "o##link", ImVec2(22, 0))) {
                linkedScale = !linkedScale;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", linkedScale
                    ? "Uniform scale ON: X drives Y and Z. Click to unlink."
                    : "Uniform scale OFF: X, Y, Z edit independently. Click to link.");
            }
        }
        ImGui::SameLine();
        Vec3 scl = sel->transform.scale.Evaluate(t);
        const Vec3 sclBefore = scl;
        if (ImGui::DragFloat3("Scale", &scl.x, 0.01f, -10.0f, 10.0f)) {
            if (linkedScale) {
                // Which component did the user actually drag? Compare against
                // sclBefore; whichever differs is the "driver". If X changed,
                // scale Y and Z by the same ratio (or absolute for zero-start).
                auto applyLink = [&](float& driverBefore, float& driverAfter,
                                     float& yBefore, float& yAfter,
                                     float& zBefore, float& zAfter) {
                    if (std::fabs(driverBefore) < 1e-6f) {
                        // Ratio undefined; fall back to setting all to driver.
                        yAfter = driverAfter;
                        zAfter = driverAfter;
                    } else {
                        const float ratio = driverAfter / driverBefore;
                        yAfter = yBefore * ratio;
                        zAfter = zBefore * ratio;
                    }
                };
                if (std::fabs(scl.x - sclBefore.x) > 1e-6f) {
                    Vec3 b = sclBefore, a = scl;
                    applyLink(b.x, a.x, b.y, a.y, b.z, a.z);
                    scl = a;
                } else if (std::fabs(scl.y - sclBefore.y) > 1e-6f) {
                    Vec3 b = sclBefore, a = scl;
                    applyLink(b.y, a.y, b.x, a.x, b.z, a.z);
                    scl = a;
                } else if (std::fabs(scl.z - sclBefore.z) > 1e-6f) {
                    Vec3 b = sclBefore, a = scl;
                    applyLink(b.z, a.z, b.x, a.x, b.y, a.y);
                    scl = a;
                }
            }
            sel->transform.scale.SetValue(t, scl);
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // ANCHOR (Vec2) — animatable but no stopwatch UI for it in Task 5.1
        Vec2 anchor = sel->transform.anchorPoint.Evaluate(t);
        if (ImGui::DragFloat2("Anchor (0..1)", &anchor.x, 0.01f, 0.0f, 1.0f))
            sel->transform.anchorPoint.SetValue(t, anchor);
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // SIZE (Vec2)
        Vec2 size = sel->transform.sizePixels.Evaluate(t);
        if (ImGui::DragFloat2("Size (px)", &size.x, 1.0f, 1.0f, 4096.0f))
            sel->transform.sizePixels.SetValue(t, size);
        if (ImGui::IsItemActivated()) MarkForSnapshot();
        ImGui::TextDisabled("Size = base authoring pixels. Scale = animation multiplier.");

        // OPACITY (float)
        if (stopwatch("op", sel->transform.opacity.HasStopwatch())) {
            MarkForSnapshot();
            sel->transform.opacity.ToggleStopwatch(t);
        }
        ImGui::SameLine();
        float op = sel->transform.opacity.Evaluate(t);
        if (ImGui::SliderFloat("Opacity", &op, 0.0f, 1.0f))
            sel->transform.opacity.SetValue(t, op);
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // Task 5.0: help hint so users don't need to guess the workflow.
        ImGui::TextDisabled("To animate: click the stopwatch, move the playhead,");
        ImGui::TextDisabled("change the value. Repeat for each keyframe.");

        // Task 4.5: quick "Alight-style HUD attachment" toggle when using
        // the Alight camera style. Grayed out under AE style.
        const bool alightMode = (cameraStyle == CameraStyle::AlightMotion);
        if (!alightMode) ImGui::BeginDisabled();
        ImGui::Checkbox("Stick to Camera (Alight HUD)", &sel->stickToCamera);
        if (!alightMode) ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Fill", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ImGui ColorEdit uses float[4] in RGBA; we store IM_COL32 ABGR.
        unsigned int c = sel->fillColor;
        float rgba[4] = {
            ((c >>  0) & 0xFF) / 255.0f,
            ((c >>  8) & 0xFF) / 255.0f,
            ((c >> 16) & 0xFF) / 255.0f,
            ((c >> 24) & 0xFF) / 255.0f,
        };
        if (ImGui::ColorEdit4("Color", rgba)) {
            sel->fillColor = IM_COL32(
                (int)(rgba[0] * 255.0f),
                (int)(rgba[1] * 255.0f),
                (int)(rgba[2] * 255.0f),
                (int)(rgba[3] * 255.0f));
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();
    }

    // Task 5.10: Layer timing — in/out point. Numeric only (drag the bar
    // in the timeline for graphical trimming). Sentinel outPoint=-1 is
    // displayed as the comp duration so the user sees an actual number;
    // typing back a value <0 restores the sentinel.
    if (ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen)) {
        float inPt  = sel->inPoint;
        float outPt = (sel->outPoint < 0.0f) ? animEngine.duration : sel->outPoint;
        if (ImGui::InputFloat("In (s)##inPt", &inPt, 0.1f, 1.0f, "%.3f")) {
            MarkForSnapshot();
            if (inPt < 0.0f) inPt = 0.0f;
            if (inPt > outPt - 0.01f) inPt = std::max(0.0f, outPt - 0.01f);
            sel->inPoint = inPt;
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();
        if (ImGui::InputFloat("Out (s)##outPt", &outPt, 0.1f, 1.0f, "%.3f")) {
            MarkForSnapshot();
            if (outPt < sel->inPoint + 0.01f) outPt = sel->inPoint + 0.01f;
            if (outPt > animEngine.duration)  outPt = animEngine.duration;
            sel->outPoint = outPt;
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();
        if (ImGui::Button("Reset (extend to comp end)")) {
            MarkForSnapshot();
            sel->outPoint = -1.0f;
        }
        ImGui::TextDisabled("Trim clips visibility only. Keyframe times don't shift.");
    }

    // Task 5.10: per-layer blend mode. Reuses Effect.h's BlendMode enum
    // (6 modes). Overlay + ColorDodge are fixed-function approximations
    // for v1 — true per-channel math needs a pixel shader (deferred).
    if (ImGui::CollapsingHeader("Blend", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kBlendLabels[] = {
            "Normal", "Additive", "Multiply", "Screen", "Overlay", "ColorDodge"
        };
        int bIdx = (int)sel->blend;
        if (ImGui::Combo("Mode##layerBlend", &bIdx, kBlendLabels, 6)) {
            MarkForSnapshot();
            sel->blend = (BlendMode)bIdx;
        }
        ImGui::TextDisabled("Overlay / ColorDodge are approximations for v1.");
    }

    // Task 5.7: Stroke controls. Live for every shape type. Setting width
    // to 0 (default) reproduces the pre-5.7 no-stroke look. MarkForSnapshot
    // fires on IsItemActivated (mouse-down) of each widget so Ctrl+Z
    // rewinds each tweak cleanly. Matches the Transform inspector's pattern.
    if (ImGui::CollapsingHeader("Stroke", ImGuiTreeNodeFlags_DefaultOpen)) {
        unsigned int sc = sel->strokeColor;
        float srgba[4] = {
            ((sc >>  0) & 0xFF) / 255.0f,
            ((sc >>  8) & 0xFF) / 255.0f,
            ((sc >> 16) & 0xFF) / 255.0f,
            ((sc >> 24) & 0xFF) / 255.0f,
        };
        if (ImGui::ColorEdit4("Stroke Color", srgba)) {
            sel->strokeColor = IM_COL32(
                (int)(srgba[0] * 255.0f),
                (int)(srgba[1] * 255.0f),
                (int)(srgba[2] * 255.0f),
                (int)(srgba[3] * 255.0f));
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        ImGui::SliderFloat("Stroke Width (px)", &sel->strokeWidth, 0.0f, 64.0f, "%.1f");
        if (ImGui::IsItemActivated()) MarkForSnapshot();
    }

    // Task 5.7: Corner radius. Rectangle only — hide for other shapes since
    // the shader ignores it there anyway. Slider max = min(w,h)/2 evaluated
    // at the current comp time so the slider adapts to animated size (pre-go
    // review #2: UI-side dynamic clamp; shader also clamps for safety).
    if (sel->type == ShapeType::Rectangle &&
        ImGui::CollapsingHeader("Corners", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Vec2 sz = sel->transform.sizePixels.Evaluate(animEngine.currentTime);
        const float maxR = std::max(0.5f, std::min(sz.x, sz.y) * 0.5f);
        ImGui::SliderFloat("Radius (px)", &sel->cornerRadius, 0.0f, maxR, "%.1f");
        if (ImGui::IsItemActivated()) MarkForSnapshot();
        // Belt-and-braces: clamp again after the slider write in case
        // sizePixels shrinks in a later frame while radius stayed large.
        sel->cornerRadius = std::clamp(sel->cornerRadius, 0.0f, maxR);
    }

    // Task 5.9: Text properties. Only visible for Text layers.
    if (sel->type == ShapeType::Text &&
        ImGui::CollapsingHeader("Text", ImGuiTreeNodeFlags_DefaultOpen)) {

        // Editable string. MarkForSnapshot on activation so a burst of
        // keystrokes collapses into one undo step.
        char textBuf[512];
        std::snprintf(textBuf, sizeof(textBuf), "%s", sel->textProps.text.c_str());
        if (ImGui::InputText("Text##textStr", textBuf, sizeof(textBuf))) {
            sel->textProps.text = textBuf;
        }
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // Font picker. Favorites section pinned at top, separator, then all
        // system fonts alphabetically. Each row has a star toggle on the
        // right that flips the favorite state (persisted to
        // %LOCALAPPDATA%\PotatoMotion\fonts.json at end of frame).
        if (ImGui::BeginCombo("Font##textFont", sel->textProps.fontFamily.c_str())) {
            // Favorites first
            for (const auto& fam : favoriteFonts) {
                ImGui::PushID(("fav_" + fam).c_str());
                const bool selected = (fam == sel->textProps.fontFamily);
                if (ImGui::Selectable(("* " + fam).c_str(), selected,
                                       ImGuiSelectableFlags_AllowOverlap)) {
                    MarkForSnapshot();
                    sel->textProps.fontFamily = fam;
                }
                // Star toggle at right edge (unstar removes from favorites)
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 20.0f);
                if (ImGui::SmallButton("x##unfav")) {
                    ToggleFontFavorite(fam);
                }
                ImGui::PopID();
            }
            if (!favoriteFonts.empty()) ImGui::Separator();
            // All system fonts
            for (const auto& fam : systemFonts) {
                ImGui::PushID(("all_" + fam).c_str());
                const bool selected = (fam == sel->textProps.fontFamily);
                if (ImGui::Selectable(fam.c_str(), selected,
                                       ImGuiSelectableFlags_AllowOverlap)) {
                    MarkForSnapshot();
                    sel->textProps.fontFamily = fam;
                }
                // Star toggle
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 20.0f);
                const bool isFav = (favoriteFonts.count(fam) > 0);
                const char* starLbl = isFav ? "*##fav" : "o##fav";
                if (ImGui::SmallButton(starLbl)) {
                    ToggleFontFavorite(fam);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        // Size (px)
        ImGui::SliderFloat("Size (px)##textSz", &sel->textProps.fontSize,
                           4.0f, 512.0f, "%.0f");
        if (ImGui::IsItemActivated()) MarkForSnapshot();

        // Task 5.9-fix: Weight as a named combo (AE/Figma convention). Slider
        // was misleading — most fonts ship only 2-4 weights and DirectWrite
        // silently substitutes, so intermediate slider values did nothing
        // visible. Named weights make substitution honest: pick 'Medium'
        // and see if it looks distinct from 'Regular' for THIS font.
        static const char* kWeightLabels[] = {
            "100 Thin", "200 ExtraLight", "300 Light",
            "400 Regular", "500 Medium", "600 SemiBold",
            "700 Bold", "800 ExtraBold", "900 Black"
        };
        static const int kWeightValues[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
        int wIdx = 3; // Regular default
        for (int i = 0; i < 9; ++i) {
            if (sel->textProps.fontWeight == kWeightValues[i]) { wIdx = i; break; }
        }
        if (ImGui::Combo("Weight##textWt", &wIdx, kWeightLabels, 9)) {
            MarkForSnapshot();
            sel->textProps.fontWeight = kWeightValues[wIdx];
        }

        // Italic
        if (ImGui::Checkbox("Italic##textIt", &sel->textProps.italic)) {
            MarkForSnapshot();
        }

        // Alignment radios
        int align = sel->textProps.alignment;
        if (ImGui::RadioButton("Left##al",   align == 0)) { MarkForSnapshot(); sel->textProps.alignment = 0; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Center##al", align == 1)) { MarkForSnapshot(); sel->textProps.alignment = 1; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Right##al",  align == 2)) { MarkForSnapshot(); sel->textProps.alignment = 2; }

        ImGui::TextDisabled("Color animates via Fill above. Position / rotation /");
        ImGui::TextDisabled("scale / opacity animate via Transform. Font size / string");
        ImGui::TextDisabled("changes rebuild the atlas (~5ms one-shot).");
    }

        ImGui::EndTabItem();
    } // end Transform tab

    // -------------------------------------------------------------------
    // Effect Controls tab — reuses the standalone panel's body so both
    // access paths (tab OR dockable "Effect Controls" panel) look identical.
    // -------------------------------------------------------------------
    if (ImGui::BeginTabItem("Effect Controls")) {
        DrawEffectControlsPanel();
        ImGui::EndTabItem();
    }

    // -------------------------------------------------------------------
    // Global tab — properties that belong to the whole composition, not
    // any single layer. Previously these were bleeding into the per-layer
    // Inspector (Task 4.5 screenshot review flagged this).
    // -------------------------------------------------------------------
    if (ImGui::BeginTabItem("Global")) {
    if (ImGui::CollapsingHeader("Composition Clock", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(animEngine.isPlaying ? "Pause" : "Play")) {
            if (animEngine.isPlaying) animEngine.Pause(); else animEngine.Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) animEngine.Reset();
        ImGui::Checkbox("Loop", &animEngine.isLooping);
        // Task 5.10 (user request #7a): slider + paired InputFloat for
        // click-to-type. Both bind to animEngine.duration.
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("##durSlider_gl", &animEngine.duration, 0.1f, 60.0f);
        if (ImGui::IsItemActivated()) MarkForSnapshot();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat("Duration (s)##durInput_gl", &animEngine.duration,
                              0.1f, 1.0f, "%.3f")) {
            MarkForSnapshot();
            if (animEngine.duration < 0.1f) animEngine.duration = 0.1f;
        }
        ImGui::Value("Time (s)", animEngine.currentTime);
    }

    if (ImGui::CollapsingHeader("Slingshot Bezier Handles", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("P1 (control)", &animEngine.currentCurve.P1.x, 0.01f, -2.0f, 2.0f);
        ImGui::DragFloat2("P2 (control)", &animEngine.currentCurve.P2.x, 0.01f, -2.0f, 2.0f);
        ImGui::TextWrapped("P1.y and P2.y are intentionally unclamped so you can push above 1.0 for slingshot overshoot or below 0.0 for rebound.");
        // Task 5.4: shortcut to push the slingshot curve into a real keyframe
        // as a per-key Bezier ease. This turns the old demo into a usable
        // preset generator: pick a key in the Graph Editor, click here, done.
        ImGui::Separator();
        const bool canApply =
            (graphSelectedLayerId >= 0 && graphSelectedKeyIndex >= 0);
        if (!canApply) ImGui::BeginDisabled();
        if (ImGui::Button("Apply as Ease to Selected Key")) {
            Layer* target = layerManager.GetLayerById(graphSelectedLayerId);
            if (target) {
                MarkForSnapshot();
                // AE-native mapping: influence % = (1 - handle.x) * 100 for
                // each side; speed left at zero for a classic ease shape.
                // Users can then drag handles in the graph to add overshoot
                // via non-zero speed. This is the same recipe AE's Easy Ease
                // uses (speed=0, influence=33.33%) just parameterized from
                // the demo curve's P1/P2 handles.
                const Vec2 p1 = animEngine.currentCurve.P1;
                const Vec2 p2 = animEngine.currentCurve.P2;
                auto applyToKey = [&](auto& prop) {
                    if (graphSelectedKeyIndex < 0 ||
                        graphSelectedKeyIndex >= (int)prop.keyframes.size()) return;
                    auto& k = prop.keyframes[graphSelectedKeyIndex];
                    k.incomingMode  = InterpMode::Bezier;
                    k.outgoingMode  = InterpMode::Bezier;
                    // P1.x is the outgoing handle's normalized time (0..1);
                    // its distance from the key = influence fraction.
                    k.outInfluence  = std::clamp(p1.x * 100.0f, 0.0f, 100.0f);
                    // P2.x is the incoming handle's normalized time; distance
                    // from the NEXT key = (1 - p2.x). That's the influence
                    // on the incoming side of our selected key.
                    k.inInfluence   = std::clamp((1.0f - p2.x) * 100.0f, 0.0f, 100.0f);
                    using SpeedT = std::decay_t<decltype(k.inSpeed)>;
                    k.inSpeed  = SpeedT{};
                    k.outSpeed = SpeedT{};
                };
                switch (graphPropGroup) {
                    case GraphPropGroup::Position: applyToKey(target->transform.position); break;
                    case GraphPropGroup::Rotation: applyToKey(target->transform.rotation); break;
                    case GraphPropGroup::Scale:    applyToKey(target->transform.scale);    break;
                    case GraphPropGroup::Opacity:  applyToKey(target->transform.opacity);  break;
                    default: break;
                }
                SetStatus("Slingshot curve applied as ease to selected key.");
            }
        }
        if (!canApply) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("(Select a key in the Graph Editor first.)");
        }
    }

    // -----------------------------------------------------------------------
    // Camera Properties (Task 4). Only shown when View -> Show 3D Features
    // is enabled (2D-first UX as of Task 5.0-b).
    // -----------------------------------------------------------------------
    const bool cameraSelected = (sel->type == ShapeType::Camera);
    if (show3DFeatures && ImGui::CollapsingHeader(cameraSelected ? "Camera Properties (Active)" : "Camera Properties",
                                cameraSelected ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        const int camLayerId = layerManager.FindActiveCameraLayerId();
        if (camLayerId >= 0) {
            ImGui::TextDisabled("Driven by Camera layer #%d", camLayerId);
        } else {
            ImGui::TextDisabled("(No Camera layer — using global camera)");
        }
        ImGui::DragFloat3("Cam Position",   &camera.position.x, 1.0f);
        ImGui::DragFloat3("Cam Target",     &camera.target.x,   1.0f);
        ImGui::Checkbox("Use LookAt Target", &camera.useTargetMode);
        if (!camera.useTargetMode) {
            ImGui::DragFloat3("Cam Rotation (deg)", &camera.rotation.x, 0.5f);
        }
        ImGui::SliderFloat("FOV (vertical, deg)", &camera.fov, 5.0f, 120.0f);
        ImGui::DragFloatRange2("Near / Far Z", &camera.nearZ, &camera.farZ,
                               1.0f, 0.1f, 100000.0f);
        if (ImGui::Button("Reset Camera")) {
            camera.ResetToDefault();
        }
    }
        ImGui::EndTabItem();
    } // end Global tab

    ImGui::EndTabBar();
}

// =============================================================================
// Viewport rendering
// =============================================================================

// Transform a Mat3-local point into viewport-panel screen space.
static ImVec2 ToScreen(const Vec2& worldPt, ImVec2 canvasOrigin) {
    return ImVec2(canvasOrigin.x + worldPt.x, canvasOrigin.y + worldPt.y);
}

void RenderEngine::DrawLayerShape(const Layer& layer, const Mat3& worldMatrix,
                                  float worldOpacity, ImVec2 canvasOrigin,
                                  ImDrawList* drawList) {
    if (!drawList) return;

    // Task 5.1: legacy path (unused since Task 5.0 CompositionRenderer took
    // over) but kept for potential preview/debug re-use. Reads AnimatedProperty
    // fields via Evaluate at the current comp time.
    const float t = animEngine.currentTime;
    const Vec2  sz = layer.transform.sizePixels.Evaluate(t);
    const float w = sz.x;
    const float h = sz.y;
    const Vec2 c0 = worldMatrix.TransformPoint(Vec2(0.0f, 0.0f));
    const Vec2 c1 = worldMatrix.TransformPoint(Vec2(w,    0.0f));
    const Vec2 c2 = worldMatrix.TransformPoint(Vec2(w,    h));
    const Vec2 c3 = worldMatrix.TransformPoint(Vec2(0.0f, h));

    const ImVec2 p0 = ToScreen(c0, canvasOrigin);
    const ImVec2 p1 = ToScreen(c1, canvasOrigin);
    const ImVec2 p2 = ToScreen(c2, canvasOrigin);
    const ImVec2 p3 = ToScreen(c3, canvasOrigin);

    // Bake opacity into the fill color's alpha channel.
    unsigned int c = layer.fillColor;
    unsigned int alpha = (c >> 24) & 0xFF;
    alpha = (unsigned int)std::clamp((int)(alpha * worldOpacity), 0, 255);
    unsigned int fill = (c & 0x00FFFFFFu) | (alpha << 24);

    switch (layer.type) {
        case ShapeType::Rectangle: {
            const ImVec2 quad[4] = { p0, p1, p2, p3 };
            drawList->AddConvexPolyFilled(quad, 4, fill);
            drawList->AddPolyline(quad, 4, IM_COL32(255,255,255, alpha), ImDrawFlags_Closed, 1.0f);
            break;
        }
        case ShapeType::Ellipse: {
            // ImGui doesn't have a rotated ellipse primitive; approximate with
            // a 48-segment polyline transformed through the world matrix.
            constexpr int kSegs = 48;
            ImVec2 pts[kSegs];
            const float rx = w * 0.5f;
            const float ry = h * 0.5f;
            for (int i = 0; i < kSegs; ++i) {
                const float t = (float)i / (float)kSegs * 2.0f * POTATO_PI;
                Vec2 local(rx + rx * std::cos(t), ry + ry * std::sin(t));
                pts[i] = ToScreen(worldMatrix.TransformPoint(local), canvasOrigin);
            }
            drawList->AddConvexPolyFilled(pts, kSegs, fill);
            drawList->AddPolyline(pts, kSegs, IM_COL32(255,255,255, alpha), ImDrawFlags_Closed, 1.0f);
            break;
        }
        case ShapeType::CustomPath: {
            // Placeholder marker until a real path editor lands.
            drawList->AddCircle(ToScreen(worldMatrix.TransformPoint(Vec2(w*0.5f, h*0.5f)), canvasOrigin),
                                std::max(w, h) * 0.35f,
                                IM_COL32(200, 200, 200, alpha), 24, 1.5f);
            break;
        }
        case ShapeType::Camera:
            // Camera has its own dedicated gizmo drawn by DrawCameraGizmos —
            // no fill in the 2D pass.
            break;
        case ShapeType::Null: {
            // Invisible transform-only layer: draw a small X + label so the
            // user can see and click it, but never a fill.
            const ImVec2 c = ToScreen(worldMatrix.TransformPoint(Vec2(w*0.5f, h*0.5f)), canvasOrigin);
            const float r = std::min(w, h) * 0.35f;
            const ImU32 col = IM_COL32(180, 180, 180, alpha);
            drawList->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.5f);
            drawList->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 1.5f);
            drawList->AddText(ImVec2(c.x + r + 4, c.y - 6), col, layer.name.c_str());
            break;
        }
    }
}

void RenderEngine::DrawSelectionGizmos(Layer& layer, const Mat3& worldMatrix,
                                       ImVec2 canvasOrigin, ImDrawList* drawList) {
    if (!drawList) return;

    // Task 5.1: legacy path (Task 5.0 inlined the equivalent into
    // DrawViewportCanvas). Kept for potential re-use; reads via Evaluate.
    const float t = animEngine.currentTime;
    const Vec2  sz = layer.transform.sizePixels.Evaluate(t);
    const float w = sz.x;
    const float h = sz.y;

    // Corners: NW=(0,0)  NE=(w,0)  SE=(w,h)  SW=(0,h)
    const Vec2 nw = worldMatrix.TransformPoint(Vec2(0.0f, 0.0f));
    const Vec2 ne = worldMatrix.TransformPoint(Vec2(w,    0.0f));
    const Vec2 se = worldMatrix.TransformPoint(Vec2(w,    h));
    const Vec2 sw = worldMatrix.TransformPoint(Vec2(0.0f, h));

    const ImVec2 pnw = ToScreen(nw, canvasOrigin);
    const ImVec2 pne = ToScreen(ne, canvasOrigin);
    const ImVec2 pse = ToScreen(se, canvasOrigin);
    const ImVec2 psw = ToScreen(sw, canvasOrigin);

    const ImU32 outline = IM_COL32(0, 255, 200, 255);
    const ImU32 handle  = IM_COL32(255, 220, 60, 255);

    const ImVec2 box[4] = { pnw, pne, pse, psw };
    drawList->AddPolyline(box, 4, outline, ImDrawFlags_Closed, 1.5f);

    const float R = 5.0f;
    drawList->AddRectFilled(ImVec2(pnw.x-R, pnw.y-R), ImVec2(pnw.x+R, pnw.y+R), handle);
    drawList->AddRectFilled(ImVec2(pne.x-R, pne.y-R), ImVec2(pne.x+R, pne.y+R), handle);
    drawList->AddRectFilled(ImVec2(pse.x-R, pse.y-R), ImVec2(pse.x+R, pse.y+R), handle);
    drawList->AddRectFilled(ImVec2(psw.x-R, psw.y-R), ImVec2(psw.x+R, psw.y+R), handle);

    // Center move handle (anchor point in world space)
    const Vec2 anchor = layer.transform.anchorPoint.Evaluate(t);
    const Vec2 center = worldMatrix.TransformPoint(Vec2(anchor.x * w, anchor.y * h));
    drawList->AddCircleFilled(ToScreen(center, canvasOrigin), 6.0f, IM_COL32(255, 80, 80, 255));
}

// Convert a viewport-panel screen point into composition ("world") space.
static Vec2 ScreenToWorld(ImVec2 screen, ImVec2 canvasOrigin) {
    return Vec2(screen.x - canvasOrigin.x, screen.y - canvasOrigin.y);
}

void RenderEngine::HandleGizmoInteraction(Layer& layer, const Mat3& worldMatrix,
                                          ImVec2 canvasOrigin, ImVec2 canvasSize) {
    // Task 5.1: reads via .Evaluate(t), writes via .SetValue(t, v).
    // If a property's stopwatch is on, the write becomes a keyframe at t;
    // otherwise it updates staticValue. Same call site either way.
    const float t = animEngine.currentTime;

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // Snapshot the animated properties for THIS frame.
    const Vec3 posEval  = layer.transform.position   .Evaluate(t);
    const Vec3 scaleEval = layer.transform.scale     .Evaluate(t);
    const Vec2 sizeEval = layer.transform.sizePixels .Evaluate(t);
    const Vec2 anchorEval = layer.transform.anchorPoint.Evaluate(t);
    const float w = sizeEval.x;
    const float h = sizeEval.y;

    const Vec2 nw = worldMatrix.TransformPoint(Vec2(0.0f, 0.0f));
    const Vec2 ne = worldMatrix.TransformPoint(Vec2(w,    0.0f));
    const Vec2 se = worldMatrix.TransformPoint(Vec2(w,    h));
    const Vec2 sw = worldMatrix.TransformPoint(Vec2(0.0f, h));
    const Vec2 center = worldMatrix.TransformPoint(Vec2(anchorEval.x * w, anchorEval.y * h));

    const Vec2 mouseCanvas = ScreenToCanvas(mouse);
    auto dist = [](Vec2 a, Vec2 b){
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx*dx + dy*dy);
    };
    // Task 5.8: pair with ScreenToCanvas (which returns COMPOSITION coords),
    // so the screen->canvas scale factor uses composition dims, not RT dims.
    const float scale = (lastCanvasLetterboxSize.x > 1.0f)
                            ? ((float)compositionWidth / lastCanvasLetterboxSize.x)
                            : 1.0f;
    const float kHit = 12.0f * scale;

    ImGui::SetCursorScreenPos(canvasOrigin);
    ImGui::InvisibleButton("ViewportHitTest", canvasSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    // Begin drag on click
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && activeGizmo == GizmoMode::None) {
        GizmoMode mode = GizmoMode::None;
        if      (dist(mouseCanvas, nw) < kHit) mode = GizmoMode::ScaleNW;
        else if (dist(mouseCanvas, ne) < kHit) mode = GizmoMode::ScaleNE;
        else if (dist(mouseCanvas, se) < kHit) mode = GizmoMode::ScaleSE;
        else if (dist(mouseCanvas, sw) < kHit) mode = GizmoMode::ScaleSW;
        else if (dist(mouseCanvas, center) < kHit + 2.0f) mode = GizmoMode::Move;
        else {
            const Mat3 inv = worldMatrix.InverseAffine();
            const Vec2 local = inv.TransformPoint(mouseCanvas);
            if (local.x >= 0.0f && local.x <= w && local.y >= 0.0f && local.y <= h) {
                mode = GizmoMode::Move;
            }
        }
        if (mode != GizmoMode::None) {
            // Task 5.3-fix: snapshot BEFORE the drag starts so Ctrl+Z jumps
            // back to the pre-drag state on the first press. Snapshotting
            // AFTER the drag ended (previous behavior) recorded the same
            // state Ctrl+Z would target, making the first Ctrl+Z look like
            // a no-op and requiring two presses to visibly rewind.
            MarkForSnapshot();
            activeGizmo         = mode;
            dragLayerId         = layer.id;
            dragStartMouseLocal = mouseCanvas;
            dragStartPosition   = { posEval.x, posEval.y };
            dragStartScale      = scaleEval;
            dragStartSize       = sizeEval;
        }
    }

    // Continue drag
    if (activeGizmo != GizmoMode::None && dragLayerId == layer.id && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 delta = { mouseCanvas.x - dragStartMouseLocal.x,
                             mouseCanvas.y - dragStartMouseLocal.y };
        if (activeGizmo == GizmoMode::Move) {
            // Task 5.1: route the write through SetValue so it auto-keys
            // if the position stopwatch is on.
            Vec3 newPos = posEval;
            newPos.x = dragStartPosition.x + delta.x;
            newPos.y = dragStartPosition.y + delta.y;
            layer.transform.position.SetValue(t, newPos);
        } else {
            const Mat3 inv = worldMatrix.InverseAffine();
            const Vec2 anchorLocal(anchorEval.x * dragStartSize.x,
                                   anchorEval.y * dragStartSize.y);
            const Vec2 mouseLocal = inv.TransformPoint(mouseCanvas);

            Vec2 handleLocalStart;
            switch (activeGizmo) {
                case GizmoMode::ScaleNW: handleLocalStart = Vec2(0.0f,             0.0f);             break;
                case GizmoMode::ScaleNE: handleLocalStart = Vec2(dragStartSize.x,  0.0f);             break;
                case GizmoMode::ScaleSE: handleLocalStart = Vec2(dragStartSize.x,  dragStartSize.y);  break;
                case GizmoMode::ScaleSW: handleLocalStart = Vec2(0.0f,             dragStartSize.y);  break;
                default: handleLocalStart = anchorLocal; break;
            }
            const float startDX = handleLocalStart.x - anchorLocal.x;
            const float startDY = handleLocalStart.y - anchorLocal.y;
            const float curDX   = mouseLocal.x       - anchorLocal.x;
            const float curDY   = mouseLocal.y       - anchorLocal.y;

            const float sx = (std::fabs(startDX) > 0.5f) ? (curDX / startDX) : 1.0f;
            const float sy = (std::fabs(startDY) > 0.5f) ? (curDY / startDY) : 1.0f;

            // Task 5.1: route write through SetValue so scale stopwatch is honored.
            Vec3 newScale = dragStartScale;
            newScale.x = std::clamp(dragStartScale.x * sx, -20.0f, 20.0f);
            newScale.y = std::clamp(dragStartScale.y * sy, -20.0f, 20.0f);
            layer.transform.scale.SetValue(t, newScale);
        }
    }

    // End drag on release. Task 5.3-fix: no snapshot here — we already
    // snapshotted BEFORE the drag started, so Ctrl+Z rewinds to pre-drag.
    if (activeGizmo != GizmoMode::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        activeGizmo = GizmoMode::None;
        dragLayerId = -1;
    }

    // Click-to-select any layer under the mouse (only when not dragging a handle)
    if (activeGizmo == GizmoMode::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto& L = layerManager.Layers();
        for (auto it = L.rbegin(); it != L.rend(); ++it) {
            if (!it->isVisible) continue;
            if (it->is3D || it->type == ShapeType::Camera) continue;
            const Mat3 wm = layerManager.GetWorldMatrix(it->id);
            const Mat3 inv = wm.InverseAffine();
            const Vec2 local = inv.TransformPoint(mouseCanvas);
            const Vec2 sizeIt = it->transform.sizePixels.Evaluate(t);
            if (local.x >= 0.0f && local.x <= sizeIt.x &&
                local.y >= 0.0f && local.y <= sizeIt.y) {
                layerManager.SetSelectedId(it->id);
                break;
            }
        }
    }

    (void)active; // silence unused-var warning on some compilers
}

// =============================================================================
// Composition Viewport (Task 5.0 rewrite)
//
// Every 2D layer is rasterized to compRTV via CompositionRenderer.
// If any layer has an enabled effect, the EffectManager ping-pong chain runs
// (source = compSRV, destination = compRTV, final back in compTexture).
// The result is displayed inside the panel via ImGui::Image() with 16:9
// letterbox scaling, so shapes always appear at correct proportions and
// the "canvas center" is a stable point independent of panel size.
//
// 3D layers, camera gizmos, and selection handles overlay on top of the
// image via ImDrawList, in SCREEN-space coordinates that are computed from
// canvas coordinates using the same letterbox transform (see CanvasToScreen
// lambdas inside).
// =============================================================================
void RenderEngine::DrawViewportCanvas() {
    // -------------------------------------------------------------------
    // Task 5.8: viewport toolbar. Preview Scale + FPS + Canvas readout.
    // Sits ABOVE the letterbox rect so its ~24 px steal from the top of
    // the panel; the rest of the panel is the composition + gizmos.
    // -------------------------------------------------------------------
    {
        // Preview dropdown
        const char* scaleLabels[3] = { "Full", "Half", "Quarter" };
        const float scaleValues[3] = { 1.0f, 0.5f, 0.25f };
        int scaleIdx = 0;
        if      (previewScale <= 0.30f) scaleIdx = 2;
        else if (previewScale <= 0.75f) scaleIdx = 1;
        else                            scaleIdx = 0;

        ImGui::TextUnformatted("Preview:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##previewScale", &scaleIdx, scaleLabels, 3)) {
            const float newScale = scaleValues[scaleIdx];
            if (std::fabs(newScale - previewScale) > 1e-4f) {
                previewScale = newScale;
                // Rebuild RT + effect ping-pong at the new size. Same path
                // as Comp Settings modal Apply and File > Open post-load.
                ReleaseCompositionRT();
                CreateCompositionRT(RtWidth(), RtHeight());
                if (effectManager.IsReady()) {
                    effectManager.Resize(RtWidth(), RtHeight());
                }
            }
        }

        // FPS readout (30-sample rolling average)
        float fpsAvg = 0.0f;
        if (fpsRingCount > 0) {
            float sum = 0.0f;
            for (int i = 0; i < fpsRingCount; ++i) sum += fpsRing[i];
            fpsAvg = sum / (float)fpsRingCount;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| FPS: %5.1f", fpsAvg);

        // Canvas dims + preview-scaled RT dims (for user confidence that the
        // downsample knob is actually doing something).
        ImGui::SameLine();
        ImGui::TextDisabled("| Canvas: %d x %d  (RT: %u x %u)",
                            compositionWidth, compositionHeight,
                            RtWidth(), RtHeight());
    }

    ImVec2 panelOrigin = ImGui::GetCursorScreenPos();
    ImVec2 panelSize   = ImGui::GetContentRegionAvail();
    if (panelSize.x < 50.0f) panelSize.x = 50.0f;
    if (panelSize.y < 50.0f) panelSize.y = 50.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    // Panel backdrop (letterbox bars will be visible around the composition).
    draw_list->AddRectFilled(panelOrigin,
        ImVec2(panelOrigin.x + panelSize.x, panelOrigin.y + panelSize.y),
        IM_COL32(15, 15, 20, 255));

    // -------------------------------------------------------------------
    // Letterbox math: fit the compTexture (compW x compH) inside the panel
    // preserving aspect ratio, centered.
    // -------------------------------------------------------------------
    // Task 5.8: letterbox math + all coord conversions use COMPOSITION dims,
    // not RT dims. Preview scale downsamples the RT but the canvas the user
    // draws on is always compositionWidth x compositionHeight. The RT is
    // just a lower-res copy — ImGui::AddImage sample-filters it up to the
    // letterbox rect for free.
    const float compW = (float)compositionWidth;
    const float compH = (float)compositionHeight;
    const float compAspect  = (compH > 0) ? (compW / compH) : (16.0f / 9.0f);
    const float panelAspect = panelSize.x / panelSize.y;

    float lbW, lbH;
    if (panelAspect > compAspect) {
        // Panel wider than comp -> bars on left/right
        lbH = panelSize.y;
        lbW = lbH * compAspect;
    } else {
        // Panel taller than comp -> bars on top/bottom
        lbW = panelSize.x;
        lbH = lbW / compAspect;
    }
    const ImVec2 lbOrigin(panelOrigin.x + (panelSize.x - lbW) * 0.5f,
                          panelOrigin.y + (panelSize.y - lbH) * 0.5f);
    const ImVec2 lbSize(lbW, lbH);

    // Publish for ScreenToCanvas() used by gizmo hit-testing and camera controls.
    lastCanvasLetterboxOrigin = lbOrigin;
    lastCanvasLetterboxSize   = lbSize;
    lastViewportSize          = panelSize;
    lastViewportCenterWorld   = { compW * 0.5f, compH * 0.5f };

    // Canvas-pixels -> screen-pixels helper (captures lbOrigin/lbSize).
    // Task 5.8: divides by comp dims so callers can pass comp-space points.
    auto CanvasToScreen = [&](Vec2 p) -> ImVec2 {
        return ImVec2(lbOrigin.x + (p.x / compW) * lbSize.x,
                      lbOrigin.y + (p.y / compH) * lbSize.y);
    };

    // -------------------------------------------------------------------
    // 1) Render all 2D layers into compRTV using CompositionRenderer.
    //    The renderer clears to the composition background color first.
    // Task 5.8: pass RT dims as target (viewport pixel count) AND comp
    // dims as logical (MVP division) so shape NDC positions stay
    // comp-correct at any preview scale.
    // -------------------------------------------------------------------
    if (compRTV && compRenderer.IsReady()) {
        // Task 5.9: re-rasterize any Text layer whose props changed since
        // last frame. Fast-path when nothing changed (hash compare only).
        RefreshTextLayerCaches();

        // -------------------------------------------------------------------
        // Task 5.13: per-layer effect isolation. Complete rewrite of the
        // previous composition-wide effect pooling.
        //
        // For each visible layer in draw order (matches CompositionRenderer's
        // internal loop):
        //   * If the layer has NO enabled effects: fast path — draw the
        //     layer directly into compRTV via RenderSingleLayer. This is
        //     Gemini's 'batching by default' from the RFC — typical scenes
        //     are 80%+ effect-free layers that pay zero effect cost.
        //   * If the layer HAS enabled effects: isolation path.
        //       1. Clear pingRTV to transparent (so alpha compositing works).
        //       2. Draw ONLY this layer into pingRTV.
        //       3. Run its effect chain: ApplyChain(pingSRV -> pongRTV).
        //          The chain's first pass detects pingSRV as source and
        //          starts writing to pongRTV automatically (see the
        //          same-texture-avoidance logic in ApplyChain).
        //       4. Composite the final pongSRV (or pingSRV — parity of
        //          effect count) over compRTV via CompositeSRVOver.
        //     Both ping and pong are shared across every isolated layer
        //     in the frame — VRAM stays at 2 * compRT size no matter how
        //     many layers have effects. Matches the exact-2-RT constraint.
        //
        // Comp duration for the visibility gate is still animEngine.duration.
        // Trim (in/out) check happens inside RenderSingleLayer's callers below.
        // -------------------------------------------------------------------
        // Start every frame by clearing compRTV to the user's bg color.
        // (Was previously done by RenderLayers as part of its first pass.)
        compRenderer.ClearComp(compRTV, bgColor);

        const float compT = layerManager.CurrentCompTime();
        const float dur   = animEngine.duration;
        anyLayerHasEffects = false;
        for (auto& layer : layerManager.Layers()) {
            // Visibility + trim gate — mirror CompositionRenderer's rules.
            if (!layer.isVisible) continue;
            if (layer.is3D)       continue;
            if (layer.type == ShapeType::Camera) continue;
            const float outT = (layer.outPoint < 0.0f)
                                    ? (dur > 0.0f ? dur : 1e9f)
                                    : layer.outPoint;
            if (compT < layer.inPoint || compT > outT) continue;

            const bool hasFx = layer.HasAnyEnabledEffect();
            if (hasFx) anyLayerHasEffects = true;

            if (!hasFx || !effectManager.IsReady()) {
                // Fast path: draw straight into compRTV (batching by default).
                compRenderer.RenderSingleLayer(layer, compRTV,
                                               compTextureWidth, compTextureHeight,
                                               layerManager,
                                               (UINT)compositionWidth,
                                               (UINT)compositionHeight);
            } else {
                // Isolation path.
                effectManager.Resize(compTextureWidth, compTextureHeight);
                // ============ DIAGNOSTIC v3 (Task 5.13-diag3) ============
                // Ultra-minimal: ClearComp pingRTV to opaque MAGENTA, then
                // composite pingSRV over compRTV. NO shape draw at all.
                //
                // Expected outcomes:
                //  (A) The whole canvas fills with magenta -> pingRTV
                //      write via ClearRenderTargetView + pingSRV read via
                //      composite work fine. Bug is in RenderLayers /
                //      RenderSingleLayer's write into pingRTV
                //      (state-corruption or dropped bind between the
                //      clear and the draw).
                //  (B) Canvas stays dark, no magenta -> the entire ping
                //      pool RT pipeline is broken. Something in the RTV
                //      binding / texture creation / SRV creation is
                //      dropping the write or the read. VERY suspicious of
                //      Resize() being called with same dims but doing
                //      something weird, or of the SRV pointing at a
                //      different texture than the RTV.
                const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
                compRenderer.ClearComp(effectManager.GetPingRTV(), magenta);
                // Skip shape draw + effect chain entirely.
                std::vector<Effect> perLayer; // unused, keep for later
                (void)perLayer;
                effectManager.CompositeSRVOver(effectManager.GetPingSRV(),
                                               compRTV);
                // ============ END DIAGNOSTIC v3 ============
            }
        }
    }

    // -------------------------------------------------------------------
    // 3) Display the composition texture inside the letterbox rect.
    //    ImGui accepts SRVs directly as ImTextureID on the DX11 backend.
    // -------------------------------------------------------------------
    if (compSRV) {
        draw_list->AddImage((ImTextureID)compSRV, lbOrigin,
                            ImVec2(lbOrigin.x + lbSize.x, lbOrigin.y + lbSize.y));
        // Thin outline around the composition so the letterbox is obvious.
        draw_list->AddRect(lbOrigin,
                           ImVec2(lbOrigin.x + lbSize.x, lbOrigin.y + lbSize.y),
                           IM_COL32(70, 70, 80, 255), 0.0f, 0, 1.0f);
    }

    // -------------------------------------------------------------------
    // 4) 3D layers, depth-sorted, drawn as ImDrawList overlay on top of
    //    the composition image. (3D still uses the perspective-project
    //    ImGui overlay path from Task 4; migrating 3D into compRTV is
    //    outside Task 5.0's scope.)
    // -------------------------------------------------------------------
    auto& L = layerManager.Layers();
    struct ThreeDEntry { int id; float depth; };
    static thread_local std::vector<ThreeDEntry> depthList;
    depthList.clear();
    Mat4 viewMat = camera.GetViewMatrix();
    const float tDepth = animEngine.currentTime;
    for (auto& layer : L) {
        if (!layer.isVisible)                    continue;
        if (!layer.is3D)                         continue;
        if (layer.type == ShapeType::Camera)     continue;
        Mat4 wm4 = layerManager.GetWorldMatrix4(layer.id);
        // Task 5.1: sample anchor + size at current comp time.
        const Vec2 anchorD = layer.transform.anchorPoint.Evaluate(tDepth);
        const Vec2 sizeD   = layer.transform.sizePixels .Evaluate(tDepth);
        const float ax = anchorD.x * sizeD.x;
        const float ay = anchorD.y * sizeD.y;
        Vec4 centerWorld = wm4.TransformVec4(Vec4(ax, ay, 0.0f, 1.0f));
        Vec4 centerView  = viewMat.TransformVec4(centerWorld);
        depthList.push_back({ layer.id, centerView.z });
    }
    std::sort(depthList.begin(), depthList.end(),
              [](const ThreeDEntry& a, const ThreeDEntry& b) { return a.depth > b.depth; });
    for (const auto& e : depthList) {
        Layer* layer = layerManager.GetLayerById(e.id);
        if (!layer) continue;
        const Mat4 wm4 = layerManager.GetWorldMatrix4(layer->id);
        const float op = layerManager.GetWorldOpacity(layer->id);
        DrawLayerShape3D(*layer, wm4, op, lbOrigin, lbSize, draw_list);
    }

    // -------------------------------------------------------------------
    // 5) Selection gizmos + interaction.
    // -------------------------------------------------------------------
    Layer* sel = layerManager.GetSelectedLayer();
    if (sel && sel->isVisible) {
        if (sel->type == ShapeType::Camera) {
            DrawCameraGizmos(*sel, lbOrigin, lbSize, draw_list);
        } else if (!sel->is3D) {
            // Task 5.1: sample size + anchor at current comp time.
            const float tSelBox = animEngine.currentTime;
            const Vec2 sizeSel   = sel->transform.sizePixels .Evaluate(tSelBox);
            const Vec2 anchorSel = sel->transform.anchorPoint.Evaluate(tSelBox);
            const Mat3 wm = layerManager.GetWorldMatrix(sel->id);
            const float w = sizeSel.x;
            const float h = sizeSel.y;
            const Vec2 c0 = wm.TransformPoint(Vec2(0.0f, 0.0f));
            const Vec2 c1 = wm.TransformPoint(Vec2(w,    0.0f));
            const Vec2 c2 = wm.TransformPoint(Vec2(w,    h));
            const Vec2 c3 = wm.TransformPoint(Vec2(0.0f, h));
            const ImVec2 p0 = CanvasToScreen(c0);
            const ImVec2 p1 = CanvasToScreen(c1);
            const ImVec2 p2 = CanvasToScreen(c2);
            const ImVec2 p3 = CanvasToScreen(c3);

            const ImU32 outline = IM_COL32(0, 255, 200, 255);
            const ImU32 handle  = IM_COL32(255, 220, 60, 255);
            const ImVec2 box[4] = { p0, p1, p2, p3 };
            draw_list->AddPolyline(box, 4, outline, ImDrawFlags_Closed, 1.5f);

            const float R = 5.0f;
            draw_list->AddRectFilled(ImVec2(p0.x-R, p0.y-R), ImVec2(p0.x+R, p0.y+R), handle);
            draw_list->AddRectFilled(ImVec2(p1.x-R, p1.y-R), ImVec2(p1.x+R, p1.y+R), handle);
            draw_list->AddRectFilled(ImVec2(p2.x-R, p2.y-R), ImVec2(p2.x+R, p2.y+R), handle);
            draw_list->AddRectFilled(ImVec2(p3.x-R, p3.y-R), ImVec2(p3.x+R, p3.y+R), handle);
            const Vec2 centerC = wm.TransformPoint(Vec2(anchorSel.x * w, anchorSel.y * h));
            draw_list->AddCircleFilled(CanvasToScreen(centerC), 6.0f, IM_COL32(255, 80, 80, 255));

            HandleGizmoInteraction(*sel, wm, lbOrigin, lbSize);
        } else {
            // 3D selection bounding quad (unchanged behavior from Task 4)
            const float tSel3D = animEngine.currentTime;
            const Vec2 size3D = sel->transform.sizePixels.Evaluate(tSel3D);
            const Mat4 wm4 = layerManager.GetWorldMatrix4(sel->id);
            const float w = size3D.x;
            const float h = size3D.y;
            const Vec4 corners_local[4] = {
                Vec4(0.0f, 0.0f, 0.0f, 1.0f),
                Vec4(w,    0.0f, 0.0f, 1.0f),
                Vec4(w,    h,    0.0f, 1.0f),
                Vec4(0.0f, h,    0.0f, 1.0f),
            };
            Vec4 projected[4];
            bool allInFront = true;
            for (int i = 0; i < 4; ++i) {
                Vec4 world = wm4.TransformVec4(corners_local[i]);
                projected[i] = camera.ProjectPoint(
                    Vec3(world.x, world.y, world.z), lbSize.x, lbSize.y);
                if (projected[i].w <= 0.0f) { allInFront = false; break; }
            }
            if (allInFront) {
                ImVec2 q[4] = {
                    ImVec2(lbOrigin.x + projected[0].x, lbOrigin.y + projected[0].y),
                    ImVec2(lbOrigin.x + projected[1].x, lbOrigin.y + projected[1].y),
                    ImVec2(lbOrigin.x + projected[2].x, lbOrigin.y + projected[2].y),
                    ImVec2(lbOrigin.x + projected[3].x, lbOrigin.y + projected[3].y),
                };
                draw_list->AddPolyline(q, 4, IM_COL32(0, 255, 200, 255),
                                       ImDrawFlags_Closed, 1.5f);
            }
        }
    }

    // -------------------------------------------------------------------
    // 6) Empty-canvas click-to-select. Hit-test uses ScreenToCanvas so the
    //    mouse works correctly regardless of letterbox scale/offset.
    // -------------------------------------------------------------------
    if (!sel || sel->type == ShapeType::Camera || sel->is3D) {
        ImGui::SetCursorScreenPos(panelOrigin);
        ImGui::InvisibleButton("ViewportHitTestEmpty", panelSize);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float tSel = animEngine.currentTime;
            const Vec2 mw = ScreenToCanvas(ImGui::GetIO().MousePos);
            auto& Ls = layerManager.Layers();
            for (auto it = Ls.rbegin(); it != Ls.rend(); ++it) {
                if (!it->isVisible)                   continue;
                if (it->is3D || it->type == ShapeType::Camera) continue;
                const Mat3 wm2 = layerManager.GetWorldMatrix(it->id);
                const Mat3 inv = wm2.InverseAffine();
                const Vec2 local = inv.TransformPoint(mw);
                const Vec2 sizeIt = it->transform.sizePixels.Evaluate(tSel);
                if (local.x >= 0.0f && local.x <= sizeIt.x &&
                    local.y >= 0.0f && local.y <= sizeIt.y) {
                    layerManager.SetSelectedId(it->id);
                    break;
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // 7) Camera navigation (orbit/pan/zoom). Uses the letterbox rect so
    //    RMB/MMB drags only fire when the mouse is inside the composition.
    //    Task 5.0-b: 2D-first UX; camera controls only run when 3D is on.
    // -------------------------------------------------------------------
    if (show3DFeatures) {
        HandleCameraControls(lbOrigin, lbSize);
    }

    // -------------------------------------------------------------------
    // 8) HUD overlay (top-left of the panel, above the letterbox).
    //    Task 5.0-b: minimal HUD in 2D-first mode; full 3D HUD only when
    //    3D features are enabled.
    // -------------------------------------------------------------------
    char hud[192];
    // Task 5.8: HUD reports COMPOSITION dims (what the user drew on), not
    // RT dims (which is a preview-scale artifact).
    if (show3DFeatures) {
        const int camLayer = layerManager.FindActiveCameraLayerId();
        std::snprintf(hud, sizeof(hud),
            "Canvas %d x %d   FOV=%.1f   Cam=(%.0f, %.0f, %.0f)%s%s",
            compositionWidth, compositionHeight,
            camera.fov, camera.position.x, camera.position.y, camera.position.z,
            (camLayer >= 0) ? "  (from Camera layer)" : "",
            anyLayerHasEffects ? "  [FX ON]" : "");
        draw_list->AddText(ImVec2(panelOrigin.x + 8, panelOrigin.y + 4),
            IM_COL32(200, 220, 255, 255), hud);
        draw_list->AddText(ImVec2(panelOrigin.x + 8, panelOrigin.y + 20),
            IM_COL32(160, 160, 170, 255),
            "RMB: Orbit   MMB: Pan   Wheel: Zoom");
    } else {
        std::snprintf(hud, sizeof(hud),
            "Canvas %d x %d%s",
            compositionWidth, compositionHeight,
            anyLayerHasEffects ? "   [FX ON]" : "");
        draw_list->AddText(ImVec2(panelOrigin.x + 8, panelOrigin.y + 4),
            IM_COL32(200, 220, 255, 255), hud);
    }

    // Task 5.2: status banner (Save/Load feedback). Bottom-center of the
    // viewport panel; auto-expires. Red on error, green on success.
    if (!statusMessage.empty()) {
        const Uint64 freq = SDL_GetPerformanceFrequency();
        const Uint64 now  = SDL_GetPerformanceCounter();
        const float  nowSec = (freq > 0) ? (float)((double)now / (double)freq) : 0.0f;
        if (nowSec < statusMessageExpiresAt) {
            const ImU32 col = statusIsError
                ? IM_COL32(255, 90, 90, 240)
                : IM_COL32(90, 255, 130, 240);
            const ImVec2 sz = ImGui::CalcTextSize(statusMessage.c_str());
            const float pad = 8.0f;
            const ImVec2 bgTL(panelOrigin.x + (panelSize.x - sz.x) * 0.5f - pad,
                              panelOrigin.y + panelSize.y - sz.y - 3.0f * pad);
            const ImVec2 bgBR(bgTL.x + sz.x + 2.0f * pad,
                              bgTL.y + sz.y + 2.0f * pad);
            draw_list->AddRectFilled(bgTL, bgBR, IM_COL32(20, 20, 30, 220), 4.0f);
            draw_list->AddText(ImVec2(bgTL.x + pad, bgTL.y + pad),
                               col, statusMessage.c_str());
        } else {
            statusMessage.clear();
        }
    }
}

// =============================================================================
// 3D layer rendering + camera gizmos + camera navigation (Task 4)
// =============================================================================

void RenderEngine::DrawLayerShape3D(const Layer& layer, const Mat4& worldMatrix4,
                                    float worldOpacity, ImVec2 canvasOrigin,
                                    ImVec2 canvasSize, ImDrawList* drawList) {
    if (!drawList) return;

    // Task 5.1: sample size at the current comp time.
    const Vec2 sz3D = layer.transform.sizePixels.Evaluate(animEngine.currentTime);
    const float w = sz3D.x;
    const float h = sz3D.y;

    // Transform the four corners through World -> Clip -> NDC -> Pixels.
    Vec4 wc0 = worldMatrix4.TransformVec4(Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    Vec4 wc1 = worldMatrix4.TransformVec4(Vec4(w,    0.0f, 0.0f, 1.0f));
    Vec4 wc2 = worldMatrix4.TransformVec4(Vec4(w,    h,    0.0f, 1.0f));
    Vec4 wc3 = worldMatrix4.TransformVec4(Vec4(0.0f, h,    0.0f, 1.0f));

    Vec4 s0 = camera.ProjectPoint(Vec3(wc0.x, wc0.y, wc0.z), canvasSize.x, canvasSize.y);
    Vec4 s1 = camera.ProjectPoint(Vec3(wc1.x, wc1.y, wc1.z), canvasSize.x, canvasSize.y);
    Vec4 s2 = camera.ProjectPoint(Vec3(wc2.x, wc2.y, wc2.z), canvasSize.x, canvasSize.y);
    Vec4 s3 = camera.ProjectPoint(Vec3(wc3.x, wc3.y, wc3.z), canvasSize.x, canvasSize.y);

    // If ANY vertex is behind the camera, skip drawing this frame — proper
    // clip-space polygon clipping is Task 5+ territory.
    if (s0.w <= 0.0f || s1.w <= 0.0f || s2.w <= 0.0f || s3.w <= 0.0f) return;

    const ImVec2 p0(canvasOrigin.x + s0.x, canvasOrigin.y + s0.y);
    const ImVec2 p1(canvasOrigin.x + s1.x, canvasOrigin.y + s1.y);
    const ImVec2 p2(canvasOrigin.x + s2.x, canvasOrigin.y + s2.y);
    const ImVec2 p3(canvasOrigin.x + s3.x, canvasOrigin.y + s3.y);

    unsigned int c = layer.fillColor;
    unsigned int alpha = (c >> 24) & 0xFFu;
    alpha = (unsigned int)std::clamp((int)(alpha * worldOpacity), 0, 255);
    unsigned int fill = (c & 0x00FFFFFFu) | (alpha << 24);

    switch (layer.type) {
        case ShapeType::Rectangle: {
            const ImVec2 quad[4] = { p0, p1, p2, p3 };
            drawList->AddConvexPolyFilled(quad, 4, fill);
            drawList->AddPolyline(quad, 4, IM_COL32(255, 255, 255, alpha),
                                  ImDrawFlags_Closed, 1.0f);
            break;
        }
        case ShapeType::Ellipse: {
            // 3D ellipse: project 48 samples of the parametric ellipse and
            // draw as a polyline. Foreshortening comes out naturally.
            constexpr int kSegs = 48;
            ImVec2 pts[kSegs];
            bool anyBehind = false;
            const float rx = w * 0.5f;
            const float ry = h * 0.5f;
            for (int i = 0; i < kSegs; ++i) {
                const float t = (float)i / (float)kSegs * 2.0f * POTATO_PI;
                Vec4 world = worldMatrix4.TransformVec4(
                    Vec4(rx + rx * std::cos(t),
                         ry + ry * std::sin(t),
                         0.0f, 1.0f));
                Vec4 sp = camera.ProjectPoint(Vec3(world.x, world.y, world.z),
                                              canvasSize.x, canvasSize.y);
                if (sp.w <= 0.0f) { anyBehind = true; break; }
                pts[i] = ImVec2(canvasOrigin.x + sp.x, canvasOrigin.y + sp.y);
            }
            if (!anyBehind) {
                drawList->AddConvexPolyFilled(pts, kSegs, fill);
                drawList->AddPolyline(pts, kSegs, IM_COL32(255, 255, 255, alpha),
                                      ImDrawFlags_Closed, 1.0f);
            }
            break;
        }
        default: {
            // CustomPath / Camera fall through: draw the projected quad outline
            // so the layer still shows up as *something* in 3D.
            const ImVec2 quad[4] = { p0, p1, p2, p3 };
            drawList->AddPolyline(quad, 4, IM_COL32(180, 180, 180, alpha),
                                  ImDrawFlags_Closed, 1.0f);
            break;
        }
    }
}

void RenderEngine::DrawCameraGizmos(const Layer& cameraLayer, ImVec2 canvasOrigin,
                                    ImVec2 canvasSize, ImDrawList* drawList) {
    if (!drawList) return;
    (void)cameraLayer; // Camera layer's transform is already synced into `camera`

    // Frustum corners at near and far planes (in view space), then transform
    // back to world via the inverse view. For visualisation we just project
    // them right back through the same camera — the result is a stable
    // frustum outline the user can see in the 2D viewport composite.
    // (Since we don't render from the camera's POV, the frustum wireframe
    // here is only really meaningful when the user is orbiting AROUND the
    // camera in an external editor camera, which is a future enhancement.
    // For now we draw a look-at line and a small camera icon.)

    // Camera icon: a triangle at eye pointing toward target.
    Vec4 eyePx    = camera.ProjectPoint(camera.position, canvasSize.x, canvasSize.y);
    Vec4 targetPx = camera.ProjectPoint(camera.target,   canvasSize.x, canvasSize.y);
    if (eyePx.w > 0.0f) {
        ImVec2 eye(canvasOrigin.x + eyePx.x, canvasOrigin.y + eyePx.y);
        drawList->AddCircleFilled(eye, 6.0f, IM_COL32(255, 200, 60, 255));
        drawList->AddText(ImVec2(eye.x + 8, eye.y - 8),
                          IM_COL32(255, 220, 120, 255), "Camera");
    }
    if (eyePx.w > 0.0f && targetPx.w > 0.0f) {
        drawList->AddLine(ImVec2(canvasOrigin.x + eyePx.x,    canvasOrigin.y + eyePx.y),
                          ImVec2(canvasOrigin.x + targetPx.x, canvasOrigin.y + targetPx.y),
                          IM_COL32(255, 200, 60, 180), 1.0f);
        ImVec2 tgt(canvasOrigin.x + targetPx.x, canvasOrigin.y + targetPx.y);
        drawList->AddCircle(tgt, 5.0f, IM_COL32(255, 120, 60, 255), 12, 1.5f);
    }
}

void RenderEngine::HandleCameraControls(ImVec2 canvasOrigin, ImVec2 canvasSize) {
    const ImGuiIO& io = ImGui::GetIO();

    // Only accept camera drags when the mouse is inside the viewport rect AND
    // no other ImGui item is claiming it. We use a "hover rect" check rather
    // than InvisibleButton because we don't want to steal clicks from the
    // layer selection code above.
    const ImVec2 mp = io.MousePos;
    const bool insideViewport =
        (mp.x >= canvasOrigin.x && mp.x <= canvasOrigin.x + canvasSize.x &&
         mp.y >= canvasOrigin.y && mp.y <= canvasOrigin.y + canvasSize.y);

    // Wheel zoom: dolly the camera along its forward vector.
    if (insideViewport && std::fabs(io.MouseWheel) > 0.0f) {
        Vec3 forward = Vec3Normalize(camera.target - camera.position);
        const float step = io.MouseWheel * 40.0f; // pixels per notch
        camera.position = camera.position + forward * step;
    }

    // Start drag on RMB or MMB inside viewport.
    if (!cameraDragActive && insideViewport) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            cameraDragActive   = true;
            cameraDragButton   = ImGuiMouseButton_Right;
            cameraDragIsPan    = io.KeyShift; // Shift+RMB = pan alt
            cameraDragLastMouse = mp;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            cameraDragActive   = true;
            cameraDragButton   = ImGuiMouseButton_Middle;
            cameraDragIsPan    = true;
            cameraDragLastMouse = mp;
        }
    }

    // Continue drag
    if (cameraDragActive && ImGui::IsMouseDown((ImGuiMouseButton)cameraDragButton)) {
        const ImVec2 delta(mp.x - cameraDragLastMouse.x,
                           mp.y - cameraDragLastMouse.y);
        cameraDragLastMouse = mp;

        if (cameraDragIsPan) {
            // Pan: move eye AND target together along camera-space X/Y.
            Vec3 forward = Vec3Normalize(camera.target - camera.position);
            Vec3 right   = Vec3Normalize(Vec3Cross(camera.up, forward));
            Vec3 upAxis  = Vec3Cross(forward, right);
            const float panSpeed = 1.0f;
            Vec3 shift = right  * (-delta.x * panSpeed)
                       + upAxis * ( delta.y * panSpeed);
            camera.position = camera.position + shift;
            camera.target   = camera.target   + shift;
        } else {
            // Orbit: rotate the eye around the target on the world Y axis
            // (horizontal drag) and around the local X axis (vertical drag).
            Vec3 toEye = camera.position - camera.target;
            const float yawDeg   = -delta.x * 0.3f;
            const float pitchDeg = -delta.y * 0.3f;

            // Yaw around world up.
            {
                const float rad = yawDeg * (POTATO_PI / 180.0f);
                const float c = std::cos(rad), s = std::sin(rad);
                const float x = toEye.x * c + toEye.z * s;
                const float z = -toEye.x * s + toEye.z * c;
                toEye.x = x; toEye.z = z;
            }
            // Pitch around the camera's right vector (approximated by cross of world up with view).
            {
                Vec3 forward = Vec3Normalize(Vec3(0,0,0) - toEye);
                Vec3 right   = Vec3Normalize(Vec3Cross(camera.up, forward));
                (void)right;
                const float rad = pitchDeg * (POTATO_PI / 180.0f);
                const float c = std::cos(rad), s = std::sin(rad);
                // Rotate toEye around the right vector — simplified as a plane
                // rotation on the YZ plane of the current basis.
                const float len = Vec3Length(toEye);
                Vec3 dir = (len > 1e-4f) ? toEye * (1.0f / len) : Vec3(0, 0, -1);
                // Convert to spherical-ish: rotate the Y component vs horizontal length.
                const float horiz = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                const float newY = dir.y * c - horiz * s;
                const float newH = dir.y * s + horiz * c;
                if (horiz > 1e-4f) {
                    const float hx = dir.x / horiz;
                    const float hz = dir.z / horiz;
                    dir.x = hx * newH;
                    dir.z = hz * newH;
                    dir.y = newY;
                } else {
                    dir.y = newY;
                }
                toEye = dir * len;
            }
            camera.position = camera.target + toEye;
            camera.useTargetMode = true; // orbit implies LookAt behavior
        }

        // If a Camera layer is driving the view, write our edits back so they
        // are captured when the user goes to keyframe (Task 5).
        const int camLayerId = layerManager.FindActiveCameraLayerId();
        if (camLayerId >= 0) {
            if (Layer* cl = layerManager.GetLayerById(camLayerId)) {
                // Task 5.1: route through SetValue so keyframing works if the
                // Camera layer's position stopwatch is on.
                cl->transform.position.SetValue(animEngine.currentTime, camera.position);
            }
        }
    }

    // End drag
    if (cameraDragActive && !ImGui::IsMouseDown((ImGuiMouseButton)cameraDragButton)) {
        cameraDragActive = false;
        cameraDragButton = -1;
    }
}

void RenderEngine::SyncCameraFromLayerIfAny() {
    const int camLayerId = layerManager.FindActiveCameraLayerId();
    if (camLayerId < 0) return;
    const Layer* cl = layerManager.GetLayerById(camLayerId);
    if (!cl) return;

    // Task 5.1: read the Camera layer's animated properties at current time.
    // If the layer has keyframes on position/rotation, the camera follows the
    // animation for free — no special sync code required.
    const float t = animEngine.currentTime;
    camera.position = cl->transform.position.Evaluate(t);
    if (!camera.useTargetMode) {
        camera.rotation = cl->transform.rotation.Evaluate(t);
    }
}

// =============================================================================
// Effects Palette (Task 5): browse available effects, click to add to selection
// =============================================================================
void RenderEngine::DrawEffectsPalettePanel() {
    if (!effectManager.IsReady()) {
        ImGui::TextDisabled("Shader pipeline not initialised.");
        ImGui::TextWrapped("Check the console for the D3DCompile error and confirm d3dcompiler.dll is loadable.");
        return;
    }
    ImGui::Text("Effects (drag or click to add to selection)");
    ImGui::Separator();

    Layer* sel = layerManager.GetSelectedLayer();
    const bool haveSel = (sel != nullptr);
    if (!haveSel) {
        ImGui::TextDisabled("Select a layer in the Timeline to add effects.");
        return;
    }

    auto addRow = [&](const char* label, const char* desc, Effect (*factory)()){
        ImGui::PushID(label);
        if (ImGui::Button(label, ImVec2(-1, 0))) {
            MarkForSnapshot();   // Task 5.3: undo covers "add effect"
            sel->AddEffect(factory());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", desc);
        }
        ImGui::PopID();
    };
    addRow("Motion Tile",             "Repeats and (optionally) mirrors the layer across UV space.", Effect::MakeMotionTile);
    addRow("Directional Motion Blur", "Blurs the layer along an angle. Cap: 16 samples for potato GPUs.", Effect::MakeMotionBlur);
    addRow("Chromatic Aberration",    "Offsets R/G/B channels radially or along an angle.", Effect::MakeChromaticAberration);
    // Task 5.13: DropShadow — first per-layer isolation-mode effect.
    addRow("Drop Shadow",             "Colored offset+blurred copy behind the layer. Per-layer only.", Effect::MakeDropShadow);

    ImGui::Separator();
    ImGui::TextDisabled("Blend Modes");
    struct BM { const char* name; BlendMode m; };
    const BM bms[] = {
        { "Normal",      BlendMode::Normal     },
        { "Additive",    BlendMode::Additive   },
        { "Multiply",    BlendMode::Multiply   },
        { "Screen",      BlendMode::Screen     },
        { "Overlay",     BlendMode::Overlay    },
        { "Color Dodge", BlendMode::ColorDodge },
    };
    for (const auto& bm : bms) {
        ImGui::PushID(bm.name);
        if (ImGui::Button(bm.name, ImVec2(-1, 0))) {
            MarkForSnapshot();   // Task 5.3
            sel->AddEffect(Effect::MakeBlendMode(bm.m));
        }
        ImGui::PopID();
    }
}

// =============================================================================
// Effect Controls (Task 5): active stack on the selected layer + parameters
// =============================================================================
void RenderEngine::DrawEffectControlsPanel() {
    Layer* sel = layerManager.GetSelectedLayer();
    if (!sel) {
        ImGui::TextDisabled("No layer selected.");
        return;
    }
    ImGui::Text("Effect stack for: %s", sel->name.c_str());
    if (sel->effects.empty()) {
        ImGui::TextDisabled("No effects. Add one from the Effects Palette.");
        return;
    }
    ImGui::TextDisabled("Effects run top-to-bottom. Later effects see earlier output.");
    ImGui::Separator();

    int  moveEffectId = -1;
    int  moveDelta    = 0;
    int  deleteId     = -1;

    for (size_t i = 0; i < sel->effects.size(); ++i) {
        Effect& e = sel->effects[i];
        ImGui::PushID(e.id);

        // Row header: [x] enabled  Name  [^] [v] [X]
        ImGui::Checkbox("##en", &e.enabled);
        ImGui::SameLine();
        ImGui::Text("%zu. %s", i + 1, e.displayName.c_str());

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 88.0f);
        if (ImGui::SmallButton("^")) { moveEffectId = e.id; moveDelta = -1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { moveEffectId = e.id; moveDelta = +1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) { deleteId = e.id; }

        // Type-specific parameter widgets
        ImGui::Indent();
        switch (e.type) {
        case EffectType::MotionTile:
            ImGui::SliderFloat("Tile Count",    &e.params.p0[0], 1.0f, 32.0f);
            ImGui::SliderFloat("Phase",         &e.params.p0[1], -1.0f, 1.0f);
            {
                bool mirror = e.params.p0[2] > 0.5f;
                if (ImGui::Checkbox("Mirror Edges", &mirror)) e.params.p0[2] = mirror ? 1.0f : 0.0f;
            }
            break;
        case EffectType::DirectionalMotionBlur:
            ImGui::SliderFloat("Angle (deg)",   &e.params.p0[0], 0.0f, 360.0f);
            ImGui::SliderFloat("Intensity",     &e.params.p0[1], 0.0f, 100.0f);
            ImGui::SliderFloat("Samples (max 16)", &e.params.p0[2], 1.0f, 16.0f);
            break;
        case EffectType::ChromaticAberration:
            ImGui::SliderFloat("Amount (px)",   &e.params.p0[0], 0.0f, 32.0f);
            ImGui::SliderFloat("Angle (deg)",   &e.params.p0[1], 0.0f, 360.0f);
            {
                bool radial = e.params.p0[2] > 0.5f;
                if (ImGui::Checkbox("Radial", &radial)) e.params.p0[2] = radial ? 1.0f : 0.0f;
            }
            break;
        case EffectType::BlendMode: {
            static const char* kNames[] = { "Normal","Additive","Multiply","Screen","Overlay","Color Dodge" };
            int mode = (int)e.params.p0[0];
            if (mode < 0) mode = 0;
            if (mode > 5) mode = 5;
            if (ImGui::Combo("Mode", &mode, kNames, 6)) e.params.p0[0] = (float)mode;
            break;
        }
        case EffectType::DropShadow:
            // Task 5.13: Distance / Angle / Softness / Opacity / Color.
            ImGui::SliderFloat("Distance (px)",   &e.params.p0[0], 0.0f, 100.0f);
            ImGui::SliderFloat("Angle (deg)",     &e.params.p0[1], 0.0f, 360.0f);
            ImGui::SliderFloat("Softness (px)",   &e.params.p0[2], 0.0f, 32.0f);
            ImGui::SliderFloat("Opacity",         &e.params.p1[0], 0.0f, 1.0f);
            ImGui::ColorEdit3("Shadow Color",     e.params.p2);
            break;
        case EffectType::COUNT: break;
        }
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::PopID();
    }

    // Apply deferred mutations (can't mutate while iterating).
    if (moveEffectId >= 0) { MarkForSnapshot(); sel->MoveEffect(moveEffectId, moveDelta); }
    if (deleteId     >= 0) { MarkForSnapshot(); sel->RemoveEffectById(deleteId); }

    ImGui::TextDisabled("Task 5.13: effects now apply PER LAYER — adding an");
    ImGui::TextDisabled("effect here only affects THIS layer, not the whole comp.");
    ImGui::TextDisabled("Layers without effects skip the ping-pong dance entirely.");
}

// =============================================================================
// Graph Editor (unchanged from Task 2 but with the same defensive guards)
// =============================================================================
// =============================================================================
// Task 5.4-fix: AE-accurate Graph Editor.
//
// Design in DESIGN_COMMIT6_AE_ACCURATE_GRAPH.md. Short version:
//   - Storage is AE-native (speed, influence) per side (see AnimatedProperty.h).
//   - Toolbar: [Value][Speed] mode toggle, property GROUP picker
//     (Position/Rotation/Scale/Opacity — not per-channel).
//   - Value mode plots ALL scalar dims of the group simultaneously:
//       X red, Y green, Z blue. Focused-dim keys render bright, others dim.
//   - Speed mode plots ONE magnitude curve: |dv|/dt combined across dims,
//     matching AE's Position speed = sqrt(vx^2 + vy^2 + vz^2).
//   - Left-click a key selects it (and picks the closest dim as focus).
//     Drag its handle: free 2D by default, Shift=influence-only,
//     Alt=speed-only. Handle position is DERIVED from (speed, influence),
//     not stored — dragging inverts back into the AE-native fields.
//   - ContinuousBezier keys mirror in/out on drag (same speed magnitude,
//     same influence).
//   - AutoBezier keys have their tangents recomputed each frame from
//     neighbor slopes; handles are drawn dimmed and don't respond to drag.
//   - Right-click a key: Linear / Bezier / Continuous Bezier / Auto Bezier /
//     Hold / Delete Keyframe.
//   - Playhead is a red vertical line.
// =============================================================================

// Helpers scoped to this TU only.
namespace {

// One accessor per (property-type, scalar-dim) so the drawing code can pull
// per-dim scalars out of Vec2/Vec3 properties without templating the whole
// draw function. C-style function pointers can't capture `dim`, so we bake
// dim into a template instantiation.

template <typename T> inline float PickScalar(const T& v, int dim);
template <> inline float PickScalar<float>(const float& v, int)     { return v; }
template <> inline float PickScalar<Vec2>(const Vec2& v, int dim)   { return dim == 0 ? v.x : v.y; }
template <> inline float PickScalar<Vec3>(const Vec3& v, int dim)   {
    if (dim == 0) return v.x;
    if (dim == 1) return v.y;
    return v.z;
}
template <typename T> inline void PutScalar(T& v, int dim, float s);
template <> inline void PutScalar<float>(float& v, int, float s)    { v = s; }
template <> inline void PutScalar<Vec2>(Vec2& v, int dim, float s)  { if (dim == 0) v.x = s; else v.y = s; }
template <> inline void PutScalar<Vec3>(Vec3& v, int dim, float s)  {
    if (dim == 0) v.x = s; else if (dim == 1) v.y = s; else v.z = s;
}

// Number of scalar dims per typed property. Used to iterate curves.
template <typename T> inline int DimCount();
template <> inline int DimCount<float>() { return 1; }
template <> inline int DimCount<Vec2>()  { return 2; }
template <> inline int DimCount<Vec3>()  { return 3; }

// Speed graph magnitude helper: for Vec3 we combine dx,dy,dz;
// for Vec2 dx,dy; for float just |dv|. AE ignores Z for Scale.
template <typename T>
inline float MagnitudeDeltaOverDt(const T& a, const T& b, float dt, bool ignoreZ);
template <>
inline float MagnitudeDeltaOverDt<float>(const float& a, const float& b, float dt, bool) {
    if (dt <= 1e-6f) return 0.0f;
    return std::fabs(b - a) / dt;
}
template <>
inline float MagnitudeDeltaOverDt<Vec2>(const Vec2& a, const Vec2& b, float dt, bool) {
    if (dt <= 1e-6f) return 0.0f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx*dx + dy*dy) / dt;
}
template <>
inline float MagnitudeDeltaOverDt<Vec3>(const Vec3& a, const Vec3& b, float dt, bool ignoreZ) {
    if (dt <= 1e-6f) return 0.0f;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = ignoreZ ? 0.0f : (b.z - a.z);
    return std::sqrt(dx*dx + dy*dy + dz*dz) / dt;
}

// Evaluate one segment's scalar dim at time t. Rebuilds the tiny Bezier
// inline so we can plot arbitrarily many samples per segment without going
// through the full templated Evaluate() per sample.
template <typename T>
inline float EvalSegScalar(const AnimatedProperty<T>& p, int i0, int i1, int dim, float t) {
    const auto& A = p.keyframes[i0];
    const auto& B = p.keyframes[i1];
    if (A.outgoingMode == InterpMode::Hold) return PickScalar<T>(A.value, dim);
    if (A.outgoingMode == InterpMode::Linear && B.incomingMode == InterpMode::Linear) {
        const float span = B.time - A.time;
        const float u = (span > 1e-6f) ? (t - A.time) / span : 0.0f;
        const float a = PickScalar<T>(A.value, dim);
        const float b = PickScalar<T>(B.value, dim);
        return a + (b - a) * u;
    }
    const BezierSegment<T> s = BuildBezierSegment(A, B);
    const float u = SolveBezierU(s, t);
    return PickScalar<T>(EvalBezierValueAtU(s, u), dim);
}

// Evaluate one segment's full T value at time t (used for Speed graph
// magnitude finite differences).
template <typename T>
inline T EvalSegValue(const AnimatedProperty<T>& p, int i0, int i1, float t) {
    const auto& A = p.keyframes[i0];
    const auto& B = p.keyframes[i1];
    if (A.outgoingMode == InterpMode::Hold) return A.value;
    if (A.outgoingMode == InterpMode::Linear && B.incomingMode == InterpMode::Linear) {
        const float span = B.time - A.time;
        const float u = (span > 1e-6f) ? (t - A.time) / span : 0.0f;
        return Lerp(A.value, B.value, u);
    }
    return EvaluateBezierSegment(A, B, t);
}

// Sample the full T value at any time (handles edge cases). Returns the
// value + a bool flag indicating "clamped to edge" so speed-mode can zero.
template <typename T>
inline T SampleValueAt(const AnimatedProperty<T>& p, float t) {
    const int n = (int)p.keyframes.size();
    if (n == 0) return p.staticValue;
    if (n == 1) return p.keyframes[0].value;
    if (t <= p.keyframes[0].time)     return p.keyframes[0].value;
    if (t >= p.keyframes[n-1].time)   return p.keyframes[n-1].value;
    for (int i = 0; i + 1 < n; ++i) {
        if (t >= p.keyframes[i].time && t <= p.keyframes[i+1].time)
            return EvalSegValue(p, i, i+1, t);
    }
    return p.keyframes[n-1].value;
}

// AutoBezier tangent auto-computation. For each AutoBezier key we set
// in/out speed = weighted average of neighbor slopes (AE's "temporal
// auto-tangent" rule), influence stays at the AE default 16.667%.
template <typename T>
inline void RecomputeAutoBezierTangents(AnimatedProperty<T>& p) {
    const int n = (int)p.keyframes.size();
    for (int i = 0; i < n; ++i) {
        auto& k = p.keyframes[i];
        const bool inAuto  = (k.incomingMode == InterpMode::AutoBezier);
        const bool outAuto = (k.outgoingMode == InterpMode::AutoBezier);
        if (!inAuto && !outAuto) continue;
        T slopeAvg{};
        int contribs = 0;
        if (i > 0) {
            const auto& prev = p.keyframes[i - 1];
            const float dt = k.time - prev.time;
            if (dt > 1e-6f) {
                slopeAvg = AddT(slopeAvg, ScaleT(SubT(k.value, prev.value), 1.0f / dt));
                contribs++;
            }
        }
        if (i + 1 < n) {
            const auto& next = p.keyframes[i + 1];
            const float dt = next.time - k.time;
            if (dt > 1e-6f) {
                slopeAvg = AddT(slopeAvg, ScaleT(SubT(next.value, k.value), 1.0f / dt));
                contribs++;
            }
        }
        if (contribs > 0) slopeAvg = ScaleT(slopeAvg, 1.0f / (float)contribs);
        if (inAuto)  { k.inSpeed  = slopeAvg; k.inInfluence  = 16.667f; }
        if (outAuto) { k.outSpeed = slopeAvg; k.outInfluence = 16.667f; }
    }
}

// ChanImpl abstracts a (property-type, scalar-dim) pair behind function
// pointers so the drawing code can uniformly read/write per-scalar values
// AND per-scalar tangent components without templating the entire draw
// function.
struct ChannelAccessor {
    int  (*keyCount)(void*)                                = nullptr;
    float(*readValue)(void*, int)                          = nullptr;
    void (*writeValue)(void*, int, float)                  = nullptr;
    // Per-dim in/out speed scalar getters/setters.
    float(*readOutSpeed)(void*, int)                       = nullptr;
    void (*writeOutSpeed)(void*, int, float)               = nullptr;
    float(*readInSpeed)(void*, int)                        = nullptr;
    void (*writeInSpeed)(void*, int, float)                = nullptr;
    // Full-vector speed getters (used for ContinuousBezier mirroring).
    // We copy the whole T so mirroring works across all dims at once.
    void (*copyOutSpeedToIn)(void*, int)                   = nullptr;
    void (*copyInSpeedToOut)(void*, int)                   = nullptr;
    // Shared per-key data.
    float(*readTime)(void*, int)                           = nullptr;
    float(*readOutInf)(void*, int)                         = nullptr;
    void (*writeOutInf)(void*, int, float)                 = nullptr;
    float(*readInInf)(void*, int)                          = nullptr;
    void (*writeInInf)(void*, int, float)                  = nullptr;
    int  (*readInMode)(void*, int)                         = nullptr;
    int  (*readOutMode)(void*, int)                        = nullptr;
    void (*writeInMode)(void*, int, int)                   = nullptr;
    void (*writeOutMode)(void*, int, int)                  = nullptr;
    void (*eraseKey)(void*, int)                           = nullptr;
    void* prop = nullptr;
    const char* label = "";
};

template <typename T, int Dim>
struct ChanImpl {
    static AnimatedProperty<T>& P(void* p) { return *reinterpret_cast<AnimatedProperty<T>*>(p); }
    static int  keyCount(void* p) { return (int)P(p).keyframes.size(); }
    static float readValue(void* p, int i)          { return PickScalar<T>(P(p).keyframes[i].value, Dim); }
    static void  writeValue(void* p, int i, float v){ PutScalar<T>(P(p).keyframes[i].value, Dim, v); }
    static float readOutSpeed(void* p, int i)       { return PickScalar<T>(P(p).keyframes[i].outSpeed, Dim); }
    static void  writeOutSpeed(void* p, int i, float v){ PutScalar<T>(P(p).keyframes[i].outSpeed, Dim, v); }
    static float readInSpeed(void* p, int i)        { return PickScalar<T>(P(p).keyframes[i].inSpeed, Dim); }
    static void  writeInSpeed(void* p, int i, float v){ PutScalar<T>(P(p).keyframes[i].inSpeed, Dim, v); }
    // Mirror the full-vector speed (used by ContinuousBezier drag).
    static void  copyOutSpeedToIn(void* p, int i)   {
        auto& k = P(p).keyframes[i];
        k.inSpeed = ScaleT(k.outSpeed, -1.0f);   // opposite sign for time-mirror
    }
    static void  copyInSpeedToOut(void* p, int i)   {
        auto& k = P(p).keyframes[i];
        k.outSpeed = ScaleT(k.inSpeed, -1.0f);
    }
    static float readTime(void* p, int i)      { return P(p).keyframes[i].time; }
    static float readOutInf(void* p, int i)    { return P(p).keyframes[i].outInfluence; }
    static void  writeOutInf(void* p, int i, float v) { P(p).keyframes[i].outInfluence = v; }
    static float readInInf(void* p, int i)     { return P(p).keyframes[i].inInfluence; }
    static void  writeInInf(void* p, int i, float v)  { P(p).keyframes[i].inInfluence = v; }
    static int   readInMode(void* p, int i)    { return (int)P(p).keyframes[i].incomingMode; }
    static int   readOutMode(void* p, int i)   { return (int)P(p).keyframes[i].outgoingMode; }
    static void  writeInMode(void* p, int i, int m)  { P(p).keyframes[i].incomingMode = (InterpMode)m; }
    static void  writeOutMode(void* p, int i, int m) { P(p).keyframes[i].outgoingMode = (InterpMode)m; }
    static void  eraseKey(void* p, int i) {
        auto& pp = P(p);
        if (i < 0 || i >= (int)pp.keyframes.size()) return;
        pp.keyframes.erase(pp.keyframes.begin() + i);
    }
    static ChannelAccessor Make(AnimatedProperty<T>& prop, const char* label) {
        ChannelAccessor a;
        a.prop = &prop; a.label = label;
        a.keyCount = &keyCount;
        a.readValue = &readValue; a.writeValue = &writeValue;
        a.readOutSpeed = &readOutSpeed; a.writeOutSpeed = &writeOutSpeed;
        a.readInSpeed  = &readInSpeed;  a.writeInSpeed  = &writeInSpeed;
        a.copyOutSpeedToIn = &copyOutSpeedToIn;
        a.copyInSpeedToOut = &copyInSpeedToOut;
        a.readTime = &readTime;
        a.readOutInf = &readOutInf; a.writeOutInf = &writeOutInf;
        a.readInInf  = &readInInf;  a.writeInInf  = &writeInInf;
        a.readInMode = &readInMode; a.readOutMode = &readOutMode;
        a.writeInMode = &writeInMode; a.writeOutMode = &writeOutMode;
        a.eraseKey = &eraseKey;
        return a;
    }
};

} // namespace

void RenderEngine::DrawGraphEditor() {
    Layer* sel = layerManager.GetSelectedLayer();
    if (!sel) {
        ImGui::TextDisabled("Select a layer to edit its animation curves.");
        return;
    }

    // Reset auto-pick + selection on layer change.
    if (sel->id != graphSelectedLayerId) {
        graphSelectedLayerId  = sel->id;
        graphSelectedKeyIndex = -1;
        graphAutoPicked       = false;
        graphDraggedTangent   = GraphTangent::None;
        graphFocusDim         = 0;
    }

    // Auto-pick the "most likely animated" property group on first sight of
    // this layer. Priority: Position -> Rotation -> Scale -> Opacity.
    if (!graphAutoPicked) {
        if      (sel->transform.position.IsAnimated()) graphPropGroup = GraphPropGroup::Position;
        else if (sel->transform.rotation.IsAnimated()) graphPropGroup = GraphPropGroup::Rotation;
        else if (sel->transform.scale.IsAnimated())    graphPropGroup = GraphPropGroup::Scale;
        else if (sel->transform.opacity.IsAnimated())  graphPropGroup = GraphPropGroup::Opacity;
        else                                           graphPropGroup = GraphPropGroup::Position;
        graphAutoPicked = true;
    }

    // ---------------------------- toolbar ------------------------------------
    {
        const bool valueSel = (graphMode == GraphMode::Value);
        if (valueSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.52f, 0.85f, 1.0f));
        if (ImGui::Button("Value")) graphMode = GraphMode::Value;
        if (valueSel) ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool speedSel = (graphMode == GraphMode::Speed);
        if (speedSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.52f, 0.85f, 1.0f));
        if (ImGui::Button("Speed")) graphMode = GraphMode::Speed;
        if (speedSel) ImGui::PopStyleColor();
    }
    ImGui::SameLine();  ImGui::Text("|");  ImGui::SameLine();
    ImGui::TextUnformatted("Property:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const char* groupLabels[(int)GraphPropGroup::COUNT] = {
        "Position", "Rotation", "Scale", "Opacity"
    };
    int gi = (int)graphPropGroup;
    if (ImGui::Combo("##graphGrp", &gi, groupLabels, (int)GraphPropGroup::COUNT)) {
        graphPropGroup = (GraphPropGroup)gi;
        graphSelectedKeyIndex = -1;
        graphFocusDim = 0;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|  Layer: %s", sel->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(" (Shift-drag: influence  |  Alt-drag: speed  |  right-click key for menu)");

    // ---------------------------- resolve property group ----------------------
    // We build an array of per-dim accessors (up to 3) for Value mode drawing.
    ChannelAccessor accs[3];
    int   nDims = 0;
    // Property axis colors (X/Y/Z or focus-only). AE default.
    ImU32 dimColor[3] = {
        IM_COL32(230, 90, 90, 255),   // X red
        IM_COL32(90, 210, 90, 255),   // Y green
        IM_COL32(90, 150, 255, 255),  // Z blue
    };
    const char* dimName[3] = { "x", "y", "z" };
    void* propRawPtr = nullptr;    // reserved for future typed dispatch needs
    // Bind property references at outer scope so the AutoBezier recompute
    // dispatch below can reference them safely. Each case sets up accessors
    // for the group's dims + calls the correctly-typed recompute inline.
    switch (graphPropGroup) {
        case GraphPropGroup::Position: {
            auto& prop = sel->transform.position;
            accs[0] = ChanImpl<Vec3, 0>::Make(prop, "Position.x");
            accs[1] = ChanImpl<Vec3, 1>::Make(prop, "Position.y");
            accs[2] = ChanImpl<Vec3, 2>::Make(prop, "Position.z");
            nDims = 3;
            propRawPtr = &prop;
            RecomputeAutoBezierTangents(prop);
            break;
        }
        case GraphPropGroup::Rotation: {
            auto& prop = sel->transform.rotation;
            accs[0] = ChanImpl<Vec3, 2>::Make(prop, "Rotation.z");
            nDims = 1;   // only Z is meaningful in 2D; hide X/Y from graph
            propRawPtr = &prop;
            RecomputeAutoBezierTangents(prop);
            // Rotation.z uses Z color to be visually consistent, but rotate
            // uses only one curve — recolor to white for readability.
            dimColor[0] = IM_COL32(220, 220, 220, 255);
            dimName[0]  = "z";
            break;
        }
        case GraphPropGroup::Scale: {
            auto& prop = sel->transform.scale;
            accs[0] = ChanImpl<Vec3, 0>::Make(prop, "Scale.x");
            accs[1] = ChanImpl<Vec3, 1>::Make(prop, "Scale.y");
            nDims = 2;   // AE ignores Z for Scale in graph
            propRawPtr = &prop;
            RecomputeAutoBezierTangents(prop);
            break;
        }
        case GraphPropGroup::Opacity: {
            auto& prop = sel->transform.opacity;
            accs[0] = ChanImpl<float, 0>::Make(prop, "Opacity");
            nDims = 1;
            propRawPtr = &prop;
            RecomputeAutoBezierTangents(prop);
            dimColor[0] = IM_COL32(220, 220, 220, 255);
            dimName[0]  = "v";
            break;
        }
        default: return;
    }
    (void)propRawPtr;

    // Clamp focus dim to available range.
    if (graphFocusDim < 0)         graphFocusDim = 0;
    if (graphFocusDim >= nDims)    graphFocusDim = 0;

    const int nKeys = accs[0].keyCount(accs[0].prop);
    if (graphSelectedKeyIndex >= nKeys) graphSelectedKeyIndex = -1;

    if (nKeys == 0) {
        ImGui::TextDisabled("No keyframes on this property. Enable the stopwatch in Inspector to add some.");
    }

    // ---------------------------- canvas backdrop ----------------------------
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 120.0f) canvas_sz.x = 120.0f;
    if (canvas_sz.y < 100.0f) canvas_sz.y = 100.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    dl->AddRectFilled(canvas_p0,
        ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
        IM_COL32(15, 15, 18, 255));

    const float padL = 48.0f, padR = 12.0f, padT = 12.0f, padB = 22.0f;
    ImVec2 g_min = ImVec2(canvas_p0.x + padL, canvas_p0.y + padT);
    ImVec2 g_max = ImVec2(canvas_p0.x + canvas_sz.x - padR,
                          canvas_p0.y + canvas_sz.y - padB);
    if (g_max.x <= g_min.x + 4.0f || g_max.y <= g_min.y + 4.0f) return;

    // ---------------------------- time bounds --------------------------------
    float duration = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
    float tMin = 0.0f;
    float tMax = duration;
    for (int i = 0; i < nKeys; ++i) {
        const float t = accs[0].readTime(accs[0].prop, i);
        if (t < tMin) tMin = t;
        if (t > tMax) tMax = t;
    }
    if (tMax - tMin < 1e-4f) tMax = tMin + 1.0f;

    // ---------------------------- sample curves ------------------------------
    // In Value mode we sample each dim independently.
    // In Speed mode we sample the FULL T-value at each t and compute a single
    // magnitude derivative across all meaningful dims.
    const int   nSamples = std::max(64, (int)(g_max.x - g_min.x) / 2);
    std::vector<float> sampleTime(nSamples);
    // Value samples per dim.
    std::vector<std::vector<float>> valPerDim(nDims, std::vector<float>(nSamples, 0.0f));
    // Speed samples (single curve).
    std::vector<float> spdSamples(nSamples, 0.0f);

    for (int s = 0; s < nSamples; ++s) {
        const float u = (nSamples == 1) ? 0.0f : (float)s / (float)(nSamples - 1);
        const float t = tMin + u * (tMax - tMin);
        sampleTime[s] = t;
        // For each dim, sample value at t using the per-dim accessor.
        if (nKeys >= 1) {
            for (int d = 0; d < nDims; ++d) {
                const auto& acc = accs[d];
                float v = 0.0f;
                if (nKeys == 1) v = acc.readValue(acc.prop, 0);
                else if (t <= acc.readTime(acc.prop, 0)) v = acc.readValue(acc.prop, 0);
                else if (t >= acc.readTime(acc.prop, nKeys - 1)) v = acc.readValue(acc.prop, nKeys - 1);
                else {
                    for (int i = 0; i + 1 < nKeys; ++i) {
                        if (t >= acc.readTime(acc.prop, i) && t <= acc.readTime(acc.prop, i + 1)) {
                            // Segment eval via templated helper — but we only
                            // have opaque prop pointer here. Reuse writeValue
                            // pattern is overkill; instead we exploit the
                            // fact that Bezier eval on one dim is independent
                            // when the property is per-dim tangent-additive.
                            // Simplest: re-derive with (speed, influence)
                            // pulled through the accessor.
                            const float t0 = acc.readTime(acc.prop, i);
                            const float t1 = acc.readTime(acc.prop, i + 1);
                            const float v0 = acc.readValue(acc.prop, i);
                            const float v1 = acc.readValue(acc.prop, i + 1);
                            const int   omA = acc.readOutMode(acc.prop, i);
                            const int   imB = acc.readInMode (acc.prop, i + 1);
                            if (omA == (int)InterpMode::Hold) { v = v0; break; }
                            if (omA == (int)InterpMode::Linear && imB == (int)InterpMode::Linear) {
                                const float span = t1 - t0;
                                const float uu = (span > 1e-6f) ? (t - t0) / span : 0.0f;
                                v = v0 + (v1 - v0) * uu;
                                break;
                            }
                            // Derive Bezier P0..P3 from AE-native fields.
                            const float dtSeg = t1 - t0;
                            const float outInf = std::clamp(acc.readOutInf(acc.prop, i), 0.0f, 100.0f) * 0.01f;
                            const float inInf  = std::clamp(acc.readInInf (acc.prop, i + 1), 0.0f, 100.0f) * 0.01f;
                            const float P0t = t0;
                            const float P1t = t0 + dtSeg * outInf;
                            const float P2t = t1 - dtSeg * inInf;
                            const float P3t = t1;
                            const float outSpd = acc.readOutSpeed(acc.prop, i);
                            const float inSpd  = acc.readInSpeed (acc.prop, i + 1);
                            const float P0v = v0;
                            const float P1v = v0 + outSpd * (dtSeg * outInf);
                            const float P2v = v1 - inSpd  * (dtSeg * inInf);
                            const float P3v = v1;
                            // Newton on time.
                            float uu = (dtSeg > 1e-6f) ? (t - t0) / dtSeg : 0.0f;
                            if (uu < 0) uu = 0; if (uu > 1) uu = 1;
                            for (int it = 0; it < 6; ++it) {
                                const float mu = 1.0f - uu;
                                const float x  = mu*mu*mu*P0t + 3*mu*mu*uu*P1t + 3*mu*uu*uu*P2t + uu*uu*uu*P3t;
                                const float dx = 3*mu*mu*(P1t - P0t) + 6*mu*uu*(P2t - P1t) + 3*uu*uu*(P3t - P2t);
                                const float err = x - t;
                                if (std::fabs(err) < 1e-5f) break;
                                if (std::fabs(dx) < 1e-6f) break;
                                uu -= err / dx;
                                if (uu < 0) uu = 0; if (uu > 1) uu = 1;
                            }
                            const float mu = 1.0f - uu;
                            v = mu*mu*mu*P0v + 3*mu*mu*uu*P1v + 3*mu*uu*uu*P2v + uu*uu*uu*P3v;
                            break;
                        }
                    }
                }
                valPerDim[d][s] = v;
            }
        }
    }

    // Speed samples: finite-difference the multi-dim value vector.
    if (graphMode == GraphMode::Speed && nSamples > 1 && nKeys > 0) {
        for (int s = 0; s < nSamples; ++s) {
            const int a = std::max(0, s - 1);
            const int b = std::min(nSamples - 1, s + 1);
            const float dt = sampleTime[b] - sampleTime[a];
            if (dt <= 1e-6f) { spdSamples[s] = 0.0f; continue; }
            float sumSq = 0.0f;
            for (int d = 0; d < nDims; ++d) {
                const float dv = valPerDim[d][b] - valPerDim[d][a];
                sumSq += dv * dv;
            }
            spdSamples[s] = std::sqrt(sumSq) / dt;
        }
    }

    // ---------------------------- Y bounds -----------------------------------
    float yMin =  1e30f, yMax = -1e30f;
    if (graphMode == GraphMode::Value) {
        for (int d = 0; d < nDims; ++d) {
            for (float v : valPerDim[d]) {
                if (v < yMin) yMin = v; if (v > yMax) yMax = v;
            }
        }
        // Include raw key values so hold keys don't drop off the top edge.
        for (int i = 0; i < nKeys; ++i) {
            for (int d = 0; d < nDims; ++d) {
                const float v = accs[d].readValue(accs[d].prop, i);
                if (v < yMin) yMin = v; if (v > yMax) yMax = v;
            }
        }
    } else {
        for (float v : spdSamples) {
            if (v < yMin) yMin = v; if (v > yMax) yMax = v;
        }
    }
    if (!(yMin < yMax)) { yMin = 0.0f; yMax = 1.0f; }
    const float yPad = (yMax - yMin) * 0.1f + 1e-3f;
    yMin -= yPad; yMax += yPad;

    // ---------------------------- axis + grid --------------------------------
    dl->AddLine(ImVec2(g_min.x, g_max.y), ImVec2(g_max.x, g_max.y), IM_COL32(80, 80, 80, 255), 1.0f);
    dl->AddLine(ImVec2(g_min.x, g_min.y), ImVec2(g_min.x, g_max.y), IM_COL32(80, 80, 80, 255), 1.0f);
    for (int i = 0; i <= 4; ++i) {
        const float u = (float)i / 4.0f;
        const float y = g_max.y + (g_min.y - g_max.y) * u;
        dl->AddLine(ImVec2(g_min.x, y), ImVec2(g_max.x, y), IM_COL32(50, 50, 55, 255), 1.0f);
        const float vv = yMin + (yMax - yMin) * u;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", vv);
        dl->AddText(ImVec2(canvas_p0.x + 4, y - 7), IM_COL32(140, 140, 140, 255), buf);
    }
    for (int i = 0; i <= 5; ++i) {
        const float u = (float)i / 5.0f;
        const float x = g_min.x + (g_max.x - g_min.x) * u;
        const float t = tMin + (tMax - tMin) * u;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2fs", t);
        dl->AddText(ImVec2(x - 14, g_max.y + 3), IM_COL32(140, 140, 140, 255), buf);
    }

    // Mode / axis label — parked at the top-right INSIDE the plot area so it
    // never collides with the leftmost Y-axis numeric label (Task 5.4-fix-2).
    {
        const char* modeLbl = (graphMode == GraphMode::Value) ? "value" : "speed (u/s)";
        const ImU32 modeCol = (graphMode == GraphMode::Value)
                                ? IM_COL32(180, 220, 255, 255)
                                : IM_COL32(255, 200, 120, 255);
        const ImVec2 lblSz = ImGui::CalcTextSize(modeLbl);
        dl->AddText(ImVec2(g_max.x - lblSz.x - 4.0f, g_min.y + 2.0f),
                    modeCol, modeLbl);
    }
    // Dim legend in value mode — kept at top-left area BUT below the axis
    // label row (y offset bumped so the color swatches don't stack with the
    // Y-axis numbers that sit at y - 7 relative to each grid line).
    if (graphMode == GraphMode::Value && nDims > 1) {
        float lx = g_min.x + 4.0f;   // just inside the plot's left edge
        const float ly = g_min.y + 2.0f;
        for (int d = 0; d < nDims; ++d) {
            const bool isFocus = (d == graphFocusDim);
            const ImU32 col = dimColor[d];
            dl->AddRectFilled(ImVec2(lx, ly + 1),
                              ImVec2(lx + 10, ly + 11), col);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%s%s", dimName[d], isFocus ? "*" : " ");
            dl->AddText(ImVec2(lx + 13, ly), col, buf);
            lx += 32.0f;
        }
    }

    // Coord mapping helpers.
    auto ToScreen = [&](float t, float v) -> ImVec2 {
        const float tx = (t - tMin) / (tMax - tMin);
        const float vy = (v - yMin) / (yMax - yMin);
        return ImVec2(g_min.x + tx * (g_max.x - g_min.x),
                      g_max.y - vy * (g_max.y - g_min.y));
    };
    auto ScreenToVal = [&](ImVec2 s) -> ImVec2 {
        const float tx = (s.x - g_min.x) / (g_max.x - g_min.x);
        const float vy = (g_max.y - s.y) / (g_max.y - g_min.y);
        return ImVec2(tMin + tx * (tMax - tMin), yMin + vy * (yMax - yMin));
    };
    auto dist2 = [](ImVec2 a, ImVec2 b) {
        const float dx = a.x - b.x, dy = a.y - b.y; return dx*dx + dy*dy;
    };

    // ---------------------------- plot curves --------------------------------
    if (graphMode == GraphMode::Value) {
        for (int d = 0; d < nDims; ++d) {
            const bool isFocus = (d == graphFocusDim);
            const int  alpha   = isFocus ? 255 : 130;
            const ImU32 base   = dimColor[d];
            const ImU32 col    = (base & 0x00FFFFFFu) | ((ImU32)alpha << 24);
            for (int s = 0; s + 1 < nSamples; ++s) {
                ImVec2 a = ToScreen(sampleTime[s],     valPerDim[d][s]);
                ImVec2 b = ToScreen(sampleTime[s + 1], valPerDim[d][s + 1]);
                dl->AddLine(a, b, col, isFocus ? 2.0f : 1.5f);
            }
        }
    } else {
        const ImU32 col = IM_COL32(255, 200, 120, 255);
        for (int s = 0; s + 1 < nSamples; ++s) {
            ImVec2 a = ToScreen(sampleTime[s],     spdSamples[s]);
            ImVec2 b = ToScreen(sampleTime[s + 1], spdSamples[s + 1]);
            dl->AddLine(a, b, col, 2.0f);
        }
    }

    // ---------------------------- key dots + handles -------------------------
    // Task 5.4-fix-2: handles work in BOTH Value and Speed modes now.
    // Handle Y in Value mode = key.value + speed * dt * inf/100.
    // Handle Y in Speed mode = |speed| (per-dim scalar magnitude).
    // Handle X (both modes) = key.time +/- dt * inf/100.
    ImVec2 selKeyPos(0, 0), selInHandlePos(0, 0), selOutHandlePos(0, 0);
    bool   selHasInHandle = false, selHasOutHandle = false;
    // Both modes are interactive now — the mode gate is gone.
    const bool interactive = true;

    // Auto-expand accumulator (see Task 5.4-fix-2 design section 2). We
    // gather any selected-key handle Y positions in yData-space so we can
    // widen yMin/yMax BEFORE final coord mapping. Because Y bounds have
    // already been computed above, we track "widening" here and apply a
    // one-shot rescale before drawing anything Y-sensitive.
    // Implementation: we redraw key dots + handles with the CURRENT yMin/yMax,
    // AND simultaneously accumulate expansion. If expansion happened, we
    // simply recompute those positions with the widened range. The graph
    // curves themselves were already drawn with the old range — that's ok
    // for one frame; the next frame the sample-based bounds will include
    // the new handle Y and everything self-corrects.
    // Cheaper single-pass approach: peek at the selected key's handle Y
    // BEFORE we draw anything, expand, then draw. We do that below.

    // Peek: if there's a selected key with any Bezier side, compute handle Y
    // in the current mode and widen bounds before drawing.
    if (graphSelectedKeyIndex >= 0 && graphSelectedKeyIndex < nKeys) {
        const int  i    = graphSelectedKeyIndex;
        for (int d = 0; d < nDims; ++d) {
            const auto& acc = accs[d];
            const int outMode = acc.readOutMode(acc.prop, i);
            const int inMode  = acc.readInMode (acc.prop, i);
            const float kt = acc.readTime (acc.prop, i);
            const float kv = acc.readValue(acc.prop, i);
            auto expand = [&](float y) {
                if (y < yMin) yMin = y;
                if (y > yMax) yMax = y;
            };
            const bool outBez = (outMode == (int)InterpMode::Bezier ||
                                 outMode == (int)InterpMode::ContinuousBezier ||
                                 outMode == (int)InterpMode::AutoBezier);
            if (outBez && i + 1 < nKeys) {
                const float dtSeg = acc.readTime(acc.prop, i + 1) - kt;
                const float inf   = std::clamp(acc.readOutInf(acc.prop, i), 0.0f, 100.0f) * 0.01f;
                const float spd   = acc.readOutSpeed(acc.prop, i);
                float hy = (graphMode == GraphMode::Speed) ? std::fabs(spd)
                                                           : (kv + spd * (dtSeg * inf));
                expand(hy);
            }
            const bool inBez  = (inMode == (int)InterpMode::Bezier ||
                                 inMode == (int)InterpMode::ContinuousBezier ||
                                 inMode == (int)InterpMode::AutoBezier);
            if (inBez && i > 0) {
                const float dtSeg = kt - acc.readTime(acc.prop, i - 1);
                const float inf   = std::clamp(acc.readInInf(acc.prop, i), 0.0f, 100.0f) * 0.01f;
                const float spd   = acc.readInSpeed(acc.prop, i);
                float hy = (graphMode == GraphMode::Speed) ? std::fabs(spd)
                                                           : (kv - spd * (dtSeg * inf));
                expand(hy);
            }
            // In Speed mode, one iteration is enough — handles map to the
            // scalar the user is editing on the focused dim.
            if (graphMode == GraphMode::Speed) break;
        }
        // Re-pad after expansion so handles never touch the panel edge.
        const float pad2 = (yMax - yMin) * 0.1f + 1e-3f;
        yMin -= pad2 * 0.5f;
        yMax += pad2 * 0.5f;
    }

    // NB: because Y bounds changed after we drew the value/speed curves above,
    // those curves may look slightly compressed for ONE frame after a handle
    // pushes the bounds. Next frame the sample loop sees the new bounds and
    // everything is consistent. This is intentional — avoids a two-pass
    // sample loop just to handle the rare expansion case.

    for (int i = 0; i < nKeys; ++i) {
        for (int d = 0; d < nDims; ++d) {
            const auto& acc = accs[d];
            const float kt = acc.readTime(acc.prop, i);
            float kv = acc.readValue(acc.prop, i);
            ImVec2 kp;
            if (graphMode == GraphMode::Value) {
                kp = ToScreen(kt, kv);
            } else {
                // Speed mode: place key dot on the sampled speed curve at kt.
                float sv = 0.0f;
                for (int s = 0; s < nSamples; ++s) {
                    if (sampleTime[s] >= kt) { sv = spdSamples[s]; break; }
                }
                kp = ToScreen(kt, sv);
            }

            const bool isSel      = (i == graphSelectedKeyIndex);
            const bool isFocusDim = (d == graphFocusDim);
            const int  alpha      = (isSel || isFocusDim) ? 255 : 130;
            const int inMode  = acc.readInMode (acc.prop, i);
            const int outMode = acc.readOutMode(acc.prop, i);
            const bool anyBezier = (inMode  == (int)InterpMode::Bezier ||
                                    inMode  == (int)InterpMode::ContinuousBezier ||
                                    inMode  == (int)InterpMode::AutoBezier ||
                                    outMode == (int)InterpMode::Bezier ||
                                    outMode == (int)InterpMode::ContinuousBezier ||
                                    outMode == (int)InterpMode::AutoBezier);
            const bool anyHold   = (inMode == (int)InterpMode::Hold || outMode == (int)InterpMode::Hold);

            ImU32 baseCol = isSel ? IM_COL32(255, 220, 80, 255)
                                  : ((graphMode == GraphMode::Value) ? dimColor[d]
                                                                     : IM_COL32(230, 230, 230, 255));
            const ImU32 col = (baseCol & 0x00FFFFFFu) | ((ImU32)alpha << 24);

            if (anyHold) {
                dl->AddRectFilled(ImVec2(kp.x - 5, kp.y - 5), ImVec2(kp.x + 5, kp.y + 5), col);
            } else if (anyBezier) {
                ImVec2 dpts[4] = { ImVec2(kp.x, kp.y - 6), ImVec2(kp.x + 6, kp.y),
                                   ImVec2(kp.x, kp.y + 6), ImVec2(kp.x - 6, kp.y) };
                dl->AddConvexPolyFilled(dpts, 4, col);
            } else {
                dl->AddCircleFilled(kp, 5.0f, col);
            }
            if (isSel && isFocusDim) {
                dl->AddCircle(kp, 9.0f, IM_COL32(255, 220, 80, 200), 12, 1.5f);
                selKeyPos = kp;
            }

            // Handles: on the selected key's focused dim, in BOTH modes.
            if (interactive && isSel && isFocusDim) {
                const bool outBez = (outMode == (int)InterpMode::Bezier ||
                                     outMode == (int)InterpMode::ContinuousBezier ||
                                     outMode == (int)InterpMode::AutoBezier);
                if (outBez && i + 1 < nKeys) {
                    const float nextT = acc.readTime(acc.prop, i + 1);
                    const float dtSeg = nextT - kt;
                    const float inf   = std::clamp(acc.readOutInf(acc.prop, i), 0.0f, 100.0f) * 0.01f;
                    const float spd   = acc.readOutSpeed(acc.prop, i);
                    const float ht    = kt + dtSeg * inf;
                    const float hv    = (graphMode == GraphMode::Speed)
                                          ? std::fabs(spd)
                                          : (kv + spd * (dtSeg * inf));
                    const ImVec2 hp   = ToScreen(ht, hv);
                    const ImU32 hcol  = (outMode == (int)InterpMode::AutoBezier)
                                        ? IM_COL32(180, 180, 180, 160)
                                        : IM_COL32(255, 200, 0, 255);
                    dl->AddLine(kp, hp, hcol, 1.5f);
                    // Bigger, ringed handle in Speed mode so it's obviously
                    // draggable even when speed=0 (handle sits AT the key dot).
                    const float hr = (graphMode == GraphMode::Speed) ? 7.0f : 5.0f;
                    dl->AddCircleFilled(hp, hr, hcol);
                    if (graphMode == GraphMode::Speed) {
                        dl->AddCircle(hp, hr + 2.0f, IM_COL32(255, 255, 255, 200), 12, 1.5f);
                    }
                    selOutHandlePos = hp; selHasOutHandle = true;
                }
                const bool inBez = (inMode == (int)InterpMode::Bezier ||
                                    inMode == (int)InterpMode::ContinuousBezier ||
                                    inMode == (int)InterpMode::AutoBezier);
                if (inBez && i > 0) {
                    const float prevT = acc.readTime(acc.prop, i - 1);
                    const float dtSeg = kt - prevT;
                    const float inf   = std::clamp(acc.readInInf(acc.prop, i), 0.0f, 100.0f) * 0.01f;
                    const float spd   = acc.readInSpeed(acc.prop, i);
                    const float ht    = kt - dtSeg * inf;
                    const float hv    = (graphMode == GraphMode::Speed)
                                          ? std::fabs(spd)
                                          : (kv - spd * (dtSeg * inf));
                    const ImVec2 hp   = ToScreen(ht, hv);
                    const ImU32 hcol  = (inMode == (int)InterpMode::AutoBezier)
                                        ? IM_COL32(180, 180, 180, 160)
                                        : IM_COL32(255, 200, 0, 255);
                    dl->AddLine(kp, hp, hcol, 1.5f);
                    const float hr = (graphMode == GraphMode::Speed) ? 7.0f : 5.0f;
                    dl->AddCircleFilled(hp, hr, hcol);
                    if (graphMode == GraphMode::Speed) {
                        dl->AddCircle(hp, hr + 2.0f, IM_COL32(255, 255, 255, 200), 12, 1.5f);
                    }
                    selInHandlePos = hp; selHasInHandle = true;
                }
            }

            if (graphMode == GraphMode::Speed) break; // one dot per key in speed mode
        }
    }

    // ---------------------------- playhead -----------------------------------
    {
        const float pt = std::clamp(animEngine.currentTime, tMin, tMax);
        const ImVec2 pa = ToScreen(pt, yMin);
        dl->AddLine(ImVec2(pa.x, g_min.y - 6), ImVec2(pa.x, g_max.y + 6),
                    IM_COL32(255, 60, 60, 220), 1.5f);
    }

    // ---------------------------- interaction --------------------------------
    ImGui::SetCursorScreenPos(canvas_p0);
    ImGui::InvisibleButton("##GraphCanvas54Fix", canvas_sz);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mp    = ImGui::GetIO().MousePos;

    // Left-click: try handle first (only if AutoBezier isn't on that side),
    // then any-dim key (auto-focuses to closest dim).
    if (interactive && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        graphDraggedTangent = GraphTangent::None;
        if (graphSelectedKeyIndex >= 0 && graphSelectedKeyIndex < nKeys) {
            const auto& facc = accs[graphFocusDim];
            const int outMode = facc.readOutMode(facc.prop, graphSelectedKeyIndex);
            const int inMode  = facc.readInMode (facc.prop, graphSelectedKeyIndex);
            const bool outLocked = (outMode == (int)InterpMode::AutoBezier);
            const bool inLocked  = (inMode  == (int)InterpMode::AutoBezier);
            // Larger hit radius in Speed mode: handles often sit AT the X-axis
            // (when speed is 0, |speed|=0) so they overlap with the key dot.
            const float hitR2 = (graphMode == GraphMode::Speed) ? (18.0f * 18.0f)
                                                                : (12.0f * 12.0f);
            if (selHasOutHandle && !outLocked && dist2(mp, selOutHandlePos) < hitR2) {
                graphDraggedTangent = GraphTangent::Out;
                MarkForSnapshot();
            } else if (selHasInHandle && !inLocked && dist2(mp, selInHandlePos) < hitR2) {
                graphDraggedTangent = GraphTangent::In;
                MarkForSnapshot();
            }
        }
        if (graphDraggedTangent == GraphTangent::None) {
            // Key hit-testing must match how keys were DRAWN. In Value mode
            // that's ToScreen(kt, readValue); in Speed mode it's the sampled
            // speed curve height (see the key-dot drawing loop above).
            // Bug in first-pass 5.4-fix-2: this loop always used readValue,
            // so keys were unclickable in Speed mode.
            int  bestIdx = -1, bestDim = graphFocusDim;
            float bestD2 = 14.0f * 14.0f;
            for (int i = 0; i < nKeys; ++i) {
                if (graphMode == GraphMode::Speed) {
                    // Single dot per key in Speed mode — place at same
                    // sampled speed height used in the draw loop.
                    const float kt = accs[0].readTime(accs[0].prop, i);
                    float sv = 0.0f;
                    for (int s = 0; s < nSamples; ++s) {
                        if (sampleTime[s] >= kt) { sv = spdSamples[s]; break; }
                    }
                    const ImVec2 kp = ToScreen(kt, sv);
                    const float d2 = dist2(mp, kp);
                    if (d2 < bestD2) { bestD2 = d2; bestIdx = i; bestDim = graphFocusDim; }
                } else {
                    for (int d = 0; d < nDims; ++d) {
                        const auto& acc = accs[d];
                        const float kt = acc.readTime(acc.prop, i);
                        const float kv = acc.readValue(acc.prop, i);
                        const ImVec2 kp = ToScreen(kt, kv);
                        const float d2 = dist2(mp, kp);
                        if (d2 < bestD2) { bestD2 = d2; bestIdx = i; bestDim = d; }
                    }
                }
            }
            if (bestIdx >= 0) {
                graphSelectedKeyIndex = bestIdx;
                graphFocusDim         = bestDim;
            } else {
                graphSelectedKeyIndex = -1;
            }
        }
    }

    // Drag: recover (speed, influence) from the mouse position and store.
    // Works in BOTH Value and Speed modes now. The Y axis of the handle
    // means different things per mode, but the X axis always means
    // influence. Shift = influence-only, Alt = speed-only.
    if (interactive && graphDraggedTangent != GraphTangent::None &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        graphSelectedKeyIndex >= 0 && graphSelectedKeyIndex < nKeys) {
        const auto& acc = accs[graphFocusDim];
        const int   i   = graphSelectedKeyIndex;
        const float kt  = acc.readTime (acc.prop, i);
        const float kv  = acc.readValue(acc.prop, i);
        const ImVec2 tv = ScreenToVal(mp);
        const bool  isOut     = (graphDraggedTangent == GraphTangent::Out);
        const bool  shiftHeld = ImGui::GetIO().KeyShift;
        const bool  altHeld   = ImGui::GetIO().KeyAlt;
        const bool  isSpeed   = (graphMode == GraphMode::Speed);

        // Helper: from the target speed magnitude tv.y, produce a scalar
        // speed value that preserves the sign of the old scalar. Falls back
        // to +magnitude when the old scalar is 0.
        auto rebuildSignedSpeed = [](float oldSpd, float newMag) -> float {
            newMag = (newMag > 0.0f) ? newMag : 0.0f;
            if (oldSpd < 0.0f) return -newMag;
            return  newMag;
        };

        if (isOut && i + 1 < nKeys) {
            const float nextT = acc.readTime(acc.prop, i + 1);
            const float dtSeg = nextT - kt;
            if (dtSeg > 1e-6f) {
                const float oldInf = acc.readOutInf(acc.prop, i);
                const float oldSpd = acc.readOutSpeed(acc.prop, i);

                float newInf;
                if (altHeld) {
                    newInf = oldInf;
                } else {
                    newInf = std::clamp((tv.x - kt) / dtSeg * 100.0f, 0.0f, 100.0f);
                }

                float newSpd;
                if (shiftHeld) {
                    newSpd = oldSpd;
                } else if (isSpeed) {
                    // In Speed mode, mouse Y directly IS the speed magnitude.
                    newSpd = rebuildSignedSpeed(oldSpd, tv.y);
                } else {
                    // Value mode: invert kv + speed*dt*inf/100 = mouse_y.
                    newSpd = (newInf > 0.1f)
                               ? (tv.y - kv) / (dtSeg * newInf * 0.01f)
                               : 0.0f;
                }

                acc.writeOutInf  (acc.prop, i, newInf);
                acc.writeOutSpeed(acc.prop, i, newSpd);
                if (acc.readOutMode(acc.prop, i) == (int)InterpMode::ContinuousBezier) {
                    acc.writeInMode  (acc.prop, i, (int)InterpMode::ContinuousBezier);
                    acc.writeInInf   (acc.prop, i, newInf);
                    acc.copyOutSpeedToIn(acc.prop, i);
                }
            }
        } else if (!isOut && i > 0) {
            const float prevT = acc.readTime(acc.prop, i - 1);
            const float dtSeg = kt - prevT;
            if (dtSeg > 1e-6f) {
                const float oldInf = acc.readInInf(acc.prop, i);
                const float oldSpd = acc.readInSpeed(acc.prop, i);

                float newInf;
                if (altHeld) {
                    newInf = oldInf;
                } else {
                    newInf = std::clamp((kt - tv.x) / dtSeg * 100.0f, 0.0f, 100.0f);
                }

                float newSpd;
                if (shiftHeld) {
                    newSpd = oldSpd;
                } else if (isSpeed) {
                    // Speed mode: mouse Y IS the speed magnitude.
                    newSpd = rebuildSignedSpeed(oldSpd, tv.y);
                } else {
                    newSpd = (newInf > 0.1f)
                               ? (kv - tv.y) / (dtSeg * newInf * 0.01f)
                               : 0.0f;
                }

                acc.writeInInf  (acc.prop, i, newInf);
                acc.writeInSpeed(acc.prop, i, newSpd);
                if (acc.readInMode(acc.prop, i) == (int)InterpMode::ContinuousBezier) {
                    acc.writeOutMode(acc.prop, i, (int)InterpMode::ContinuousBezier);
                    acc.writeOutInf (acc.prop, i, newInf);
                    acc.copyInSpeedToOut(acc.prop, i);
                }
            }
        }
    }
    if (graphDraggedTangent != GraphTangent::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        graphDraggedTangent = GraphTangent::None;
    }

    // Right-click a key -> context menu. Same mode-aware hit-testing as
    // left-click.
    if (interactive && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int bestIdx = -1, bestDim = graphFocusDim;
        float bestD2 = 14.0f * 14.0f;
        for (int i = 0; i < nKeys; ++i) {
            if (graphMode == GraphMode::Speed) {
                const float kt = accs[0].readTime(accs[0].prop, i);
                float sv = 0.0f;
                for (int s = 0; s < nSamples; ++s) {
                    if (sampleTime[s] >= kt) { sv = spdSamples[s]; break; }
                }
                const ImVec2 kp = ToScreen(kt, sv);
                const float d2 = dist2(mp, kp);
                if (d2 < bestD2) { bestD2 = d2; bestIdx = i; bestDim = graphFocusDim; }
            } else {
                for (int d = 0; d < nDims; ++d) {
                    const auto& acc = accs[d];
                    const float kt = acc.readTime(acc.prop, i);
                    const float kv = acc.readValue(acc.prop, i);
                    const ImVec2 kp = ToScreen(kt, kv);
                    const float d2 = dist2(mp, kp);
                    if (d2 < bestD2) { bestD2 = d2; bestIdx = i; bestDim = d; }
                }
            }
        }
        if (bestIdx >= 0) {
            graphContextKeyIndex  = bestIdx;
            graphSelectedKeyIndex = bestIdx;
            graphFocusDim         = bestDim;
            ImGui::OpenPopup("##GraphKeyCtx54Fix");
        }
    }

    if (ImGui::BeginPopup("##GraphKeyCtx54Fix")) {
        const int i = graphContextKeyIndex;
        if (i < 0 || i >= accs[0].keyCount(accs[0].prop)) {
            ImGui::EndPopup();
        } else {
            const auto& acc = accs[graphFocusDim];
            ImGui::TextDisabled("Key #%d (%s @ %.3fs)", i, acc.label, acc.readTime(acc.prop, i));
            ImGui::Separator();
            auto setMode = [&](InterpMode m) {
                MarkForSnapshot();
                // Apply mode to BOTH sides on ALL dims (AE convention when
                // you right-click and set a mode from the key body).
                for (int d = 0; d < nDims; ++d) {
                    accs[d].writeInMode (accs[d].prop, i, (int)m);
                    accs[d].writeOutMode(accs[d].prop, i, (int)m);
                }
                // For fresh Bezier: seed with Easy-Ease-ish influence.
                if (m == InterpMode::Bezier || m == InterpMode::ContinuousBezier) {
                    for (int d = 0; d < nDims; ++d) {
                        accs[d].writeInInf (accs[d].prop, i, 33.333f);
                        accs[d].writeOutInf(accs[d].prop, i, 33.333f);
                        accs[d].writeInSpeed (accs[d].prop, i, 0.0f);
                        accs[d].writeOutSpeed(accs[d].prop, i, 0.0f);
                    }
                }
                if (m == InterpMode::AutoBezier) {
                    // Recompute happens next frame at the top of DrawGraphEditor.
                }
                if (m == InterpMode::Hold) {
                    // Nothing else to seed; evaluator short-circuits.
                }
            };
            if (ImGui::MenuItem("Set to Linear"))            setMode(InterpMode::Linear);
            if (ImGui::MenuItem("Set to Bezier (Easy Ease)"))    setMode(InterpMode::Bezier);
            if (ImGui::MenuItem("Set to Continuous Bezier")) setMode(InterpMode::ContinuousBezier);
            if (ImGui::MenuItem("Set to Auto Bezier"))       setMode(InterpMode::AutoBezier);
            if (ImGui::MenuItem("Set to Hold"))              setMode(InterpMode::Hold);
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Keyframe")) {
                MarkForSnapshot();
                // Erase on ALL dims (they share the same key vector when the
                // property is a single AnimatedProperty<Vec3>, so calling
                // erase on accs[0] is enough — but be safe with float / scalar
                // properties: only call erase once per underlying property).
                accs[0].eraseKey(accs[0].prop, i);
                graphSelectedKeyIndex = -1;
                graphContextKeyIndex  = -1;
            }
            ImGui::EndPopup();
        }
    }
}

// =============================================================================
// End of frame / present
// =============================================================================
// =============================================================================
// Render Queue panel (Task 6): FFmpeg export UI
// =============================================================================
void RenderEngine::DrawRenderQueuePanel() {
    ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Queue", &showRenderQueue)) {
        ImGui::End();
        renderQueueVisibleLastFrame = false;
        return;
    }

    // Task 5.10 (user request #7b): auto-populate export duration from
    // the timeline's last visible layer whenever the panel transitions
    // from hidden -> visible. User override latches so we don't stomp a
    // manually-typed value on subsequent re-opens.
    if (!renderQueueVisibleLastFrame && !exportDurationUserOverridden) {
        RecomputeAutoExportDuration();
    }
    renderQueueVisibleLastFrame = true;

    const auto& status = exportEngine.GetStatus();

    if (status.active) {
        // Live progress mode — show progress + Cancel only.
        ImGui::Text("Rendering %d x %d @ %d fps",
                    exportEngine.Width(), exportEngine.Height(),
                    exportEngine.GetSettings().fps);
        const float frac = (status.totalFrames > 0)
                               ? (float)status.frameIndex / (float)status.totalFrames
                               : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(-1, 0));
        ImGui::Text("Frame %d / %d", status.frameIndex, status.totalFrames);

        // Simple ETA: elapsed / frac - elapsed
        double eta = 0.0;
        if (frac > 1e-4 && status.secondsElapsed > 0.01) {
            eta = (status.secondsElapsed / frac) - status.secondsElapsed;
        }
        ImGui::Text("Elapsed: %.1fs   ETA: %.1fs", status.secondsElapsed, eta);

        ImGui::Separator();
        if (ImGui::Button("Cancel / Stop", ImVec2(-1, 0))) {
            exportEngine.End(true);
        }
    } else {
        // Idle mode — full settings UI.
        // Task 5.0-b: put the FFmpeg diagnostic block AT THE TOP so users
        // discover it before hitting Start Export and getting a cryptic
        // "encoder died" pipe error.
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
            "Step 1: Verify FFmpeg is installed and reachable.");
        if (ImGui::Button("Test FFmpeg (click me FIRST)", ImVec2(-1, 28))) {
            ffmpegTestOk = ExportEngine::TestFfmpegBinary(pendingExport.ffmpegPath,
                                                          ffmpegTestResult);
        }
        if (!ffmpegTestResult.empty()) {
            const ImVec4 col = ffmpegTestOk ? ImVec4(0.4f, 1.0f, 0.6f, 1.0f)
                                            : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(col, ffmpegTestOk ? "OK:" : "Problem:");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", ffmpegTestResult.c_str());
            if (!ffmpegTestOk) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                    "Get ffmpeg.exe from https://ffmpeg.org/download.html "
                    "(pick a Windows build). Extract it, then either add it "
                    "to your system PATH, or paste the full path (e.g. "
                    "C:\\\\ffmpeg\\\\bin\\\\ffmpeg.exe) into the FFmpeg path "
                    "field below and hit Test again.");
            }
        }
        ImGui::Separator();

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
            "Step 2: Configure the export.");
        // Task 6.1: resolution is driven directly by Composition Settings
        // (Composition -> Settings...). The old Preset + Width/Height inputs
        // caused an export/comp size mismatch bug that killed the ffmpeg
        // pipe when the comp was portrait and the preset was landscape.
        // Change the comp size upstream, not here.
        ImGui::Text("Resolution: %d x %d", compositionWidth, compositionHeight);
        ImGui::SameLine();
        ImGui::TextDisabled("(from Composition Settings)");

        // FPS
        static const char* kFpsLabels[] = { "24", "30", "60" };
        static const int   kFpsValues[] = { 24,   30,   60   };
        int fpsIdx = 1;
        for (int i = 0; i < 3; ++i) if (pendingExport.fps == kFpsValues[i]) fpsIdx = i;
        if (ImGui::Combo("Framerate", &fpsIdx, kFpsLabels, 3)) {
            pendingExport.fps = kFpsValues[fpsIdx];
        }

        ImGui::SliderInt("Bitrate (kbps)", &pendingExport.bitrateKbps, 500, 50000);
        // Task 5.10 (user request #7b): slider + paired InputFloat for
        // click-to-type. Both bind to pendingExportSeconds. Editing
        // EITHER widget latches the "user override" flag so auto-populate
        // stops firing on subsequent panel re-opens.
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("##expDurSlider", &pendingExportSeconds, 0.1f, 120.0f)) {
            exportDurationUserOverridden = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat("Duration (s)##expDurInput", &pendingExportSeconds,
                              0.1f, 1.0f, "%.3f")) {
            exportDurationUserOverridden = true;
            if (pendingExportSeconds < 0.1f) pendingExportSeconds = 0.1f;
        }
        // Small reset button: re-enable auto and repopulate now.
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto##expDurAuto")) {
            exportDurationUserOverridden = false;
            RecomputeAutoExportDuration();
        }

        char pathBuf[512];
        std::snprintf(pathBuf, sizeof(pathBuf), "%s", pendingExport.outputPath.c_str());
        if (ImGui::InputText("Output path", pathBuf, sizeof(pathBuf))) {
            pendingExport.outputPath = pathBuf;
        }
        char ffmpegBuf[256];
        std::snprintf(ffmpegBuf, sizeof(ffmpegBuf), "%s", pendingExport.ffmpegPath.c_str());
        if (ImGui::InputText("FFmpeg path", ffmpegBuf, sizeof(ffmpegBuf))) {
            pendingExport.ffmpegPath = ffmpegBuf;
        }

        // Task 5.0-b: the Test FFmpeg block moved to the TOP of the panel
        // (see Step 1). This spot used to duplicate it.

        ImGui::Separator();

        // Show the last error (if any) so the user knows what happened.
        if (status.error) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Last export failed: %s", status.errorMsg.c_str());
        } else if (status.finished) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f),
                               "Last export finished OK (%d frames).", status.frameIndex);
        }
        if (showFfmpegMissingPopup) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "FFmpeg not found. Install ffmpeg.exe and put it in PATH, "
                "or set the FFmpeg path above.");
        }

        if (ImGui::Button("Start Export", ImVec2(-1, 32))) {
            // Task 6.1: force dimensions from the composition. Ignores any
            // stale pendingExport.width/height from a pre-6.1 saved session.
            pendingExport.width       = compositionWidth;
            pendingExport.height      = compositionHeight;
            pendingExport.totalFrames = (int)(pendingExportSeconds *
                                              (float)pendingExport.fps);
            showFfmpegMissingPopup = false;
            if (!exportEngine.Begin(device, context, pendingExport)) {
                showFfmpegMissingPopup = true;
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Task 5.0: exports the ACTUAL composition (shapes +");
        ImGui::TextDisabled("effects) at the requested resolution. Every frame");
        ImGui::TextDisabled("stays under ~33 MB RAM regardless of clip length.");
    }

    ImGui::End();
}

// -----------------------------------------------------------------------------
// PumpExportOneFrameIfActive (Task 6)
//
// Called every frame from the top of EndFrame(). Renders one frame at the
// export resolution into the export RT, then hands the pixels to the
// ExportEngine which pipes them to FFmpeg. This runs on the main thread on
// purpose (see ExportEngine.h header comment for why) but is cheap enough
// that the ImGui UI keeps ticking between frames.
// -----------------------------------------------------------------------------
// =============================================================================
// Debug diagnostic panel (Task 5.0-b)
//
// Prints every value that would need to be inspected to diagnose a "shape
// doesn't move / gizmo is off / transform is weird" bug. Meant to be
// screenshot-able so remote debugging works even when the user can't share
// their PC. Off by default; toggled via View -> Show Debug Panel.
// =============================================================================
void RenderEngine::DrawDebugPanel() {
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug", &showDebugPanel)) { ImGui::End(); return; }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Vec2   canvasMouse = ScreenToCanvas(mouse);

    ImGui::TextDisabled("--- Viewport geometry ---");
    ImGui::Text("Panel size: (%.0f x %.0f)", lastViewportSize.x, lastViewportSize.y);
    ImGui::Text("Letterbox origin: (%.1f, %.1f)",
                lastCanvasLetterboxOrigin.x, lastCanvasLetterboxOrigin.y);
    ImGui::Text("Letterbox size:   (%.1f x %.1f)",
                lastCanvasLetterboxSize.x, lastCanvasLetterboxSize.y);
    ImGui::Text("Composition:      (%d x %d)   preview=%.2f",
                compositionWidth, compositionHeight, previewScale);
    ImGui::Text("Comp texture RT:  (%u x %u)   [comp * previewScale]",
                compTextureWidth, compTextureHeight);

    ImGui::Separator();
    ImGui::TextDisabled("--- Mouse ---");
    ImGui::Text("Screen: (%.1f, %.1f)", mouse.x, mouse.y);
    ImGui::Text("Canvas: (%.1f, %.1f)", canvasMouse.x, canvasMouse.y);

    ImGui::Separator();
    ImGui::TextDisabled("--- Comp clock ---");
    ImGui::Text("Time: %.3f s   Duration: %.3f s   %s%s",
                animEngine.currentTime, animEngine.duration,
                animEngine.isPlaying ? "PLAYING" : "PAUSED",
                animEngine.isLooping ? "  LOOP" : "");

    ImGui::Separator();
    ImGui::TextDisabled("--- Selected layer ---");
    Layer* sel = layerManager.GetSelectedLayer();
    if (!sel) {
        ImGui::TextDisabled("(none)");
    } else {
        // Task 5.1: print BOTH static value and current evaluated value so it's
        // obvious when keyframes are interpolating vs. holding.
        const float tDbg = animEngine.currentTime;
        const Vec3 posE = sel->transform.position.Evaluate(tDbg);
        const Vec3 rotE = sel->transform.rotation.Evaluate(tDbg);
        const Vec3 sclE = sel->transform.scale.Evaluate(tDbg);
        const Vec2 anE  = sel->transform.anchorPoint.Evaluate(tDbg);
        const Vec2 szE  = sel->transform.sizePixels.Evaluate(tDbg);
        const float opE = sel->transform.opacity.Evaluate(tDbg);
        ImGui::Text("id=%d  parent=%d  '%s'", sel->id, sel->parentId, sel->name.c_str());
        ImGui::Text("Position (eval): (%.2f, %.2f, %.2f)  keys=%zu",
                    posE.x, posE.y, posE.z, sel->transform.position.keyframes.size());
        ImGui::Text("Rotation (eval): (%.2f, %.2f, %.2f)  keys=%zu",
                    rotE.x, rotE.y, rotE.z, sel->transform.rotation.keyframes.size());
        ImGui::Text("Scale    (eval): (%.3f, %.3f, %.3f)  keys=%zu",
                    sclE.x, sclE.y, sclE.z, sel->transform.scale.keyframes.size());
        ImGui::Text("Anchor:  (%.2f, %.2f)   Size: (%.0f, %.0f)",
                    anE.x, anE.y, szE.x, szE.y);
        ImGui::Text("Opacity: %.3f  keys=%zu", opE, sel->transform.opacity.keyframes.size());
        ImGui::Text("Stopwatches: pos=%d  rot=%d  scl=%d  op=%d",
                    (int)sel->transform.position.HasStopwatch(),
                    (int)sel->transform.rotation.HasStopwatch(),
                    (int)sel->transform.scale.HasStopwatch(),
                    (int)sel->transform.opacity.HasStopwatch());
        ImGui::Text("Effects: %zu  (%d enabled)",
                    sel->effects.size(),
                    (int)std::count_if(sel->effects.begin(), sel->effects.end(),
                                       [](const Effect& e){ return e.enabled; }));
    }

    ImGui::Separator();
    ImGui::TextDisabled("--- Gizmo drag state ---");
    const char* modeName = "None";
    switch (activeGizmo) {
        case GizmoMode::None:    modeName = "None"; break;
        case GizmoMode::Move:    modeName = "Move"; break;
        case GizmoMode::ScaleNW: modeName = "ScaleNW"; break;
        case GizmoMode::ScaleNE: modeName = "ScaleNE"; break;
        case GizmoMode::ScaleSW: modeName = "ScaleSW"; break;
        case GizmoMode::ScaleSE: modeName = "ScaleSE"; break;
    }
    ImGui::Text("Active: %s   Drag layer id: %d", modeName, dragLayerId);
    ImGui::Text("Drag start mouse (canvas): (%.1f, %.1f)",
                dragStartMouseLocal.x, dragStartMouseLocal.y);
    ImGui::Text("Drag start position:       (%.2f, %.2f)",
                dragStartPosition.x, dragStartPosition.y);
    ImGui::Text("Drag start scale:          (%.3f, %.3f, %.3f)",
                dragStartScale.x, dragStartScale.y, dragStartScale.z);

    ImGui::End();
}

void RenderEngine::PumpExportOneFrameIfActive() {
    if (!context) return;
    const auto& st = exportEngine.GetStatus();
    if (!st.active) return;

    ID3D11RenderTargetView* rtv = exportEngine.GetRenderTargetView();
    if (!rtv) return;

    // -------------------------------------------------------------------
    // Task 6.1-fix: DETERMINISTIC comp-time advancement during export.
    //
    // Previously we relied on animEngine.Update(wall-clock delta) from
    // BeginFrame to advance currentTime. Two problems with that:
    //   1. If the user never hit Play, animEngine.isPlaying == false so
    //      currentTime stays frozen and every exported frame is identical.
    //      -> "the rectangle doesn't move" bug the user reported.
    //   2. Even with Play on, real wall-clock timing means the exported
    //      animation runs at real time (correct only if export FPS matches
    //      render FPS exactly, which almost never happens on potato HW).
    //
    // Fix: derive comp time from the frame index directly:
    //      t = frameIndex / fps
    // Force it into animEngine.currentTime, then re-run layerManager.
    // BeginFrame so every downstream AnimatedProperty<T>::Evaluate() call
    // during the export render pass sees the correct time. isPlaying state
    // is irrelevant — we bypass the wall-clock path entirely.
    // -------------------------------------------------------------------
    const int   fpsSafe    = (exportEngine.GetSettings().fps > 0)
                              ? exportEngine.GetSettings().fps : 30;
    const float exportTime = (float)st.frameIndex / (float)fpsSafe;
    animEngine.currentTime = exportTime;
    layerManager.BeginFrame(exportTime);
    // Camera-follows-layer path also needs a re-sync at the new time so a
    // Camera layer with animated position exports correctly.
    SyncCameraFromLayerIfAny();

    // Task 5.0: the composition texture has already been drawn to during
    // DrawViewportCanvas earlier in the frame. Render the scene AGAIN at
    // export resolution into the export RT, so the MP4 shows the actual
    // composition and not the letterboxed viewport preview.
    //
    // We reuse CompositionRenderer against the export RT directly (the
    // exporter's RTV is BGRA whereas compRTV is RGBA -- the shape shader
    // writes normalized RGBA which the hardware reinterprets for BGRA,
    // so red and blue would swap in the output file. We fix that by
    // pre-swapping the color channels below.)
    if (compRenderer.IsReady()) {
        // Task 6.1: honor the user's Composition Settings background color
        // instead of the pre-5.6 hard-coded dark gray.
        const float* bg = bgColor;

        // Temporarily swap R/B in every layer's fillColor for this pass so
        // the export MP4 (BGRA) reads correctly. Since fillColor is stored
        // as IM_COL32 (which the CompositionRenderer already unpacks as
        // R->r, G->g, B->b for the RGBA compRTV), when we render into a
        // BGRA export RT we need to swap so R goes into the B channel of
        // the encoder input and vice versa.
        //
        // Simpler alternative used here: render into compRTV (RGBA), then
        // CopyResource with a format-view swap into the exporter's staging.
        // Actually simplest: just render into compRTV again at export
        // resolution... but compRTV is fixed at compTextureWidth/Height.
        //
        // Cleanest path: use CompositionRenderer against exportRTV. The
        // BGRA vs RGBA channel-swap is handled by ffmpeg's -pix_fmt bgra
        // flag on the input, which tells ffmpeg that byte order in the
        // stream is B,G,R,A. Our shader outputs 0..1 float RGBA. The
        // hardware writes those floats as R->byte0, G->byte1, B->byte2,
        // A->byte3 for R8G8B8A8_UNORM, but as B->byte0, G->byte1,
        // R->byte2, A->byte3 for B8G8R8A8_UNORM. So when the export RT
        // is BGRA and ffmpeg is told "bgra" (meaning bytes B,G,R,A),
        // everything lines up naturally with NO swap. The color shows
        // correctly.
        // Task 5.13: export uses the SAME per-layer isolation dispatch as
        // the viewport (see DrawViewportCanvas for the full commentary).
        // Layers without effects draw straight to the export RT (fast
        // path). Layers with effects go through the ping-pong dance +
        // composite over the export RT.
        //
        // Task 5.8 / 6.1: full comp resolution export, comp dims = export
        // dims.
        compRenderer.ClearComp(rtv, bg);

        const float compT = layerManager.CurrentCompTime();
        const float dur   = animEngine.duration;
        effectManager.Resize((UINT)exportEngine.Width(),
                             (UINT)exportEngine.Height());
        for (auto& layer : layerManager.Layers()) {
            if (!layer.isVisible) continue;
            if (layer.is3D)       continue;
            if (layer.type == ShapeType::Camera) continue;
            const float outT = (layer.outPoint < 0.0f)
                                    ? (dur > 0.0f ? dur : 1e9f)
                                    : layer.outPoint;
            if (compT < layer.inPoint || compT > outT) continue;

            const bool hasFx = layer.HasAnyEnabledEffect();
            if (!hasFx || !effectManager.IsReady()) {
                compRenderer.RenderSingleLayer(layer, rtv,
                                               (UINT)exportEngine.Width(),
                                               (UINT)exportEngine.Height(),
                                               layerManager,
                                               (UINT)compositionWidth,
                                               (UINT)compositionHeight);
            } else {
                const float transparent[4] = { 0, 0, 0, 0 };
                compRenderer.ClearComp(effectManager.GetPingRTV(), transparent);
                compRenderer.RenderSingleLayer(layer,
                                               effectManager.GetPingRTV(),
                                               (UINT)exportEngine.Width(),
                                               (UINT)exportEngine.Height(),
                                               layerManager,
                                               (UINT)compositionWidth,
                                               (UINT)compositionHeight);
                std::vector<Effect> perLayer;
                perLayer.reserve(layer.effects.size());
                for (const auto& e : layer.effects) {
                    if (e.enabled) perLayer.push_back(e);
                }
                effectManager.ApplyChain(effectManager.GetPingSRV(),
                                         effectManager.GetPongRTV(),
                                         perLayer);
                effectManager.CompositeSRVOver(effectManager.GetPongSRV(), rtv);
            }
        }
    }

    // Hand this GPU frame off to the exporter (CopyResource + Map + fwrite).
    exportEngine.WriteCurrentFrame();

    // Rebind the swap chain's back buffer so the rest of the frame (ImGui)
    // draws where it expects to.
    if (mainRenderTargetView) {
        context->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
    }
}

void RenderEngine::EndFrame() {
    if (!context || !swapChain) return;

    // Task 6: if a Render Queue export is active, produce and pipe one frame
    // BEFORE we render ImGui to the swap chain, so the export RT rebind
    // doesn't leave a stale RT bound when ImGui goes to draw.
    PumpExportOneFrameIfActive();

    ImGui::Render();

    const float clear_color[4] = { 0.12f, 0.12f, 0.14f, 1.00f };
    if (mainRenderTargetView) {
        context->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
        context->ClearRenderTargetView(mainRenderTargetView, clear_color);
    }

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    swapChain->Present(1, 0);
}

// Task 5.0: single source of truth for "give me a new shape".
// Places the shape at the CENTER of the fixed composition canvas (not the
// viewport panel), so new shapes always land in the middle of the visible
// letterboxed image regardless of panel size or camera pan.
void RenderEngine::SpawnShapeAtViewportCenter(ShapeType type, const char* nameHint) {
    // Task 5.3: undo snapshot BEFORE the mutation so Ctrl+Z returns to
    // pre-spawn state.
    MarkForSnapshot();
    const int newId = layerManager.AddLayer(type, nameHint ? std::string(nameHint) : std::string());
    Layer* layer = layerManager.GetLayerById(newId);
    if (!layer) return;

    // Task 5.1: newly-spawned layers have their stopwatches off, so writing
    // to .staticValue directly is correct (no keyframe to key).
    layer->transform.position.staticValue = Vec3(
        (float)compositionWidth  * 0.5f,
        (float)compositionHeight * 0.5f,
        0.0f);

    // Sensible per-type defaults so the shape is visible immediately.
    switch (type) {
        case ShapeType::Rectangle:
            layer->transform.sizePixels.staticValue = Vec2(200.0f, 120.0f);
            break;
        case ShapeType::Ellipse:
            layer->transform.sizePixels.staticValue = Vec2(120.0f, 120.0f);
            break;
        case ShapeType::CustomPath:
            layer->transform.sizePixels.staticValue = Vec2(160.0f, 160.0f);
            break;
        case ShapeType::Camera:
            layer->is3D = true;
            layer->transform.sizePixels.staticValue = Vec2(60.0f, 40.0f);
            layer->transform.position.staticValue   = camera.position; // camera lives in 3D
            break;
        case ShapeType::Null:
            layer->transform.sizePixels.staticValue = Vec2(60.0f, 60.0f);
            layer->fillColor                        = 0xFFAAAAAA; // gray marker
            break;
        case ShapeType::Text:
            // Task 5.9: Text layer. sizePixels is IGNORED at draw time
            // (the atlas dims drive the quad), but we set a nominal value
            // for gizmo drag hit-testing which uses it. TextProps carries
            // the string + font; defaults are set in TextProps constructor.
            layer->transform.sizePixels.staticValue = Vec2(400.0f, 100.0f);
            layer->fillColor                        = 0xFFFFFFFF; // opaque white
            break;
    }
}

// =============================================================================
// Task 5.0: composition render target lifecycle + screen->canvas coord helper
// =============================================================================
bool RenderEngine::CreateCompositionRT(UINT width, UINT height) {
    if (!device) return false;
    if (width  < 16) width  = 16;
    if (height < 16) height = 16;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = width;
    td.Height     = height;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &compTexture);
    if (FAILED(hr) || !compTexture) return false;
    hr = device->CreateRenderTargetView(compTexture, nullptr, &compRTV);
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(compTexture, nullptr, &compSRV);
    if (FAILED(hr)) return false;

    compTextureWidth  = width;
    compTextureHeight = height;
    return true;
}

void RenderEngine::ReleaseCompositionRT() {
    if (compSRV)     { compSRV->Release();     compSRV     = nullptr; }
    if (compRTV)     { compRTV->Release();     compRTV     = nullptr; }
    if (compTexture) { compTexture->Release(); compTexture = nullptr; }
    compTextureWidth = compTextureHeight = 0;
}

// Convert a viewport-panel screen point (Windows-DPI pixels) into composition
// pixels (0..compositionWidth, 0..compositionHeight). Returns (0,0) if the
// letterbox rect is degenerate (panel too small to display anything).
// Task 5.8: returns COMPOSITION coords (not RT coords) so gizmo hit-tests
// stay stable under preview downscaling.
Vec2 RenderEngine::ScreenToCanvas(ImVec2 screen) const {
    if (lastCanvasLetterboxSize.x < 1.0f || lastCanvasLetterboxSize.y < 1.0f) {
        return Vec2(0.0f, 0.0f);
    }
    const float u = (screen.x - lastCanvasLetterboxOrigin.x) / lastCanvasLetterboxSize.x;
    const float v = (screen.y - lastCanvasLetterboxOrigin.y) / lastCanvasLetterboxSize.y;
    // Task 5.8: return coordinates in COMPOSITION-PIXEL space, not RT-pixel
    // space. Every downstream caller (gizmo hit-test, click-to-select,
    // camera drag) authors positions in comp coords, so this keeps them
    // stable regardless of whether the RT is downsampled by preview scale.
    return Vec2(u * (float)compositionWidth,
                v * (float)compositionHeight);
}

// =============================================================================
// Task 5.2: Win32 file dialog wrapper + status banner
// =============================================================================
//
// Uses classic Windows GetOpenFileNameA / GetSaveFileNameA from <commdlg.h>
// (linked via Comdlg32.lib in CMake). Feels native, zero new dependency, no
// COM initialization required. Filter string uses the classic double-null-
// terminated format: "Description\0*.ext\0". Returned path is empty on cancel.
//
// Native Windows dialogs pop up MODAL — they block the app's message loop
// until the user picks or cancels. For a save/open workflow that's what users
// expect, so we don't attempt any async trickery here.
#include <commdlg.h>

std::string RenderEngine::OpenSaveFileDialog(const char* defaultName, bool save) {
    char buffer[MAX_PATH] = {0};
    if (defaultName && *defaultName) {
        std::snprintf(buffer, sizeof(buffer), "%s", defaultName);
    }

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = "Potato Motion Editor project (*.pmge)\0*.pmge\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = buffer;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "pmge";
    ofn.Flags       = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                      (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL ok = save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok) return std::string{};   // user cancelled or error
    return std::string(buffer);
}

void RenderEngine::SetStatus(const std::string& msg, bool isError, float durationSeconds) {
    statusMessage = msg;
    statusIsError = isError;
    // Use SDL performance counter for a wall-clock-independent timer.
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 now  = SDL_GetPerformanceCounter();
    const float  nowSec = (freq > 0) ? (float)((double)now / (double)freq) : 0.0f;
    statusMessageExpiresAt = nowSec + durationSeconds;
    // Also log to console so failures aren't invisible if the banner scrolls off.
    std::cerr << "[Status] " << msg << std::endl;

    // Task 5.2: keep the window title in sync with the currently-open file.
    // Extract the leaf filename from lastSavePath for a cleaner title.
    if (window) {
        std::string title = "Potato Motion Graphics Editor - x64";
        if (!lastSavePath.empty()) {
            const size_t slash = lastSavePath.find_last_of("\\/");
            const std::string leaf = (slash == std::string::npos)
                                         ? lastSavePath
                                         : lastSavePath.substr(slash + 1);
            title += " - " + leaf;
        }
        SDL_SetWindowTitle(window, title.c_str());
    }
}

// =============================================================================
// Task 5.3: undo/redo + AppState helpers
// =============================================================================
void RenderEngine::BuildAppState(AppState& out) {
    out.layerManager      = &layerManager;
    out.camera            = &camera;
    out.animEngine        = &animEngine;
    out.compositionWidth  = compositionWidth;
    out.compositionHeight = compositionHeight;
    out.cameraStyleInt    = (int)cameraStyle;
    out.show3DFeatures    = show3DFeatures;
    // Task 5.6: fps + bgColor.
    out.compositionFps    = compositionFps;
    for (int i = 0; i < 4; ++i) out.bgColor[i] = bgColor[i];
}

void RenderEngine::ApplyLoadedScalars(const AppState& st) {
    compositionWidth  = st.compositionWidth;
    compositionHeight = st.compositionHeight;
    cameraStyle       = (st.cameraStyleInt == 1) ? CameraStyle::AlightMotion
                                                 : CameraStyle::AfterEffects;
    show3DFeatures    = st.show3DFeatures;
    // Task 5.6: fps + bgColor propagate from load / undo.
    compositionFps    = st.compositionFps;
    for (int i = 0; i < 4; ++i) bgColor[i] = st.bgColor[i];
    // Task 5.8: compare RT dims against the CURRENT preview-scaled comp size,
    // not raw comp size. Save/load doesn't touch previewScale (editor state)
    // so this always resizes to the right target after a load/undo.
    if (compTextureWidth  != RtWidth() ||
        compTextureHeight != RtHeight()) {
        ReleaseCompositionRT();
        CreateCompositionRT(RtWidth(), RtHeight());
        effectManager.Resize(RtWidth(), RtHeight());
    }
}

// Task 5.6: MarkForSnapshot now pushes SYNCHRONOUSLY the first time it's
// called in a given frame, guarded by lastSnapshotFrame == currentFrameNumber
// so continuous drags (which mark on mouse-down then mutate over many
// following frames) still coalesce into a single undo entry.
//
// Old model (pendingSnapshot flag + FlushPendingSnapshot at top of next frame)
// was correct for drags but broken for atomic ops in one frame: the snapshot
// captured POST-mutation state so Ctrl+Z was a no-op. Reworked here.
void RenderEngine::MarkForSnapshot() {
    // Same-frame guard: coalesce multiple marks per frame into one push.
    // currentFrameNumber == 0 only during Initialize/pre-first-frame; treat
    // that as "always push" to be safe.
    if (currentFrameNumber != 0 && currentFrameNumber == lastSnapshotFrame) {
        return;
    }
    AppState st{};
    BuildAppState(st);
    undoStack.PushSnapshot(st);
    lastSnapshotFrame = currentFrameNumber;
}

// Legacy no-op kept as a symbol for source compatibility with the previous
// coalescing model. Every caller can be removed in a follow-up commit; this
// stub keeps the current call sites compiling without change.
void RenderEngine::FlushPendingSnapshot() {
    // Intentionally empty. See MarkForSnapshot() above.
}

// =============================================================================
// Task 5.6: Composition Settings modal
// =============================================================================
// OpenCompSettingsModal seeds the staging (pending*) fields from the CURRENT
// live values and sets the show flag so the modal opens on the next frame.
// This is the ONE place these values get copied — DrawCompSettingsModal treats
// them as read/write scratch space and only propagates back on Apply.
void RenderEngine::OpenCompSettingsModal() {
    pendingCompW     = compositionWidth;
    pendingCompH     = compositionHeight;
    pendingCompFps   = compositionFps;
    pendingCompDur   = animEngine.duration;
    for (int i = 0; i < 4; ++i) pendingBgColor[i] = bgColor[i];
    showCompSettingsModal = true;
}

void RenderEngine::DrawCompSettingsModal() {
    if (!showCompSettingsModal) return;

    // ImGui popup lifetime: we call OpenPopup once (guarded), then BeginPopupModal
    // handles show/hide from there.
    static const char* kModalId = "Composition Settings##CompSet";
    ImGui::OpenPopup(kModalId);

    // Center the modal on the main viewport.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal(kModalId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {

        // -------- Resolution ---------------------------------------------
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "Resolution");
        ImGui::Separator();
        // Task 5.6 pre-go review #1: floor is 64px, not 16, to keep letterbox
        // math sane and ImGui::Image() from choking on sub-pixel divisions.
        // Ceiling is 8192 for iGPU safety (larger allocations OOM on potato).
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Width##compW",  &pendingCompW,  0, 0);
        pendingCompW = std::clamp(pendingCompW, 64, 8192);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Height##compH", &pendingCompH, 0, 0);
        pendingCompH = std::clamp(pendingCompH, 64, 8192);

        // Preset dropdown — just fills the W/H fields; user still Applies.
        static const struct { const char* label; int w, h; } kPresets[] = {
            { "1920 x 1080 (16:9 HD)",     1920, 1080 },
            { "1080 x 1920 (9:16 vertical)", 1080, 1920 },
            { "1080 x 1080 (square)",      1080, 1080 },
            { "3840 x 2160 (4K UHD)",      3840, 2160 },
            { "1280 x 720  (720p HD)",     1280, 720  },
            { "640 x 360   (proxy)",       640,  360  },
        };
        const int nPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
        int presetIdx = -1;
        for (int i = 0; i < nPresets; ++i) {
            if (pendingCompW == kPresets[i].w && pendingCompH == kPresets[i].h) {
                presetIdx = i; break;
            }
        }
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("Preset##compPreset",
                              (presetIdx >= 0) ? kPresets[presetIdx].label : "(custom)")) {
            for (int i = 0; i < nPresets; ++i) {
                const bool sel = (i == presetIdx);
                if (ImGui::Selectable(kPresets[i].label, sel)) {
                    pendingCompW = kPresets[i].w;
                    pendingCompH = kPresets[i].h;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 6));

        // -------- Timing -------------------------------------------------
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "Timing");
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("Duration (s)##compDur", &pendingCompDur, 0.0f, 0.0f, "%.2f");
        pendingCompDur = std::clamp(pendingCompDur, 0.1f, 3600.0f);

        // FPS combo with Custom preservation (pre-go review #3).
        static const int   kFpsPresets[] = { 24, 30, 60 };
        static const char* kFpsLabels[]  = { "24 fps", "30 fps", "60 fps" };
        const int nFps = (int)(sizeof(kFpsPresets) / sizeof(kFpsPresets[0]));
        int fpsIdx = -1;
        for (int i = 0; i < nFps; ++i) {
            if (pendingCompFps == kFpsPresets[i]) { fpsIdx = i; break; }
        }
        char customLbl[32];
        std::snprintf(customLbl, sizeof(customLbl), "Custom: %d fps", pendingCompFps);
        const char* preview = (fpsIdx >= 0) ? kFpsLabels[fpsIdx] : customLbl;

        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("Frame Rate##compFps", preview)) {
            // If we currently hold a custom value, list it FIRST so the user
            // can re-select it (or just close without changing).
            if (fpsIdx < 0) {
                if (ImGui::Selectable(customLbl, true)) { /* keep as-is */ }
                ImGui::Separator();
            }
            for (int i = 0; i < nFps; ++i) {
                const bool sel = (i == fpsIdx);
                if (ImGui::Selectable(kFpsLabels[i], sel)) {
                    pendingCompFps = kFpsPresets[i];
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 6));

        // -------- Appearance ---------------------------------------------
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "Appearance");
        ImGui::Separator();
        ImGui::ColorEdit4("Background##compBg", pendingBgColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();

        // -------- Buttons ------------------------------------------------
        const float btnW = 100.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        // Right-align the two buttons.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - (btnW * 2 + 8));
        if (ImGui::Button("Cancel##compCancel", ImVec2(btnW, 0))) {
            showCompSettingsModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply##compApply", ImVec2(btnW, 0))) {
            // Snapshot BEFORE mutations so Ctrl+Z restores the pre-Apply
            // state. Task 5.6 MarkForSnapshot rework makes this synchronous.
            MarkForSnapshot();

            const bool sizeChanged =
                (pendingCompW != compositionWidth ||
                 pendingCompH != compositionHeight);

            compositionWidth   = pendingCompW;
            compositionHeight  = pendingCompH;
            compositionFps     = pendingCompFps;
            for (int i = 0; i < 4; ++i) bgColor[i] = pendingBgColor[i];
            animEngine.duration = pendingCompDur;

            if (sizeChanged) {
                // Task 5.8: allocate at preview-scaled dims, not raw comp dims.
                ReleaseCompositionRT();
                CreateCompositionRT(RtWidth(), RtHeight());
                effectManager.Resize(RtWidth(), RtHeight());
            }

            // Ship-this-commit-only wiring (design section 12): Export Queue
            // default FPS follows the comp's frame rate. Deferred: timeline
            // ruler tick spacing, snap-to-frame scrub.
            pendingExport.fps = compositionFps;

            SetStatus("Composition settings applied.");
            showCompSettingsModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void RenderEngine::Shutdown() {
    if (shutdownCalled) return;
    shutdownCalled = true;

    // Task 6: close any live ffmpeg pipe + release export textures BEFORE
    // the device/context go away.
    exportEngine.End(true);

    // Task 5.0: release the composition renderer + composition RT.
    compRenderer.Shutdown();
    ReleaseCompositionRT();

    // Task 5.9: release DirectWrite factory + text-sprite atlases (each
    // Layer's ComPtr auto-releases on layer destruction, so the only
    // thing to clean up here is the TextRenderer instance itself).
    if (textRenderer) {
        textRenderer->Shutdown();
        delete textRenderer;
        textRenderer = nullptr;
    }

    // Task 5: release GPU resources owned by the effect stack BEFORE the
    // device/context go away.
    effectManager.Shutdown();

    if (imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }

    CleanupRenderTarget();
    if (swapChain) { swapChain->Release(); swapChain = nullptr; }
    if (context)   { context->Release();   context   = nullptr; }
    if (device)    { device->Release();    device    = nullptr; }

    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    SDL_Quit();
}

// =============================================================================
// Task 5.9: Text layer support — font favorites persistence + per-frame
// atlas refresh.
// =============================================================================

// Compute %LOCALAPPDATA%\PotatoMotion\fonts.json. Falls back to CWD file
// if SHGetKnownFolderPath fails (very rare — no-network sandboxed builds).
static std::string GetFontsFavoritePath() {
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wpath))
        && wpath) {
        // UTF-16 -> UTF-8
        const int u8len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                              nullptr, 0, nullptr, nullptr);
        std::string base(u8len > 0 ? u8len - 1 : 0, '\0');
        if (u8len > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                base.data(), u8len, nullptr, nullptr);
        }
        CoTaskMemFree(wpath);
        // Ensure the PotatoMotion subfolder exists (silent if it already does).
        const std::string dir = base + "\\PotatoMotion";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\fonts.json";
    }
    return std::string("fonts.json");
}

void RenderEngine::LoadFontFavorites() {
    favoriteFonts.clear();
    const std::string path = GetFontsFavoritePath();
    std::ifstream f(path);
    if (!f.is_open()) return;

    // Deliberately minimal parser — the file is our own tiny format:
    //   { "favorites": ["Foo", "Bar Baz"] }
    // No nlohmann/json include here (kept isolated to Serialization.cpp
    // per the Task 5.2 rule). Manual bracket-and-comma scan; robust to
    // whitespace but not to arbitrary JSON. If the file's malformed, we
    // silently start with an empty set — no user-facing error dialog.
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    const auto arrStart = s.find('[');
    const auto arrEnd   = s.find(']', arrStart);
    if (arrStart == std::string::npos || arrEnd == std::string::npos) return;
    std::string body = s.substr(arrStart + 1, arrEnd - arrStart - 1);
    size_t i = 0;
    while (i < body.size()) {
        // Advance to next opening quote
        while (i < body.size() && body[i] != '"') ++i;
        if (i >= body.size()) break;
        ++i;
        const size_t start = i;
        while (i < body.size() && body[i] != '"') ++i;
        if (i >= body.size()) break;
        favoriteFonts.insert(body.substr(start, i - start));
        ++i;
    }
}

void RenderEngine::SaveFontFavorites() {
    const std::string path = GetFontsFavoritePath();
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "{\n  \"favorites\": [";
    bool first = true;
    for (const auto& name : favoriteFonts) {
        if (!first) f << ", ";
        first = false;
        // Escape internal quotes and backslashes so exotic font names don't
        // corrupt the file. std::string family names shouldn't contain any
        // of these normally, but defensive is cheap.
        f << "\"";
        for (char c : name) {
            if (c == '\\' || c == '"') f << '\\';
            f << c;
        }
        f << "\"";
    }
    f << "]\n}\n";
    favoritesDirty = false;
}

void RenderEngine::ToggleFontFavorite(const std::string& family) {
    auto it = favoriteFonts.find(family);
    if (it == favoriteFonts.end()) favoriteFonts.insert(family);
    else                            favoriteFonts.erase(it);
    favoritesDirty = true;
}

void RenderEngine::RefreshTextLayerCaches() {
    if (!textRenderer || !device) return;
    for (auto& layer : layerManager.Layers()) {
        if (layer.type != ShapeType::Text) continue;
        // EnsureLayerCache is a no-op fast path when cache-key matches.
        textRenderer->EnsureLayerCache(layer, device);
    }
    // Deferred favorites write. Runs at most once per frame; toggle bursts
    // collapse into one file write.
    if (favoritesDirty) SaveFontFavorites();
}

// =============================================================================
// Task 5.10 (user request #7b): auto-populate export duration from timeline.
// Scans visible layers for the largest resolved outPoint (sentinel -1 = comp
// end), clamps to comp duration. Called on Render Queue panel show
// transitions and on the "Auto" reset button.
// =============================================================================
void RenderEngine::RecomputeAutoExportDuration() {
    float autoExtent = 0.0f;
    for (const auto& L : layerManager.Layers()) {
        if (!L.isVisible) continue;
        const float end = (L.outPoint < 0.0f) ? animEngine.duration : L.outPoint;
        if (end > autoExtent) autoExtent = end;
    }
    // Fallback: empty or all-hidden scene -> comp duration.
    if (autoExtent < 0.1f) autoExtent = animEngine.duration;
    // Never exceed comp duration (playback would just loop past that anyway).
    if (autoExtent > animEngine.duration) autoExtent = animEngine.duration;
    if (autoExtent < 0.1f) autoExtent = 0.1f;
    pendingExportSeconds = autoExtent;
}

// =============================================================================
// Task 5.12: bottom-dock settings persistence via ImGui SettingsHandler.
//
// The bottomPaneMode + bottomPaneSplitFrac values are editor state (not
// scene state), and we want them to survive across app restarts. imgui.ini
// already persists window sizes / dock layouts; we tack our two values into
// the same file via the SettingsHandler API. That way there's ONE settings
// file per user, no extra I/O in Shutdown, and if imgui.ini gets deleted
// (which is how users "reset all UI") our settings reset too — matches
// user expectations.
//
// Handler is registered ONCE on first DrawTimelinePanel call (guarded via
// bottomDockSettingsRegistered) — earlier than that, RenderEngine's `this`
// pointer isn't stable enough to be captured in the handler.
// =============================================================================
namespace {
    // ImGui's SettingsHandler is C-style — the callbacks take a void*
    // UserData that we set to `this`. Statics here so the API stays clean.
    // We ONLY handle one entry per section ("[PotatoBottomDock][State]");
    // parser is single-line "mode=N" / "split=F".
    struct BottomDockSettingsBinding {
        int   modePersisted     = 0;    // 0 = Bars, 1 = Graph
        float splitFracPersisted = 0.30f;
    };

    static void* BottomDock_ReadOpen(ImGuiContext*, ImGuiSettingsHandler* h,
                                     const char* name) {
        auto* bind = reinterpret_cast<BottomDockSettingsBinding*>(h->UserData);
        (void)name;
        // We only expose one section: "State". Any name returns the same
        // binding since there's only one blob to fill.
        return bind;
    }
    static void BottomDock_ReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                     void* entry, const char* line) {
        auto* bind = reinterpret_cast<BottomDockSettingsBinding*>(entry);
        if (!bind || !line) return;
        int   iv = 0;
        float fv = 0.0f;
        if (std::sscanf(line, "mode=%d",  &iv) == 1) {
            bind->modePersisted = iv;
        } else if (std::sscanf(line, "split=%f", &fv) == 1) {
            bind->splitFracPersisted = fv;
        }
    }
    static void BottomDock_ApplyAll(ImGuiContext*, ImGuiSettingsHandler*) {
        // Nothing to do here — the parsed values sit in g_bottomDockBinding
        // and are propagated into live RenderEngine members by
        // RenderEngine::RegisterBottomDockSettings after LoadIniSettings
        // completes. Kept as a no-op stub so the ImGui handler struct is
        // fully populated (some ImGui versions warn on null ApplyAllFn).
    }
    static void BottomDock_WriteAll(ImGuiContext*, ImGuiSettingsHandler* h,
                                     ImGuiTextBuffer* buf) {
        auto* bind = reinterpret_cast<BottomDockSettingsBinding*>(h->UserData);
        if (!bind) return;
        buf->appendf("[%s][State]\n", h->TypeName);
        buf->appendf("mode=%d\n",    bind->modePersisted);
        buf->appendf("split=%.4f\n", bind->splitFracPersisted);
        buf->append("\n");
    }
    // One static binding shared with the RenderEngine — updated each
    // frame from live values (so writes at shutdown capture the current
    // state) and consumed each frame BY the RenderEngine (so reads at
    // startup propagate into live values).
    static BottomDockSettingsBinding g_bottomDockBinding;
}

void RenderEngine::RegisterBottomDockSettings() {
    if (bottomDockSettingsRegistered) {
        // Every frame: sync live values -> binding (for the next Write pass).
        g_bottomDockBinding.modePersisted      = (int)bottomPaneMode;
        g_bottomDockBinding.splitFracPersisted = bottomPaneSplitFrac;
        return;
    }
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return;

    ImGuiSettingsHandler h{};
    h.TypeName   = "PotatoBottomDock";
    h.TypeHash   = ImHashStr("PotatoBottomDock");
    h.ReadOpenFn = BottomDock_ReadOpen;
    h.ReadLineFn = BottomDock_ReadLine;
    h.ApplyAllFn = BottomDock_ApplyAll;
    h.WriteAllFn = BottomDock_WriteAll;
    h.UserData   = &g_bottomDockBinding;
    ImGui::AddSettingsHandler(&h);

    // If imgui.ini was already loaded before we registered (typical since
    // RenderEngine::Initialize creates the ImGui context and ini load
    // happens on first NewFrame), re-load so our handler sees the file.
    // Cheap: imgui.ini is a few KB. Guard on IniFilename in case the user
    // configured io.IniFilename = nullptr to disable persistence entirely.
    if (ctx->IO.IniFilename) {
        ImGui::LoadIniSettingsFromDisk(ctx->IO.IniFilename);
    }

    // Now propagate whatever was in the ini into our live values (with
    // sanity clamps in case the file was hand-edited).
    int   m = g_bottomDockBinding.modePersisted;
    float f = g_bottomDockBinding.splitFracPersisted;
    if (m < 0) m = 0;
    if (m > 1) m = 1;
    if (f < 0.15f) f = 0.15f;
    if (f > 0.60f) f = 0.60f;
    bottomPaneMode      = (m == 1) ? BottomPaneMode::Graph : BottomPaneMode::Bars;
    bottomPaneSplitFrac = f;

    bottomDockSettingsRegistered = true;
}
