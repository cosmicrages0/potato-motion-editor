#include "UndoStack.h"
#include "Serialization.h"

#include <iostream>
#include <string>

UndoStack::UndoStack() {}

bool UndoStack::PushSnapshot(const AppState& state) {
    std::string snap;
    std::string err;
    if (!SaveProjectToString(state, snap, &err)) {
        std::cerr << "[UndoStack] PushSnapshot failed: " << err << std::endl;
        return false;
    }
    // Standard undo semantics: any new authoring action clears the redo tail.
    // Otherwise Ctrl+Z Ctrl+Z new-edit Ctrl+Y would replay a state that isn't
    // reachable from the current history, which is confusing.
    futureStates.clear();
    pastStates.push_back(std::move(snap));
    // Cap depth to bound memory. Drop the oldest entry when we exceed cap.
    while (pastStates.size() > maxDepth) pastStates.pop_front();
    return true;
}

bool UndoStack::Undo(AppState& state) {
    if (pastStates.empty()) return false;

    // First capture the CURRENT state onto the future stack so Redo can
    // bring it back. If this snapshot fails we abort the undo -- better to
    // do nothing than to lose the ability to redo.
    std::string currentSnap;
    std::string err;
    if (!SaveProjectToString(state, currentSnap, &err)) {
        std::cerr << "[UndoStack] Undo: current-snapshot failed: " << err << std::endl;
        return false;
    }

    // Now peel off the most recent past entry and apply it.
    std::string target = std::move(pastStates.back());
    pastStates.pop_back();
    if (!LoadProjectFromString(state, target, &err)) {
        // Restore failed; push the peeled entry back so we haven't lost it.
        pastStates.push_back(std::move(target));
        std::cerr << "[UndoStack] Undo: restore failed: " << err << std::endl;
        return false;
    }
    futureStates.push_back(std::move(currentSnap));
    while (futureStates.size() > maxDepth) futureStates.pop_front();
    return true;
}

bool UndoStack::Redo(AppState& state) {
    if (futureStates.empty()) return false;

    std::string currentSnap;
    std::string err;
    if (!SaveProjectToString(state, currentSnap, &err)) {
        std::cerr << "[UndoStack] Redo: current-snapshot failed: " << err << std::endl;
        return false;
    }

    std::string target = std::move(futureStates.back());
    futureStates.pop_back();
    if (!LoadProjectFromString(state, target, &err)) {
        futureStates.push_back(std::move(target));
        std::cerr << "[UndoStack] Redo: restore failed: " << err << std::endl;
        return false;
    }
    pastStates.push_back(std::move(currentSnap));
    while (pastStates.size() > maxDepth) pastStates.pop_front();
    return true;
}

void UndoStack::Clear() {
    pastStates.clear();
    futureStates.clear();
}
