# Design Doc — Commit 6 (Task 5.4-fix): AE-Accurate Graph Editor

**Base commit:** `05bfb40` (Task 5.4 — first pass Graph Editor)
**LOC delta estimate:** +450 / -180 (net ~+270)
**User-visible change:** the Graph Editor becomes bit-for-bit faithful to
After Effects' internal model — speed + influence per side, all 5 interp
types (Linear / Bezier / Continuous Bezier / Auto Bezier / Hold), X/Y/Z
axis coloring on the Value graph, magnitude-combined Speed graph for
multi-dim properties, and AE-authentic handle drag (with a Shift/Alt
modifier extension so free-drag still works for beginners).

**Why this commit exists:** the first Task 5.4 pass shipped
Lottie/CSS-style (time_offset, value_offset) tangents. That's what most
custom editors use and it's exactly why "they feel different." AE
stores speed (units/sec) and influence (%) as source of truth and
derives the Bezier at eval time. Getting this right is what makes the
motion feel "professional" instead of "prototype."

---

## 1. What AE actually stores per keyframe

Per side (in AND out):

- `type`: `Linear` | `Bezier` | `Continuous Bezier` | `Auto Bezier` | `Hold`
- `speed`: units/second at which the value leaves/enters the key
- `influence`: 0-100% — how far in TIME toward the next/prev key the tangent handle reaches

Per key (shared):

- `time`: seconds in comp
- `value`: T (float / Vec2 / Vec3)
- `roving`: bool (spatial interp related — stored now, evaluated in a later commit)

**Easy Ease (F9) = both sides `Bezier`, speed=0, influence=33.33%.** That's
the canonical AE recipe.

### Converting (speed, influence) → cubic Bezier for evaluation

Between key `A` at time `tA`, value `vA` and key `B` at time `tB`, value `vB`,
with `dt = tB - tA`:

```
P0 = vA
P3 = vB
P1 = vA + A.outSpeed * (dt * A.outInfluence / 100)     // value delta
     -> control-point time is tA + (dt * A.outInfluence / 100)
P2 = vB - B.inSpeed  * (dt * B.inInfluence  / 100)     // value delta
     -> control-point time is tB - (dt * B.inInfluence  / 100)
```

Then evaluate value with cubic Bezier `B(u)` (Newton-Raphson on X = time,
same as before), and speed with `|B'(u)| / dt`. This means Value AND Speed
graphs both come from ONE evaluator — no double truth.

### Backward compat with commit 5.4 files

Files saved by commit 5.4 have `it/ot/iv/ov/im/om` fields. Loader detects
those and converts on read: `speed = value_offset / time_offset` (with
divide-by-zero guard), `influence = |time_offset| / segment_span * 100`.
Old fields never written again. Commit 5.2 files (no tangent fields at
all) load with all-Linear defaults, unchanged behavior.

---

## 2. Data model changes

### Current `Keyframe<T>` (Task 5.4):

```cpp
enum class InterpMode : int { Linear, Bezier, Hold };
struct Keyframe {
    float time, T value;
    T inTangentValue, outTangentValue;
    float inTangentTime, outTangentTime;
    InterpMode incomingMode, outgoingMode;
};
```

### After this commit:

```cpp
enum class InterpMode : int {
    Linear           = 0,
    Bezier           = 1,   // user drags handles independently
    ContinuousBezier = 2,   // handles are mirrored (equal-length, opposite dirs)
    AutoBezier       = 3,   // AE auto-computes from neighbor slopes; handles locked
    Hold             = 4,
};

template <typename T>
struct Keyframe {
    float time  = 0.0f;
    T     value = T{};

    // AE-native per-side data. Speed is in T-space (units/sec for float,
    // Vec2/Vec3 vector velocity for vector properties). Influence is 0..100.
    T     inSpeed        = T{};
    T     outSpeed       = T{};
    float inInfluence    = 16.667f;  // AE default per Adobe docs
    float outInfluence   = 16.667f;  // (F9 Easy Ease bumps these to 33.33%)

    InterpMode incomingMode = InterpMode::Linear;
    InterpMode outgoingMode = InterpMode::Linear;

    bool  roving = false;   // spatial interp; evaluator ignores for now
};
```

### Evaluator (`AnimatedProperty::Evaluate` + `EvaluateBezierSegment`)

**Hold** — `A.outgoingMode == Hold` short-circuits to `A.value`. Unchanged.

**Linear** — both sides Linear → Lerp fast path. Unchanged.

**Any Bezier flavor** — derive P0..P3 from speed/influence per the formula
above, run existing Newton-Raphson on X, evaluate Y. `ContinuousBezier` and
`AutoBezier` differ only in HOW their speed/influence get set, not in HOW
they're evaluated — so the eval path is one uniform function.

**ContinuousBezier bookkeeping:** when the user drags the outgoing handle of
a `ContinuousBezier` key, we mirror to incoming (same speed magnitude,
same influence). Done in the UI layer, not the evaluator.

**AutoBezier bookkeeping:** when a key is marked Auto, we recompute both
sides' speed at UI-render time as the average of `(prev-slope, next-slope)`
projected onto the segment. Handles are non-draggable in the graph editor
(rendered but grayed out).

---

## 3. Speed graph: magnitude-combined for multi-dim

For a `float` property (Opacity, Rotation.z), speed = signed derivative.
For a `Vec2`/`Vec3` property (Position, Scale), the Speed graph shows
**one curve = `sqrt(dx² + dy² [+ dz²])/dt`**. Not per-channel. This
matches AE and is what makes "Position speed" mean something.

Consequence: in Speed mode, the property picker changes:

- `Position` (single entry, computed magnitude)
- `Rotation.z`
- `Scale` (single entry, magnitude across x,y — z ignored per AE)
- `Opacity`

In Value mode the picker keeps the current per-channel breakdown (X/Y/Z
plotted separately with red/green/blue).

---

## 4. Value graph: X/Y/Z axis colors

For a `Vec3` property in Value mode, we draw all three channel curves on
the same graph simultaneously:

- X → red   (`IM_COL32(230, 80, 80, 255)`)
- Y → green (`IM_COL32(80, 200, 80, 255)`)
- Z → blue  (`IM_COL32(80, 140, 255, 255)`)

Keys of the currently-selected dim get full-brightness diamonds; other
dims render at half alpha. Clicking a key on any curve selects that dim
automatically (updates the picker). This mirrors AE's default behavior.

For `Vec2` — X red, Y green, no Z. For `float` — single white curve.

---

## 5. Handle drag: AE semantics + free-drag modifier

Per your call: **both modes**.

- Default: **free 2D drag** — hor moves the handle in time, ver moves in
  value. Under the hood, we decompose into (speed, influence) and store
  that.
- **Shift held** = horizontal-only drag → influence only, speed
  unchanged. This is AE's default (their handles are constrained this
  way natively).
- **Alt held** = vertical-only drag → speed only, influence unchanged.

Handle screen position derives FROM (speed, influence) each frame — the
handle is a *view* of the stored data, not the data itself. That's the
whole point of the redesign.

For `ContinuousBezier` keys, dragging either handle mirrors to the other
(matched speed magnitude, matched influence). For `AutoBezier` keys, the
handles are drawn dimmed and don't respond to drag (right-click → promote
to `Bezier` to unlock).

---

## 6. Serialization schema (extends Task 5.4)

Per-keyframe fields become:

```json
{
  "t": 0.0, "v": [960,540,0],
  "im": "bezier",         // in mode
  "om": "continuousBezier", // out mode
  "is": [100, 0, 0],      // in speed (T-space; scalar for float, array for Vec2/Vec3)
  "os": [200, 0, 0],      // out speed
  "ii": 33.33,            // in influence 0..100
  "oi": 33.33,            // out influence
  "r": true               // roving; omitted when false
}
```

New field names avoid collision with the commit-5.4 `it/ot/iv/ov`. Legacy
loader path:

```cpp
if (kj.contains("it") || kj.contains("iv") || kj.contains("ot") || kj.contains("ov")) {
    // Commit-5.4 format: convert (time_offset, value_offset) tangents to
    // (speed, influence) per the derivation in Section 1.
    // Result is bit-identical evaluation because we're the same math.
}
```

Interp mode strings: `"linear" | "bezier" | "continuousBezier" | "autoBezier" | "hold"`.

Fields only emitted when they differ from defaults, so purely-linear
files stay short and Task 5.2 files still round-trip clean.

---

## 7. UI changes

### Graph Editor toolbar (top of panel)

```
[Value][Speed] | Property: [Position ▼] | Layer: <name>
                                          | Right-click key: L / B / CB / AB / H / Delete
```

For a Vec3 in Value mode the "Position" picker also gets radio-style
dim highlight so you know which channel is "selected" for tangent editing.

### Right-click context menu

```
Key #3 (Position @ 1.500s)
─────────────────────────
Set to Linear
Set to Bezier (F9 Easy Ease)
Set to Continuous Bezier
Set to Auto Bezier
Set to Hold
─────────────────────────
Delete Keyframe
```

### Inspector "Apply as Ease" button (existing)

Still works — but instead of writing tangent offsets, it now writes
(speed=0, influence from slingshot.P1.x * 100). Cleaner mapping.

---

## 8. Files changing

```
src/AnimatedProperty.h  MODIFIED  +80 / -40    Keyframe reshape; new evaluator
                                                that derives P0..P3 from speed/inf;
                                                Continuous/Auto stay same eval,
                                                mirrored/auto-set at write time
src/RenderEngine.h      MODIFIED  +6           GraphPropertyGroup enum + drag mode
src/RenderEngine.cpp    MODIFIED  +250 / -140  DrawGraphEditor rewritten:
                                                X/Y/Z multi-curve draw, magnitude
                                                speed calc, Shift/Alt handle drag,
                                                context menu gains CB/AB entries,
                                                AutoBezier recompute pass
src/Serialization.cpp   MODIFIED  +90 / -30    New schema + legacy 5.4 tangent
                                                → speed/influence converter
DESIGN_COMMIT6_AE_ACCURATE_GRAPH.md  NEW     this file
```

Total ~+270 net LOC. Binary impact ~+10 KB after LTCG.

---

## 9. Test plan

1. Load a Task 5.2 `.pmge` → linear playback, byte-identical to before.
2. Load a Task 5.4 `.pmge` (my previous commit) → animation plays with
   the same Bezier shape (converter maps old tangent offsets → new
   speed/inf, evaluator produces same value curve).
3. New Position keys → right-click → "Continuous Bezier" → drag out
   handle up → in handle mirrors automatically.
4. Set to "Auto Bezier" → handles gray out; motion is smooth without
   any manual dragging; add a third key in the middle → all three
   auto-recompute.
5. Value graph: Position shows THREE curves (red X, green Y, blue Z);
   only Y is animated → red and blue are flat lines at 540 and 0.
6. Speed graph: Position shows ONE combined-magnitude curve.
7. Shift-drag a handle → only influence changes (handle stays at
   same value height, slides in time). Alt-drag → only speed changes.
8. F9 shortcut (add later) or right-click "Bezier (Easy Ease)" →
   both sides speed=0, influence=33.33%.
9. Save → reopen → all interp modes preserved; JSON contains `is/os/ii/oi`.
10. Ctrl+Z undoes every drag + mode change (MarkForSnapshot on drag start
    and menu action).

---

## 10. Questions before I execute

None. All four decisions locked in your last message. Say
**"go single commit"** and I execute.

If you'd rather split into two commits (storage+eval first, UI polish
after) tell me now — same code either way, just different push points.
