# Test feedback triage — Session 3, Task 5.14 twirl-down build

Full user feedback verbatim, then triaged into buckets. Items 1 and 7 are
**real bugs** that must fix now. Items 2-6 and 8-10 are a coherent set of
UX changes that redefine how the property twirl-down should behave — this
is a v2 design refinement, needs planning before code.

Item 11 was a duplicate paste of items 1-10 — no new content.

---

## BUGS (fix now)

### B1 — Selection sticks after clicking on empty canvas (item 1)
User: "when open or first layer is added the bounding comes its okey but
the bounding box and anchor doesnt go away when clicked randomly
anywhere, i have to add another shape to remove the selection of first
layer"

**Diagnosis:** The viewport canvas has no "click-in-empty-space
deselects the selected layer" handler. When there's only 1 layer,
clicking outside its bounds hits nothing that clears
`layerManager.SetSelectedId(-1)`.

**Fix scope:** small (~20 LOC). Add to `HandleGizmoInteraction` or the
viewport hit-test region: if `IsMouseClicked(Left) && !hovered_any_layer
&& !hovered_any_gizmo` → deselect. Standard AE behavior.

**Priority:** ship in the next commit — it's obviously wrong.

---

### B2 — Property stopwatch off ≠ animation stops (item 7)
User: "when i was doing the rotation i then felt its not necesccary then
i clciked the stopwatch to remove the keyframes it did remove the
keyframes but the the ball was selected when played the roation was
still happening then i thought lets make another layer and the
selection was on rectangle then i again went back to circle to test
again and played it ran without which i was wanted so thats the bug"

**Diagnosis:** In `AnimatedProperty::ToggleStopwatch`, turning
stopwatch OFF should either:
- (option A) Clear all keyframes so evaluation returns `staticValue`,
  OR
- (option B) Bake the CURRENT evaluated value into `staticValue` so the
  layer "freezes" at where it was.

Currently ToggleStopwatch clears the keyframes but the `staticValue`
still holds whatever it was before the stopwatch was FIRST turned on
(possibly the identity value like `(0,0,0)` or the layer's initial
value). Combined with a race condition where the layer keeps its
"last evaluated" value cached somewhere.

Actually — user says "keyframes did remove" — so ToggleStopwatch IS
clearing. Then why does rotation still happen at playback? Let me
re-read:

> "clciked the stopwatch to remove the keyframes it did remove the
> keyframes but ... the rotation was still happening"

So keyframes.empty() is true after toggle, `stopwatchEnabled` is
false, but rotation still applies during playback. Most likely
cause: `staticValue` was overwritten during a prior SetValue call
while stopwatch was on. When we killed the stopwatch, the last
keyframe's value became the frozen static, which was the rotation
end-state.

Actually — that's not it either. If keyframes are empty and
stopwatch is off, `Evaluate()` returns `staticValue`. If
staticValue happens to equal identity, no rotation. If it holds a
rotated pose, that pose persists but doesn't ANIMATE — the ball
would be at a fixed rotated angle, not rotating.

So maybe the real bug is deeper — maybe ToggleStopwatch isn't
actually clearing keyframes on the second toggle. Or maybe the
Rotation property has separate `.z` component keyframes that don't
get cleared.

Then user says "made another layer, selection was on rectangle,
went back to circle, played" — the circle STILL rotates. That
confirms it's not a UI staleness issue — the circle's rotation
property literally still has keyframes.

**Repro required.** Ask user to save that .pmge and share. Meanwhile
inspect `ToggleStopwatch` in AnimatedProperty.h.

**Priority:** must fix now. Blocks real work. Small (~15 LOC once
root cause is confirmed).

---

## DESIGN CHANGES (need discussion + probable design doc)

The heart of the feedback: **the current twirl-down is too eager**.
It shows all 6 Transform sub-rows and all 5 DropShadow param rows the
moment the user opens Transform / Effects. User wants **lazy
population** — only properties that have been TOUCHED (edited or
stopwatched) appear in the timeline.

This is closer to what AE actually does. AE's property list is
lazy: a property doesn't appear in the timeline until you keyframe
it or explicitly reveal it (`U` key = show animated props, `UU` =
show modified props).

### D3 — Editable value in sub-row (item 2)
User wants clicking the value readout to make it editable
(drag-float or type-in-place).

Ships as inline drag-float widget per sub-row. This was the
"~200 LOC scope creep" DeepSeek flagged for v1 — user is now
explicitly asking for it, so it's approved for v2.

### D4 — More properties get keyframe/stopwatch (item 3)
User: "fill, stroke, size, anchor should also have keyframe
button in properties panel and their stopwatch in the timeline"

- **Anchor** and **Size** already have stopwatches (they're
  `AnimatedProperty` — I just added them to the timeline in
  Task 5.14).
- **Fill color** is currently `unsigned int fillColor` (NOT an
  AnimatedProperty). Needs conversion to `AnimatedProperty<Vec4>`
  or `AnimatedProperty<unsigned int>`.
- **Stroke color** and **stroke width** similar.

Scope: real work. Layer.h changes + Serialization changes +
Inspector UI + timeline sub-row rendering + Evaluate() calls in
CompositionRenderer where fillColor is currently read.

This is closer to a Commit 20-scope task ("effect param animation
+ related property-animation gaps"). Deserves its own commit or
sub-commit.

### D5 — Timeline label column layout (item 4)
User wants: `[eye][arrow][blend mode][layer name][fx icon]`

Currently we have: `[twirl][eye][name][parent combo][fx chip]`.

User wants **BlendMode selector directly in the label column**
(currently only in Inspector via effects). "Arrow" is the twirl.
No mention of Parent combo — either remove it from strip or move
it to the Inspector's parent selector.

Also — the ORDER user wants:
1. eye icon (Vis toggle) — currently 2nd, user wants 1st
2. arrow (twirl) — currently 1st, user wants 2nd
3. blend mode dropdown — currently absent from strip
4. layer name — currently 3rd
5. fx icon — currently 5th, keep

Priority: small UX polish. ~30 LOC to swap order + add blend mode
combo. Parent combo removal separate decision.

### D6 — Lazy property revelation (item 5)
User: "below layer dropdown if something is clicked or value has
changed in properties panel like position, scale, rotation, opacity,
size, anchor these all come in transform so it will be layer name
-> transform -> scale"

This is the **big design change**. Instead of "twirl Transform
opens all 6 property rows always," it should be:

- Twirl Transform → shows sub-heading "Transform" but no property
  rows unless one has been touched
- User clicks a property in Inspector (say Scale) → Scale sub-row
  appears under Transform
- User keyframes another property (say Rotation) → Rotation sub-row
  ALSO appears

Effectively: `expandedProperties` per layer = the SET of
properties currently "revealed." Toggling stopwatch → adds to set.
Editing a value in Inspector (even without keyframing) → adds to
set. Right-click a revealed row → "Hide Property" removes from set.

AE's actual behavior:
- Default state: only Transform's sub-heading shows, no
  properties revealed yet
- Touch a property → it becomes "revealed"
- `UU` shortcut = show ALL modified properties (rewrites the
  revealed set to = touched properties)

### D7 — Same for effect params (item 6)
User: "there is not keyframe parameter or clickable stopwatch of
effects, only those parameter will show down in the timeline whose
is clicked or changed"

Same lazy-reveal model but for effect params. Twirl an effect
open → shows just the effect header. Touch a param → param row
appears. Right now we show all params always which is noisy.

Also: user wants effect params to have their OWN stopwatch. That's
D3 for effects — the Commit 20 work on backlog. This is closer to
"add per-param stopwatch UI in the Inspector's effect controls
panel AND wire it into a real AnimatedProperty per param."

### D8 — Graph icon lazy visibility (item 8)
User: "the property selection of the graph now, it should be like
when i clicked on the stop watch it the graph of the property which
has changed will appear when graph icon just like fx is toggle when
the stop watch is not selected the graph icon does not show or like
be less opaque"

Per-property "open in graph editor" button that only lights up when
that property has been touched. Consistent with the lazy-reveal
model.

Right now the graph editor has a global "which property" dropdown.
User wants per-property affordance.

### D9 — Graph editor layout modes (item 9)
User: "the graph will show in two ways, 1 hiding the right bars and
show graph with left side, 2 the graph will take full space when
toggled our current version"

Current: graph mode REPLACES the timeline strip (full-width). User
wants an ADDITIONAL layout where the graph shows to the RIGHT of
the timeline strip (both visible side-by-side, splitter between
them).

Nice-to-have. Not urgent. Adds a mode toggle to the bottom dock's
toolbar.

### D10 — Speed Graph vs Value Graph review (item 10)
User: "ask this graph part to deepseek once again, see this first,
also speed graph and value graphs are on ae and how to replicate
full logic into our project"

Already researched in the last session (see
TEST_FEEDBACK_2026-07-24_BOUNCING_BALL.md item 1.2 which was
updated post-web-search). Our Speed Graph math is correct;
per-property routing needs the D6/D8 changes above to feel right.

Second review pass: reasonable to send DeepSeek the updated design
before shipping the graph editor rework.

---

## Grouping into commits

Two paths forward. Both fix B1 + B2 IMMEDIATELY as one small hotfix
commit, then diverge:

### Path A: Ship bugfixes now, then design doc for v2 lazy-reveal
1. **Commit 18 (hotfix):** B1 (deselect) + B2 (rotation bug) —
   ~30 LOC, one commit, ship fast.
2. **DESIGN_COMMIT19:** full lazy-reveal design (D3, D5, D6, D7,
   D8) + graph editor rework (D9). Send to DeepSeek before
   implementing.
3. **Commit 19:** implement approved design.
4. **Later:** D4 (fill/stroke color as AnimatedProperty) as a
   commit on its own.

### Path B: Ship bugfixes now, then keep rolling with the original roadmap
1. **Commit 18 (hotfix):** B1 + B2 (same as Path A).
2. **Commit 19:** Split Layer + motion path overlay (original
   roadmap).
3. **Later:** lazy-reveal + property enhancements as a
   dedicated design pass.

**Path A** matches what the user actually asked for. Path B stays
loyal to the earlier locked roadmap. I recommend Path A because
the twirl-down UX in front of the user right now is measurably
inferior to what they want — shipping the roadmap without the
polish means the twirl feature stays half-done for weeks.
