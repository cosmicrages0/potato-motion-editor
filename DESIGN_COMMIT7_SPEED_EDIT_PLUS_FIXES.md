# Design Doc — Commit 7 (Task 5.4-fix-2): Speed-graph editable + UX fixes

**Base commit:** `5510244` (Task 5.4-fix)
**LOC delta estimate:** +90 / -25
**User-visible changes:**
1. Speed graph handles are now **draggable** — same data as Value graph,
   different Y axis.
2. Graph Y bounds **auto-expand** when a handle is dragged past the current
   top/bottom edge (AE behavior — no more handles disappearing outside
   the panel).
3. Top-left "value / speed" label no longer overlaps the numeric Y-axis
   labels.
4. Removed "F9" from the Easy Ease menu entry — Windows F-lock on some
   laptops intercepts F9 and locks the screen. Menu-only for now.

**No changes to storage or serialization.** No new dependencies.

---

## 1. Speed-graph editable (main feature)

### The rule (from user's fix spec)

- Value mode: handle Y = `key.value + speed * dt * inf/100`
- Speed mode: handle Y = `|speed|` (magnitude)

Both graphs read the SAME `(speed, influence)` fields — only the Y-axis
interpretation of the handle differs.

### Dragging in Speed mode

Given mouse (mx, my) and the active key at time `kt`:

```
dtSeg      = |neighbor.time - kt|                     // positive
newInfluence = clamp(|mx - kt| / dtSeg * 100, 0, 100)  // horizontal is always influence

if Speed mode:
    newSpeedMag = max(my, 0)                          // Y in speed mode IS magnitude
    // For scalar (float) speed:
    //   Sign is preserved from the old speed (so pulling up on a
    //   negative-speed handle stays negative).
    // For vector (Vec2/Vec3) speed:
    //   Preserve direction: newSpeed = normalize(oldSpeed) * newSpeedMag
    //   Zero-length oldSpeed edge case: use unit vector along +value delta
    //   to next/prev key, else all-zero (handle stays flat).
else:
    // Existing Value-mode logic unchanged.
```

### Shift / Alt modifiers extend to Speed mode

Same rules as Value mode:
- **Shift** = influence only (Y pinned to current speed in Speed mode)
- **Alt** = speed only (X pinned to current influence)

### AutoBezier / Hold / Linear

Same as before: AutoBezier handles are dimmed, non-draggable in either
mode. Hold and Linear have no handles.

### ContinuousBezier mirroring works in Speed mode too

When dragging one handle on a `ContinuousBezier` key, we mirror
influence + speed vector (sign-flipped) to the other side. Logic already
exists; just needs to fire from the Speed-mode drag branch as well.

---

## 2. Auto-expand Y bounds

Current code computes `yMin`/`yMax` from sampled data plus 10% pad, then
those bounds stay fixed for the frame. When the user drags a handle above
`yMax`, the handle disappears out the top of the panel.

Fix: after computing `yMin`/`yMax` from data, also compute the screen Y
that each visible handle would occupy, and expand bounds if any handle is
outside the current range. Applies to Value mode (handle Y = value+offset)
and Speed mode (handle Y = |speed|).

Cheap because we already build handle positions each frame; we just do it
BEFORE deciding the final Y range instead of after.

### Sketch

```cpp
float yMin, yMax = compute from samples...

// Peek at handles the user is about to interact with (selected key).
if (graphSelectedKeyIndex >= 0) {
    for each dim in group:
        if key.outgoingMode is any Bezier flavor and next key exists:
            hy = compute handle Y (mode-dependent)
            yMin = min(yMin, hy); yMax = max(yMax, hy);
        // same for incoming
}
// Then pad + render.
```

Applies bounds expansion only to the selected key's handles — non-selected
handles aren't drawn, so no point expanding for them.

---

## 3. Top-left label overlap fix

Current: mode label ("value" / "speed (u/s)") renders at
`(canvas_p0.x + 4, canvas_p0.y + 2)`. The topmost Y-axis numeric label
(from the grid loop) also renders at `(canvas_p0.x + 4, y_of_top_grid - 7)`
where `y_of_top_grid` is very close to `canvas_p0.y + padT = canvas_p0.y + 12`.
That's a 10-pixel vertical stack — too close, so the two texts touch.

Fix: shift mode label right past the reserved axis-label column.
New position: `(canvas_p0.x + 48, canvas_p0.y + 2)` — clears the ~44 px
padL used for axis labels. The dim legend that follows already starts at
`canvas_p0.x + 44` on a separate downstream line, so no cascade.

Actually cleaner: put the mode label INSIDE the graph area (top-right of
plot area). Doesn't collide with anything, matches AE (they show mode
name inside the plot).

Going with the top-right-inside-plot placement.

---

## 4. F9 label removal

Current context menu item: `"Set to Bezier (F9 Easy Ease)"`. My code
never bound F9 as a keyboard shortcut — the label was aspirational. But
Windows laptops with an F-lock feature (Lenovo, HP, Dell all do this)
intercept F9 as "sleep" or "lock screen" at the OS level, so a user
hitting F9 expecting the label promise gets their session locked.

Change: menu label becomes `"Set to Bezier (Easy Ease)"`. No shortcut
bound. If we later add a shortcut it will be `Ctrl+Alt+E` (Blender
convention) after user confirmation.

---

## 5. Files changing

```
src/RenderEngine.cpp   MODIFIED  +90 / -25   Speed-mode drag branch,
                                              auto-expand Y bounds,
                                              mode-label reposition,
                                              context menu label edit
```

No header, storage, or serialization changes.

---

## 6. Test plan

1. **Speed graph drag** — set two keys, both Bezier. Switch to Speed
   mode. Select a key. Yellow handle now sits at speed height. Drag it
   up → speed magnitude grows. Switch back to Value mode → curve is
   steeper.
2. **Shift-drag in Speed mode** — only slides X (influence changes).
3. **Alt-drag in Speed mode** — only slides Y (speed changes).
4. **Auto-expand** — drag handle up until it would leave the panel;
   graph rescales so the handle stays visible.
5. **Top-left label** — no visual overlap with Y-axis numbers on any
   panel size.
6. **Menu label** — right-click a key: text reads "Set to Bezier (Easy
   Ease)". No F9 text anywhere.
7. **Backward compat** — every existing Value-mode test still passes;
   files load and save unchanged.

---

## 7. Go

Say **"go single commit"** to execute.
