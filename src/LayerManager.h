#pragma once
#include <vector>
#include <unordered_map>
#include <string>

#include "Layer.h"

// -----------------------------------------------------------------------------
// LayerManager: owns all layers in the current composition.
//
// Design notes:
//  * Layer ownership is by-value in a std::vector for cache locality and
//    zero-allocation iteration inside the render loop.
//  * The stable `id` field of Layer is the ONLY thing external code should
//    reference. Vector indices are considered volatile.
//  * idToIndex is a private lookup cache rebuilt after any structural mutation
//    (add/delete/reorder). It's the only heap allocation we accept on those
//    (rare) code paths.
//  * All matrix concatenation and cycle detection lives here so that
//    Layer.h stays a pure data description.
// -----------------------------------------------------------------------------
class LayerManager {
public:
    LayerManager();

    // Structural mutations. Return the id of the affected layer, or -1 on failure.
    int  AddLayer(ShapeType type, const std::string& nameHint = "");
    bool DeleteLayerById(int id);

    // Task 5.2: wipe all layers and reset id counter / selection. Called by
    // LoadProject before deserializing a saved file so the loaded scene
    // doesn't collide with whatever was in memory.
    void Clear();

    // Task 5.2: called by LoadProject to bump nextId past any restored ids.
    void SetNextId(int nextId) { this->nextId = nextId; }
    int  GetNextId() const     { return this->nextId; }

    // Parent-child hierarchy. Returns false if the operation would create a
    // cycle (A->B->A) or if either id is unknown. Passing parentId = -1
    // detaches the child from any parent.
    //
    // SetParent (raw): just writes child.parentId. The child's authored values
    // stay as-is, so its VISIBLE world position generally jumps because the
    // new world matrix = new_parent_world * child_local. Kept for internal /
    // future use (e.g. a "keep local values" modifier); most UI callers want
    // SetParentPreservingWorld below.
    bool SetParent(int childId, int parentId);

    // Task 5.3-fix-2: change parent while preserving the child's visible
    // world transform (position + rotation + scale). Matches AE and Alight
    // Motion behavior. Rewrites the child's authored position/rotation/scale
    // so that new_parent_world * new_child_local == old_child_world.
    //
    // For 2D layers (is3D == false) the math is a full 2D affine decompose.
    // For 3D layers (is3D == true) we currently fall through to the raw
    // SetParent — TODO: 3D matrix decomposition when 3D re-enters focus.
    //
    // Returns false on the same conditions as SetParent (cycle, unknown id).
    bool SetParentPreservingWorld(int childId, int parentId, float compTime);

    // Selection is stored as a stable id, not a vector index.
    void SetSelectedId(int id) { selectedLayerId = id; }
    int  GetSelectedId() const { return selectedLayerId; }
    Layer* GetSelectedLayer(); // may return nullptr

    // Layer lookup helpers.
    Layer*        GetLayerById(int id);
    const Layer*  GetLayerById(int id) const;
    int           IndexOfId(int id) const;    // -1 if not found

    // Direct access for rendering / UI iteration.
    std::vector<Layer>&       Layers()       { return layers; }
    const std::vector<Layer>& Layers() const { return layers; }
    size_t Count() const { return layers.size(); }

    // Compute the world-space (composition-space) matrix for a layer by
    // walking up the parent chain. Uses per-frame memoization so a deep chain
    // costs O(depth) once and O(1) on subsequent queries within the same frame.
    // Call BeginFrame(compTime) at the start of each frame to reset the cache
    // AND publish the composition time all Evaluate() calls will sample at.
    void BeginFrame(float compTime);
    Mat3 GetWorldMatrix(int layerId);            // 2D affine (Task 3 path)
    Mat4 GetWorldMatrix4(int layerId);           // Full 3D (Task 4 path)
    float GetWorldOpacity(int layerId);

    // Composition time for THIS frame. Set by BeginFrame(); available so any
    // per-frame code that needs to Evaluate() a property (Inspector, gizmo,
    // camera sync) samples at exactly the same instant as the renderer.
    float CurrentCompTime() const { return currentCompTime; }

    // Find the first Camera-type layer, if any (returns -1 if none exist).
    int FindActiveCameraLayerId() const;

    // Cycle test used by SetParent. Also exposed so UI can gray out invalid
    // parent choices in the Inspector dropdown.
    bool WouldCreateCycle(int childId, int candidateParentId) const;

private:
    void RebuildIndex();

    std::vector<Layer> layers;
    std::unordered_map<int, size_t> idToIndex;
    int nextId = 1;
    int selectedLayerId = -1;

    // Per-frame memoization for world-matrix / opacity chain.
    std::unordered_map<int, Mat3>   frameMatrixCache;
    std::unordered_map<int, Mat4>   frameMatrix4Cache;
    std::unordered_map<int, float>  frameOpacityCache;

    // Composition time for the current frame, set by BeginFrame().
    float currentCompTime = 0.0f;
};
