#pragma once
#include <deque>
#include <string>

// -----------------------------------------------------------------------------
// UndoStack — snapshot-based undo/redo for Task 5.3.
//
// Every snapshot is the ENTIRE AppState serialized to a JSON string via the
// existing Task 5.2 serializer. Round-trips through the same code path as
// File > Save / Open, which means every future feature (masks, text, sub-
// comps, expressions) gets undo automatically the moment it's added to the
// serializer -- zero per-feature undo code required.
//
// Cost analysis (validated in DESIGN_COMMIT3_UNDO_KEYFRAMES_PARENT.md):
//   * Typical scene serializes to 5-50 KB of JSON
//   * 50-deep stack = 250 KB - 2.5 MB peak memory
//   * On a 4GB potato PC this is a rounding error
//
// Coalescing (also documented in DESIGN_COMMIT3): RenderEngine calls
// MarkForSnapshot() when a snapshot-worthy event happens. The actual
// PushSnapshot() runs once at the top of the next frame. Multiple marks in
// the same frame collapse to one entry. Gizmo drags (60 mouse-move frames
// per second) do NOT push a snapshot each frame -- only the mouse-up does.
// -----------------------------------------------------------------------------

struct AppState; // fwd decl; full definition in Serialization.h

class UndoStack {
public:
    UndoStack();

    // Capture the current AppState as a snapshot on the past stack. Called
    // BEFORE structural mutations and AFTER interactive edits complete.
    // Any pending redo history is cleared (standard undo semantics).
    // Returns false if serialization fails; the stack is unchanged in that case.
    bool PushSnapshot(const AppState& state);

    // Restore the most recent past snapshot into `state`. The current state
    // (before restore) is pushed onto the redo stack so Redo can bring it back.
    // Returns false if there's nothing to undo (stack empty) or restore fails.
    bool Undo(AppState& state);

    // Symmetric: pop from future stack, push current onto past, apply future.
    bool Redo(AppState& state);

    bool CanUndo() const { return !pastStates.empty(); }
    bool CanRedo() const { return !futureStates.empty(); }

    size_t PastCount()   const { return pastStates.size(); }
    size_t FutureCount() const { return futureStates.size(); }

    // Clear both stacks. Called on File > Open (loading a project is not
    // undoable back into whatever was in memory before -- that would just be
    // confusing).
    void Clear();

    void SetMaxDepth(size_t d) { maxDepth = d; }
    size_t GetMaxDepth() const { return maxDepth; }

private:
    std::deque<std::string> pastStates;
    std::deque<std::string> futureStates;
    size_t maxDepth = 50;
};
