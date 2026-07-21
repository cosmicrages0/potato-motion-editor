#include "RenderEngine.h"

#include <algorithm>
#include <cstdio>
#include <vector>

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

    // Task 5: initialise the HLSL effect pipeline. Failure is logged but
    // NON-FATAL — the editor still runs; effects panels just show "not ready".
    if (!effectManager.Initialize(device, context,
                                   (UINT)compositionWidth,
                                   (UINT)compositionHeight)) {
        std::cerr << "[RenderEngine] EffectManager init failed; effects disabled" << std::endl;
    }

    // Seed a couple of demo layers so the app is immediately editable.
    // Positions are set explicitly here (SpawnShapeAtViewportCenter uses the
    // last-seen viewport center, which isn't populated yet on first init).
    layerManager.AddLayer(ShapeType::Rectangle, "Background Rect");
    if (Layer* bg = layerManager.GetSelectedLayer()) {
        bg->transform.sizePixels = { 400.0f, 240.0f };
        bg->transform.position   = { 640.0f, 360.0f, 0.0f };
        bg->fillColor = 0xFF3A3A55; // dark violet-ish (ABGR)
    }
    layerManager.AddLayer(ShapeType::Ellipse, "Bouncing Ball");
    if (Layer* ball = layerManager.GetSelectedLayer()) {
        ball->transform.sizePixels = { 120.0f, 120.0f };
        ball->transform.position   = { 640.0f, 360.0f, 0.0f };
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

    animEngine.Update(deltaTime);
    layerManager.BeginFrame(); // reset per-frame world-matrix cache

    // Task 4.5: bake keyframe tracks into live transforms BEFORE anything
    // else reads them (camera sync, matrix build, gizmos, hit-test).
    for (auto& layer : layerManager.Layers()) {
        layer.SampleTracks(animEngine.currentTime);
    }

    SyncCameraFromLayerIfAny(); // Task 4: let a Camera layer drive the view

    // Legacy Task 2 demo: apply the global slingshot curve as a scale
    // multiplier on the selected layer. Only kicks in for layers that don't
    // already have a real scale track — keyframes win.
    if (applySlingshotToSelected) {
        if (Layer* sel = layerManager.GetSelectedLayer()) {
            const bool hasScaleTrack = sel->scaleTrack && !sel->scaleTrack->empty();
            if (!hasScaleTrack) {
                const float safeDur = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
                const float t = std::clamp(animEngine.currentTime / safeDur, 0.0f, 1.0f);
                const float k = animEngine.currentCurve.Evaluate(t);
                const float mul = std::max(k, 0.0f);
                sel->transform.scale.x = mul;
                sel->transform.scale.y = mul;
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
void RenderEngine::RenderUI() { RenderAEDockingLayout(); }

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
            ImGui::MenuItem("New Composition");
            ImGui::MenuItem("Open...");
            ImGui::MenuItem("Save");
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
                } else {
                    std::cerr << "[Import] " << xmlImporter.LastError() << std::endl;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) { ImGui::MenuItem("Undo"); ImGui::MenuItem("Redo"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Layer")) {
            if (ImGui::MenuItem("New Rectangle")) SpawnShapeAtViewportCenter(ShapeType::Rectangle);
            if (ImGui::MenuItem("New Ellipse"))   SpawnShapeAtViewportCenter(ShapeType::Ellipse);
            if (ImGui::MenuItem("New Null Object")) SpawnShapeAtViewportCenter(ShapeType::Null, "Null");
            if (ImGui::MenuItem("New Camera"))    SpawnShapeAtViewportCenter(ShapeType::Camera,  "Camera");
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Selected", "Del")) {
                if (layerManager.GetSelectedId() != -1) layerManager.DeleteLayerById(layerManager.GetSelectedId());
            }
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
            if (ImGui::MenuItem("Add Motion Tile"))         { if (selForFx) selForFx->AddEffect(Effect::MakeMotionTile()); }
            if (ImGui::MenuItem("Add Directional Motion Blur")) { if (selForFx) selForFx->AddEffect(Effect::MakeMotionBlur()); }
            if (ImGui::MenuItem("Add Chromatic Aberration")) { if (selForFx) selForFx->AddEffect(Effect::MakeChromaticAberration()); }
            ImGui::Separator();
            if (ImGui::BeginMenu("Add Blend Mode")) {
                if (ImGui::MenuItem("Normal"))     { if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::Normal)); }
                if (ImGui::MenuItem("Additive"))   { if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::Additive)); }
                if (ImGui::MenuItem("Multiply"))   { if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::Multiply)); }
                if (ImGui::MenuItem("Screen"))     { if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::Screen)); }
                if (ImGui::MenuItem("Overlay"))    { if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::Overlay)); }
                if (ImGui::MenuItem("Color Dodge")){ if (selForFx) selForFx->AddEffect(Effect::MakeBlendMode(BlendMode::ColorDodge)); }
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
    ImGui::SameLine();
    if (ImGui::Button("+ Camera"))  SpawnShapeAtViewportCenter(ShapeType::Camera, "Camera");
    ImGui::SameLine();
    if (ImGui::Button("Delete Selected")) {
        if (layerManager.GetSelectedId() != -1) layerManager.DeleteLayerById(layerManager.GetSelectedId());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Slingshot -> Selected Scale", &applySlingshotToSelected);

    ImGui::Separator();

    // Task 4.5: real timeline strip with playhead + keyframe diamonds.
    DrawTimelineStrip();

    ImGui::Separator();

    if (ImGui::BeginTable("LayerTable", 5,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("3D",  ImGuiTableColumnFlags_WidthFixed, 28.0f);
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

            // 3D toggle (reserved; visible now so users see the roadmap)
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("##3d", &layer.is3D);

            // Selectable name (with [fx] badge if the layer has any enabled effects)
            ImGui::TableSetColumnIndex(2);
            const bool isSelected = (layer.id == layerManager.GetSelectedId());
            char label[160];
            const bool fx = layer.HasAnyEnabledEffect();
            std::snprintf(label, sizeof(label), "%s%s",
                          fx ? "[fx] " : "",
                          layer.name.c_str());
            if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                layerManager.SetSelectedId(layer.id);
            }

            // Type (read-only column)
            ImGui::TableSetColumnIndex(3);
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
            ImGui::TableSetColumnIndex(4);
            const Layer* parent = layerManager.GetLayerById(layer.parentId);
            const char* preview = parent ? parent->name.c_str() : "(none)";
            if (ImGui::BeginCombo("##parent", preview, ImGuiComboFlags_HeightSmall)) {
                if (ImGui::Selectable("(none)", layer.parentId == -1)) {
                    layerManager.SetParent(layer.id, -1);
                }
                for (const auto& candidate : L) {
                    if (candidate.id == layer.id) continue;
                    const bool wouldCycle = layerManager.WouldCreateCycle(layer.id, candidate.id);
                    ImGuiSelectableFlags flags = wouldCycle ? ImGuiSelectableFlags_Disabled : 0;
                    const bool sel = (layer.parentId == candidate.id);
                    if (ImGui::Selectable(candidate.name.c_str(), sel, flags)) {
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
    const float labelW = 140.0f;       // reserved column for layer names on the left
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

        // Draw diamonds for each track type this layer owns
        auto drawKeys = [&](const std::optional<PropertyTrack>& tr, ImU32 col) {
            if (!tr || tr->empty()) return;
            for (const auto& k : tr->keys) {
                const float x = TimeToX(k.time);
                const ImVec2 p(x, rowYc);
                const float r = 4.5f;
                const ImVec2 tri[4] = {
                    ImVec2(p.x,     p.y - r),
                    ImVec2(p.x + r, p.y),
                    ImVec2(p.x,     p.y + r),
                    ImVec2(p.x - r, p.y),
                };
                dl->AddConvexPolyFilled(tri, 4, col);
            }
        };
        drawKeys(layer.positionTrack, IM_COL32(120, 200, 255, 255));
        drawKeys(layer.scaleTrack,    IM_COL32(255, 200, 120, 255));
        drawKeys(layer.rotationTrack, IM_COL32(200, 255, 120, 255));
        drawKeys(layer.opacityTrack,  IM_COL32(255, 120, 200, 255));
    }

    // Playhead
    const float phX = TimeToX(animEngine.currentTime);
    dl->AddLine(ImVec2(phX, origin.y),
                ImVec2(phX, origin.y + totalH),
                IM_COL32(255, 60, 60, 255), 2.0f);
    dl->AddTriangleFilled(
        ImVec2(phX - 5.0f, origin.y),
        ImVec2(phX + 5.0f, origin.y),
        ImVec2(phX,        origin.y + 8.0f),
        IM_COL32(255, 60, 60, 255));

    // Interaction: click / drag anywhere in the strip below the ruler to
    // scrub, click in the ruler to also scrub. Selecting a layer via the
    // strip is handled by the layer table below — one thing per widget.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("TimelineStripHit", ImVec2(stripW, totalH));
    if (ImGui::IsItemActive()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x >= trackX0 && mp.x <= trackX1) {
            animEngine.currentTime = XToTime(mp.x);
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

    // Editable layer name
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        sel->name = nameBuf;
    }
    ImGui::Text("Layer ID: %d   Parent ID: %d", sel->id, sel->parentId);

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Task 4.5: a diamond button beside each property sets a keyframe at
        // the current comp time. A subtle color hint shows whether the
        // property already has any keys.
        const float t = animEngine.currentTime;
        auto kfButton = [&](const char* id, bool has) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                has ? IM_COL32(200, 150, 30, 255) : IM_COL32(60, 60, 70, 255));
            const bool clicked = ImGui::Button(id);
            ImGui::PopStyleColor();
            return clicked;
        };

        if (kfButton("K##pos", sel->positionTrack && !sel->positionTrack->empty())) sel->KeyPosition(t);
        ImGui::SameLine();
        ImGui::DragFloat3("Position (x,y,z)", &sel->transform.position.x, 1.0f);

        if (kfButton("K##rot", sel->rotationTrack && !sel->rotationTrack->empty())) sel->KeyRotation(t);
        ImGui::SameLine();
        ImGui::DragFloat3("Rotation (deg)",   &sel->transform.rotation.x, 0.5f);

        if (kfButton("K##scl", sel->scaleTrack && !sel->scaleTrack->empty())) sel->KeyScale(t);
        ImGui::SameLine();
        ImGui::DragFloat3("Scale",            &sel->transform.scale.x,    0.01f, -10.0f, 10.0f);

        ImGui::DragFloat2("Anchor (0..1)",    &sel->transform.anchorPoint.x, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat2("Size (px)",        &sel->transform.sizePixels.x,  1.0f, 1.0f, 4096.0f);
        ImGui::TextDisabled("Size = base authoring pixels. Scale = animation multiplier.");

        if (kfButton("K##op", sel->opacityTrack && !sel->opacityTrack->empty())) sel->KeyOpacity(t);
        ImGui::SameLine();
        ImGui::SliderFloat("Opacity", &sel->transform.opacity, 0.0f, 1.0f);

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

    if (ImGui::CollapsingHeader("Composition Clock", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(animEngine.isPlaying ? "Pause" : "Play")) {
            if (animEngine.isPlaying) animEngine.Pause(); else animEngine.Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) animEngine.Reset();
        ImGui::Checkbox("Loop", &animEngine.isLooping);
        ImGui::SliderFloat("Duration (s)", &animEngine.duration, 0.1f, 5.0f);
        ImGui::Value("Time (s)", animEngine.currentTime);
    }

    if (ImGui::CollapsingHeader("Slingshot Bezier Handles", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("P1 (control)", &animEngine.currentCurve.P1.x, 0.01f, -2.0f, 2.0f);
        ImGui::DragFloat2("P2 (control)", &animEngine.currentCurve.P2.x, 0.01f, -2.0f, 2.0f);
        ImGui::TextWrapped("P1.y and P2.y are intentionally unclamped so you can push above 1.0 for slingshot overshoot or below 0.0 for rebound.");
    }

    // -----------------------------------------------------------------------
    // Camera Properties (Task 4). Shown when the selected layer is the Camera,
    // or unconditionally at the bottom so users can always tweak the view.
    // -----------------------------------------------------------------------
    const bool cameraSelected = (sel->type == ShapeType::Camera);
    if (ImGui::CollapsingHeader(cameraSelected ? "Camera Properties (Active)" : "Camera Properties",
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

    // Four corners of the layer's local box (pre-anchor space).
    const float w = layer.transform.sizePixels.x;
    const float h = layer.transform.sizePixels.y;
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

    const float w = layer.transform.sizePixels.x;
    const float h = layer.transform.sizePixels.y;

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
    const Vec2 center = worldMatrix.TransformPoint(Vec2(layer.transform.anchorPoint.x * w,
                                                        layer.transform.anchorPoint.y * h));
    drawList->AddCircleFilled(ToScreen(center, canvasOrigin), 6.0f, IM_COL32(255, 80, 80, 255));
}

// Convert a viewport-panel screen point into composition ("world") space.
static Vec2 ScreenToWorld(ImVec2 screen, ImVec2 canvasOrigin) {
    return Vec2(screen.x - canvasOrigin.x, screen.y - canvasOrigin.y);
}

void RenderEngine::HandleGizmoInteraction(Layer& layer, const Mat3& worldMatrix,
                                          ImVec2 canvasOrigin, ImVec2 canvasSize) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    const float w = layer.transform.sizePixels.x;
    const float h = layer.transform.sizePixels.y;

    const Vec2 nw = worldMatrix.TransformPoint(Vec2(0.0f, 0.0f));
    const Vec2 ne = worldMatrix.TransformPoint(Vec2(w,    0.0f));
    const Vec2 se = worldMatrix.TransformPoint(Vec2(w,    h));
    const Vec2 sw = worldMatrix.TransformPoint(Vec2(0.0f, h));
    const Vec2 center = worldMatrix.TransformPoint(Vec2(layer.transform.anchorPoint.x * w,
                                                        layer.transform.anchorPoint.y * h));

    const Vec2 mouseWorld = ScreenToWorld(mouse, canvasOrigin);
    auto dist = [](Vec2 a, Vec2 b){
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx*dx + dy*dy);
    };
    const float kHit = 12.0f;

    // An invisible full-canvas button so we own click focus even over empty space.
    ImGui::SetCursorScreenPos(canvasOrigin);
    ImGui::InvisibleButton("ViewportHitTest", canvasSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    // Begin drag on click
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && activeGizmo == GizmoMode::None) {
        GizmoMode mode = GizmoMode::None;
        if      (dist(mouseWorld, nw) < kHit) mode = GizmoMode::ScaleNW;
        else if (dist(mouseWorld, ne) < kHit) mode = GizmoMode::ScaleNE;
        else if (dist(mouseWorld, se) < kHit) mode = GizmoMode::ScaleSE;
        else if (dist(mouseWorld, sw) < kHit) mode = GizmoMode::ScaleSW;
        else if (dist(mouseWorld, center) < kHit + 2.0f) mode = GizmoMode::Move;
        else {
            // Fall back to "click inside the bounding box = start move".
            // Bounds test via inverse matrix, so rotation is respected.
            const Mat3 inv = worldMatrix.InverseAffine();
            const Vec2 local = inv.TransformPoint(mouseWorld);
            if (local.x >= 0.0f && local.x <= w && local.y >= 0.0f && local.y <= h) {
                mode = GizmoMode::Move;
            }
        }
        if (mode != GizmoMode::None) {
            activeGizmo         = mode;
            dragLayerId         = layer.id;
            dragStartMouseLocal = mouseWorld;
            dragStartPosition   = { layer.transform.position.x, layer.transform.position.y };
            dragStartScale      = layer.transform.scale;
            dragStartSize       = layer.transform.sizePixels;
        }
    }

    // Continue drag
    if (activeGizmo != GizmoMode::None && dragLayerId == layer.id && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 delta = { mouseWorld.x - dragStartMouseLocal.x,
                             mouseWorld.y - dragStartMouseLocal.y };
        if (activeGizmo == GizmoMode::Move) {
            layer.transform.position.x = dragStartPosition.x + delta.x;
            layer.transform.position.y = dragStartPosition.y + delta.y;
        } else {
            // Uniform corner-scale about the anchor. We convert the mouse
            // delta into the layer's LOCAL space using the inverse matrix,
            // then compute a scale ratio relative to the box size.
            const Mat3 inv = worldMatrix.InverseAffine();
            const Vec2 anchorLocal(layer.transform.anchorPoint.x * dragStartSize.x,
                                   layer.transform.anchorPoint.y * dragStartSize.y);
            const Vec2 mouseLocal = inv.TransformPoint(mouseWorld);

            // Pick the "opposite" corner of the dragged handle as the fixed pivot.
            // In local (pre-scale) coords the corners are just (0/w, 0/h). We
            // measure the mouse distance from the anchor and scale so that
            // handle stays under the cursor.
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

            // Clamp to a sane range so a wild mouse fling doesn't NaN us.
            layer.transform.scale.x = std::clamp(dragStartScale.x * sx, -20.0f, 20.0f);
            layer.transform.scale.y = std::clamp(dragStartScale.y * sy, -20.0f, 20.0f);
        }
    }

    // End drag on release
    if (activeGizmo != GizmoMode::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        activeGizmo = GizmoMode::None;
        dragLayerId = -1;
    }

    // Click-to-select any layer under the mouse (only when not dragging a handle)
    if (activeGizmo == GizmoMode::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Iterate top-down so upper layers hit-test first.
        auto& L = layerManager.Layers();
        for (auto it = L.rbegin(); it != L.rend(); ++it) {
            if (!it->isVisible) continue;
            const Mat3 wm = layerManager.GetWorldMatrix(it->id);
            const Mat3 inv = wm.InverseAffine();
            const Vec2 local = inv.TransformPoint(mouseWorld);
            if (local.x >= 0.0f && local.x <= it->transform.sizePixels.x &&
                local.y >= 0.0f && local.y <= it->transform.sizePixels.y) {
                layerManager.SetSelectedId(it->id);
                break;
            }
        }
    }

    (void)active; // silence unused-var warning on some compilers
}

void RenderEngine::DrawViewportCanvas() {
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 50.0f) canvas_sz.x = 50.0f;
    if (canvas_sz.y < 50.0f) canvas_sz.y = 50.0f;

    // Task 4.5: remember viewport geometry so "Add Shape" spawns in view.
    // Viewport is a 1:1 window on composition space (no zoom yet), so the
    // world-space center of the viewport is simply half the canvas size.
    lastViewportSize         = canvas_sz;
    lastViewportCenterWorld  = { canvas_sz.x * 0.5f, canvas_sz.y * 0.5f };

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    // Background + faint composition guide (assumed 1280x720 comp)
    draw_list->AddRectFilled(canvas_p0,
        ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
        IM_COL32(20, 20, 25, 255));
    draw_list->AddRect(
        ImVec2(canvas_p0.x + 20, canvas_p0.y + 20),
        ImVec2(canvas_p0.x + std::min(canvas_sz.x - 20, 20.0f + 1280.0f),
               canvas_p0.y + std::min(canvas_sz.y - 20, 20.0f + 720.0f)),
        IM_COL32(60, 60, 70, 255));

    // -------------------------------------------------------------------
    // Two-pass rendering (Task 4):
    //   Pass A: all 2D layers in timeline order (like Task 3)
    //   Pass B: all 3D layers, depth-sorted by camera-space Z (far -> near)
    // Camera-type layers themselves are skipped in the visual pass; only
    // their gizmo is drawn when selected.
    // -------------------------------------------------------------------
    auto& L = layerManager.Layers();

    // Pass A: 2D. Under Alight camera style, layers flagged stickToCamera
    // get an extra pre-translate matching the camera XY offset so they float
    // as a HUD overlay that follows the camera. Under AE style the flag is
    // ignored (real AE achieves this via parenting to a Null attached to the
    // camera; we support both patterns).
    const bool alightMode = (cameraStyle == CameraStyle::AlightMotion);
    const Vec2 camScreenOffset = { camera.position.x - 640.0f,
                                   camera.position.y - 360.0f };
    for (auto& layer : L) {
        if (!layer.isVisible)                    continue;
        if (layer.is3D)                          continue;
        if (layer.type == ShapeType::Camera)     continue;
        Mat3 wm  = layerManager.GetWorldMatrix(layer.id);
        if (alightMode && layer.stickToCamera) {
            wm = Mat3::Translation(camScreenOffset.x, camScreenOffset.y) * wm;
        }
        const float op = layerManager.GetWorldOpacity(layer.id);
        DrawLayerShape(layer, wm, op, canvas_p0, draw_list);
    }

    // Pass B: 3D — collect visible IDs, sort by view-space Z (descending)
    struct ThreeDEntry { int id; float depth; };
    static thread_local std::vector<ThreeDEntry> depthList;
    depthList.clear();

    Mat4 viewMat = camera.GetViewMatrix();
    for (auto& layer : L) {
        if (!layer.isVisible)                    continue;
        if (!layer.is3D)                         continue;
        if (layer.type == ShapeType::Camera)     continue;
        Mat4 wm4 = layerManager.GetWorldMatrix4(layer.id);
        // Sample the layer's anchor point in view space for a stable depth key.
        const float ax = layer.transform.anchorPoint.x * layer.transform.sizePixels.x;
        const float ay = layer.transform.anchorPoint.y * layer.transform.sizePixels.y;
        Vec4 centerWorld = wm4.TransformVec4(Vec4(ax, ay, 0.0f, 1.0f));
        Vec4 centerView  = viewMat.TransformVec4(centerWorld);
        depthList.push_back({ layer.id, centerView.z });
    }
    // Sort far -> near so the nearest layer is drawn last (on top).
    std::sort(depthList.begin(), depthList.end(),
              [](const ThreeDEntry& a, const ThreeDEntry& b) { return a.depth > b.depth; });

    for (const auto& e : depthList) {
        Layer* layer = layerManager.GetLayerById(e.id);
        if (!layer) continue;
        const Mat4 wm4 = layerManager.GetWorldMatrix4(layer->id);
        const float op = layerManager.GetWorldOpacity(layer->id);
        DrawLayerShape3D(*layer, wm4, op, canvas_p0, canvas_sz, draw_list);
    }

    // -------------------------------------------------------------------
    // Selection gizmos + interaction
    // -------------------------------------------------------------------
    Layer* sel = layerManager.GetSelectedLayer();
    if (sel && sel->isVisible) {
        if (sel->type == ShapeType::Camera) {
            // Camera has its own wireframe gizmo (frustum + look-at line).
            DrawCameraGizmos(*sel, canvas_p0, canvas_sz, draw_list);
        } else if (!sel->is3D) {
            const Mat3 wm = layerManager.GetWorldMatrix(sel->id);
            DrawSelectionGizmos(*sel, wm, canvas_p0, draw_list);
            HandleGizmoInteraction(*sel, wm, canvas_p0, canvas_sz);
        } else {
            // 3D selection: draw a simple projected bounding quad. On-canvas
            // scale gizmos in perspective space are their own milestone, so
            // we leave 3D transform editing to the Inspector for now.
            const Mat4 wm4 = layerManager.GetWorldMatrix4(sel->id);
            const float w = sel->transform.sizePixels.x;
            const float h = sel->transform.sizePixels.y;
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
                    Vec3(world.x, world.y, world.z), canvas_sz.x, canvas_sz.y);
                if (projected[i].w <= 0.0f) { allInFront = false; break; }
            }
            if (allInFront) {
                ImVec2 q[4] = {
                    ImVec2(canvas_p0.x + projected[0].x, canvas_p0.y + projected[0].y),
                    ImVec2(canvas_p0.x + projected[1].x, canvas_p0.y + projected[1].y),
                    ImVec2(canvas_p0.x + projected[2].x, canvas_p0.y + projected[2].y),
                    ImVec2(canvas_p0.x + projected[3].x, canvas_p0.y + projected[3].y),
                };
                draw_list->AddPolyline(q, 4, IM_COL32(0, 255, 200, 255),
                                       ImDrawFlags_Closed, 1.5f);
            }
        }
    }

    // Empty-canvas click-to-select (works both when nothing selected and
    // when the current selection is a Camera layer with no bounding box).
    if (!sel || sel->type == ShapeType::Camera || sel->is3D) {
        ImGui::SetCursorScreenPos(canvas_p0);
        ImGui::InvisibleButton("ViewportHitTestEmpty", canvas_sz);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Vec2 mw = ScreenToWorld(ImGui::GetIO().MousePos, canvas_p0);
            // Only 2D-space hit-test here (3D hit-testing is out of Task 4 scope).
            auto& Ls = layerManager.Layers();
            for (auto it = Ls.rbegin(); it != Ls.rend(); ++it) {
                if (!it->isVisible)                   continue;
                if (it->is3D || it->type == ShapeType::Camera) continue;
                const Mat3 wm2 = layerManager.GetWorldMatrix(it->id);
                const Mat3 inv = wm2.InverseAffine();
                const Vec2 local = inv.TransformPoint(mw);
                if (local.x >= 0.0f && local.x <= it->transform.sizePixels.x &&
                    local.y >= 0.0f && local.y <= it->transform.sizePixels.y) {
                    layerManager.SetSelectedId(it->id);
                    break;
                }
            }
        }
    }

    // Camera navigation (orbit / pan / zoom) — always active over the viewport.
    HandleCameraControls(canvas_p0, canvas_sz);

    // HUD
    const int camLayer = layerManager.FindActiveCameraLayerId();
    char hud[128];
    std::snprintf(hud, sizeof(hud),
        "Active Camera [3D View]  FOV=%.1f  Pos=(%.0f, %.0f, %.0f)%s",
        camera.fov, camera.position.x, camera.position.y, camera.position.z,
        (camLayer >= 0) ? "  (from Camera layer)" : "");
    draw_list->AddText(ImVec2(canvas_p0.x + 10, canvas_p0.y + 10),
        IM_COL32(200, 220, 255, 255), hud);
    draw_list->AddText(ImVec2(canvas_p0.x + 10, canvas_p0.y + 26),
        IM_COL32(160, 160, 170, 255),
        "RMB/Alt+RMB: Orbit   MMB/Alt+MMB: Pan   Wheel: Zoom");
}

// =============================================================================
// 3D layer rendering + camera gizmos + camera navigation (Task 4)
// =============================================================================

void RenderEngine::DrawLayerShape3D(const Layer& layer, const Mat4& worldMatrix4,
                                    float worldOpacity, ImVec2 canvasOrigin,
                                    ImVec2 canvasSize, ImDrawList* drawList) {
    if (!drawList) return;

    const float w = layer.transform.sizePixels.x;
    const float h = layer.transform.sizePixels.y;

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
                cl->transform.position = camera.position;
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

    // The Camera layer's transform.position becomes the eye. Rotation is
    // interpreted as pitch/yaw/roll. If the layer is currently being edited
    // via keyframes, this is the point where the animated value takes effect.
    camera.position = cl->transform.position;
    // If user has toggled off LookAt mode in the inspector, use the layer's
    // rotation directly; otherwise keep the existing target.
    if (!camera.useTargetMode) {
        camera.rotation = cl->transform.rotation;
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
            sel->AddEffect(factory());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(desc);
            ImGui::EndTooltip();
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
    if (moveEffectId >= 0) sel->MoveEffect(moveEffectId, moveDelta);
    if (deleteId     >= 0) sel->RemoveEffectById(deleteId);

    ImGui::TextDisabled("Note: Task 5 wires the pipeline data + UI. Per-layer");
    ImGui::TextDisabled("shader application to the ImGui viewport ships in");
    ImGui::TextDisabled("Task 5.0 Usability Pass (see PROJECT_BRIEFING Section 9.5).");
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
        ImGui::TextDisabled("Task 6 caveat: the current frame content is a");
        ImGui::TextDisabled("solid time-varying color at the export resolution");
        ImGui::TextDisabled("(proves the DX11 -> pipe -> MP4 path end to end).");
        ImGui::TextDisabled("Real composition rendering into the export RT ships");
        ImGui::TextDisabled("with the Task 5.0 Usability Pass.");
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
void RenderEngine::PumpExportOneFrameIfActive() {
    if (!context) return;
    const auto& st = exportEngine.GetStatus();
    if (!st.active) return;

    ID3D11RenderTargetView* rtv = exportEngine.GetRenderTargetView();
    if (!rtv) return;

    // Placeholder frame content: a moving diagonal color gradient in the
    // requested (width x height) buffer. This produces a real playable MP4
    // that clearly shows FPS/resolution/duration are all wired up correctly.
    // Real per-frame composition rendering into this RT ships with 5.0.
    const float t = (float)st.frameIndex / (float)std::max(1, st.totalFrames);
    const float r = 0.30f + 0.30f * std::sin(t * 6.2831853f);
    const float g = 0.30f + 0.30f * std::sin(t * 6.2831853f + 2.0f);
    const float b = 0.30f + 0.30f * std::sin(t * 6.2831853f + 4.0f);
    const float clear[4] = { r, g, b, 1.0f };

    D3D11_VIEWPORT vp = { 0.0f, 0.0f,
                          (float)exportEngine.Width(),
                          (float)exportEngine.Height(),
                          0.0f, 1.0f };
    context->RSSetViewports(1, &vp);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear);

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

// Task 4.5: single source of truth for "give me a new shape".
// Places the shape at whatever the user currently sees as the middle of the
// composition viewport (not the fixed world origin), so on any window size
// or after camera panning the new shape appears right where the user expects.
void RenderEngine::SpawnShapeAtViewportCenter(ShapeType type, const char* nameHint) {
    const int newId = layerManager.AddLayer(type, nameHint ? std::string(nameHint) : std::string());
    Layer* layer = layerManager.GetLayerById(newId);
    if (!layer) return;

    layer->transform.position.x = lastViewportCenterWorld.x;
    layer->transform.position.y = lastViewportCenterWorld.y;
    layer->transform.position.z = 0.0f;

    // Sensible per-type defaults so the shape is visible immediately.
    switch (type) {
        case ShapeType::Rectangle:
            layer->transform.sizePixels = { 200.0f, 120.0f };
            break;
        case ShapeType::Ellipse:
            layer->transform.sizePixels = { 120.0f, 120.0f };
            break;
        case ShapeType::CustomPath:
            layer->transform.sizePixels = { 160.0f, 160.0f };
            break;
        case ShapeType::Camera:
            layer->is3D = true;
            layer->transform.sizePixels = { 60.0f, 40.0f };
            layer->transform.position   = camera.position; // camera lives in 3D
            break;
        case ShapeType::Null:
            layer->transform.sizePixels = { 60.0f, 60.0f };
            layer->fillColor            = 0xFFAAAAAA; // gray marker
            break;
    }
}

void RenderEngine::Shutdown() {
    if (shutdownCalled) return;
    shutdownCalled = true;

    // Task 6: close any live ffmpeg pipe + release export textures BEFORE
    // the device/context go away.
    exportEngine.End(true);

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
