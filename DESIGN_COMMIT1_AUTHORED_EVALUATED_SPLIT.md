# Design Doc — Commit 1: Authored vs Evaluated Transform Split

**Status:** waiting for user approval before touching any code
**Base commit:** `b090d0f`
**Estimated LOC delta:** +80 / -30 (mostly rename + method-signature changes)
**User-visible behavior change:** **none intended.** If anything looks different, that's a bug.

---

## 1. What "Authored vs Evaluated" means in one sentence

**Authored state** = what the user typed into the Inspector fields (their intent).
**Evaluated state** = what the renderer draws this frame, after keyframe interpolation and easing.

Right now these are the same struct (`Layer::transform`), and `Layer::SampleTracks(t)` overwrites the authored values with the evaluated values every frame. Which means:

- The Inspector shows evaluated values while the playhead is moving. The user thinks they typed `position.x = 500` but sees `423.7` because the playhead is halfway between two keys.
- Undo would restore evaluated values (nonsensical), not the authored key values.
- Gizmo drag reads and writes the same field the animation is currently mutating, causing tug-of-war during scrub.
- Save/load would serialize the sampled runtime state, not the underlying keyframes and defaults.

Every downstream feature (undo, save/load, expressions, masks) NEEDS this separation.

---

## 2. The specific data model change

### Before (current, `Layer.h` around line 55-95)

```cpp
struct Transform {
    Vec3  position    = { 0.0f, 0.0f, 0.0f };
    Vec3  rotation    = { 0.0f, 0.0f, 0.0f };
    Vec3  scale       = { 1.0f, 1.0f, 1.0f };
    Vec2  anchorPoint = { 0.5f, 0.5f };
    Vec2  sizePixels  = { 200.0f, 120.0f };
    float opacity     = 1.0f;
};

struct Layer {
    // ... id, name, type, etc. ...
    Transform transform;   // <-- ONE struct, mutated in-place by SampleTracks
    std::optional<PropertyTrack> positionTrack;
    // ...
};
```

### After (proposed)

```cpp
// Renamed for clarity: this is what the artist authored, the "source of truth".
struct AuthoredTransform {
    Vec3  position    = { 0.0f, 0.0f, 0.0f };  // default value (or the ONLY value if no track)
    Vec3  rotation    = { 0.0f, 0.0f, 0.0f };
    Vec3  scale       = { 1.0f, 1.0f, 1.0f };
    Vec2  anchorPoint = { 0.5f, 0.5f };
    Vec2  sizePixels  = { 200.0f, 120.0f };
    float opacity     = 1.0f;
};

// New: what actually gets rendered this frame. Cheap to copy (48 bytes).
// Contains only the same 6 fields but semantically means "post-keyframe evaluation".
struct EvaluatedTransform {
    Vec3  position    = { 0.0f, 0.0f, 0.0f };
    Vec3  rotation    = { 0.0f, 0.0f, 0.0f };
    Vec3  scale       = { 1.0f, 1.0f, 1.0f };
    Vec2  anchorPoint = { 0.5f, 0.5f };
    Vec2  sizePixels  = { 200.0f, 120.0f };
    float opacity     = 1.0f;

    // Same matrix builders as Transform used to have, but now they live on
    // the EVALUATED struct so the renderer never needs to touch authored.
    Mat3 ToLocalMatrix() const;   // 2D
    Mat4 ToLocalMatrix4() const;  // 3D
};

struct Layer {
    // ... id, name, type, etc. (unchanged) ...

    AuthoredTransform  authored;    // what the artist edits (was: transform)
    EvaluatedTransform evaluated;   // what the renderer reads this frame (NEW)

    std::optional<PropertyTrack> positionTrack;  // unchanged
    // ... rest unchanged ...
};
```

### What functions change

| Function | Before | After |
|---|---|---|
| `Layer::SampleTracks(t)` | Mutates `transform` in place | **Pure-ish**: reads `authored` + `tracks`, writes `evaluated`. Never touches `authored`. |
| `Layer::KeyPosition(t)` | `positionTrack->SetKey(t, transform.position)` | `positionTrack->SetKey(t, authored.position)` |
| `Layer::AutoKeyPositionIfEnabled(t)` | reads `transform.position` | reads `authored.position` |
| `Layer::ToggleAnimatePosition(t)` | seeds first key from `transform.position` | seeds first key from `authored.position` |
| `Layer::HasAnyEnabledEffect()` | unchanged | unchanged |
| **Inspector DragFloat3 for Position** | writes `sel->transform.position` | writes `sel->authored.position`, then calls `AutoKeyPositionIfEnabled` |
| **Gizmo drag** | writes `layer.transform.position` | writes `layer.authored.position` (drag = authoring), then calls `AutoKeyPositionIfEnabled` so keyframes are stamped mid-drag if stopwatch is on |
| **`LayerManager::GetWorldMatrix(id)`** | reads `layer.transform` | reads `layer.evaluated` |
| **`LayerManager::GetWorldOpacity(id)`** | reads `layer.transform.opacity` | reads `layer.evaluated.opacity` |
| **`CompositionRenderer::RenderLayers()`** | reads `layer.transform.sizePixels` etc | reads `layer.evaluated` for size/color/matrix; note that `sizePixels` and `anchorPoint` are copied from authored→evaluated at sample-time (they're editable but not keyframable... yet) |
| **`SyncCameraFromLayerIfAny`** | reads `cl->transform.position` | reads `cl->evaluated.position` (camera should follow animated pos, not authored) |
| **`SpawnShapeAtViewportCenter`** | writes `layer->transform.position` | writes `layer->authored.position` (spawn = authoring); `evaluated` gets filled by the next `SampleTracks` in `BeginFrame` |

### The invariant this establishes

```
Every frame:
  1. Inspector / gizmo / any UI edit -> writes to layer.authored only
  2. AutoKey helpers write to tracks based on layer.authored
  3. BeginFrame calls layer.SampleTracks(compTime) which:
        - Starts from layer.authored (default value)
        - Overrides only fields where a track exists AND is enabled
        - Writes the result to layer.evaluated
        - NEVER touches layer.authored
  4. Renderer + gizmo hit-test + matrix build ALL read from layer.evaluated
```

**Result:** Inspector always shows what the user typed. Renderer shows the animated result. Both are correct simultaneously. Undo will restore `authored + tracks`. Save/load will serialize `authored + tracks`.

---

## 3. What deliberately does NOT change in this commit

- `Transform` struct name — I'm renaming to `AuthoredTransform` for semantic clarity, but I'll leave a `using Transform = AuthoredTransform;` alias at the bottom of `Layer.h` for zero-friction migration. Downstream code that says `Transform` keeps compiling.
- `PropertyTrack` internals — untouched.
- Any effect / camera / null / export code beyond the transform touchpoints.
- ImGui panels' visual layout — same panels, same widgets, same tabs.
- No performance changes (extra 48 bytes per layer for `evaluated`; negligible).

---

## 4. Edge cases I'll handle explicitly

1. **Gizmo dragging while stopwatch is ON:** each mouse-move frame calls `AutoKeyPositionIfEnabled(currentTime)` which stamps a keyframe at every intermediate position. That's actually correct behavior AE-wise for keyframe-per-frame recording, but it might spam the timeline with 60 keys/sec. **Fix:** only auto-key on mouse-**up** (end of drag), not mid-drag. This is a minor UX-cleanup that lands naturally with this refactor.
2. **First frame of a fresh layer:** `evaluated` is default-constructed. Before the first `SampleTracks` runs, gizmo/renderer would read defaults. **Fix:** call `SampleTracks` once at the end of `SpawnShapeAtViewportCenter` so `evaluated` is populated immediately.
3. **`transform.sizePixels` and `transform.anchorPoint`** — these are on `AuthoredTransform` only right now (never animated). `SampleTracks` needs to COPY them to `evaluated.sizePixels` / `evaluated.anchorPoint` even though there's no track. Otherwise renderer sees stale defaults.
4. **Camera layer sync:** camera reads `evaluated.position`, but `SyncCameraFromLayerIfAny` runs BEFORE `SampleTracks` currently (BeginFrame order). **Fix:** move `SyncCameraFromLayerIfAny` to AFTER the `SampleTracks` loop in `BeginFrame`.

---

## 5. Files that will change

```
src/Layer.h              MODIFIED  (~40 lines rewritten around Transform struct + helpers)
src/LayerManager.h       MODIFIED  (~2 lines: signatures of GetWorldMatrix/Opacity — actually no, they take layer id not layer&, so signatures stay identical. Only internal reads change.)
src/LayerManager.cpp     MODIFIED  (~5 lines: internal reads of layer.transform -> layer.evaluated for matrix build, layer.authored for AddLayer default)
src/RenderEngine.cpp     MODIFIED  (~25 lines: split Inspector edits into authored writes + auto-key, split gizmo drag same way, reorder BeginFrame to Sample before SyncCamera, spawn helper writes authored)
src/CompositionRenderer.cpp MODIFIED  (~5 lines: read layer.evaluated instead of layer.transform for size and color)
```

**No new files in this commit.** The `EvaluatedTransform` struct lives inside `Layer.h` alongside `AuthoredTransform`.

---

## 6. Test plan (I'll run these in my head before committing)

Every test from `TEST_CHECKLIST.md` should pass IDENTICALLY to the current build. If ANY visible behavior changes, it's a bug in this commit.

Additionally, one new invariant to verify manually after the commit lands:
- Set position (960, 540), click stopwatch, scrub to t=1s, set position (1400, 540), scrub playhead to t=0.5s → Inspector Position field should read **either 960 (start-key value) or 1400 (end-key value) depending on which is closer**, but NOT the interpolated 1180. Because the Inspector shows authored, not evaluated. **Wait** — that's actually a problem. The user WANTS to see the current interpolated value in the Inspector during scrub, so they can drag it (which stamps a mid-keyframe).

Let me reconsider. **The correct AE behavior is:**
- When a property has NO track: Inspector shows authored (the only value).
- When a property HAS a track: Inspector shows **evaluated** (the animated value at current time), and typing/dragging a new value auto-keys at current time.

So the Inspector reads from `evaluated` (for display), but writes to `authored` when a change happens... no wait, that's also wrong, because if it writes to `authored` but the track evaluation dominates, the write does nothing visible.

**The actually correct model:**
- Inspector display: `evaluated` (what user sees on canvas = what they see in field)
- Inspector edit: if property has a track → SetKey(t, newValue) directly + also update evaluated for this frame; if no track → update authored + evaluated
- Undo/save: serialize `authored` (default value for the property when the track doesn't override) + `tracks` (the keyframes)

This is subtler than a simple "authored vs evaluated" split. Let me update the plan:

### Revised model

```
AuthoredTransform  = default values for properties that don't have a track
                     + serves as fallback when a track is disabled
                     + this is what save/load serializes
                     + this is what undo restores

EvaluatedTransform = the sampled result each frame
                     + built by SampleTracks() from (authored + tracks + compTime)
                     + this is what renderer + gizmo + Inspector-display read
                     + this is what CompositionRenderer sees

Inspector edits:
  If track exists for this property AND enabled:
      track->SetKey(currentTime, newValue)
      (SampleTracks next frame will produce newValue at this time)
  Else:
      authored.<prop> = newValue
      (SampleTracks will copy authored to evaluated)

Gizmo drag same rules; mouse-up commits.
```

**Under this model, the visible Inspector value always matches the canvas — solving the "field says 423.7 while I typed 500" confusion.** And save/undo still work correctly because they only touch `authored + tracks`.

---

## 7. If you approve this design, here's what I'll do

1. Write the code (targeted edits to the 5 files listed in §5)
2. Run local syntax check with stubs
3. Commit as `Task 5.1: Authored vs Evaluated transform split (Phase 1, Commit 1)`
4. Push, wait for CI
5. Ping you with build link + a mini test-list of the invariants above to verify
6. **Then wait for your approval before Commit 2 (Serialization).**

---

## 8. Questions before I proceed

**Q1.** Do you want me to preserve the `Transform` name as an alias, or fully rename to `AuthoredTransform` everywhere?
  - Alias (my recommendation): faster ship, less noise in diff, gradual migration
  - Full rename: cleaner grep-ability, semantic clarity, ~10 more diff lines

**Q2.** For the "gizmo drag stamps keyframe on mouse-up" behavior — is that OK or do you prefer keyframe-per-frame during drag (spammy but faithful to some AE motion-tracking workflows)?
  - Mouse-up (my recommendation): matches most artists' expectations; still records the final position
  - Per-frame: records the exact drag path (useful for hand-drawn animation feel)

**Q3.** The `evaluated` field adds 48 bytes per layer. On the "4 GB RAM potato PC" ceiling, this is ~5 KB for a 100-layer scene. Trivial. But is there any objection to the per-layer size growing?

If you're OK with the design as written, and answer Q1-Q3, I'll write the code.
