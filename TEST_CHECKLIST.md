# Test Checklist — Does It Actually Work?

**Build under test:** commit `64241a0` (code identical to `888d409` — Task 5.0-a)
**Download page:** https://github.com/cosmicrages0/potato-motion-editor/actions/runs/29897802625
**Artifact:** `MotionGraphicsEditor-x64-Windows` (1.05 MB, x64)

## How to use this checklist

For each row: launch the app, do the exact steps, mark the result.

- **[ ]** = not tested yet
- **[✓]** = works exactly as described
- **[⚠]** = works but has a quirk (write what)
- **[✗]** = broken (write what happens instead)

Send me back the marked-up copy (or a screenshot) and I'll fix everything marked ⚠ or ✗ in one commit.

---

## 1. First-launch sanity (5 tests, 60 seconds)

| # | Step | Expected result | Result |
|---|---|---|---|
| 1.1 | Extract the zip, double-click `MotionEngine.exe` | Window opens **maximized** (fills your screen), title says "Potato Motion Graphics Editor - x64" | [ ] |
| 1.2 | Look at the layout | Five panels visible: **Project Assets** (left), **Composition Viewport** (center, biggest), **Inspector & Effects** (right), **Timeline** (bottom-left), **Graph Editor** (bottom-right) | [ ] |
| 1.3 | Look at the Composition Viewport | Dark background with a **light-colored rectangle in the middle** (that's the 1920×1080 composition with letterbox bars). Inside it: a dark violet rectangle + a cyan circle stacked at the center | [ ] |
| 1.4 | Look at top-left of the viewport | HUD text reads `Canvas 1920 x 1080   FOV=45.0   Cam=(960, 540, -1000)` | [ ] |
| 1.5 | Look at the Timeline table (bottom-left) | Two rows: `Background Rect` and `Bouncing Ball`, both with visibility checkboxes checked, Type column showing "Rectangle" / "Ellipse" | [ ] |

---

## 2. Add shapes (4 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 2.1 | In the Timeline panel, click **`+ Rect`** | A new rectangle appears **at the center of the composition** (inside the letterboxed area) | [ ] |
| 2.2 | Click **`+ Ellipse`** in Timeline | New ellipse appears at composition center | [ ] |
| 2.3 | Click **`+ Null`** in Timeline | A gray **X marker** appears at center (Null Object) | [ ] |
| 2.4 | Click **`+ Camera`** in Timeline | A new "Camera" row appears in Timeline table (no visible shape — camera is a 3D-only construct) | [ ] |

---

## 3. Select + move + scale (5 tests — this is where the gizmo quirks show up)

| # | Step | Expected result | Result |
|---|---|---|---|
| 3.1 | Click on the cyan ball in the viewport | A **green bounding box with 4 yellow corner handles + red center dot** appears around it | [ ] |
| 3.2 | Click and drag the center red dot | Ball moves smoothly under your cursor | [ ] |
| 3.3 | Click and drag a yellow corner handle | Ball scales larger/smaller. **Warning:** may jump wildly near the anchor (known bug) | [ ] |
| 3.4 | Click on the dark violet rectangle behind the ball | Selection switches to the rectangle | [ ] |
| 3.5 | Press `Delete` key with a layer selected | The selected layer is removed from the scene | [ ] |

---

## 4. Inspector tabs + values (4 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 4.1 | Select any layer, look at Inspector panel (right) | See layer name field, "Layer ID: N   Parent ID: -1", then **three tabs**: `[Transform] [Effect Controls] [Global]` | [ ] |
| 4.2 | On Transform tab, type new numbers into "Position (x,y,z)" | The shape immediately moves on the canvas | [ ] |
| 4.3 | On Transform tab, look for **orange dot** buttons on left of Position / Rotation / Scale / Opacity | Four small orange/gray circular buttons visible (stopwatches) | [ ] |
| 4.4 | Click the **Global** tab | Shows "Composition Clock" (Play/Reset/Loop/Duration/Time), "Slingshot Bezier Handles", "Camera Properties" | [ ] |

---

## 5. Real animation (the "can I actually animate" test — 6 steps in one workflow)

| # | Step | Expected result | Result |
|---|---|---|---|
| 5.1 | Select the cyan ball. On Transform tab, click the **orange dot next to Position** | Dot lights up bright orange (stopwatch is now ON) | [ ] |
| 5.2 | On Global tab, drag "Time (s)" slider to `0.000` (or hit Reset) | Playhead in Timeline strip goes to the far left | [ ] |
| 5.3 | Go back to Transform tab. Note the ball's current position (say 960, 540) | Position field reads (960, 540, 0) | [ ] |
| 5.4 | Drag Time slider to `1.000` (or drag the playhead in the timeline strip to the right end) | Playhead moves to the far right | [ ] |
| 5.5 | Change Position X to `1500` (drag or type) | Ball jumps to the right side of canvas. In Timeline strip you should see **two blue diamonds** on the Bouncing Ball row | [ ] |
| 5.6 | On Global tab, click **Play** | Ball animates from (960,540) at t=0 back to (1500,540) at t=1, loops. **This is the animation working.** | [ ] |

---

## 6. Effects visibly apply (3 tests — these confirm Task 5.0 wiring)

| # | Step | Expected result | Result |
|---|---|---|---|
| 6.1 | Select the cyan ball. Open Inspector → Effect Controls tab (or the standalone "Effect Controls" panel if visible) | Says "No effects. Add one from the Effects Palette." | [ ] |
| 6.2 | Click the "Effects Palette" tab (docked with Project Assets on the left). Click **Chromatic Aberration** | Ball on canvas now shows **red/green/blue channel offset** — a rainbow fringe around its edge. HUD in viewport should show `[FX ON]` | [ ] |
| 6.3 | Add **Directional Motion Blur**, set Intensity to 30 | Ball now looks visibly smeared along the angle axis | [ ] |

---

## 7. Timeline scrubbing + keyframe diamonds (3 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 7.1 | With animation set up from step 5, look at the Timeline strip | You see a horizontal time ruler with `0.00s / 0.50s / 1.00s` markers, a red vertical playhead, and colored diamonds on the animated layer's row (blue for position keys) | [ ] |
| 7.2 | Click and drag the red playhead left/right | Playhead moves, and if animation is set up, the ball moves accordingly on the canvas. Playback pauses when you start scrubbing | [ ] |
| 7.3 | Try to right-click a diamond in the strip | **Nothing happens** (no context menu implemented yet — known missing feature) | [ ] |

---

## 8. Export path (5 tests — the FFmpeg workflow)

| # | Step | Expected result | Result |
|---|---|---|---|
| 8.1 | Menu bar → **Export → Render Queue** | A "Render Queue" floating window opens with preset dropdown, width/height, fps combo, bitrate slider, duration slider, output path field, FFmpeg path field | [ ] |
| 8.2 | Click the **Test FFmpeg** button | Either: green "FFmpeg OK: ffmpeg version..." OR red "FFmpeg problem: No output..." with an orange install hint | [ ] |
| 8.3 | If red: install ffmpeg from https://ffmpeg.org/download.html (Windows builds), unzip somewhere, and put the FULL path to `ffmpeg.exe` in the "FFmpeg path" field. Test again | Should now show green | [ ] |
| 8.4 | With green ffmpeg: set duration to 3 seconds, output path to something like `C:\Users\YOU\Desktop\test.mp4`, click **Start Export** | Progress bar appears, ticks up frame by frame, shows "Frame X / 90" and ETA | [ ] |
| 8.5 | When export finishes, open the output MP4 in VLC or any player | Video plays. Shows your actual composition (shapes and any effects) at 1080p 30fps for 3 seconds | [ ] |

---

## 9. 3D camera (4 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 9.1 | In the viewport, click and hold **right mouse button**, drag around | Camera orbits around the composition (LookAt target). Shapes appear to swing around in space | [ ] |
| 9.2 | Click and hold **middle mouse button**, drag | Camera pans (both eye and target move together) | [ ] |
| 9.3 | Scroll the mouse wheel over the viewport | Camera dollies in / out | [ ] |
| 9.4 | Select any layer, check the `[3D]` column checkbox in the Timeline table | Layer flips into 3D mode. Should now respond to camera moves with perspective (foreshortening) | [ ] |

---

## 10. Parenting + Null Objects (3 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 10.1 | Add a Null (`+ Null` in Timeline). In the Timeline table, find the **Parent** dropdown for the Bouncing Ball row. Set it to the Null | Ball is now parented to Null | [ ] |
| 10.2 | Select the Null layer, drag its center handle | The Ball moves along with the Null (following its parent) | [ ] |
| 10.3 | In the Ball's Parent dropdown, try to set the Ball's parent = itself | Option is **grayed out** (cycle detection) | [ ] |

---

## 11. Alight Motion XML importer (2 tests, optional)

| # | Step | Expected result | Result |
|---|---|---|---|
| 11.1 | Create an `import.xml` file next to the .exe with content like:<br>`<keyframe time="0.5" value="1.2" curve="0.42,0.0 0.58,1.0" />`<br>Then Menu → **File → Import Alight Motion .xml** | The Graph Editor now shows a Bezier curve with those P1/P2 handles | [ ] |
| 11.2 | Without an `import.xml` file, try the same menu item | Silently fails (check the console if launched from cmd for `[Import] Could not open file: import.xml`) | [ ] |

---

## 12. Persistence + resets (2 tests)

| # | Step | Expected result | Result |
|---|---|---|---|
| 12.1 | Rearrange the docks (drag a panel to a different edge). Close the app. Relaunch | Layout is preserved (from `imgui.ini` next to the exe) | [ ] |
| 12.2 | Try Menu → **File → Save** | **Nothing happens** (save/load not implemented — known missing feature) | [ ] |

---

## 13. Menus that are still stubs (write "stub" for each)

| # | Menu item | Expected | Result |
|---|---|---|---|
| 13.1 | File → New Composition | stub | [ ] |
| 13.2 | File → Open... | stub | [ ] |
| 13.3 | Edit → Undo | stub | [ ] |
| 13.4 | Edit → Redo | stub | [ ] |
| 13.5 | Composition → Composition Settings... | stub | [ ] |

---

## 14. Things you should NOT be able to do (edge cases)

| # | Try this | Should NOT do this | Result |
|---|---|---|---|
| 14.1 | With no layer selected, try to add an effect via `Effect` menu | Menu items are grayed out (disabled) | [ ] |
| 14.2 | Delete all layers, then try to click on the viewport | No crash; no selection highlights | [ ] |
| 14.3 | Set composition duration to 0.1s in Global tab | Should clamp / not crash; timeline strip stays usable | [ ] |
| 14.4 | Rapidly click Play/Pause many times | No crash | [ ] |

---

## Summary

- **Total tests:** 47
- **Time to run all:** ~15 minutes
- **What I need from you:** the count of ✓ / ⚠ / ✗ per section, and a couple sentences on any ⚠ or ✗ so I know what to fix

**If you want to be faster:** just run **Sections 5 (animation), 6 (effects visible), 8 (export)** — those are the three that Task 5.0 and 5.0-a were supposed to fix. If those three work, the rest of the app almost certainly does too.

---

*Test any additional workflows I forgot? Add rows to Section 15 below with your own steps.*

## 15. Additional tests you invented (open row)

| # | Step | Expected | Result |
|---|---|---|---|
| 15.1 | | | [ ] |
| 15.2 | | | [ ] |
