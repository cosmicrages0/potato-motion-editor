#pragma once
#include <string>
#include <vector>
#include "Layer.h"
#include "AnimationEngine.h"

// -----------------------------------------------------------------------------
// AlightXmlImporter — lightweight, single-purpose XML parser for Alight Motion
// keyframe curve preset files.
//
// Scope: we do NOT ship a general XML library (bloat, wrong for a potato-PC
// binary). This parser only recognizes the specific patterns used by Alight
// Motion presets, which take the form:
//
//   <keyframe time="0.25" value="1.20" curve="0.42,0.0 0.58,1.0" />
//
// where `curve` is two "x,y" pairs = P1 and P2 for the Bezier segment leaving
// this keyframe. P0 = (0,0) and P3 = (1,1) are implicit AE-style handles at
// the segment endpoints in normalized time/value.
//
// The parser tolerates whitespace, single/double quotes, and extra attributes
// it doesn't understand. Malformed elements are silently skipped -- we never
// throw, we never abort the caller mid-import.
//
// Returned data model:
//   ParsedKeyframe { time, value, p1, p2 }
//   The caller (RenderEngine) decides how to slot these into a PropertyTrack
//   and, if desired, promote the first segment's (p1,p2) into the global
//   Slingshot Bezier for the Graph Editor.
// -----------------------------------------------------------------------------

struct ParsedKeyframe {
    float time  = 0.0f;
    float value = 0.0f;
    Vec2  p1    = { 0.25f, 0.25f }; // Bezier control 1, in (t,v) 0..1 space
    Vec2  p2    = { 0.75f, 0.75f }; // Bezier control 2
    bool  hasCurve = false;
};

class AlightXmlImporter {
public:
    // Reads the file at `path` and returns every <keyframe .../> it finds,
    // in document order. Returns an empty vector on any I/O error (with the
    // reason available via LastError()).
    std::vector<ParsedKeyframe> ImportKeyframesFromFile(const std::string& path);

    // Same as above but from a string already in memory. Useful for tests
    // and for pasting XML into a future "Import from clipboard" menu item.
    std::vector<ParsedKeyframe> ImportKeyframesFromString(const std::string& xml);

    const std::string& LastError() const { return lastError_; }

private:
    std::string lastError_;
};
