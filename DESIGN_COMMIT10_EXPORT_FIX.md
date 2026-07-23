# Design Doc — Commit 10 (Task 6.1): Export Fix — CreateProcess + stderr log

**Base commit:** `9a4c137` (Task 5.7: strokes + rounded corners)
**LOC delta estimate:** +180 / -70
**User-visible change:** Export actually works when the composition
size differs from the preset. When it fails, you get the real ffmpeg
error message instead of `"encoder died?"`. Render Queue's Width /
Height fields are gone — export always matches the composition.

---

## 1. Root causes of the "pipe closed unexpectedly" bug

Three suspects, only one of which we can currently see:

### 1a. `_popen("wb")` on Windows only opens the C stream in binary mode

The underlying anonymous pipe handle can still translate `0x0A` (LF)
bytes in the pixel data to `0x0D 0x0A` if MSVCRT decides to. On raw BGRA
pixel streams this corrupts frame data and ffmpeg's `-f rawvideo`
demuxer aborts. Documented Microsoft quirk; the safer path is
`CreateProcess` with explicit `CreatePipe` handles whose
`fOverlapped=FALSE` + no text conversion in the child.

### 1b. Comp/export size mismatch

Comp is 1080×1920 (portrait). Export preset defaults to 1920×1080. We
render into a 1920×1080 export RT via `CompositionRenderer::RenderLayers`
— but shape positions are in comp coordinates (0..1080 X, 0..1920 Y).
`BuildShapeMVP` scales pixel positions relative to `targetW/targetH`, so
a shape at comp-x=900 renders at export-x=900/1920 ≈ 0.47 of the
1920-wide export → 0.47 × 1920 = 900. Actually **the shape position
survives**; what breaks is the shape SIZE ratio — a 100×100 rect in a
1080×1920 comp is 10% wide × 5% tall, but in a 1920×1080 export it
becomes 5% × 10%. Aspect skew.

Design fix: **export always matches the comp** (user decision, locked
above). Remove W/H fields from the Render Queue UI. Simplest, matches
AE ("Comp Settings drives export resolution"), kills the mismatch bug
entirely.

### 1c. We swallow ffmpeg's stderr

`_popen("wb")` gives us stdin only. ffmpeg's actual error message goes
to stderr, which we never read. So every failure looks like the generic
"pipe closed unexpectedly" message. **First fix priority** — the log
file will tell us if 1a or something else is the real killer.

---

## 2. Plan

### 2.1 Windows: CreateProcess instead of _popen

Rewrite `ExportEngine::Begin` and `WriteCurrentFrame` on Windows to use:

- `CreatePipe(&hChildStdIn_Read, &hChildStdIn_Write, &sa, 0)` — our
  side gets the WRITE handle.
- `CreateFile(logPath, GENERIC_WRITE, FILE_SHARE_READ, ...)` — file
  handle for ffmpeg's stderr.
- `STARTUPINFOA si; si.hStdInput = hChildStdIn_Read; si.hStdError = hLog;
   si.hStdOutput = hLog; si.dwFlags = STARTF_USESTDHANDLES;`
- `CreateProcessA(nullptr, cmdLine, ...)` with `CREATE_NO_WINDOW` so no
  spurious console pops up.

WriteCurrentFrame:
- Old: `fwrite(ptr, 4, W*H, ffmpegPipe_)`
- New: `WriteFile(hChildStdIn_Write, ptr, W*H*4, &bytesWritten, nullptr)`
  in a loop until the whole frame is delivered. Check return; if it
  fails, `GetLastError()` + read the tail of the log file into the
  error message.

End:
- `CloseHandle(hChildStdIn_Write)` (signals EOF to ffmpeg)
- `WaitForSingleObject(procHandle, 5000ms)`; if timeout, `TerminateProcess`
- `CloseHandle(hLog)`
- If failure, `ReadFile` the last ~2 KB of the log for status message

Handles + PROCESS_INFORMATION stored on `ExportEngine` as private members
inside a `#ifdef _WIN32` guard so the class stays portable (the current
`FILE*` field goes away on Windows; kept as fallback for non-Windows
builds if we ever have them, but for now Windows-only is fine — this
IS a Windows-only editor).

### 2.2 stderr → log file

Always. Path = `<output_dir>/ffmpeg_last.log`. Truncated on each Begin.
On failure, `status_.errorMsg` becomes:

```
FFmpeg encoder failed. See log for details:
  <path>\ffmpeg_last.log
Last 4 lines:
  <tail>
```

That way the user sees the money quote in the UI AND has the full log
for deep dives.

### 2.3 Render Queue UI: remove W/H fields

Task 5.6 already made comp resolution user-editable in the Composition
Settings modal. Having a SECOND resolution setting in the export dialog
was redundant and a footgun (this bug). Replace the W/H inputs with a
read-only display: `"Resolution: 1080 × 1920 (from Composition
Settings)"`. `pendingExport.width/height` still exists in the struct
but is overwritten from `compositionWidth/Height` at export-start time.
Framerate stays in the dialog for now (user might want to render a
30 fps comp at 60 fps for slow-motion tricks).

### 2.4 Comp dimensions drive export

At `Begin`, we now set:
```
pendingExport.width  = compositionWidth;
pendingExport.height = compositionHeight;
```
This happens in `RenderEngine` right before `exportEngine.Begin(...)`,
not inside ExportEngine itself, so ExportEngine keeps its no-dependency-
on-comp-state invariant.

---

## 3. Files changing

```
src/ExportEngine.h    MODIFIED  +25   Windows CreateProcess handles;
                                       BuildFfmpegCommand output arg
                                       string only (no ffmpegPath prefix
                                       — CreateProcessA takes those
                                       separately)
src/ExportEngine.cpp  MODIFIED  +180/-90  Windows-native CreatePipe +
                                       CreateProcess + log file capture;
                                       WriteCurrentFrame uses WriteFile;
                                       End reads log tail on failure
src/RenderEngine.cpp  MODIFIED  +15/-10  strip W/H inputs from Render
                                       Queue UI; force pendingExport
                                       dimensions from comp before Begin
DESIGN_COMMIT10_EXPORT_FIX.md  NEW  this file
```

Net ~+110 LOC. Binary impact ~+2 KB.

---

## 4. Not doing in this commit (documented for follow-up)

**Preview vs Composition resolution split (Task 5.8).** The
"shapes go invisible when I lower comp resolution" symptom is a
different bug: the viewport canvas letterbox stays the same size,
but the internal comp RT shrinks, so shapes drawn at 300px in a
1920x1080 comp become huge (300/1920 = 16%) in a 640×360 proxy
(16% × 640 = 100px — they're actually smaller on screen, not
invisible). The real user-visible symptom is probably that
gizmo/hit-test coordinates go out of sync when the comp RT is
smaller than the viewport it renders into. Separate Task 5.8 will
add an independent Preview Scale (Full/Half/Quarter) that keeps
comp dimensions locked but downsamples the RT for perf.

**Chunked frame memory.** User's bug report asks about "10 frames at
a time, not whole clip in RAM." Current impl already streams: each
frame writes to the pipe and moves on — no accumulated buffer. The
33 MB RAM budget is holding. No change needed.

**Non-Windows builds.** ExportEngine goes Windows-only in this
commit. The current codebase is Windows-only end-to-end (DX11 + SDL2
Windows + MSVC), so this is a no-op restriction that gets called out
explicitly in comments.

---

## 5. Test plan

1. Test FFmpeg in Render Queue → OK (unchanged).
2. Comp Settings: set 1080×1920. Confirm Render Queue now shows
   "Resolution: 1080 × 1920" as read-only text.
3. Start Export → progress bar advances → final MP4 exists → open
   in VLC → 1080×1920 vertical MP4, shapes in correct positions.
4. Corrupt the ffmpeg path (Settings → set ffmpegPath to
   "nonexistent"). Start Export → error message includes the tail
   of the log file (probably "The system cannot find the file
   specified" or similar).
5. Deliberately break the pipe mid-export (kill ffmpeg from Task
   Manager). App shows real error from the log ("moov atom not
   found" or similar), doesn't hang.
6. Cancel button still works (calls TerminateProcess + CloseHandle
   cleanup).

---

## 6. Go

Executing single commit. No open questions.