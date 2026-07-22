# Design Doc — Commit 2: Save/Load (`.pmge` JSON)

**Base commit:** `a1f863b` (Task 5.1: AnimatedProperty<T>)
**Goal:** close the app, reopen it, project is still there. Deferred bug from Section 9.5 of PROJECT_BRIEFING.md.
**LOC delta estimate:** +350 / -0 (all new). No existing code deleted or restructured.
**User-visible change:** three new menu items work — `File → Save`, `File → Save As...`, `File → Open...`. Plus `Ctrl+S` / `Ctrl+Shift+S` / `Ctrl+O` shortcuts.

---

## 1. What "close the app, reopen, project is still there" means

Right now, closing the app loses every layer, every keyframe, every camera setting. That's the single worst failure mode in the current build and Gemini/Claude/ChatGPT all identified save/load as P0.

Success criterion for this commit:
1. Set up a scene (add 3 shapes, parent one to another, animate a position via stopwatch + 2 keyframes, add an effect, tweak the camera)
2. `Ctrl+S`, pick a path like `myscene.pmge`, close the app
3. Relaunch, `Ctrl+O`, open `myscene.pmge`
4. **Every layer, keyframe, effect, and camera setting is exactly as before.** Play the animation — it plays identically.

---

## 2. Format: `.pmge` — human-readable JSON, Lottie-shaped where it matters

The file is UTF-8 JSON. Human-readable so you can `cat` it during debugging or diff two versions in git.

### Top-level shape

```json
{
  "pmge_version": 1,
  "composition": {
    "width": 1920,
    "height": 1080,
    "duration": 1.86,
    "isPlaying": false,
    "isLooping": true,
    "cameraStyle": "AfterEffects",
    "show3DFeatures": false
  },
  "camera": {
    "position": [0, 0, -1000],
    "target":   [0, 0, 0],
    "rotation": [0, 0, 0],
    "up":       [0, 1, 0],
    "fov": 45.0,
    "nearZ": 1.0,
    "farZ": 10000.0,
    "zoom": 1000.0,
    "useTargetMode": true
  },
  "slingshotCurve": {
    "p0": [0.0, 0.0],
    "p1": [0.25, 1.25],
    "p2": [0.50, 0.90],
    "p3": [1.0, 1.0]
  },
  "layers": [
    {
      "id": 1,
      "parentId": -1,
      "name": "Background Rect",
      "type": "Rectangle",
      "isVisible": true, "is3D": false, "isSolo": false, "isLocked": false,
      "stickToCamera": false,
      "fillColor": "0xFF3A3A55",
      "transform": {
        "position":    { "sw": false, "static": [960, 540, 0], "keys": [] },
        "rotation":    { "sw": false, "static": [0, 0, 0],     "keys": [] },
        "scale":       { "sw": false, "static": [1, 1, 1],     "keys": [] },
        "anchorPoint": { "sw": false, "static": [0.5, 0.5],    "keys": [] },
        "sizePixels":  { "sw": false, "static": [800, 480],    "keys": [] },
        "opacity":     { "sw": false, "static": 1.0,           "keys": [] }
      },
      "effects": []
    },
    {
      "id": 2,
      "name": "Bouncing Ball",
      "type": "Ellipse",
      "transform": {
        "position": {
          "sw": true,
          "static": [960, 540, 0],
          "keys": [
            { "t": 0.0, "v": [960, 540, 0] },
            { "t": 1.0, "v": [1400, 540, 0] }
          ]
        }
        // ... other properties (compact form when no keys)
      },
      "effects": [
        {
          "id": 1,
          "type": "ChromaticAberration",
          "enabled": true,
          "displayName": "Chromatic Aberration",
          "p0": [4.0, 0.0, 1.0, 0.0],
          "p1": [0, 0, 0, 0],
          "p2": [0, 0, 0, 0],
          "p3": [0, 0, 0, 0]
        }
      ]
    }
  ]
}
```

**Key design choices for the format:**

- **`"sw"` short for stopwatch enabled** — keeps the JSON compact when hundreds of properties are all static (most projects). If `sw == false` and `keys == []`, only `static` matters.
- **AnimatedProperty represented uniformly regardless of T** — for `Vec3`, `static` and `v` are JSON arrays of 3 floats; for `Vec2`, arrays of 2; for `float`, plain numbers. The C++ writer picks the right shape per template instantiation.
- **`"pmge_version": 1`** at the top so future schema evolutions can detect old files and migrate.
- **`ShapeType` and `EffectType` serialized as strings** — not the raw enum ints — so an enum reorder can never silently corrupt data.
- **`fillColor` as a hex string** — obvious what it means, survives roundtrip.
- **Effect params serialized as 4 flat arrays of 4 floats** — mirrors the `EffectParams` struct exactly. Wasteful when many params are 0, but survives every future effect type without schema changes.

**What's deliberately NOT in the file yet:**
- Effect stack keyframe animation (Phase 5+; effects params aren't AnimatedProperty yet)
- Sub-compositions (they don't exist yet)
- Media asset references (Task 6 didn't finish)
- Undo history (undo is Commit 3; each undo state is a snapshot of this same JSON)

---

## 3. Library choice — nlohmann/json, single translation unit

**Decision confirmed by Claude + Gemini:**
- Use `nlohmann/json` (single-header) for parsing.
- Isolate the `#include <nlohmann/json.hpp>` **strictly inside `src/Serialization.cpp`**. Nowhere else in the codebase sees the header.
- With `/GL /LTCG` (MSVC's LTO), the template instantiation bloat stays under ~120 KB.

**How we get the header:** vendor it via CMake FetchContent, same pattern as SDL2 and ImGui. The build already pulls remote dependencies at configure time; one more is fine.

**CMakeLists.txt addition** (only change to build system):
```cmake
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

target_link_libraries(${PROJECT_NAME} PRIVATE
    # ... existing libs ...
    nlohmann_json::nlohmann_json
)
```

That link is header-only (`INTERFACE` target), so it only adds an include path — no linker cost.

---

## 4. Module layout

Two new files:
```
src/Serialization.h    ~40 lines — public API + version constant
src/Serialization.cpp  ~450 lines — nlohmann/json included ONLY here
```

### `Serialization.h` — the tiny public API

```cpp
#pragma once
#include <string>

class LayerManager;
class Camera;
class AnimationEngine;
struct AppState;  // aggregate; see below

// AppState = the whole savable universe. Populated by RenderEngine before
// SaveProject; consumed by RenderEngine after LoadProject.
struct AppState {
    LayerManager*   layerManager   = nullptr;
    Camera*         camera         = nullptr;
    AnimationEngine* animEngine    = nullptr;
    int             compositionWidth  = 1920;
    int             compositionHeight = 1080;
    int             cameraStyleInt = 0;   // 0 = AE, 1 = Alight
    bool            show3DFeatures = false;
};

// Returns true on success; on failure sets *outError to a human message.
bool SaveProject(const AppState& state, const std::string& path, std::string* outError = nullptr);
bool LoadProject(AppState& state,       const std::string& path, std::string* outError = nullptr);
```

That's the ENTIRE surface. No nlohmann types leak into any other file.

### `Serialization.cpp` — where nlohmann lives

```cpp
#include "Serialization.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
// ... internal helpers: writeVec2/writeVec3, writeAnimatedProperty<T>,
//     writeLayer, writeEffect, writeCamera, writeTransform ...
// ... symmetric readers ...
// SaveProject: builds json, writes to file with pretty-print (2-space indent).
// LoadProject: parses file, calls readers, populates state. Best-effort:
//              unknown fields ignored, missing fields default. Version
//              mismatch produces a warning but attempts to load anyway.
```

**Key implementation notes:**
- `writeAnimatedProperty<T>` is a template that dispatches to `writeVec2/Vec3/float` based on T. Same for the reader.
- `LayerManager` needs a small `Clear()` method for LoadProject to wipe existing state before deserializing. It doesn't exist yet — I'll add it.
- Deserialization rebuilds each Layer's `id` from the file (preserving parenting) and updates `LayerManager::nextId` to `max(existingIds) + 1` so future AddLayer calls don't collide.
- All numeric reads use nlohmann's `.value(key, default)` so missing fields don't throw — every field has a defined fallback.
- File writes go via `std::ofstream` in binary mode with `std::setw(2)` for indentation.

---

## 5. UI wiring

### `File` menu (RenderEngine.cpp)

```
File
├── New Composition         (existing stub — Commit 4)
├── Open...        Ctrl+O   ← NEW: file dialog, then LoadProject
├── Save           Ctrl+S   ← NEW: if lastSavePath, save; else Save As
├── Save As...     Ctrl+Shift+S ← NEW: file dialog, then SaveProject
├── ─────
└── Import Alight Motion .xml (existing)
```

`RenderEngine` gains one new field: `std::string lastSavePath;` — remembers where you last saved so `Ctrl+S` doesn't re-prompt.

### File dialog approach

**Pragmatic choice:** use the Win32 `GetOpenFileName` / `GetSaveFileName` COM-free classic API. It's part of `<commdlg.h>`, already available with `<windows.h>`, no extra dependency, feels native. Add `Comdlg32.lib` to `target_link_libraries` (one word).

Wrapped in a `RenderEngine::OpenFileDialog(const char* filter, bool save)` helper returning `std::string` (empty on cancel).

### Status feedback

Save success → title bar shows `Potato Motion Graphics Editor - x64 — myscene.pmge`. Save failure → red text banner at the bottom of the viewport panel for 3 seconds ("Save failed: [reason]"). Load success → same title bar update. Load failure → same red banner.

---

## 6. Edge cases

| Case | Behavior |
|---|---|
| Open a `.pmge` while an export is running | Cancel export first; then load |
| Open a `.pmge` with unknown effect type | Skip that effect, log to console, keep loading |
| Open a `.pmge` from a future version (`pmge_version > 1`) | Load anyway with best effort, banner warns |
| Open a corrupt JSON | Show error banner, existing state preserved |
| Save while no shape selected | Fine; save-what-exists |
| Save to a read-only path | Error banner |
| Load a file that references a Camera layer while `show3DFeatures = false` | Load fine, keep the flag from the file |
| Very large project (~1000 layers) | Should be under 100ms save+load on any potato PC — nlohmann is fast |
| Filename with Unicode chars | `<windows.h>` file APIs are ANSI by default; use narrow strings for now, upgrade to wchar_t in a later commit if this bites anyone |

---

## 7. Test plan

Manual test I'll run through my head before committing:

1. Launch app, add 2 shapes, animate one's position with 2 keyframes, tweak camera FOV. Ctrl+S → `test.pmge` on Desktop. Verify file exists, is valid JSON (pretty-printed).
2. Close app entirely.
3. Relaunch. Ctrl+O → `test.pmge`. Verify:
   - Both shapes are present at correct positions
   - The animated shape's stopwatch is lit, `keys=2`, playhead scrub shows interpolation
   - Camera FOV matches
4. Save the just-loaded state again → diff old vs new file. Should be byte-identical (round-trip stability).
5. Edit `test.pmge` in a text editor, delete a keyframe from the JSON, save, reload. Verify the layer now has one fewer keyframe.

Every test in `TEST_CHECKLIST.md` should pass identically (no regressions).

---

## 8. What deliberately does NOT change in this commit

- Undo/redo (Commit 3)
- Keyframe diamond drag/delete UX (Commit 3 or 4)
- Composition Settings modal (later commit)
- Media asset paths (Task 6 hasn't shipped media support anyway)
- Any behavior in the viewport, gizmos, effects, or camera controls

---

## 9. Files changing

```
CMakeLists.txt              MODIFIED  +7 lines (FetchContent nlohmann + link)
src/Serialization.h         NEW       ~40 lines
src/Serialization.cpp       NEW       ~450 lines
src/LayerManager.h          MODIFIED  +2 lines (Clear() declaration)
src/LayerManager.cpp        MODIFIED  +8 lines (Clear() implementation)
src/RenderEngine.h          MODIFIED  +6 lines (lastSavePath, OpenFileDialog decl, notification state)
src/RenderEngine.cpp        MODIFIED  ~+80 lines (File menu items, Ctrl shortcuts, OpenFileDialog impl, banner display)
```

Net binary size expected impact: **+80 to +150 KB** with LTCG. Well within our <5 MB constraint (currently 1.06 MB → 1.15-1.20 MB estimated).

---

## 10. What I'll do if approved

1. Update `CMakeLists.txt` with the FetchContent block for nlohmann + link
2. Add `Comdlg32` to linked libraries for Win32 file dialogs
3. Add `LayerManager::Clear()` (delete all layers, reset nextId/selection)
4. Write `Serialization.h` (public API)
5. Write `Serialization.cpp` (nlohmann included ONLY here; write + read for AppState, Camera, LayerManager, Layer, Transform, AnimatedProperty<T>, Effect, BezierCurve)
6. Add `File → Open... / Save / Save As...` menu items to RenderEngine
7. Add keyboard shortcuts (Ctrl+S, Ctrl+Shift+S, Ctrl+O) via ImGui's `ImGuiKey_S` etc + `io.KeyCtrl / KeyShift`
8. Add title bar update after successful save/load
9. Add red-banner error display
10. Local compile check
11. Commit as `Task 5.2: Save/Load .pmge JSON (nlohmann isolated in Serialization.cpp)`
12. Push, wait for CI
13. Report back with build link + test steps

---

## 11. One question before I go

**Q1.** For the file dialog: Win32 `GetOpenFileName` / `GetSaveFileName` (native, zero dependency, ~40 lines of wrapping code) OR ImGui-only text input where the user types the path (portable but ugly)?

**My recommendation: Win32 native.** We're already Windows-only per project constraint, and a native file picker is what users expect. It's ~40 lines of `OPENFILENAMEA` boilerplate wrapped in `RenderEngine::OpenFileDialog()`. No new dependency (comdlg32 is a system DLL).

If you agree with Win32, say "approved go" or "single commit go" and I execute. If you want to see the JSON schema evolve differently, or want ImGui text input instead of native dialog, tell me now.
