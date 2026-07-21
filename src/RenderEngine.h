#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <string>
#include <iostream>
#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_dx11.h"

#include "AnimationEngine.h"
#include "LayerManager.h"

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
    void DrawViewportCanvas();
    void DrawGraphEditor();
    void DrawTimelinePanel();
    void DrawInspectorPanel();
    void DrawProjectAssetsPanel();
    void DrawLayerShape(const Layer& layer, const Mat3& worldMatrix,
                        float worldOpacity, ImVec2 canvasOrigin,
                        ImDrawList* drawList);
    void DrawSelectionGizmos(Layer& layer, const Mat3& worldMatrix,
                             ImVec2 canvasOrigin, ImDrawList* drawList);
    void HandleGizmoInteraction(Layer& layer, const Mat3& worldMatrix,
                                ImVec2 canvasOrigin, ImVec2 canvasSize);
    void CreateRenderTarget();
    void CleanupRenderTarget();

    SDL_Window* window = nullptr;
    HWND hwnd = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView = nullptr;

    int windowWidth  = 1600;
    int windowHeight = 900;

    bool imguiInitialized = false;
    bool shutdownCalled   = false;

    // Global animation clock. In Task 3 it drives a demonstrator "slingshot
    // scale" applied to the currently selected layer; Task 5 will replace this
    // single shared engine with per-property keyframe tracks.
    AnimationEngine animEngine;
    bool applySlingshotToSelected = false;

    LayerManager layerManager;

    // Gizmo interaction state — sticky across frames while dragging.
    enum class GizmoMode { None, Move, ScaleNW, ScaleNE, ScaleSW, ScaleSE };
    GizmoMode activeGizmo = GizmoMode::None;
    Vec2 dragStartMouseLocal   = { 0.0f, 0.0f };
    Vec2 dragStartPosition     = { 0.0f, 0.0f };
    Vec3 dragStartScale        = { 1.0f, 1.0f, 1.0f };
    Vec2 dragStartSize         = { 200.0f, 120.0f };
    int  dragLayerId           = -1;

    Uint64 lastTime = 0;
};
