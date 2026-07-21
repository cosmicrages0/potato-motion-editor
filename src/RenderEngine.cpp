#include "RenderEngine.h"

#include <algorithm>
#include <cstdio>

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

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    window = SDL_CreateWindow(
        title ? title : "Potato Motion Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        windowWidth, windowHeight, window_flags);

    if (!window) {
        std::cerr << "Error creating SDL Window: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        std::cerr << "Error getting SDL Window Info" << std::endl;
        return false;
    }
    hwnd = wmInfo.info.win.window;

    if (!InitDirectX()) return false;
    if (!InitImGui())   return false;

    // Seed a couple of demo layers so the app is immediately editable.
    layerManager.AddLayer(ShapeType::Rectangle, "Background Rect");
    if (Layer* bg = layerManager.GetSelectedLayer()) {
        bg->transform.sizePixels = { 400.0f, 240.0f };
        bg->fillColor = 0xFF3A3A55; // dark violet-ish (ABGR)
    }
    layerManager.AddLayer(ShapeType::Ellipse, "Bouncing Ball");
    if (Layer* ball = layerManager.GetSelectedLayer()) {
        ball->transform.sizePixels = { 120.0f, 120.0f };
        ball->transform.position   = { 640.0f, 360.0f, 0.0f };
        ball->fillColor = 0xFF00B4FF;
    }
    applySlingshotToSelected = true;

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

    // Apply the slingshot Bezier as a live scale multiplier on the selected
    // layer, purely so the Task 2 demo still visibly works. Task 5 will replace
    // this with per-property PropertyTracks.
    if (applySlingshotToSelected) {
        if (Layer* sel = layerManager.GetSelectedLayer()) {
            const float safeDur = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
            const float t = std::clamp(animEngine.currentTime / safeDur, 0.0f, 1.0f);
            const float k = animEngine.currentCurve.Evaluate(t);
            // Non-destructive: we don't overwrite the authored scale, we just
            // multiply it. Reset the scale slider in the inspector to see this
            // clearly. Clamp low end so a negative Bezier value doesn't flip.
            const float mul = std::max(k, 0.0f);
            sel->transform.scale.x = mul;
            sel->transform.scale.y = mul;
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
        ImGui::DockBuilderDockWindow("Composition Viewport",  dock_main_id);
        ImGui::DockBuilderDockWindow("Inspector & Effects",   dock_right_id);
        ImGui::DockBuilderDockWindow("Timeline",              dock_bottom_id);
        ImGui::DockBuilderDockWindow("Graph Editor",          dock_bottom_right_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File"))        { ImGui::MenuItem("New Composition"); ImGui::MenuItem("Open..."); ImGui::MenuItem("Save"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Edit"))        { ImGui::MenuItem("Undo"); ImGui::MenuItem("Redo"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Composition")) { ImGui::MenuItem("Composition Settings..."); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Layer")) {
            if (ImGui::MenuItem("New Rectangle")) layerManager.AddLayer(ShapeType::Rectangle);
            if (ImGui::MenuItem("New Ellipse"))   layerManager.AddLayer(ShapeType::Ellipse);
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Selected", "Del")) {
                if (layerManager.GetSelectedId() != -1) layerManager.DeleteLayerById(layerManager.GetSelectedId());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Effect"))      { ImGui::MenuItem("Motion Blur"); ImGui::MenuItem("Chromatic Aberration"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Export"))      { ImGui::MenuItem("Render Queue..."); ImGui::EndMenu(); }
        ImGui::EndMenuBar();
    }
    ImGui::End();

    // Panels
    ImGui::Begin("Project Assets");        DrawProjectAssetsPanel();  ImGui::End();
    ImGui::Begin("Composition Viewport");  DrawViewportCanvas();      ImGui::End();
    ImGui::Begin("Inspector & Effects");   DrawInspectorPanel();      ImGui::End();
    ImGui::Begin("Timeline");              DrawTimelinePanel();       ImGui::End();
    ImGui::Begin("Graph Editor");          DrawGraphEditor();         ImGui::End();
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
    if (ImGui::Button("+ Rect"))    layerManager.AddLayer(ShapeType::Rectangle);
    ImGui::SameLine();
    if (ImGui::Button("+ Ellipse")) layerManager.AddLayer(ShapeType::Ellipse);
    ImGui::SameLine();
    if (ImGui::Button("Delete Selected")) {
        if (layerManager.GetSelectedId() != -1) layerManager.DeleteLayerById(layerManager.GetSelectedId());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Slingshot -> Selected Scale", &applySlingshotToSelected);

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

            // Selectable name
            ImGui::TableSetColumnIndex(2);
            const bool isSelected = (layer.id == layerManager.GetSelectedId());
            char label[128];
            std::snprintf(label, sizeof(label), "%s", layer.name.c_str());
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
        ImGui::DragFloat3("Position (x,y,z)", &sel->transform.position.x, 1.0f);
        ImGui::DragFloat3("Rotation (deg)",   &sel->transform.rotation.x, 0.5f);
        ImGui::DragFloat3("Scale",            &sel->transform.scale.x,    0.01f, -10.0f, 10.0f);
        ImGui::DragFloat2("Anchor (0..1)",    &sel->transform.anchorPoint.x, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat2("Size (px)",        &sel->transform.sizePixels.x,  1.0f, 1.0f, 4096.0f);
        ImGui::SliderFloat("Opacity",         &sel->transform.opacity, 0.0f, 1.0f);
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

    // Render every visible layer bottom-up.
    auto& L = layerManager.Layers();
    for (auto& layer : L) {
        if (!layer.isVisible) continue;
        const Mat3 wm = layerManager.GetWorldMatrix(layer.id);
        const float op = layerManager.GetWorldOpacity(layer.id);
        DrawLayerShape(layer, wm, op, canvas_p0, draw_list);
    }

    // Selected-layer overlay + interaction
    Layer* sel = layerManager.GetSelectedLayer();
    if (sel && sel->isVisible) {
        const Mat3 wm = layerManager.GetWorldMatrix(sel->id);
        DrawSelectionGizmos(*sel, wm, canvas_p0, draw_list);
        HandleGizmoInteraction(*sel, wm, canvas_p0, canvas_sz);
    } else {
        // Still need an invisible click-catch so unselected clicks can select a layer.
        ImGui::SetCursorScreenPos(canvas_p0);
        ImGui::InvisibleButton("ViewportHitTestEmpty", canvas_sz);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Vec2 mw = ScreenToWorld(ImGui::GetIO().MousePos, canvas_p0);
            auto& Ls = layerManager.Layers();
            for (auto it = Ls.rbegin(); it != Ls.rend(); ++it) {
                if (!it->isVisible) continue;
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

    draw_list->AddText(ImVec2(canvas_p0.x + 10, canvas_p0.y + 10),
        IM_COL32(200, 200, 200, 255),
        "Composition Canvas (Layers + Gizmos)");
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
void RenderEngine::EndFrame() {
    if (!context || !swapChain) return;

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

void RenderEngine::Shutdown() {
    if (shutdownCalled) return;
    shutdownCalled = true;

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
