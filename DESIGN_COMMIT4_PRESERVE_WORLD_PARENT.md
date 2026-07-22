# Design Doc — Commit 4 (Task 5.3-fix-2): Preserve-World-On-Parent

**Base commit:** `8ac2d4b`
**LOC delta estimate:** ~+80 / -5
**User-visible change:** parenting a layer no longer causes it to jump. Matches AE and Alight Motion.

---

## 1. The bug in plain words

You said (paraphrased):
> "I parented a rectangle to a Null and the rectangle jumped way off canvas. In Alight, when I parent, the child stays put. Only when I move the parent afterward does the child move."

You are right. That's how AE and Alight both work. **Ours is wrong.**

## 2. Why ours does the wrong thing today

`LayerManager::SetParent(child, parent)` just writes `child.parentId = parent`. The renderer then computes `world = parent_world × child_local` — because both had `position ≈ (960, 540)`, the multiplied result places the child at approximately `(1920, 1080)`, way off canvas.

## 3. What AE actually does — confirmed by research

From an AE community answer I dug up (video.stackexchange 17615):
> "When you parent a layer to another layer its position becomes relative to its parent. So if for example you have a null at [960,540] and you parent it to a layer whose position is also [960,540] the null's position now becomes [0,0]."

That's the "preserve world on parent" behavior. When AE changes the parent, it *automatically rewrites the child's own position/scale/rotation values* so the child's world transform doesn't change.

Two other AE sources confirmed the math extends to scale and rotation:
- world scale: `child.scale × parent.world_scale`
- world rotation: `child.rotation + parent.world_rotation`
- full: `child_world_matrix = parent_world_matrix × child_local_matrix`

To preserve world on re-parenting, you compute:
```
oldWorld       = old_parent_world × child_local
newChildLocal  = inverse(new_parent_world) × oldWorld
```

## 4. Concrete plan

Add a new helper method on `LayerManager`:

```cpp
// Task 5.3-fix-2: change a child's parent while preserving its VISIBLE world
// transform. Matches AE / Alight Motion behavior. Rewrites the child's
// authored position / rotation / scale so old_world == new_world.
//
// Special cases:
//   * newParentId == -1 (detach): rewrite child so its authored values ARE
//     its previous world values.
//   * old parent was -1 (re-parent from world): rewrite child so its authored
//     values become the local values inside the new parent's frame.
bool SetParentPreservingWorld(int childId, int newParentId);
```

Existing `SetParent(childId, parentId)` stays as-is for callers that WANT the raw naive behavior (none in the current codebase, but keeping it lets a future "keep local, ignore world" toggle just call the raw version).

The parent dropdown in the Timeline calls `SetParentPreservingWorld` instead of `SetParent`.

## 5. The math

Working in 2D (the 2D path is what users interact with; 3D reparenting stays naive for now and we mark it as such).

Given:
- `child.authored` = the AnimatedProperty values written by user
- `oldWorld` = the child's current world 3×3 matrix (`layerManager.GetWorldMatrix(childId)` at current comp time — the animation-baked world position that's on screen right now)
- `newParentWorld` = the world matrix of the new parent (Identity if `newParentId == -1`)

Solve for `newChildLocal = inverse(newParentWorld) × oldWorld`. Then decompose `newChildLocal` into position, rotation (Z-axis only in 2D), and scale. Write those values into `child.transform.position/rotation/scale` — as `staticValue` if the corresponding stopwatch is OFF, or as a keyframe at the current comp time if the stopwatch is ON (so the "preserve world" happens at THIS frame; earlier keyframes stay where they were, which is what AE does).

### 2D affine decomposition

For a 2D affine matrix `M = [[a, b, tx], [c, d, ty], [0, 0, 1]]`:
```
translate = (tx, ty)
scaleX    = sign(a) * sqrt(a*a + c*c)
scaleY    = sign(d) * sqrt(b*b + d*d)
rotation  = atan2(c/scaleX, a/scaleX)   in radians -> convert to degrees
```

This works cleanly for any TRS matrix (no shear). Since AnimatedProperty stores angles in degrees, convert.

Edge cases:
- Degenerate parent (scale=0): `inverse` returns identity per `Mat3::InverseAffine`. Child ends up at its old world values with no scale — best we can do.
- Scale contains a sign flip: preserved via the `sign(a)` and `sign(d)` guards.
- Anchor point / size: NOT recomputed — those aren't part of the parenting chain math. Only position/rotation/scale.

## 6. Small side change: Null default position

Now that reparenting preserves world, the Null's spawn location is a UX choice, not a correctness one. I'll leave Null spawning at canvas center (960, 540) since it's the same rule as every other shape — user sees the marker immediately, drags/re-parents freely, no jumps.

## 7. Files changing

```
src/LayerManager.h    +5   declaration of SetParentPreservingWorld
src/LayerManager.cpp  +55  implementation (world sample, decompose, write)
src/RenderEngine.cpp  ~10  parent dropdown call sites now use ...PreservingWorld
                           + MarkForSnapshot before it (which they already do)
```

That's it. No new files, no schema changes, no breaking anything else.

## 8. Test plan

1. Add Rectangle (spawns at 960, 540). Add Null (also at 960, 540).
2. Set Rectangle's parent = Null. **Rectangle should stay put visually.** ← this is the fix.
3. Drag the Null via its Inspector Position field. Rectangle follows offset-by-offset.
4. Scale the Null in Inspector. Rectangle scales visibly.
5. Un-parent the Rectangle (Parent → (none)). **Rectangle should stay put visually.**
6. Ctrl+Z should undo the un-parent AND undo re-parent (each was a single snapshot before the mutation).
7. Every existing test (animation, effects, save/load) should still pass.

## 9. What deliberately does NOT change

- 3D layer re-parenting stays naive for now (3D matrix decomposition is more involved and 3D isn't the current focus; I'll add a `// TODO: 3D preserve-world` comment and one-line safety fallback to `SetParent` for `is3D` layers).
- Parenting behavior when the CHILD has animated position (stopwatch on): the preserve-world happens at the current comp time only; earlier keyframes retain their authored values. This is AE-standard.
- No toggle for "naive parenting" — nobody has asked for it, we can add later.

## 10. Question before I execute

None. Say **"go single commit"** and I ship. If you want the `SetParent` (raw) accessible via Shift+drag or Alt-modifier, let me know now; otherwise raw stays internal to `LayerManager` only.
