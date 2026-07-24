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
#include <vector>
#include <set>
#include <iostream>
#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_dx11.h"

#include "AnimationEngine.h"
#include "LayerManager.h"
#include "Camera.h"
#include "EffectManager.h"
#include "ExportEngine.h"
#include "AlightXmlImporter.h"
#include "CompositionRenderer.h"
#include "UndoStack.h"

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
    void DrawEffectsPalettePanel(); // Task 5: available effects to add
    void DrawEffectControlsPanel(); // Task 5: active effects on selected layer
    void DrawRenderQueuePanel();    // Task 6: FFmpeg export UI
    void PumpExportOneFrameIfActive(); // Task 6: called each frame during export
    void DrawDebugPanel();          // Task 5.0-b: live diagnostic overlay
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
    // Task 5.3-fix-3: slingshot demo used to leave the selected layer's
    // scale permanently shrunk after the toggle turned off. We now cache
    // the pre-demo scale and the layer id it belonged to, so toggling off
    // (or switching layers) restores the original scale cleanly.
    bool  slingshotWasActiveLastFrame = false;
    int   slingshotOriginalLayerId    = -1;
    Vec3  slingshotOriginalScale      = { 1.0f, 1.0f, 1.0f };
    // Task 5.3-fix-3: linked-scale toggle (chain icon next to Scale field).
    // When true, editing scale.x drives scale.y to match, preserving aspect
    // ratio. AE default is unlinked; users click the chain to link.
    bool  linkedScale = false;

    LayerManager   layerManager;
    Camera         camera;
    EffectManager  effectManager;   // Task 5: HLSL shader stack owner
    ExportEngine   exportEngine;    // Task 6: FFmpeg direct-stream exporter
    AlightXmlImporter xmlImporter;  // Task 6: Alight Motion .xml curve parser
    CompositionRenderer compRenderer; // Task 5.0: rasterizes layers to compRTV

    // Task 5.9: DirectWrite text sprite renderer. Owns the DirectWrite
    // factory + shared bitmap-render-target. Called every frame from
    // BeginFrame's pre-render sweep to keep each Text layer's cached atlas
    // up to date (fast-path when cache-key unchanged).
    class TextRenderer* textRenderer = nullptr; // heap-owned; fwd-declared here
    // Cached system-font list, populated once at Initialize. UI-facing
    // std::string labels sorted alphabetically. Rebuilding it hits
    // DirectWrite so we do it once.
    std::vector<std::string> systemFonts;
    // Per-user favorites (starred fonts), persisted to
    // %LOCALAPPDATA%/PotatoMotion/fonts.json. NOT in .pmge.
    std::set<std::string>    favoriteFonts;
    // Deferred write flag: any favorite toggle sets this; SaveFontFavorites
    // fires at EndFrame so a burst of toggles collapses to one file write.
    bool                     favoritesDirty = false;
    // Task 5.9: helpers.
    void LoadFontFavorites();
    void SaveFontFavorites();
    void ToggleFontFavorite(const std::string& family);
    // Ensures each Text layer's rasterized atlas is current. Called once per
    // frame from BeginFrame before compRenderer.RenderLayers runs.
    void RefreshTextLayerCaches();

    // Task 5.0: fixed-size composition render target. Every 2D layer draws
    // into this each frame; the ping-pong effect chain runs against it; the
    // viewport panel displays it via ImGui::Image(); ExportEngine copies from
    // it into its staging texture. Resolution is user-configurable at Comp
    // Settings time (default 1920x1080 per user's Task 4.5 decision).
    ID3D11Texture2D*          compTexture = nullptr;
    ID3D11RenderTargetView*   compRTV     = nullptr;
    ID3D11ShaderResourceView* compSRV     = nullptr;
    UINT compTextureWidth  = 0;
    UINT compTextureHeight = 0;

    // Task 6: Render Queue panel state (settings edited in UI live here)
    ExportEngine::Settings pendingExport;
    float                  pendingExportSeconds = 5.0f;
    // Task 5.10 (user request #7b): auto-populate export duration from
    // max(layer.outPoint) across visible layers when the Render Queue
    // becomes visible. Once the user touches the field manually,
    // exportDurationUserOverridden latches true and we stop clobbering
    // their value on subsequent panel opens.
    bool                   exportDurationUserOverridden = false;
    bool                   renderQueueVisibleLastFrame  = false;
    void                   RecomputeAutoExportDuration();
    bool                   showRenderQueue = false;
    bool                   showFfmpegMissingPopup = false;

    // Task 5.2: save/load state.
    std::string            lastSavePath;                  // "" until first save/load
    std::string            statusMessage;                 // shown at bottom of viewport
    float                  statusMessageExpiresAt = 0.0f; // seconds (comp-clock-independent)
    bool                   statusIsError = false;
    std::string OpenSaveFileDialog(const char* defaultName, bool save);
    void SetStatus(const std::string& msg, bool isError = false, float durationSeconds = 4.0f);

    // Task 5.3 / Task 5.6 revision: undo/redo. See
    // DESIGN_COMMIT3_UNDO_KEYFRAMES_PARENT.md for the original coalescing
    // model. Original impl deferred snapshots to the top of the NEXT frame,
    // which captured post-mutation state for atomic operations (Apply modal,
    // Delete Keyframe, Set to Bezier) so Ctrl+Z became a no-op for them.
    //
    // Task 5.6 fix: MarkForSnapshot pushes SYNCHRONOUSLY the first time
    // it's called in a given frame. A per-frame-number guard preserves the
    // coalescing behavior for continuous drags (all frame-N marks pre-drag
    // collapse into one push). FlushPendingSnapshot is now a no-op kept as
    // a compatibility symbol; callers can be removed in a follow-up.
    UndoStack undoStack;
    uint64_t  currentFrameNumber = 0;    // bumped at top of BeginFrame
    uint64_t  lastSnapshotFrame  = 0;    // frame number of most recent push
    void MarkForSnapshot();
    // Retained for one commit for source compatibility; body does nothing.
    void FlushPendingSnapshot();
    // Build the AppState aggregate that undo/save/load all consume.
    void BuildAppState(struct AppState& out);
    // Apply post-load / post-undo state back onto the RenderEngine members
    // (composition size, camera style, show3D). Handles compRT resize.
    void ApplyLoadedScalars(const struct AppState& st);

    // Task 5.3: keyframe diamond interaction state for the timeline strip.
    // Not a public type; scoped to timeline drawing.
    enum class DiamondProperty : int { Position = 0, Rotation = 1, Scale = 2, Opacity = 3 };
    struct DiamondHit {
        int             layerId  = -1;
        DiamondProperty which    = DiamondProperty::Position;
        int             keyIndex = -1;
        float           origTime = 0.0f;
        bool valid() const { return layerId >= 0 && keyIndex >= 0; }
        void clear() { layerId = -1; keyIndex = -1; }
    };
    DiamondHit  draggedDiamond;
    bool        diamondDragActive = false;

    // Task 5.11: drag-to-reorder state for the timeline label column.
    // Left-click-and-hold on a layer name and drag up/down; when the
    // cursor crosses the midpoint of a neighbor row we swap indices via
    // LayerManager::MoveLayerToIndex. Snapshot fires ONCE on mouse-down.
    int  layerReorderDragId       = -1;   // id of layer being moved; -1 = idle
    bool layerReorderSnapshotDone = false;// guards duplicate MarkForSnapshot per drag
    // Right-click context menu target (persisted across frames because ImGui
    // popups open on the frame AFTER the click).
    DiamondHit  contextDiamond;

    // -------------------------------------------------------------------------
    // Task 5.4-fix: Graph Editor state (AE-accurate).
    //
    // AE draws the Value graph as one curve per scalar dimension (X=red,
    // Y=green, Z=blue) on top of each other for Vec2/Vec3 properties, and
    // the Speed graph as ONE magnitude curve = sqrt(dx^2+dy^2+...)/dt for
    // multi-dim properties. The property picker therefore has fewer entries
    // than my first-pass 5.4: we pick the WHOLE property group, not one
    // scalar channel at a time. Tangent editing still targets one "focus
    // dim" at a time (the selectedChannelDim) because Bezier handles are
    // scalar concepts — but all dims render simultaneously.
    //
    // graphSelectedKey identifies the currently-selected keyframe for handle
    // dragging + context menu. draggedTangent flags which of the two tangents
    // on that key the user is currently dragging.
    // -------------------------------------------------------------------------
    enum class GraphPropGroup : int {
        Position = 0,   // Vec3 — X red, Y green, Z blue
        Rotation,       // Vec3 — z channel used in 2D
        Scale,          // Vec3 — X red, Y green (Z ignored per AE)
        Opacity,        // float — single curve
        COUNT
    };
    enum class GraphMode    : int { Value = 0, Speed = 1 };
    enum class GraphTangent : int { None = 0, In = 1, Out = 2 };

    GraphMode      graphMode              = GraphMode::Value;
    GraphPropGroup graphPropGroup         = GraphPropGroup::Position;
    // Which scalar dim of the group is currently the "focus" for tangent
    // editing: 0=x, 1=y, 2=z (or 0 for float / rotation).
    int            graphFocusDim          = 0;
    bool           graphAutoPicked        = false;   // per-layer auto-pick guard
    int            graphSelectedLayerId   = -1;
    int            graphSelectedKeyIndex  = -1;
    GraphTangent   graphDraggedTangent    = GraphTangent::None;
    int            graphContextKeyIndex   = -1;      // right-click target
    // Task 5.0: last "Test FFmpeg" result shown under the button.
    std::string            ffmpegTestResult;
    bool                   ffmpegTestOk = false;
    // Task 5.0-b: 2D-first UX. 3D controls (Timeline [3D] column, Camera
    // buttons, Camera Properties tab, orbit/pan HUD) stay in the binary but
    // are hidden from the UI until the user opts in via View menu.
    bool                   show3DFeatures = false;
    // Task 5.0-b: dev diagnostic panel toggle. Shows live mouse-canvas coord,
    // selected layer transform values, and drag state so the user can screenshot
    // any weird behavior and I can debug from the numbers instead of guessing.
    bool                   showDebugPanel = false;

    // Task 5.0: viewport-panel geometry, updated each frame in DrawViewportCanvas.
    // Used by gizmo hit-testing to convert screen pixels to composition pixels.
    ImVec2 lastCanvasLetterboxOrigin = ImVec2(0, 0);
    ImVec2 lastCanvasLetterboxSize   = ImVec2(1920, 1080);
    // Cached per-frame effect flags so we can decide whether to run the chain
    // (skipping it entirely is faster than running an empty chain).
    bool   anyLayerHasEffects = false;

    bool CreateCompositionRT(UINT width, UINT height);
    void ReleaseCompositionRT();
    // Convert a viewport-panel screen point to composition-canvas pixels
    // (0..compWidth, 0..compHeight). Returns (0,0) if letterbox is degenerate.
    Vec2 ScreenToCanvas(ImVec2 screen) const;

    // Task 5: composition resolution. Fixed centered canvas uses this in the
    // deferred Task 5.0 usability pass. For now it drives the size of the
    // EffectManager's ping-pong render targets.
    int   compositionWidth  = 1920;
    int   compositionHeight = 1080;

    // Task 5.6: composition framerate + background color. Framerate defaults
    // to 30 fps (matches Adobe / social-media convention). Custom values
    // (25, 29.97 etc.) load and save intact through Serialization.cpp; the
    // Comp Settings modal presets are just shortcuts, not clamps.
    int   compositionFps    = 30;
    float bgColor[4]        = { 0.08f, 0.08f, 0.10f, 1.0f };

    // Task 5.8: Preview scale — how much we downsample the composition RT
    // for perf. Composition dims stay authoritative; the RT is sized at
    // (compositionWidth * previewScale, compositionHeight * previewScale).
    // All coord math (gizmos, ScreenToCanvas, click hit-test) remains in
    // composition-pixel space regardless of this value.
    //
    // Editor state only. NOT persisted in .pmge (matches AE convention:
    // preview quality doesn't travel with the project). Loads default Full.
    // Only three values are legal: 1.0 / 0.5 / 0.25 (Full/Half/Quarter).
    float previewScale = 1.0f;

    // Task 5.8: dead-simple FPS smoothing ring buffer for the toolbar readout.
    // 30-sample rolling average of `1.0f / deltaTime`. Fixed-size, zero heap.
    static constexpr int kFpsRingCap = 30;
    float fpsRing[kFpsRingCap] = {};
    int   fpsRingHead = 0;
    int   fpsRingCount = 0;

    // Task 5.8: helpers — the RT allocation dims are always
    // (compositionWidth * previewScale, compositionHeight * previewScale),
    // clamped to at least 1 pixel so the D3D allocator can't be handed a
    // zero-size texture even if some weird combination pushes it there.
    UINT RtWidth()  const {
        const int w = (int)((float)compositionWidth  * previewScale + 0.5f);
        return (UINT)((w < 1) ? 1 : w);
    }
    UINT RtHeight() const {
        const int h = (int)((float)compositionHeight * previewScale + 0.5f);
        return (UINT)((h < 1) ? 1 : h);
    }

    // Task 5.6: Composition Settings modal state. Edits go to `pending*`
    // fields first so Cancel really cancels. Apply copies pending -> real
    // and (if W/H changed) rebuilds the composition RT and effect ping-pong
    // RTs. `showCompSettingsModal` opens the modal on the next frame.
    bool  showCompSettingsModal = false;
    int   pendingCompW    = 1920;
    int   pendingCompH    = 1080;
    int   pendingCompFps  = 30;
    float pendingCompDur  = 5.0f;
    float pendingBgColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    void  OpenCompSettingsModal();   // seed pending* from live values + open
    void  DrawCompSettingsModal();   // per-frame; no-op if not open

    // Task 5.6: Reset Layout. Sets the flag; next-frame RenderAEDockingLayout
    // detects a null dock node and re-runs the initial AE builder.
    bool  pendingResetLayout = false;

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
