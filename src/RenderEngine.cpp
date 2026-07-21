#include "RenderEngine.h"

RenderEngine::RenderEngine() {}

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
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowWidth, windowHeight, window_flags);
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
    const D3D_FEATURE_LEVEL featureLevelArray[3] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_9_3,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 3,
        D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context
    );

    if (FAILED(hr)) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr) || !pBackBuffer) return false;

    hr = device->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
    pBackBuffer->Release();

    return !FAILED(hr);
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

    return true;
}

void RenderEngine::HandleEvents(bool& running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) running = false;
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
            running = false;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
            if (swapChain && mainRenderTargetView) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                mainRenderTargetView->Release();
                mainRenderTargetView = nullptr;

                swapChain->ResizeBuffers(0, (UINT)event.window.data1, (UINT)event.window.data2, DXGI_FORMAT_UNKNOWN, 0);

                ID3D11Texture2D* pBackBuffer = nullptr;
                swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
                if (pBackBuffer) {
                    device->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
                    pBackBuffer->Release();
                }
            }
        }
    }
}

void RenderEngine::BeginFrame() {
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
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Edit")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Composition")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Layer")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Effect")) { ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Export")) { ImGui::EndMenu(); }
        ImGui::EndMenuBar();
    }
    ImGui::End();

    // Panel 1: Project Bin
    ImGui::Begin("Project Assets");
    ImGui::Text("Media Library & Assets");
    ImGui::End();

    // Panel 2: Viewport
    ImGui::Begin("Composition Viewport");
    ImGui::Text("Render Viewport Canvas");
    ImGui::End();

    // Panel 3: Inspector
    ImGui::Begin("Inspector & Effects");
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float pos[3] = { 0.0f, 0.0f, 0.0f };
        static float scale[3] = { 100.0f, 100.0f, 100.0f };
        ImGui::DragFloat3("Position", pos);
        ImGui::DragFloat3("Scale (%)", scale);
    }
    ImGui::End();

    // Panel 4: Timeline
    ImGui::Begin("Timeline");
    ImGui::Text("Layer Stack");
    ImGui::End();

    // Panel 5: Graph Editor
    ImGui::Begin("Graph Editor");
    ImGui::Text("Bézier Curves");
    ImGui::End();
}

void RenderEngine::EndFrame() {
    ImGui::Render();

    const float clear_color[4] = { 0.12f, 0.12f, 0.14f, 1.00f };
    context->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
    context->ClearRenderTargetView(mainRenderTargetView, clear_color);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_window, backup_context);
    }

    swapChain->Present(1, 0);
}

void RenderEngine::Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (mainRenderTargetView) { mainRenderTargetView->Release(); mainRenderTargetView = nullptr; }
    if (swapChain) { swapChain->Release(); swapChain = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }

    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    SDL_Quit();
}
