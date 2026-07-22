# Design Doc REVISED — Commit 1: `AnimatedProperty<T>` (the AE / Lottie / Pikimov pattern)

**Supersedes:** `DESIGN_COMMIT1_AUTHORED_EVALUATED_SPLIT.md` (my previous over-engineered two-struct proposal)
**Base commit:** `8ebff44`
**Estimated LOC delta:** +250 / -100 (bigger than the previous plan, but collapses Phase 2's separate `AnimatedProperty<T>` step into this same commit, so total work goes down)
**User-visible behavior change:** **NONE intended for defaults.** Enable a stopwatch, animate a property — should feel identical to today. But `AnimatedProperty<T>.Evaluate()` is now the ONE place any value is read from, which quietly fixes ~5 subtle bugs.

---

## 1. Why the revision

After I wrote the first design doc, the user asked me to research how AE actually works and to look at Pikimov (a browser-based AE-alike). I found:

- **Adobe's own community docs**: *"AE evaluates property streams: **base value → keyframe value → expression result**"* — every animatable field is a stream with 3 override layers, not a struct with fields.
- **Adobe's scripting API for `Property.value`**: *"If expressionEnabled is true, returns the evaluated expression value. If there are keyframes, returns the keyframed value at the current time. Otherwise, returns the static value."* — the property IS the resolver.
- **Lottie's JSON schema** (which IS AE's export format): each property has `"a": 0|1` (animated flag) + `"k"` (either a static value or an array of keyframes). One field, self-describing.
- **Pikimov** (the tool the user referenced) uses the same layer-based composition + per-property keyframe workflow because it explicitly targets AE-familiar users.

**The pattern in one line: every animatable field of every layer is not a `T`, it's an `AnimatedProperty<T>` that knows how to evaluate itself at time t.**

My original two-struct authored/evaluated split was solving the same underlying problem (Inspector-vs-canvas value mismatch, undo-restores-wrong-thing, gizmo-drag-fights-animation) — but by copying the ENTIRE transform each frame. The AE way solves it at the field level with a smaller memory footprint and better semantics.

---

## 2. The core type

```cpp
// Keyframe carries just (time, value) for Commit 1. Tangents/easing are Phase 2's
// per-keyframe Bezier work. Interpolation between keys is linear until then.
template <typename T>
struct Keyframe {
    float time  = 0.0f;   // seconds in composition time
    T     value = T{};    // value at that time
};

// AnimatedProperty<T> — the AE Property object.
//
// staticValue:      the "base value" (AE terminology). Used when the stopwatch
//                   is off OR when the stopwatch is on but there are no keys.
// keyframes:        sparse list. When stopwatchEnabled && !empty, Evaluate()
//                   interpolates between adjacent keys.
// stopwatchEnabled: matches AE's stopwatch icon. When true, SetValue() creates
//                   or updates a keyframe at compTime. When false, SetValue()
//                   writes staticValue.
//
// This mirrors Lottie's schema (a=0 -> k is a scalar; a=1 -> k is a keyframe array).
template <typename T>
struct AnimatedProperty {
    T                       staticValue{};
    std::vector<Keyframe<T>> keyframes;
    bool                    stopwatchEnabled = false;

    // Evaluate at composition time t. This is the ONE read path for the value.
    // AE evaluation order: base -> keys -> expression. Expressions are Phase 3.
    T Evaluate(float t) const {
        if (!stopwatchEnabled || keyframes.empty()) return staticValue;
        if (keyframes.size() == 1)         return keyframes.front().value;
        if (t <= keyframes.front().time)   return keyframes.front().value;
        if (t >= keyframes.back().time)    return keyframes.back().value;
        for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
            const auto& a = keyframes[i];
            const auto& b = keyframes[i + 1];
            if (t >= a.time && t <= b.time) {
                const float span = b.time - a.time;
                const float u    = (span > 1e-6f) ? (t - a.time) / span : 0.0f;
                return Lerp(a.value, b.value, u);   // free function per T
            }
        }
        return keyframes.back().value;
    }

    // Write path. If stopwatch is on, insert or replace a keyframe at t.
    // If stopwatch is off, just update the static value.
    // This is the ONE write path — Inspector/gizmo/drag all go through here.
    void SetValue(float t, const T& v) {
        if (!stopwatchEnabled) { staticValue = v; return; }
        constexpr float kEps = 1e-3f;
        for (auto& k : keyframes) {
            if (std::fabs(k.time - t) < kEps) { k.value = v; return; }
        }
        keyframes.push_back({ t, v });
        std::sort(keyframes.begin(), keyframes.end(),
                  [](const Keyframe<T>& a, const Keyframe<T>& b){ return a.time < b.time; });
    }

    // Stopwatch toggle. Turning ON drops a first key at t seeded from staticValue.
    // Turning OFF wipes all keys AND leaves staticValue as-is (last-edit-wins).
    void ToggleStopwatch(float t) {
        if (stopwatchEnabled) { stopwatchEnabled = false; keyframes.clear(); }
        else { stopwatchEnabled = true; SetValue(t, staticValue); }
    }

    bool IsAnimated() const { return stopwatchEnabled && !keyframes.empty(); }
    bool HasStopwatch() const { return stopwatchEnabled; }
};

// Free-function Lerp per component type. Vec2/Vec3/float provided; expanding
// to Color / Bezier handles in later commits is cheap.
inline float Lerp(float a, float b, float u)       { return a + (b - a) * u; }
inline Vec2  Lerp(const Vec2& a, const Vec2& b, float u);
inline Vec3  Lerp(const Vec3& a, const Vec3& b, float u);
```

That's it. **~60 lines** covers the entire animation-of-any-property capability.

---

## 3. What `Transform` becomes

### Before (current, `Layer.h`)

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
    Transform transform;
    std::optional<PropertyTrack> positionTrack;   // parallel-array style
    std::optional<PropertyTrack> scaleTrack;      // (this is the old hack)
    std::optional<PropertyTrack> rotationTrack;
    std::optional<PropertyTrack> opacityTrack;
    // ...
};
```

### After (Commit 1 target)

```cpp
struct Transform {
    AnimatedProperty<Vec3>  position    { {0, 0, 0} };
    AnimatedProperty<Vec3>  rotation    { {0, 0, 0} };
    AnimatedProperty<Vec3>  scale       { {1, 1, 1} };
    AnimatedProperty<Vec2>  anchorPoint { {0.5f, 0.5f} };
    AnimatedProperty<Vec2>  sizePixels  { {200, 120} };
    AnimatedProperty<float> opacity     { 1.0f };
};

struct Layer {
    Transform transform;   // holds 6 AnimatedProperty<T>'s, no separate track fields
    // ... rest of Layer stays identical (id, name, type, parent, effects, etc.)
};
```

**Notice what disappeared:** the 4 parallel `std::optional<PropertyTrack>` fields and their SampleTracks bookkeeping. Each `AnimatedProperty<T>` carries its own keys inline. Cleaner + one place per field.

---

## 4. How things read the values (this is the key insight)

**Everywhere a value is currently read, add `.Evaluate(compTime)`:**

| Code site | Before | After |
|---|---|---|
| Renderer world matrix (`Transform::ToLocalMatrix()`) | uses `position, rotation, scale` fields directly | takes `float compTime` param, calls `position.Evaluate(t)` etc |
| `LayerManager::GetWorldMatrix(id)` | walks parent chain reading raw values | walks parent chain calling `layer.transform.position.Evaluate(currentCompTime)` etc |
| `CompositionRenderer::RenderLayers()` | reads `layer.transform.sizePixels` etc | reads `layer.transform.sizePixels.Evaluate(t)` etc |
| Inspector DragFloat3 for Position | writes `sel->transform.position` | reads `sel->transform.position.Evaluate(t)` into a temp float3, DragFloat3 mutates the temp, if DragFloat3 returned true call `sel->transform.position.SetValue(t, tempFloat3)` |
| Gizmo drag on Move | writes `layer.transform.position.x/y` | calls `layer.transform.position.SetValue(t, newPos)` — auto-keys if stopwatch on |
| Stopwatch click | complex `ToggleAnimatePosition(t)` helper on Layer | direct `sel->transform.position.ToggleStopwatch(t)` |
| Save/load (Commit 2) | doesn't exist | serialize each `AnimatedProperty<T>` in Lottie-style JSON |
| Undo (Commit 3) | doesn't exist | snapshot the `Transform` struct — auto-includes keyframes because they're inline |

**One read path, one write path, per property.** Nothing else in the codebase needs to know whether a property is animated or not.

---

## 5. Composition time — how it gets in

We need `compTime` at the read sites. Currently the code reads `animEngine.currentTime` directly, which is a global. Two options:

**Option A (my recommendation):** `Transform` helpers take `compTime` as a parameter:
```cpp
Mat3 Transform::ToLocalMatrix(float compTime) const;
Mat4 Transform::ToLocalMatrix4(float compTime) const;
```
Every call site changes from `layer.transform.ToLocalMatrix()` to `layer.transform.ToLocalMatrix(compTime)`. Compiler catches every miss.

**Option B:** thread-local composition time. Cleaner call sites but hides the dependency.

**Going with A.** Explicit > implicit. Also lets Commit 2's save-file be pure data with no clock coupling.

---

## 6. What the Inspector actually looks like after this

Say the user selects the Bouncing Ball and looks at Position.

**Current (Task 5.0-c) code path:**
```
DragFloat3("Position", &sel->transform.position.x)
     ↓ (if changed)
sel->transform.position.x = 500        // authored write
     ↓
sel->AutoKeyPositionIfEnabled(t)       // if stopwatch on, adds key
     ↓
next frame: SampleTracks(t) overwrites sel->transform.position with evaluated value
     ↓
Inspector displays the evaluated value on the next redraw = confused user
```

**After Commit 1:**
```
Vec3 v = sel->transform.position.Evaluate(t);     // 1. read current value (evaluated)
if (DragFloat3("Position", &v.x)) {                // 2. edit local copy
    sel->transform.position.SetValue(t, v);         // 3. write goes to key OR static
}
```

**Three lines per property in the Inspector.** No AutoKey helpers. No SampleTracks calling. The `Evaluate` reads what's on canvas, `SetValue` writes intelligently based on stopwatch state.

---

## 7. Files changing

```
src/AnimatedProperty.h   NEW    ~90 lines (template + Lerp overloads)
src/Layer.h              MODIFIED  ~-60 / +30 (delete parallel-track fields, redeclare Transform)
src/LayerManager.h       MODIFIED  ~5 lines (matrix getters take compTime)
src/LayerManager.cpp     MODIFIED  ~15 lines (pass compTime through parent-chain walk)
src/CompositionRenderer.cpp MODIFIED  ~10 lines (Evaluate calls for size/color)
src/RenderEngine.cpp     MODIFIED  ~60 lines (Inspector, gizmo, spawn helper, sync-camera)
```

Old files being SIMPLIFIED (net-negative lines):
- `Layer.h` loses `KeyPosition/AutoKeyPositionIfEnabled/ToggleAnimatePosition/IsPositionAnimated` and the 4 parallel opt-tracks + `SampleTracks`
- `RenderEngine.cpp` loses the `stopwatch` lambda's `id` gymnastics — each button now just calls `sel->transform.position.ToggleStopwatch(t)` directly

---

## 8. Edge cases addressed by the AE model automatically

| Concern from my old design doc | Fate under this model |
|---|---|
| "Gizmo drag while stopwatch on stamps 60 keys/sec" | `SetValue` with the SAME time (float compare within 1ms) overwrites the existing key. Drag ends → one final key at the drag-end time. No spam. |
| "First frame of a fresh layer has default evaluated" | `Evaluate` on empty keys returns `staticValue` (the constructor default). No delay. |
| "Non-animated size/anchor need to be copied from authored to evaluated" | They're `AnimatedProperty<Vec2>` too; `.Evaluate(t)` on an unanimated one just returns `staticValue`. Same code path, no special case. |
| "Camera sync order in BeginFrame" | Camera reads `cl->transform.position.Evaluate(t)` directly at read time. No pre-sample pass needed at all. |

**Bonus edge cases we now handle correctly that the old design missed:**
- If two layers share a compTime but the render is called in the middle of an ImGui event → both reads pass their own `t`, no shared mutable state to corrupt.
- If we ever add expressions (Phase 5+) → `AnimatedProperty::Evaluate` gets one more override branch inside itself. Zero changes to any call site.
- Lottie export becomes ~30 lines of code because our in-memory format IS Lottie's schema.

---

## 9. What deliberately does NOT change in this commit

- No new UI elements
- No changes to how stopwatches look on screen (still the small orange dot with `##pos/##rot/##scl/##op` ID trick)
- No changes to keyframe diamonds in the timeline strip (diamond drag is a later commit)
- No new easing (still linear interp; Bezier per key is Phase 2 as planned)
- Camera behavior unchanged
- Effects unchanged
- Export unchanged (but Commit 4/5 later will export real scenes properly)

---

## 10. Test plan

Every test in `TEST_CHECKLIST.md` should PASS IDENTICALLY. If anything visually differs, it's a bug in this commit.

Plus one new positive test I want to verify by hand:
- Select ball, click position stopwatch, playhead at 0s, position = (500, 500), scrub to 1s, position = (1500, 500), scrub playhead back and forth. **Inspector Position field should show 500 at t=0, 1000 at t=0.5, 1500 at t=1.** (Previously would have showed the authored default the whole time.) **Canvas should match.**

---

## 11. Answers to my old design doc's Q1/Q2/Q3

**Q1 (rename Transform to AuthoredTransform?):** N/A — no rename needed. `Transform` keeps its name, just contains `AnimatedProperty<T>`'s instead of raw values.

**Q2 (mouse-up-only keyframing?):** N/A — `SetValue(t, v)` with same-time-tolerance handles this naturally. Drag stamps at the same time repeatedly = one final key. No mouse-up-only special case needed.

**Q3 (48 bytes per layer for evaluated?):** Now moot. `AnimatedProperty<T>` is `sizeof(T) + sizeof(vector) + bool` ≈ 48 bytes per property × 6 properties = 288 bytes per layer static. Empty keyframes = 0 heap. First keyframe added = ~48 bytes heap. For a 100-layer scene with 5 animated properties, ~24 KB. Still trivial on 4GB.

---

## 12. Migration risk

The one risk: the existing tests all worked with the old `PropertyTrack` structure. Every call site that reads a transform field needs the `.Evaluate(t)` call. If I miss ONE call site, that property silently displays the static value forever instead of animating.

**Mitigation:** delete the `PropertyTrack` type entirely at the start of the commit. Compiler errors will point to every read site that needs updating. Fix all of them. If the codebase compiles, no read site was missed.

---

## 13. What I'll do if approved

1. Create `src/AnimatedProperty.h` with the template + Lerp overloads
2. Rewrite `Transform` in `Layer.h` to use `AnimatedProperty<T>`
3. Delete `PropertyTrack`, `SampleTracks`, `KeyX/AutoKeyX/ToggleAnimateX/IsXAnimated`, and the 4 parallel optional fields on `Layer`
4. Add `compTime` parameter to `Transform::ToLocalMatrix` / `ToLocalMatrix4`
5. Add `compTime` parameter to `LayerManager::GetWorldMatrix` / `GetWorldMatrix4` / `GetWorldOpacity`
6. Update every call site (compiler will find them all)
7. Rewrite Inspector's DragFloat3 calls to the Evaluate/edit-temp/SetValue pattern
8. Rewrite gizmo drag to call `SetValue`
9. Update `SyncCameraFromLayerIfAny` to Evaluate at current compTime
10. Update `SpawnShapeAtViewportCenter` — just write `staticValue`
11. Local syntax check with stubs
12. Commit as `Task 5.1: AnimatedProperty<T> pattern (AE/Lottie architecture)`
13. Push, wait for CI
14. Report back with build link + the test above

**Then wait for your approval before Commit 2 (Save/Load).**

---

## 14. One thing I want you to be aware of

This commit is bigger than my original plan (~+250 LOC net vs +50). The upside: it makes save/load, undo, per-keyframe Bezier, and future expressions ALL simpler because there's ONE read path and ONE write path per property. Estimated Phase 1 total (5 commits) actually goes DOWN by ~1 day because we skip a separate "introduce AnimatedProperty<T>" step in Phase 2.

If you're worried about the commit size, I can split it in two: Commit 1a = introduce the template + refactor Transform + fix all read/write sites (invisible refactor). Commit 1b = Inspector rewrite to Evaluate/SetValue pattern (visible: Inspector now shows current animated value instead of authored default). Same end state, smaller diffs.

**Preference: single commit is fine because there's a clean cut point (all-old vs all-new for AnimatedProperty). Split-in-two adds review overhead for marginal safety.**

Say the word — "single commit" or "split in two" or "approved, go" or "wait, one thing" — and I execute.
