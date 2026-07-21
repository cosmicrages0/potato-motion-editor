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
#include "Camera.h"

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
    void DrawTimelineStrip();       // Task 4.5: time ruler + playhead + kf diamonds
    void DrawInspectorPanel();
    void DrawProjectAssetsPanel();
    void DrawLayerShape(const Layer& layer, const Mat3& worldMatrix,
                        float worldOpacity, ImVec2 canvasOrigin,
                        ImDrawList* drawList);
    void DrawLayerShape3D(const Layer& layer, const Mat4& worldMatrix4,
                          float worldOpacity, ImVec2 canvasOrigin,
                          ImVec2 canvasSize, ImDrawList* drawList);
    void DrawSelectionGizmos(Layer& layer, const Mat3& worldMatrix,
                             ImVec2 canvasOrigin, ImDrawList* drawList);
    void DrawCameraGizmos(const Layer& cameraLayer, ImVec2 canvasOrigin,
                          ImVec2 canvasSize, ImDrawList* drawList);
    void HandleGizmoInteraction(Layer& layer, const Mat3& worldMatrix,
                                ImVec2 canvasOrigin, ImVec2 canvasSize);
    void HandleCameraControls(ImVec2 canvasOrigin, ImVec2 canvasSize);
    void SyncCameraFromLayerIfAny();
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
    Camera       camera;

    // Task 4.5: composition-wide setting for how the camera relates to layers.
    // AE mode = camera is a normal layer, parented freely (default).
    // Alight mode = layers with stickToCamera=true render as HUD attached to camera.
    enum class CameraStyle : int { AfterEffects = 0, AlightMotion = 1 };
    CameraStyle cameraStyle = CameraStyle::AfterEffects;

    // Task 4.5: last viewport panel geometry (updated every frame) so that
    // "New Rectangle / Ellipse / etc." buttons can spawn the layer at the
    // center of what the user is actually looking at, not the world origin.
    Vec2 lastViewportCenterWorld = { 640.0f, 360.0f };
    ImVec2 lastViewportSize      = ImVec2(1280.0f, 720.0f);

    // Helper used by all "add layer" callsites so spawn positioning stays
    // consistent whether the user clicks a menu, a button, or a shortcut.
    void SpawnShapeAtViewportCenter(ShapeType type, const char* nameHint = nullptr);

    // Camera interaction state (orbit/pan/zoom drag tracking)
    bool  cameraDragActive     = false;
    int   cameraDragButton     = -1;   // 0=LMB, 1=RMB, 2=MMB (SDL numbering via ImGui)
    bool  cameraDragIsPan      = false;
    ImVec2 cameraDragLastMouse = ImVec2(0, 0);

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
