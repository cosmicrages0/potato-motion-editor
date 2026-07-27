# Bouncing Ball test session — feedback triage

Real user test of everything shipped through Task 5.13-fix2. Findings
grouped by area, with my read on each (bug vs missing feature vs UX polish),
severity, and rough fix effort. Ordered by area, then severity within.

Legend:
- **[BUG]** — something implemented but wrong
- **[MISSING]** — feature that should exist but doesn't
- **[UX]** — works, but painful to use
- **[POLISH]** — cosmetic

Effort: S (< 30 min), M (< 2 hr), L (< 1 day), XL (multi-day + design doc)

---

## 1. Graph Editor / Speed Graph

### 1.1 [BUG] Speed graph handles unclickable when many keyframes present — S
User: "handles doesnt even show when more keyframes are placed"
User: "speed graph is editable when 2 or 4 keyframes are, if there are many
keyframes it cant move"

Almost certainly the Task 5.4-fix-3 hit-testing regression — a fixed
hit-radius that gets swamped when keyframes crowd. Or the handle-drawing
is culled past N keyframes. Need to reproduce with 6+ keyframes on one
property.

### 1.2 [BUG or CORRECT-BY-DESIGN?] Speed graph doesn't show "big U, middle U,
small U" shape for a bouncing-ball animation — S to investigate

**Post-research verdict (AE 2025 reference):** Our behavior is likely
CORRECT and the user's intuition was correct too — for a *pure position
keyframe bouncing ball* (no scale/rotation keys), the speed graph should
show one big inverted-V mountain per bounce, decreasing in size, with the
peak of each mountain at the ground-impact frame (where velocity is
maximum). Sources: effectscollective.com, creativecow.net threads.

In the bouncing-ball project we built, the squash-and-stretch scale
keyframes at 0.35 / 0.40 / 0.45 (etc.) subdivide the position trajectory
into sub-segments. Each sub-segment gets its own mini-mountain. That's
mathematically correct — the ball's aggregate velocity DOES fluctuate
because scale changes momentarily change the sampled position (anchor
point stays fixed but geometry deforms).

**However** — AE's Speed Graph specifically shows *only the animated
property's own velocity magnitude*. If Speed Graph is opened on
"Position", it should NOT be influenced by Scale keyframes. That's the
one thing worth verifying in our code — is our Speed Graph aggregating
velocity across ALL animated properties, or just the selected one?

**Also from research (important for the Graph Editor backlog):**

AE convention is that Speed Graph is BEST for UI-style motion (fades,
slides, rhythmic pacing), and Value Graph is BEST for physics
(bounces, overshoots, arcs). Our Graph Editor should probably default
to Value Graph when the user is on Position of a physics-y layer. The
Pro Tip from [designkkashi] source: "For bouncing balls or swinging
pendulums, you MUST use the Value Graph."

**Verification project:** Delete all Scale keyframes, keep only 6
Position keyframes (start / land / peak / land / peak / rest). Speed
graph on Position should then show 5 clean inverted-V mountains of
decreasing size.

**Follow-up work if wrong:**
- Ensure Speed Graph samples only the picked property, not aggregate
  motion
- Make sure our "Position" is treated as vector 2D magnitude
  (sqrt(vx² + vy²)/dt), not per-axis (which would need separate
  dimensions like AE)

**"Separate Dimensions" feature (deferred):** AE lets user right-click
Position → Separate Dimensions, which splits it into Position X and
Position Y so each axis can have its own curve. Essential for real
physics animation (horizontal = constant velocity, vertical = gravity
curve). Add to backlog as item 17.

### 1.3 [MISSING] "Select all keyframes → one-click bounce/ease preset" — M
User: "there should be option of select all keyframes and in one click to
get bouncing ball effect"

AE has this as **"Easy Ease" (F9)**, **"Easy Ease In" (Shift+F9)**,
**"Easy Ease Out" (Ctrl+Shift+F9)**. Also has an **Animation → Keyframe
Assistant** submenu with more presets.

Concretely for us: right now the user has to right-click each keyframe
individually and pick Auto Bezier. Add:
- Marquee-select multiple keyframes in the graph
- Bulk apply an interp mode to selection
- Keyboard shortcuts F9 / Shift+F9 / Ctrl+Shift+F9

Also worth adding a preset dropdown ("Bounce", "Overshoot", "Ease In-Out")
that applies a canned tangent-shape template to the selection.

### 1.4 [MISSING] Alight Motion-style per-segment curve edit in Bars mode — L
User: "in alight motion each 2 key frame middle curve is editable"

Currently in Bars mode the timeline shows keyframe diamonds but curve
editing is only in Graph mode. Alight/Pikimov show a mini S-curve
between each adjacent keyframe pair right in the bars view, and let
you drag it to change easing without opening Graph. This is UI-heavy
but a huge quality-of-life feature.

---

## 2. Effects — keyframing + polish

### 2.1 [MISSING] Effect properties can't be keyframed — L
User: "the effects doenst have yet keyframes to change thier property"

Right — every `Effect.params.p0/p1/p2/p3` is a raw `float[4]`, not an
`AnimatedProperty<>`. To keyframe Drop Shadow Distance over time (fade
shadow in, etc), we'd need to:
- Wrap each effect parameter in `AnimatedProperty<float>` with its own
  stopwatch in the Inspector
- Serialize per-param keyframes in the .pmge
- Sample at frame time inside `ApplyChain` before uploading to cb_effect_

This is architectural — touches Effect struct, Serialization, Inspector
UI, and ApplyChain. Design doc required.

### 2.2 [BUG] Drop Shadow makes the shape go transparent/black — S to M
User: "drop shadow has a problem it makes the shape goes full transparent
like black"

**FIXED in Task 5.13-fix4** (commit 9af3c11) — fused two-pass DropShadow
into a single-pass shader that eliminates the same-texture SRV+RTV bind
conflict. Layer now keeps its color; shadow renders as an actual offset
shadow at the configured Angle.

**FOLLOWUP (from Task 5.13-fix5 test)** — user: "drop shadow isnt that
good the softness is still not gives that blurry vibe". The 13-tap 2D
gaussian ring is a step up from the old 5-tap perpendicular line but
still not truly smooth at softness > ~20 px. A cloud of ~13 samples
can't approximate a large-radius blur well. Real fix: proper separable
Gaussian (H pass + V pass, ~9 taps each = 18 samples with proper
falloff, dramatically smoother than a 13-tap 2D). Needs an additional
render target slot because DropShadow becomes 2-pass again but this
time BOTH passes read from and write to different textures cleanly.
Deferred as a proper effect-pipeline commit (~1-2 hr).

### 2.3 [BUG] Directional Motion Blur is static — S
User: "direction blur also working but its a static"

Same root cause as 2.1. Also this specific effect could benefit from
sampling multiple frames of the PREVIOUS state to blur "toward where
the layer WAS a few ms ago" (AE's approach). Currently we just do a
fixed-angle box blur at whatever the current angle is. Both fixes
are the same feature request.

### 2.4 [BUG] Text stroke goes "wild like tile effects when more than 12" — S
User: "the text troke goes wild like its have tile effects when its more
then 12"

Known issue from Task 5.11-fix-3 — the 8-sample outline dilation
produces ghost duplicates past ~15 px stroke width. Fix: adaptive
sample count (16 or 32 samples at large widths). ~10 shader lines.
This one is already on the backlog.

---

## 3. Timeline UI — critical UX

### 3.1 [MISSING + UX] All keyframes stack on one row per layer — L
User: "in timeline all the keyframes are one on layer looks stressfull and
not good when editing it overlaps other cant use"
User: "i need like ae that when i select any property or effect it shows
under the name of the layer like a dropdown, then it has options like
visibility/show, stopwatch to add keyframes"

This is **AE's twirl-down property list**. Currently we compress every
Position + Scale + Rotation + Opacity + Anchor + Size keyframe onto
ONE row per layer, so a layer with 6 animated properties looks like a
diamond soup. AE puts each property on its own sub-row under the
layer name.

Concrete design:
- Small ▶ / ▼ twirly next to layer name
- Expanded: 6 sub-rows (Position / Scale / Rotation / Anchor / Opacity /
  Size), each with its own name, its own stopwatch, its own diamond track
- Each sub-row's height = ~18 px
- Effects list appears BELOW transform sub-rows when the layer has
  effects, each effect gets its own twirly too

This is a big chunk of work but it's the single biggest UX unlock right
now. Should be next big task if user agrees.

### 3.2 [MISSING] Split tool for layer bars — M
User: "trimming works but we need split tools also to divide the layer into
middle, only left or right"

AE has this as **Edit → Split Layer** (Ctrl+Shift+D). Splits the
selected layer at the current playhead into two independent layers:
- Left half: keeps original layer's inPoint, outPoint = playhead
- Right half: new layer copy, inPoint = playhead, outPoint = original

Everything (keyframes, effects) copies to both halves. Small feature,
big feel — essential for editing flow.

### 3.3 [MISSING + UX] Effect bars should look different from plain layer bars — S to M
User: "on right side where are the bars the effects will have their own
default bar or something different like plain layer type"

Once 3.1 lands, each effect sub-row needs its own visual style —
different color, maybe a small effect-icon prefix, etc. Cosmetic but
important for readability.

---

## 4. Canvas / Viewport

### 4.1 [BUG or MISSING] "Canvas looks like old paint graphics" — likely AA — M
User: "why our canvas looks like old paint graphics i know we are doing in
c++, i think is it because we dont have AA anti aliasing on shapes"

**PARTIALLY RESOLVED (2026-07-25):** After investigation, text-only issue.
Quick-win #7 (`ca1fcf0`) 2x oversamples the text atlas so text stays crisp
under zoom / scale up to ~200%. User confirmed shapes look fine (SDF AA
already working via Iñigo Quílez fwidth trick).

**Follow-up (2026-07-25):** user wants per-shape AA options:
> "we give options on shapes like little antialiasing or crisp, or a
> toggle button for all the elements shapes etc will have feature like
> to doesnt stressed out the computer, we gives options like crisp
> detailed, smooth edges, and antialiasing"

Concrete design (parked, medium task):
- Per-layer enum: `AAMode { Crisp=0, Smooth=1, HighQuality=2 }`
  - Crisp: no AA, hard edges (fastest, for pixel-art / retro looks)
  - Smooth: current 1-pixel SDF AA (default, what we have now)
  - HighQuality: 2-pixel SDF AA + 4-tap supersample inside the pixel shader
- Global default in Composition Settings ("Default shape AA")
- Per-layer override in Inspector (dropdown next to shape type)
- Serialization: optional field in .pmge, defaults to Smooth for old files
- ~40 LOC change: enum + shader branch on cb.params[3] + UI dropdown +
  serialization roundtrip

### 4.2 [POLISH] Anchor point too big, looks cartoonish — S
User: "the anchor point is looking like cartoonish, the anchor point is
big needs to be very small tiny dot"

Currently it's a filled circle at ~12 px radius. Change to:
- 4 px radius filled dot with a 1 px white outline
- Or the AE crosshair-style: small `+` symbol, 8 px total

### 4.3 [MISSING] Motion path — trace anchor's animated path — M
User: "when we set keyframes from from one to another, the anchor should
trace a thin line like its going from this to this like a path"

AE draws a "motion path" — a spline connecting all position keyframes,
with dots along it representing per-frame samples. Amazing feedback
tool for adjusting bounce arcs visually.

Concretely for us:
- When a layer with Position keyframes is selected AND we're in Bars
  mode of the timeline (or always?)
- Sample Position at every frame from inPoint to outPoint
- Draw a polyline through those samples on the canvas overlay via
  ImDrawList

Bonus: dot every N frames, larger dot at each keyframe location.

### 4.4 [MISSING] Rectangle bounding box needs mid-side handles — S
User: "when i drag the shape it has only 4 points for draggable its okay
for ellipse but not for rectangle at some point it will have midpoint
it will be easier"

Add 4 mid-side square handles (top-center, right-center, bottom-center,
left-center). Corners scale both axes; mid-handles scale only one axis.
AE standard.

### 4.5 [BUG] Edge flickering during drag hold — M
User: "when dragging is on hold there is weird flickering of the shape
from edges when"

Sub-pixel positions during drag jitter the SDF's `fwidth` gradient
between frames. Fix: either round position to integer pixels during
drag (loses precision but eliminates flicker), or ensure the SDF has a
constant-width AA band regardless of sub-pixel offset.

Actually — could also be a snapshot-timing thing. Every mouse drag
frame calls `MarkForSnapshot()` which snapshots to the undo stack;
if the transform recomputes twice per frame, the shape flickers.
Would need to trace with a repro.

### 4.6 [BUG] Scale link toggle should equalize X/Y when turned on — S
User: "if i adjust x or y value and then toggle the link button then the
x or y should be equal to each other and the total scale should work
without the toggle button"

**RESOLVED as no-op (2026-07-25).** On re-read with user, they confirmed
they want pure AE behavior: link toggle PRESERVES the current X:Y:Z
ratio (does NOT snap-equalize on toggle). Current implementation in
DrawInspectorPanel at RenderEngine.cpp:1576 already does this — ratio
preservation via applyLink() lambda. No code change needed.

Original feedback likely meant "when I click link and then drag one
axis, all three should update together" — which is exactly what pure
AE does. The word "equal" in the report was ambiguous.

If a future user actually wants Photoshop-style snap-to-equal on
toggle, that's a 1-line addition to the linkedScale=!linkedScale
handler: `if (linkedScale) { scl.y = scl.x; scl.z = scl.x; sel->transform.scale.SetValue(t, scl); }`.

---

## 5. Not raised but should probably ship alongside

- **Undo memory cap** — deque is unbounded (from earlier backlog)
- **Better Drop Shadow defaults** (Distance 20 / Angle 135 / Softness 8 /
  Opacity 0.75) so first-drop is visible

---

# Proposed priority for next session(s)

## Quick wins (all could fit in ONE session, ~2 hr total)
1. Speed graph handle hit-test fix (1.1) — S
2. Text stroke ghosting fix (2.4) — S
3. Scale link equalization (4.6) — S
4. Anchor point cosmetic (4.2) — S
5. Rectangle mid-side handles (4.4) — S
6. Better Drop Shadow defaults — S

## Medium tasks (each is its own session)
7. **Split Layer tool** (3.2) — M
8. **Motion path overlay** (4.3) — M
9. **Drop Shadow "shape vanishes" bug** (2.2) — S to M, needs shader debug
10. **Edge flickering during drag** (4.5) — M, needs repro

## Big architectural tasks (design doc required first)
11. **AE-style property twirl-down in timeline** (3.1) — L, biggest UX unlock
12. **Effect param animation** (2.1, 2.3) — L, unlocks real effects work
13. **Alight-Motion per-segment curves in Bars mode** (1.4) — L, only after 11

## Backlog polish
14. Easy Ease preset + F9 shortcut (1.3) — M
15. Effect bar visual differentiation (3.3) — after 11
16. Anti-aliasing quality pass (4.1) — M, investigate first
