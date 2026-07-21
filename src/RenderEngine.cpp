#include "RenderEngine.h"

RenderEngine::RenderEngine() {
    lastTime = SDL_GetPerformanceCounter();
}

RenderEngine::~RenderEngine() {
    Shutdown();
}

bool RenderEngine::Initialize(const char* title, int width, int height) {
    windowWidth = width;
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
    if (!InitImGui()) return false;

    lastTime = SDL_GetPerformanceCounter();
    return true;
}

bool RenderEngine::InitDirectX() {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    // Potato PC: try DX11 first, fall back all the way to DX9.3 for weak integrated GPUs
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
        // Fallback: WARP software rasterizer for headless CI / no-GPU boxes
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
    if (FAILED(hr)) {
        mainRenderTargetView = nullptr;
    }
}

void RenderEngine::CleanupRenderTarget() {
    if (mainRenderTargetView) {
        mainRenderTargetView->Release();
        mainRenderTargetView = nullptr;
    }
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
    }
}

void RenderEngine::BeginFrame() {
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    float deltaTime = (freq > 0) ? (float)((now - lastTime) / (double)freq) : 0.0f;
    lastTime = now;

    // Clamp huge deltas (e.g., after debugger break) to keep animation stable.
    if (deltaTime > 0.25f) deltaTime = 0.25f;

    animEngine.Update(deltaTime);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void RenderEngine::RenderUI() {
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                 |  ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

    // First-run automatic After Effects layout split
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
        if (ImGui::BeginMenu("Layer"))       { ImGui::MenuItem("New Shape Layer"); ImGui::MenuItem("New Solid"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Effect"))      { ImGui::MenuItem("Motion Blur"); ImGui::MenuItem("Chromatic Aberration"); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Export"))      { ImGui::MenuItem("Render Queue..."); ImGui::EndMenu(); }
        ImGui::EndMenuBar();
    }
    ImGui::End();

    // Panel 1: Project Bin
    ImGui::Begin("Project Assets");
    ImGui::Text("Media Library");
    ImGui::Separator();
    ImGui::BulletText("Rectangle Layer 1");
    ImGui::BulletText("Slingshot Comp");
    ImGui::End();

    // Panel 2: Composition Viewport
    ImGui::Begin("Composition Viewport");
    DrawViewportCanvas();
    ImGui::End();

    // Panel 3: Inspector
    ImGui::Begin("Inspector & Effects");
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        float currentScale = animEngine.GetAnimatedScale();
        ImGui::Value("Calculated Scale (px)", currentScale);
        ImGui::Value("Current Time (s)",      animEngine.currentTime);

        if (ImGui::Button(animEngine.isPlaying ? "Pause" : "Play")) {
            if (animEngine.isPlaying) animEngine.Pause(); else animEngine.Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) animEngine.Reset();

        ImGui::Checkbox("Loop", &animEngine.isLooping);
        ImGui::SliderFloat("Duration (s)", &animEngine.duration, 0.1f, 5.0f);
    }
    if (ImGui::CollapsingHeader("Bezier Handles", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("P1 (control)", &animEngine.currentCurve.P1.x, 0.01f, -2.0f, 2.0f);
        ImGui::DragFloat2("P2 (control)", &animEngine.currentCurve.P2.x, 0.01f, -2.0f, 2.0f);
        ImGui::TextWrapped("P1.y and P2.y are intentionally unclamped so you can push above 1.0 for slingshot overshoot or below 0.0 for rebound.");
    }
    ImGui::End();

    // Panel 4: Timeline
    ImGui::Begin("Timeline");
    ImGui::Text("Layers");
    ImGui::Separator();
    ImGui::Text("[3D] [fx]  Layer 1: Rectangle (Animated)");
    float safeDuration = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
    ImGui::SliderFloat("Scrub Time", &animEngine.currentTime, 0.0f, safeDuration);
    ImGui::End();

    // Panel 5: Graph Editor
    ImGui::Begin("Graph Editor");
    DrawGraphEditor();
    ImGui::End();
}

void RenderEngine::DrawViewportCanvas() {
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 50.0f) canvas_sz.x = 50.0f;
    if (canvas_sz.y < 50.0f) canvas_sz.y = 50.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    draw_list->AddRectFilled(canvas_p0,
        ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
        IM_COL32(20, 20, 25, 255));

    ImVec2 center = ImVec2(canvas_p0.x + canvas_sz.x * 0.5f,
                           canvas_p0.y + canvas_sz.y * 0.5f);
    float size = animEngine.GetAnimatedScale();
    if (size < 1.0f) size = 1.0f;

    // Animated target rectangle - responds live to slingshot Bezier evaluation
    ImVec2 rect_min = ImVec2(center.x - size * 0.5f, center.y - size * 0.5f);
    ImVec2 rect_max = ImVec2(center.x + size * 0.5f, center.y + size * 0.5f);

    draw_list->AddRectFilled(rect_min, rect_max, IM_COL32(0, 180, 255, 220), 8.0f);
    draw_list->AddRect      (rect_min, rect_max, IM_COL32(255, 255, 255, 255), 8.0f, 0, 2.0f);

    draw_list->AddText(ImVec2(canvas_p0.x + 10, canvas_p0.y + 10),
        IM_COL32(200, 200, 200, 255),
        "Composition Canvas (Live DX11 / ImGui Viewport)");
}

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

    // Guard against squished panels
    if (g_max.x <= g_min.x + 4.0f || g_max.y <= g_min.y + 4.0f) return;

    // 0% and 100% reference lines - leave 35% headroom above for overshoot
    float y_0   = g_max.y;
    float y_100 = g_min.y + (g_max.y - g_min.y) * 0.35f;
    float y_range = y_0 - y_100;
    if (fabsf(y_range) < 0.001f) return; // divide-by-zero guard

    draw_list->AddLine(ImVec2(g_min.x, y_0),   ImVec2(g_max.x, y_0),   IM_COL32(80, 80, 80, 255), 1.0f);
    draw_list->AddLine(ImVec2(g_min.x, y_100), ImVec2(g_max.x, y_100), IM_COL32(0, 255, 120, 180), 1.0f);
    draw_list->AddText(ImVec2(g_min.x - 30, y_100 - 6), IM_COL32(0, 255, 120, 255), "100%");
    draw_list->AddText(ImVec2(g_min.x - 20, y_0   - 6), IM_COL32(150, 150, 150, 255), "0%");

    float x_range = g_max.x - g_min.x;

    auto ToScreen = [&](Vec2 v) -> ImVec2 {
        float x = g_min.x + v.x * x_range;
        float y = y_0 - v.y * y_range;
        return ImVec2(x, y);
    };

    auto ToCurve = [&](ImVec2 screen) -> Vec2 {
        float x = (screen.x - g_min.x) / x_range;
        float y = (y_0 - screen.y) / y_range;
        // X is clamped to keep the curve monotonic in time; Y is INTENTIONALLY
        // unclamped so the artist can drag past 100% for slingshot overshoot.
        return Vec2(std::clamp(x, 0.0f, 1.0f), y);
    };

    BezierCurve& curve = animEngine.currentCurve;
    ImVec2 p0 = ToScreen(curve.P0);
    ImVec2 p1 = ToScreen(curve.P1);
    ImVec2 p2 = ToScreen(curve.P2);
    ImVec2 p3 = ToScreen(curve.P3);

    // Handle tangent lines
    draw_list->AddLine(p0, p1, IM_COL32(255, 180, 0, 255), 1.5f);
    draw_list->AddLine(p3, p2, IM_COL32(255, 180, 0, 255), 1.5f);

    // The Bezier curve itself
    draw_list->AddBezierCubic(p0, p1, p2, p3, IM_COL32(0, 200, 255, 255), 3.0f, 64);

    // Endpoint markers
    draw_list->AddCircleFilled(p0, 5.0f, IM_COL32(200, 200, 200, 255));
    draw_list->AddCircleFilled(p3, 5.0f, IM_COL32(200, 200, 200, 255));

    // Draggable control handles
    draw_list->AddCircleFilled(p1, 6.0f, IM_COL32(255, 200, 0, 255));
    draw_list->AddCircleFilled(p2, 6.0f, IM_COL32(255, 200, 0, 255));

    // Invisible hit-test region for dragging
    ImGui::SetCursorScreenPos(canvas_p0);
    ImGui::InvisibleButton("GraphCanvas", canvas_sz);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        float d1 = sqrtf((mousePos.x - p1.x) * (mousePos.x - p1.x) +
                         (mousePos.y - p1.y) * (mousePos.y - p1.y));
        float d2 = sqrtf((mousePos.x - p2.x) * (mousePos.x - p2.x) +
                         (mousePos.y - p2.y) * (mousePos.y - p2.y));

        if (d1 < d2 && d1 < 30.0f) {
            curve.P1 = ToCurve(mousePos);
        } else if (d2 < 30.0f) {
            curve.P2 = ToCurve(mousePos);
        }
    }

    // Live playhead
    float duration = (animEngine.duration > 0.0001f) ? animEngine.duration : 1.0f;
    float normalizedTime = std::clamp(animEngine.currentTime / duration, 0.0f, 1.0f);
    float playheadX = g_min.x + normalizedTime * x_range;
    draw_list->AddLine(ImVec2(playheadX, g_min.y - 10),
                       ImVec2(playheadX, g_max.y + 10),
                       IM_COL32(255, 50, 50, 255), 2.0f);
}

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
        // DX11 backend needs no GL context restore; ImGui manages HWND per-viewport.
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
