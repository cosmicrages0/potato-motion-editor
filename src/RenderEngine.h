#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <string>
#include <iostream>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_dx11.h"

class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

    bool Initialize(const char* title, int width, int height);
    void HandleEvents(bool& running);
    void BeginFrame();
    void RenderUI();
    void EndFrame();
    void Shutdown();

private:
    bool InitDirectX();
    bool InitImGui();
    void RenderAEDockingLayout();

    SDL_Window* window = nullptr;
    HWND hwnd = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView = nullptr;

    int windowWidth = 1280;
    int windowHeight = 720;
};
