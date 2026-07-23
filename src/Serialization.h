#pragma once
#include <string>

// -----------------------------------------------------------------------------
// Task 5.2: Save/Load for .pmge project files.
//
// Design decision (per Gemini + Claude tiebreaker): nlohmann/json is included
// ONLY in Serialization.cpp; nothing else in the codebase sees it. That keeps
// template instantiation bloat isolated to a single translation unit — the
// binary size impact under MSVC LTCG stays ~120 KB.
//
// The public API is deliberately minimal: two free functions and an aggregate
// AppState struct of non-owning pointers. RenderEngine builds an AppState from
// its members, calls SaveProject/LoadProject, and consumes the result.
// -----------------------------------------------------------------------------

class LayerManager;
class Camera;
class AnimationEngine;

// Aggregate of everything that gets persisted to a .pmge file.
// All pointers are NON-OWNING; caller retains ownership of the underlying
// objects. On LoadProject, the pointed-to objects are mutated in place.
struct AppState {
    LayerManager*    layerManager      = nullptr;
    Camera*          camera            = nullptr;
    AnimationEngine* animEngine        = nullptr;
    int              compositionWidth  = 1920;
    int              compositionHeight = 1080;
    int              cameraStyleInt    = 0;   // 0 = AfterEffects, 1 = AlightMotion
    bool             show3DFeatures    = false;
    // Task 5.6: composition-wide framerate + background color. Missing from
    // pre-5.6 .pmge files; defaults keep old files loading identically.
    int              compositionFps    = 30;
    float            bgColor[4]        = { 0.08f, 0.08f, 0.10f, 1.0f };
};

// Current on-disk schema version. Bump when the JSON layout changes
// incompatibly; readers will attempt a best-effort load with a warning
// when the file's version differs.
constexpr int kPmgeFormatVersion = 1;

// Returns true on success. On failure, *outError (if provided) is populated
// with a human-readable reason. Never throws.
bool SaveProject(const AppState& state,
                 const std::string& path,
                 std::string* outError = nullptr);

bool LoadProject(AppState& state,
                 const std::string& path,
                 std::string* outError = nullptr);

// Task 5.3: string-based (de)serialization. Same JSON layout as the file
// versions, but the caller supplies/receives the JSON as a std::string. Used
// by UndoStack for in-memory snapshots so we don't touch the disk on Ctrl+Z.
bool SaveProjectToString(const AppState& state,
                         std::string& outJson,
                         std::string* outError = nullptr);

bool LoadProjectFromString(AppState& state,
                           const std::string& inJson,
                           std::string* outError = nullptr);
