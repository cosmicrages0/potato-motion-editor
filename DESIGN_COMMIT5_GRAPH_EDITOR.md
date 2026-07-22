# Design Doc — Commit 5 (Task 5.4): Real Graph Editor + Per-Keyframe Bezier Easing

**Base commit:** `719c793` (Task 5.3-fix-3)
**LOC delta estimate:** +300 / -80
**User-visible change:** the Graph Editor stops being a showpiece. Every keyframe gets Bezier easing handles that actually drive animation. The comp playback finally feels like real motion design.
**Reference:** Adobe After Effects Graph Editor behavior. Research summary in Section 2.

---

## 1. Why this commit matters (ChatGPT's Q16 in the RFC)

> *"Per-keyframe Bezier easing is what makes it 'real'. When easing is good, every animation suddenly feels professional. Linear interpolation screams 'prototype.' Good interpolation screams 'motion design software.'"*

Right now every keyframe interpolates linearly (`AnimatedProperty::Evaluate` calls `Lerp`). That's why the animation feels robotic. This commit swaps the inner loop from `Lerp` to a cubic Bezier evaluator, adds `inTangent` / `outTangent` handles to each keyframe, and rewires the Graph Editor panel to actually show and edit them.

---

## 2. Research summary: how AE does it

Confirmed from Adobe community + design-K + Effects Collective:

### 2.1 Two graph modes: Value vs Speed

**Value Graph** — the property value plotted against comp time.
- Y axis = the property value (e.g. Position.x pixels, Opacity %)
- X axis = comp time in seconds
- Slope = speed
- Best for: bouncy motion, overshoots (P1.y > 1 or < 0), arc trajectories where you need to see the actual path
- Right-click → "Edit Value Graph"

**Speed Graph** — the property's speed (first derivative) plotted against comp time.
- Y axis = velocity (units per second)
- X axis = comp time
- Best for: acceleration/deceleration feel, synchronizing multiple properties to the same rhythm
- Right-click → "Edit Speed Graph"

**Pro users toggle between them constantly** depending on what they need to control. Both operate on the SAME underlying keyframe data; only the visualization differs.

### 2.2 Per-keyframe interpolation modes

Each keyframe has TWO interpolation flags:
- **Incoming** (how it eases coming INTO this key from the previous key)
- **Outgoing** (how it eases going OUT of this key toward the next key)

Each can be one of:
- **Linear** — constant velocity
- **Bezier** — user-defined tangent handles (this is what "F9 / Easy Ease" produces)
- **Hold** — value snaps at this key, no interpolation until next key (useful for stepped animation)

For our Task 5.4 we implement **Linear + Bezier**. Hold ships in a later commit as a simple checkbox — the framework will already support it.

### 2.3 Handle representation

A Bezier keyframe stores two 2D handle vectors: one leaving the key (outTangent) and one arriving at the next key (inTangent of the NEXT key). Each handle is expressed in **(time_offset, value_offset)** relative to the key. AE also exposes "Influence" (a 0-100% slider) and "Speed" (units/sec) as an alternative way to edit the same handles via the "Keyframe Velocity" dialog.

For simplicity our storage is direct (time_offset, value_offset) — same as Lottie's `i` and `o` fields per keyframe.

### 2.4 Evaluation math

Between two adjacent keyframes `A` (at time tA, value vA, outTangent oA) and `B` (at time tB, value vB, inTangent iB), the value at time t is computed as:

```
u = normalize(t, tA, tB)              // 0..1 across the segment
                                       // First: use tangents to solve for x-t
P0 = (tA, vA)
P1 = (tA + oA.time, vA + oA.value)
P2 = (tB + iB.time, vB + iB.value)     // iB.time is typically negative
P3 = (tB, vB)

// Standard cubic Bezier eval at parameter u:
B(u) = (1-u)^3 * P0 + 3*(1-u)^2*u * P1 + 3*(1-u)*u^2 * P2 + u^3 * P3
```

Since AE's per-property Bezier is technically a 2D curve in (time, value) space, and time doesn't move backward, we need to iterate to find the `u` such that `B(u).x == t` — a Newton-Raphson solve in ~4 iterations, cheap. Then use that `u` in the value formula. This is the same math CSS cubic-beziers (`cubic-bezier(0.4, 0, 0.2, 1)`) use.

**Fallback: if both tangents are (0, 0) at a segment, the result reduces to linear interp** — so the existing Task 5.1 keyframes (which have no tangent data) continue to work identically. Backward compatibility is free.

---

## 3. Data model changes

### Current `Keyframe<T>` (in `AnimatedProperty.h`):

```cpp
template <typename T>
struct Keyframe {
    float time = 0.0f;
    T     value = T{};
};
```

### After Task 5.4:

```cpp
enum class InterpMode : int {
    Linear = 0,  // constant velocity through this segment
    Bezier = 1,  // uses (inTangent, outTangent) — the default when you drag a handle
    Hold   = 2,  // (Task 5.4.x future) — value snaps, no interp until next key
};

template <typename T>
struct Keyframe {
    float      time     = 0.0f;
    T          value    = T{};
    // Bezier tangents in (time_offset_seconds, value_offset_in_T_units).
    // Zero-vectors mean "linear" behavior even in Bezier mode.
    T          inTangentValue  = T{};   // value delta relative to this key
    float      inTangentTime   = 0.0f;  // time delta (typically negative)
    T          outTangentValue = T{};
    float      outTangentTime  = 0.0f;  // time delta (typically positive)
    InterpMode incomingMode = InterpMode::Linear;
    InterpMode outgoingMode = InterpMode::Linear;
};
```

The `T` in inTangentValue is important: we need offsets in the SAME space as `value`, so a Vec3 position keyframe has Vec3 tangent value offsets. This works because our `Lerp` overloads already cover float/Vec2/Vec3.

### `Evaluate()` change:

```cpp
T Evaluate(float t) const {
    // ... existing empty / single-key / clamp guards unchanged ...
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        const auto& A = keyframes[i];
        const auto& B = keyframes[i + 1];
        if (t >= A.time && t <= B.time) {
            const bool linearSegment =
                (A.outgoingMode == InterpMode::Linear &&
                 B.incomingMode == InterpMode::Linear);
            if (linearSegment) {
                // Existing linear path — Task 5.1 backward compat.
                const float span = B.time - A.time;
                const float u = (span > 1e-6f) ? (t - A.time) / span : 0.0f;
                return Lerp(A.value, B.value, u);
            }
            // Bezier segment: Newton-Raphson to solve for parametric u,
            // then eval the value cubic.
            return EvaluateBezierSegment(A, B, t);
        }
    }
    return keyframes.back().value;
}
```

`EvaluateBezierSegment` is ~15 lines: build P0..P3 in (time,value) space, Newton-Raphson 4 iterations on X to find the parametric u that matches the requested time, plug that u into the standard cubic Bezier for the value coordinate.

### Backward-compat guarantee

Existing `.pmge` files (from Task 5.2 saves) have keyframes with NO tangent fields. When loaded, the tangents default to zero and the interp modes default to Linear — so old files play back IDENTICALLY. New saves include the tangent data (with default = zeros, which round-trips safely).

---

## 4. UI: Graph Editor panel becomes real

Current `DrawGraphEditor()` shows the global slingshot Bezier as a decorative curve. Replace with a real per-property editor:

### Layout inside the Graph Editor panel

```
+------------------------------------------------------------+
| [Value] [Speed]  Property: [Position ▼]  Layer: <selected> |   <- mode toggle + property picker
| ------------------------------------------------------------
|          ↑ value                                            |
|   1.5 ---+                                                  |
|          |         ●───────────                             |
|          |        /                                          |
|   1.0 ---+       ●                                          |
|          |      /|                                          |
|          |     / |                                          |
|   0.5 ---+    /  |                                          |
|          |   /   |                                          |
|          |  /    |                                          |
|   0.0 ---●───────+─── time →                                |
|          0.0     0.5     1.0     1.5 s                     |
|                                                             |
|  Right-click a key: Linear | Bezier | Hold                  |
|  Drag tangent handles to shape the curve                    |
+------------------------------------------------------------+
```

### Interactions

- **Mode toggle** at the top: two buttons `[Value] [Speed]` — clicking switches the visualization. Task 5.4 ships Value mode; Speed mode ships as a follow-up (needs derivative computation over sampled points; not hard, just separate).
- **Property picker** dropdown: `Position.x`, `Position.y`, `Position.z`, `Rotation.z`, `Scale.x`, `Scale.y`, `Opacity`. Since each `AnimatedProperty<T>` has independent tangents per keyframe, we plot ONE scalar dimension at a time. This is exactly what AE does (AE calls this "Separate Dimensions" — for us it's the default).
- **Left-click a keyframe dot** → selects it. Selected key shows its two tangent handles as small circles connected by dashed lines.
- **Drag a tangent handle** → updates the corresponding tangent offset. Updates ripple to the value graph in real time.
- **Right-click a keyframe** → context menu: `Set to Linear`, `Set to Bezier` (F9-equivalent, symmetric ease), `Set to Hold`, `Delete Keyframe`.
- **The red playhead line** from the timeline strip also renders here in comp time — makes it obvious which segment is currently playing.

### Removed: the old global slingshot demo curve

The static P0/P1/P2/P3 curve at the bottom of the Graph Editor becomes obsolete now that every keyframe has its own tangents. I'll delete it. The Inspector's "Slingshot Bezier Handles" collapsing header becomes a small helper widget instead: "Apply This Curve as Ease" — a button that copies the currently-displayed slingshot P1/P2 as tangents to the selected keyframe. Useful shortcut for anyone who wants the overshoot behavior.

---

## 5. Serialization additions (Task 5.2 compat)

Extend the AnimatedProperty JSON schema:

**Before (Task 5.2):**
```json
"keys": [ { "t": 0.0, "v": [960, 540, 0] }, { "t": 1.0, "v": [1400, 540, 0] } ]
```

**After (Task 5.4):**
```json
"keys": [
  {
    "t": 0.0, "v": [960, 540, 0],
    "ot": 0.3, "ov": [200, 0, 0],   // outTangent time + value delta
    "om": "bezier"                    // outgoing mode: "linear"|"bezier"|"hold"
  },
  {
    "t": 1.0, "v": [1400, 540, 0],
    "it": -0.3, "iv": [-200, 0, 0],   // inTangent time + value delta
    "im": "bezier"                     // incoming mode
  }
]
```

Old files (no `ot`/`iv`/etc fields) load with defaults = zero + Linear, so they play back identically.

---

## 6. Files changing

```
src/AnimatedProperty.h      MODIFIED  +30 / -0    Keyframe<T> gains tangent + mode fields
                                                   Evaluate() dispatches to Bezier path
                                                   New EvaluateBezierSegment helper
src/RenderEngine.h          MODIFIED  +8          graph editor mode + selected-key state
src/RenderEngine.cpp        MODIFIED  +250 / -80  DrawGraphEditor rewritten;
                                                   context menu on graph keys;
                                                   remove old slingshot decorative curve
src/Serialization.cpp       MODIFIED  +30         extend AnimatedProperty JSON read/write
src/Layer.h                 MODIFIED  0           no changes (AnimatedProperty is the storage)
```

Total ~+260 net LOC. Binary impact: maybe +5-15 KB after LTCG.

---

## 7. Scope for this commit vs. deferred

### In this commit

- Per-keyframe `inTangent` + `outTangent` + `incomingMode` + `outgoingMode` in `Keyframe<T>`
- Bezier evaluation with Newton-Raphson time-inversion in `Evaluate()`
- Graph Editor panel rewrite: Value mode with draggable tangent handles + property picker + selected-key highlight
- Right-click context menu (Linear / Bezier / Hold / Delete)
- Serialization round-trip for the new fields
- Backward-compat: Task 5.2 files load with all tangents zero + Linear, no change in behavior

### Explicitly NOT in this commit (each is its own follow-up)

- **Speed graph mode** (needs derivative visualization; not hard, just separate)
- **"Influence %" alternative handle representation** — we use raw time/value offsets. Later we can add a Keyframe Velocity dialog.
- **Auto-Bezier / Continuous-Bezier smoothing** (AE's F9-with-modifier variants) — for now only user-dragged Bezier + Linear + Hold. Auto-modes are polish.
- **"Roving" keyframes** (AE spatial interpolation that keeps a key on the motion path without a fixed time) — 3D-focused feature, skip.
- **Multi-selection of keys with box-select** — nice UX but Phase 3 polish. For now one key at a time.

---

## 8. Test plan

1. Load a Task 5.2 `.pmge` file → animation plays identically (linear interp fallback works).
2. Set a Position stopwatch, add 2 keys at t=0 (0,0) and t=1 (500,0). Play. Ball moves linearly.
3. Open Graph Editor → select Position.x from picker → see two dots on the curve.
4. Right-click first key → Set to Bezier. Right-click second → same. Two tangent handles appear on each.
5. Drag first key's outgoing handle up and to the right → curve becomes an "ease-out" shape.
6. Drag second key's incoming handle down and to the left → segment becomes classic ease-in/out.
7. Play → animation now accelerates from rest, decelerates to stop. Feels like motion design.
8. Save → close → open → animation preserved with tangents intact (verify by inspecting `.pmge` for `ot`/`iv` fields).
9. Right-click a key → Delete Keyframe → gone. Undo → restored.
10. All existing tests in `TEST_CHECKLIST.md` pass identically.

---

## 9. Question(s) before I execute

**Q1.** Confirm the picker default: I'm going to make the Graph Editor auto-select the "most likely" property when a layer is selected — Position if any position tangents differ from linear, else the first animated property found, else Position.x by default. Acceptable, or would you rather it default to whatever property was last picked (session sticky)?

**Recommendation: auto-select as described.** Reduces clicks in the common case. If you disagree, tell me now.

**Q2.** Speed graph mode: I'm shipping only Value mode in this commit and leaving Speed for a follow-up. Both graph modes share ~90% of code — Speed adds derivative computation on top. Splitting them means Value ships in ~1 day, Speed in ~half day more. Acceptable, or bundle both?

**Recommendation: bundle both — Speed adds ~50 LOC.** Real AE users switch between modes constantly. Skipping Speed would leave a visible menu item that says "not implemented".

Say **"go single commit, bundle Speed graph"** and I execute both.
