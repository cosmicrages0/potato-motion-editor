#include "RenderEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// Task 5.2: save/load
#include "Serialization.h"

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
    if (!CreateCompositionRT((UINT)compositionWidth, (UINT)compositionHeight)) {
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
        // Keyboard shortcuts (Delete = remove selected layer)
        if (event.type == SDL_KEYDOWN && !ImGui::GetIO().WantTextInput) {
            const SDL_Keycode k = event.key.keysym.sym;
            if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                if (layerManager.GetSelectedId() != -1) {
                    MarkForSnapshot();
                    layerManager.DeleteLayerById(layerManager.GetSelectedId());
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

    // Task 5.3: coalesce any snapshots requested during the previous frame
    // into a single undo entry BEFORE the new frame's input runs. Guarantees
    // that a mouse-move-heavy drag records only one undo state per drag.
    FlushPendingSnapshot();

    animEngine.Update(deltaTime);

    // Task 5.1: LayerManager now publishes composition time so every downstream
    // read (matrix build, opacity chain, camera sync, Inspector) samples at
    // exactly the same instant. No more per-layer SampleTracks pre-pass — the
    // AnimatedProperty<T>::Evaluate() call at each read site IS the sampling.
    layerManager.BeginFrame(animEngine.currentTime);

    SyncCameraFromLayerIfAny(); // Task 4: let a Camera layer drive the view

    // Legacy Task 2 demo: apply the global slingshot curve as a scale
    // multiplier on the selected layer. Only kicks in for layers whose scale
    // stopwatch is off — real keyframes always win over this demo hack.
    if (applySlingshotToSelected) {
        if (Layer* sel = layerManager.GetSelectedLayer()) {
            if (!sel->transform.scale.IsAnimated()) {
                const float safeDur = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
                const float t = std::clamp(animEngine.currentTime / safeDur, 0.0f, 1.0f);
                const float k = animEngine.currentCurve.Evaluate(t);
                const float mul = std::max(k, 0.0f);
                sel->transform.scale.staticValue = Vec3(mul, mul, 1.0f);
            }
        }
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
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_bottom_id       = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Down,  0.35f, nullptr, &dock_main_id);
        ImGuiID dock_left_id         = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Left,  0.20f, nullptr, &dock_main_id);
        ImGuiID dock_right_id        = ImGui::DockBuilderSplitNode(dock_main_id,   ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);
        ImGuiID dock_bottom_right_id = ImGui::DockBuilderSplitNode(dock_bottom_id, ImGuiDir_Right, 0.50f, nullptr, &dock_bottom_id);

        ImGui::DockBuilderDockWindow("Project Assets",        dock_left_id);
        ImGui::DockBuilderDockWindow("Effects Palette",       dock_left_id);   // tabbed under Project Assets
        ImGui::DockBuilderDockWindow("Composition Viewport",  dock_main_id);
        ImGui::DockBuilderDockWindow("Inspector & Effects",   dock_right_id);
        ImGui::DockBuilderDockWindow("Effect Controls",       dock_right_id);  // tabbed under Inspector
        ImGui::DockBuilderDockWindow("Timeline",              dock_bottom_id);
        ImGui::DockBuilderDockWindow("Graph Editor",          dock_bottom_right_id);

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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Composition")) {
            ImGui::MenuItem("Composition Settings...");
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
    ImGui::Begin("Timeline");              DrawTimelinePanel();          ImGui::End();
    ImGui::Begin("Graph Editor");          DrawGraphEditor();            ImGui::End();

    // Task 6: Render Queue window (only shown after Export -> Render Queue).
    if (showRenderQueue) {
        DrawRenderQueuePanel();
    }

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
    if (ImGui::Button("+ Rect"))    SpawnShapeAtViewportCenter(ShapeType::Rectangle);
    ImGui::SameLine();
    if (ImGui::Button("+ Ellipse")) SpawnShapeAtViewportCenter(ShapeType::Ellipse);
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

    // Task 5.0-b: composition duration is now editable directly from the
    // timeline panel (was previously buried in the Global tab). Users hit
    // the end of the strip almost immediately at the 1-second default and
    // couldn't extend it.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Duration (s)##tl", &animEngine.duration, 0.5f, 60.0f, "%.2f s");
    ImGui::SameLine();
    ImGui::TextDisabled("(scrub playhead or drag it below)");

    ImGui::Separator();

    // Task 4.5: real timeline strip with playhead + keyframe diamonds.
    DrawTimelineStrip();

    ImGui::Separator();

    // Task 5.0-b: 3D column only appears when 3D features are enabled.
    const int  colCount = show3DFeatures ? 5 : 4;
    const int  nameCol  = show3DFeatures ? 2 : 1;
    const int  typeCol  = show3DFeatures ? 3 : 2;
    const int  parentCol= show3DFeatures ? 4 : 3;
    if (ImGui::BeginTable("LayerTable", colCount,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        if (show3DFeatures) ImGui::TableSetupColumn("3D",  ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        auto& L = layerManager.Layers();
        for (size_t i = 0; i < L.size(); ++i) {
            Layer& layer = L[i];
            ImGui::PushID(layer.id);

            ImGui::TableNextRow();
            // Vis toggle
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##vis", &layer.isVisible);

            // 3D toggle (only when 3D features enabled)
            if (show3DFeatures) {
                ImGui::TableSetColumnIndex(1);
                ImGui::Checkbox("##3d", &layer.is3D);
            }

            // Selectable name (with [fx] badge if the layer has any enabled effects)
            ImGui::TableSetColumnIndex(nameCol);
            const bool isSelected = (layer.id == layerManager.GetSelectedId());
            char label[160];
            const bool fx = layer.HasAnyEnabledEffect();
            std::snprintf(label, sizeof(label), "%s%s",
                          fx ? "[fx] " : "",
                          layer.name.c_str());
            // Task 5.3 fix: without AllowOverlap the Selectable steals every
            // click across the entire row (including on the Parent Combo two
            // cells over) because SpanAllColumns extends its hit-rect
            // across all columns. AllowOverlap lets subsequent widgets in the
            // row (like the Combo) claim their own hit-rects and receive
            // clicks normally.
            const ImGuiSelectableFlags selFlags =
                (ImGuiSelectableFlags)((int)ImGuiSelectableFlags_SpanAllColumns |
                                       (int)ImGuiSelectableFlags_AllowOverlap);
            if (ImGui::Selectable(label, isSelected, selFlags)) {
                layerManager.SetSelectedId(layer.id);
            }

            // Type (read-only column)
            ImGui::TableSetColumnIndex(typeCol);
            const char* typeName = "Rect";
            switch (layer.type) {
                case ShapeType::Rectangle:  typeName = "Rectangle"; break;
                case ShapeType::Ellipse:    typeName = "Ellipse";   break;
                case ShapeType::CustomPath: typeName = "Path";      break;
                case ShapeType::Camera:     typeName = "Camera";    break;
                case ShapeType::Null:       typeName = "Null";      break;
            }
            ImGui::TextUnformatted(typeName);

            // Parent dropdown
            ImGui::TableSetColumnIndex(parentCol);
            // Task 5.3 fix: paired with the Selectable's AllowOverlap flag
            // above. Tells ImGui this widget's hit-rect wins over any prior
            // overlapping item — the Combo now receives clicks that used to
            // be stolen by the Name column's row-spanning Selectable.
            ImGui::SetNextItemAllowOverlap();
            const Layer* parent = layerManager.GetLayerById(layer.parentId);
            const char* preview = parent ? parent->name.c_str() : "(none)";
            if (ImGui::BeginCombo("##parent", preview, ImGuiComboFlags_HeightSmall)) {
                if (ImGui::Selectable("(none)", layer.parentId == -1)) {
                    MarkForSnapshot();
                    layerManager.SetParent(layer.id, -1);
                }
                for (const auto& candidate : L) {
                    if (candidate.id == layer.id) continue;
                    const bool wouldCycle = layerManager.WouldCreateCycle(layer.id, candidate.id);
                    ImGuiSelectableFlags flags = wouldCycle ? ImGuiSelectableFlags_Disabled : 0;
                    const bool sel = (layer.parentId == candidate.id);
                    if (ImGui::Selectable(candidate.name.c_str(), sel, flags)) {
                        MarkForSnapshot();
                        layerManager.SetParent(layer.id, candidate.id);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
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

    // Task 5.0: label column width scales with the longest layer name so
    // names no longer clip to "Background..." style ellipses.
    float labelW = 100.0f;
    for (const auto& layer : layerManager.Layers()) {
        const ImVec2 sz = ImGui::CalcTextSize(layer.name.c_str());
        if (sz.x + 24.0f > labelW) labelW = sz.x + 24.0f;
    }
    // Cap at 30% of strip so the ruler always has room; floor at 100px.
    if (labelW > stripW * 0.30f) labelW = stripW * 0.30f;
    if (labelW < 100.0f) labelW = 100.0f;
    const float rulerH = 22.0f;
    const float rowH   = 18.0f;
    const auto& layers = layerManager.Layers();
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

    // Per-layer rows with keyframe diamonds
    const int selectedId = layerManager.GetSelectedId();
    // Task 5.3-fix: precompute playhead X once so per-diamond render code can
    // highlight keys that are near the playhead.
    const float playheadX = TimeToX(animEngine.currentTime);
    constexpr float kNearPlayheadPixels = 10.0f;
    for (size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = layers[i];
        const float rowY0 = origin.y + rulerH + rowH * (float)i;
        const float rowYc = rowY0 + rowH * 0.5f;

        // Row background: highlight selected
        if (layer.id == selectedId) {
            dl->AddRectFilled(ImVec2(origin.x, rowY0),
                              ImVec2(origin.x + stripW, rowY0 + rowH),
                              IM_COL32(50, 50, 80, 200));
        }
        // Label column
        dl->AddText(ImVec2(origin.x + 6.0f, rowY0 + 2.0f),
                    IM_COL32(220, 220, 230, 255), layer.name.c_str());

        // Track baseline
        dl->AddLine(ImVec2(trackX0, rowYc), ImVec2(trackX1, rowYc),
                    IM_COL32(60, 60, 70, 255), 1.0f);

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

        // SCALE (Vec3)
        if (stopwatch("scl", sel->transform.scale.HasStopwatch())) {
            MarkForSnapshot();
            sel->transform.scale.ToggleStopwatch(t);
        }
        ImGui::SameLine();
        Vec3 scl = sel->transform.scale.Evaluate(t);
        if (ImGui::DragFloat3("Scale", &scl.x, 0.01f, -10.0f, 10.0f))
            sel->transform.scale.SetValue(t, scl);
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
        ImGui::SliderFloat("Duration (s)", &animEngine.duration, 0.1f, 60.0f);
        ImGui::Value("Time (s)", animEngine.currentTime);
    }

    if (ImGui::CollapsingHeader("Slingshot Bezier Handles", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("P1 (control)", &animEngine.currentCurve.P1.x, 0.01f, -2.0f, 2.0f);
        ImGui::DragFloat2("P2 (control)", &animEngine.currentCurve.P2.x, 0.01f, -2.0f, 2.0f);
        ImGui::TextWrapped("P1.y and P2.y are intentionally unclamped so you can push above 1.0 for slingshot overshoot or below 0.0 for rebound.");
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
    const float scale = (lastCanvasLetterboxSize.x > 1.0f)
                            ? ((float)compTextureWidth / lastCanvasLetterboxSize.x)
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
    const float compW = (float)compTextureWidth;
    const float compH = (float)compTextureHeight;
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
    auto CanvasToScreen = [&](Vec2 p) -> ImVec2 {
        return ImVec2(lbOrigin.x + (p.x / compW) * lbSize.x,
                      lbOrigin.y + (p.y / compH) * lbSize.y);
    };

    // -------------------------------------------------------------------
    // 1) Render all 2D layers into compRTV using CompositionRenderer.
    //    The renderer clears to the composition background color first.
    // -------------------------------------------------------------------
    if (compRTV && compRenderer.IsReady()) {
        const float bg[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        compRenderer.RenderLayers(compRTV, compTextureWidth, compTextureHeight,
                                  layerManager, bg);

        // 2) If ANY layer has an enabled effect, run its chain against the
        //    whole composition. NOTE: this is a composition-wide effect model
        //    (all shapes go through the same chain), not per-layer effects.
        //    We use the FIRST layer's effect stack that has enabled entries;
        //    per-layer isolated effect passes will land in Task 5.5.
        anyLayerHasEffects = false;
        std::vector<Effect> combined;
        for (auto& layer : layerManager.Layers()) {
            if (!layer.isVisible) continue;
            for (auto& e : layer.effects) {
                if (e.enabled) { combined.push_back(e); anyLayerHasEffects = true; }
            }
        }
        if (anyLayerHasEffects && effectManager.IsReady()) {
            // Resize ping-pong to match composition if it drifted.
            effectManager.Resize(compTextureWidth, compTextureHeight);
            // Run chain: read compSRV -> write into ping's RT -> ... -> final into compRTV.
            // But we can't read and write the same texture in one pass; the
            // chain writes to its own ping-pong then we need one more blit
            // back into compTexture so the ImGui::Image below shows the
            // filtered result. ApplyChain writes the FINAL result into
            // whatever dstRTV we pass, so pass compRTV as final destination
            // AFTER first blitting compSRV into ping (chain naturally handles this).
            effectManager.ApplyChain(compSRV, compRTV, combined);
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
    if (show3DFeatures) {
        const int camLayer = layerManager.FindActiveCameraLayerId();
        std::snprintf(hud, sizeof(hud),
            "Canvas %u x %u   FOV=%.1f   Cam=(%.0f, %.0f, %.0f)%s%s",
            compTextureWidth, compTextureHeight,
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
            "Canvas %u x %u%s",
            compTextureWidth, compTextureHeight,
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
        case EffectType::COUNT: break;
        }
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::PopID();
    }

    // Apply deferred mutations (can't mutate while iterating).
    if (moveEffectId >= 0) { MarkForSnapshot(); sel->MoveEffect(moveEffectId, moveDelta); }
    if (deleteId     >= 0) { MarkForSnapshot(); sel->RemoveEffectById(deleteId); }

    ImGui::TextDisabled("Task 5.0: effects now visually apply to the whole");
    ImGui::TextDisabled("composition (all enabled effects across all layers");
    ImGui::TextDisabled("combine into one chain). Per-layer isolated passes");
    ImGui::TextDisabled("land in Task 5.5.");
}

// =============================================================================
// Graph Editor (unchanged from Task 2 but with the same defensive guards)
// =============================================================================
void RenderEngine::DrawGraphEditor() {
    ImGui::Text("Interactive Slingshot / Overshoot Curve (P1 & P2 handles can exceed 100%%)");

    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 80.0f) canvas_sz.x = 80.0f;
    if (canvas_sz.y < 80.0f) canvas_sz.y = 80.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    draw_list->AddRectFilled(canvas_p0,
        ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
        IM_COL32(15, 15, 18, 255));

    float pad = 40.0f;
    ImVec2 g_min = ImVec2(canvas_p0.x + pad, canvas_p0.y + pad);
    ImVec2 g_max = ImVec2(canvas_p0.x + canvas_sz.x - pad, canvas_p0.y + canvas_sz.y - pad);
    if (g_max.x <= g_min.x + 4.0f || g_max.y <= g_min.y + 4.0f) return;

    float y_0   = g_max.y;
    float y_100 = g_min.y + (g_max.y - g_min.y) * 0.35f;
    float y_range = y_0 - y_100;
    if (std::fabs(y_range) < 0.001f) return;

    draw_list->AddLine(ImVec2(g_min.x, y_0),   ImVec2(g_max.x, y_0),   IM_COL32(80, 80, 80, 255), 1.0f);
    draw_list->AddLine(ImVec2(g_min.x, y_100), ImVec2(g_max.x, y_100), IM_COL32(0, 255, 120, 180), 1.0f);
    draw_list->AddText(ImVec2(g_min.x - 30, y_100 - 6), IM_COL32(0, 255, 120, 255), "100%");
    draw_list->AddText(ImVec2(g_min.x - 20, y_0   - 6), IM_COL32(150, 150, 150, 255), "0%");

    float x_range = g_max.x - g_min.x;
    auto ToScreenG = [&](Vec2 v) -> ImVec2 {
        float x = g_min.x + v.x * x_range;
        float y = y_0 - v.y * y_range;
        return ImVec2(x, y);
    };
    auto ToCurve = [&](ImVec2 screen) -> Vec2 {
        float x = (screen.x - g_min.x) / x_range;
        float y = (y_0 - screen.y) / y_range;
        return Vec2(std::clamp(x, 0.0f, 1.0f), y);
    };

    BezierCurve& curve = animEngine.currentCurve;
    ImVec2 p0 = ToScreenG(curve.P0);
    ImVec2 p1 = ToScreenG(curve.P1);
    ImVec2 p2 = ToScreenG(curve.P2);
    ImVec2 p3 = ToScreenG(curve.P3);

    draw_list->AddLine(p0, p1, IM_COL32(255, 180, 0, 255), 1.5f);
    draw_list->AddLine(p3, p2, IM_COL32(255, 180, 0, 255), 1.5f);
    draw_list->AddBezierCubic(p0, p1, p2, p3, IM_COL32(0, 200, 255, 255), 3.0f, 64);
    draw_list->AddCircleFilled(p0, 5.0f, IM_COL32(200, 200, 200, 255));
    draw_list->AddCircleFilled(p3, 5.0f, IM_COL32(200, 200, 200, 255));
    draw_list->AddCircleFilled(p1, 6.0f, IM_COL32(255, 200, 0, 255));
    draw_list->AddCircleFilled(p2, 6.0f, IM_COL32(255, 200, 0, 255));

    ImGui::SetCursorScreenPos(canvas_p0);
    ImGui::InvisibleButton("GraphCanvas", canvas_sz);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        float d1 = std::sqrt((mousePos.x - p1.x)*(mousePos.x - p1.x) + (mousePos.y - p1.y)*(mousePos.y - p1.y));
        float d2 = std::sqrt((mousePos.x - p2.x)*(mousePos.x - p2.x) + (mousePos.y - p2.y)*(mousePos.y - p2.y));
        if (d1 < d2 && d1 < 30.0f) curve.P1 = ToCurve(mousePos);
        else if (d2 < 30.0f)       curve.P2 = ToCurve(mousePos);
    }

    float duration = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
    float normalizedTime = std::clamp(animEngine.currentTime / duration, 0.0f, 1.0f);
    float playheadX = g_min.x + normalizedTime * x_range;
    draw_list->AddLine(ImVec2(playheadX, g_min.y - 10),
                       ImVec2(playheadX, g_max.y + 10),
                       IM_COL32(255, 50, 50, 255), 2.0f);
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
        return;
    }

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
        // Preset dropdown
        static const char* kPresets[] = { "720p HD (1280x720)",
                                          "1080p Full HD (1920x1080)",
                                          "4K Ultra HD (3840x2160)",
                                          "Custom" };
        static int kPresetSizes[][2] = { {1280, 720}, {1920, 1080},
                                         {3840, 2160}, {0, 0} };
        int presetIdx = 3; // Custom by default; snap if size matches a preset
        for (int i = 0; i < 3; ++i) {
            if (pendingExport.width == kPresetSizes[i][0] &&
                pendingExport.height == kPresetSizes[i][1]) {
                presetIdx = i; break;
            }
        }
        if (ImGui::Combo("Preset", &presetIdx, kPresets, 4)) {
            if (presetIdx < 3) {
                pendingExport.width  = kPresetSizes[presetIdx][0];
                pendingExport.height = kPresetSizes[presetIdx][1];
            }
        }
        ImGui::InputInt("Width",  &pendingExport.width);
        ImGui::InputInt("Height", &pendingExport.height);

        // FPS
        static const char* kFpsLabels[] = { "24", "30", "60" };
        static const int   kFpsValues[] = { 24,   30,   60   };
        int fpsIdx = 1;
        for (int i = 0; i < 3; ++i) if (pendingExport.fps == kFpsValues[i]) fpsIdx = i;
        if (ImGui::Combo("Framerate", &fpsIdx, kFpsLabels, 3)) {
            pendingExport.fps = kFpsValues[fpsIdx];
        }

        ImGui::SliderInt("Bitrate (kbps)", &pendingExport.bitrateKbps, 500, 50000);
        ImGui::SliderFloat("Duration (s)", &pendingExportSeconds, 0.1f, 120.0f);

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
    ImGui::Text("Comp texture:     (%u x %u)", compTextureWidth, compTextureHeight);

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
        const float bg[4] = { 0.08f, 0.08f, 0.10f, 1.0f };

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
        compRenderer.RenderLayers(rtv, (UINT)exportEngine.Width(),
                                  (UINT)exportEngine.Height(),
                                  layerManager, bg);

        // Apply the effect chain if any layer has effects enabled.
        if (anyLayerHasEffects && effectManager.IsReady()) {
            effectManager.Resize((UINT)exportEngine.Width(),
                                 (UINT)exportEngine.Height());
            std::vector<Effect> combined;
            for (auto& layer : layerManager.Layers()) {
                if (!layer.isVisible) continue;
                for (auto& e : layer.effects) {
                    if (e.enabled) combined.push_back(e);
                }
            }
            if (!combined.empty()) {
                // Chain reads from the export SRV -> writes into the export RTV.
                effectManager.ApplyChain(exportEngine.GetShaderResourceView(),
                                         rtv, combined);
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
// pixels (0..compTextureWidth, 0..compTextureHeight). Returns (0,0) if the
// letterbox rect is degenerate (panel too small to display anything).
Vec2 RenderEngine::ScreenToCanvas(ImVec2 screen) const {
    if (lastCanvasLetterboxSize.x < 1.0f || lastCanvasLetterboxSize.y < 1.0f) {
        return Vec2(0.0f, 0.0f);
    }
    const float u = (screen.x - lastCanvasLetterboxOrigin.x) / lastCanvasLetterboxSize.x;
    const float v = (screen.y - lastCanvasLetterboxOrigin.y) / lastCanvasLetterboxSize.y;
    return Vec2(u * (float)compTextureWidth,
                v * (float)compTextureHeight);
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
}

void RenderEngine::ApplyLoadedScalars(const AppState& st) {
    compositionWidth  = st.compositionWidth;
    compositionHeight = st.compositionHeight;
    cameraStyle       = (st.cameraStyleInt == 1) ? CameraStyle::AlightMotion
                                                 : CameraStyle::AfterEffects;
    show3DFeatures    = st.show3DFeatures;
    if (compTextureWidth  != (UINT)compositionWidth ||
        compTextureHeight != (UINT)compositionHeight) {
        ReleaseCompositionRT();
        CreateCompositionRT((UINT)compositionWidth, (UINT)compositionHeight);
        effectManager.Resize((UINT)compositionWidth, (UINT)compositionHeight);
    }
}

void RenderEngine::FlushPendingSnapshot() {
    if (!pendingSnapshot) return;
    pendingSnapshot = false;
    AppState st{};
    BuildAppState(st);
    // Silent on success (undo works transparently). Log on failure only.
    undoStack.PushSnapshot(st);
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
