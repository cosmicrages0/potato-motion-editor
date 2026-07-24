// =============================================================================
// Serialization.cpp — Task 5.2 .pmge JSON save/load.
//
// nlohmann/json is #included ONLY in this file. Every other translation unit
// links against clean C++ types via Serialization.h — the whole reason for
// isolating the include is to prevent template instantiation bloat from
// leaking across TUs. MSVC's /GL + /LTCG then strips the redundant symbols
// down to ~120 KB total binary impact.
// =============================================================================

#include "Serialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "LayerManager.h"
#include "Layer.h"
#include "Camera.h"
#include "AnimationEngine.h"
#include "AnimatedProperty.h"
#include "MathTypes.h"
#include "Effect.h"

using nlohmann::json;

// =============================================================================
// Internal helpers — kept in anonymous namespace so nothing leaks out of the TU.
// =============================================================================
namespace {

// ---- Vec2 / Vec3 <-> JSON array ---------------------------------------------
json WriteVec2(const Vec2& v) { return json::array({ v.x, v.y }); }
json WriteVec3(const Vec3& v) { return json::array({ v.x, v.y, v.z }); }

Vec2 ReadVec2(const json& j, const Vec2& fallback = Vec2()) {
    if (!j.is_array() || j.size() < 2) return fallback;
    return Vec2(j[0].get<float>(), j[1].get<float>());
}
Vec3 ReadVec3(const json& j, const Vec3& fallback = Vec3()) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// ---- AnimatedProperty<T> <-> JSON -------------------------------------------
// Compact shape:
//   { "sw": false, "static": <T>, "keys": [] }
//   { "sw": true,  "static": <T>, "keys": [ {"t": 0.5, "v": <T>}, ... ] }
// where <T> is a scalar for float, a 2-array for Vec2, a 3-array for Vec3.

template <typename T> json WriteValueT(const T& v);
template <> json WriteValueT<float>(const float& v) { return v; }
template <> json WriteValueT<Vec2>(const Vec2& v)   { return WriteVec2(v); }
template <> json WriteValueT<Vec3>(const Vec3& v)   { return WriteVec3(v); }

template <typename T> T ReadValueT(const json& j);
template <> float ReadValueT<float>(const json& j) { return j.is_number() ? j.get<float>() : 0.0f; }
template <> Vec2  ReadValueT<Vec2>(const json& j)  { return ReadVec2(j); }
template <> Vec3  ReadValueT<Vec3>(const json& j)  { return ReadVec3(j); }

// Task 5.4-fix: serialize per-side interpolation mode as a short string so the
// .pmge file stays human-readable. Unknown / missing => Linear.
inline const char* InterpModeToString(InterpMode m) {
    switch (m) {
        case InterpMode::Bezier:           return "bezier";
        case InterpMode::ContinuousBezier: return "continuousBezier";
        case InterpMode::AutoBezier:       return "autoBezier";
        case InterpMode::Hold:             return "hold";
        case InterpMode::Linear:
        default:                           return "linear";
    }
}
inline InterpMode InterpModeFromString(const std::string& s) {
    if (s == "bezier")           return InterpMode::Bezier;
    if (s == "continuousBezier") return InterpMode::ContinuousBezier;
    if (s == "autoBezier")       return InterpMode::AutoBezier;
    if (s == "hold")             return InterpMode::Hold;
    return InterpMode::Linear;
}

// Convert AE-native (speed, influence) tangent data <-> JSON. Speed is a
// value in T-space (scalar for float, array for Vec2/Vec3). Influence is a
// single float in [0,100].
template <typename T> json WriteSpeedT(const T& v)         { return WriteValueT(v); }
template <typename T> T    ReadSpeedT(const json& j)       { return ReadValueT<T>(j); }

// Legacy converter: Task 5.4 (commit 05bfb40) stored Bezier as raw
// (time_offset, value_offset) tangents. We convert on load so evaluator only
// sees the AE-native (speed, influence) form. Result is bit-identical because
// it's the same underlying math on both sides of the conversion.
//   time_offset  -> influence = |time_offset| / segment_span * 100
//   value_offset -> speed     = value_offset / time_offset
// The segment span comes from the neighbor key's time. If there's no neighbor
// (edge key), we fall back to the full comp duration or 1s so the divide is
// non-degenerate.
template <typename T>
struct Task54Legacy {
    T     inValueOffset  = T{};
    T     outValueOffset = T{};
    float inTimeOffset   = 0.0f;
    float outTimeOffset  = 0.0f;
    bool  hasAny         = false;
};

template <typename T>
inline void ApplyTask54Legacy(Keyframe<T>& k, const Task54Legacy<T>& lg,
                              float leftSpan, float rightSpan) {
    if (!lg.hasAny) return;
    // Guard degenerate spans.
    if (leftSpan  < 1e-4f) leftSpan  = 1.0f;
    if (rightSpan < 1e-4f) rightSpan = 1.0f;
    // Incoming (negative time offset expected).
    if (lg.inTimeOffset != 0.0f) {
        k.inInfluence = std::clamp(std::fabs(lg.inTimeOffset) / leftSpan * 100.0f,
                                   0.0f, 100.0f);
        const float denom = lg.inTimeOffset;  // negative
        if (std::fabs(denom) > 1e-6f) {
            k.inSpeed = ScaleT(lg.inValueOffset, 1.0f / denom);
        }
    }
    // Outgoing (positive time offset expected).
    if (lg.outTimeOffset != 0.0f) {
        k.outInfluence = std::clamp(lg.outTimeOffset / rightSpan * 100.0f,
                                    0.0f, 100.0f);
        const float denom = lg.outTimeOffset;
        if (std::fabs(denom) > 1e-6f) {
            k.outSpeed = ScaleT(lg.outValueOffset, 1.0f / denom);
        }
    }
}

template <typename T>
json WriteAnimatedProperty(const AnimatedProperty<T>& p) {
    json out;
    out["sw"]     = p.stopwatchEnabled;
    out["static"] = WriteValueT(p.staticValue);
    json keys = json::array();
    for (const auto& k : p.keyframes) {
        json kj;
        kj["t"] = k.time;
        kj["v"] = WriteValueT(k.value);
        // Only emit non-default fields — purely-linear files stay short and
        // Task 5.2 output is byte-identical.
        if (k.incomingMode != InterpMode::Linear)
            kj["im"] = InterpModeToString(k.incomingMode);
        if (k.outgoingMode != InterpMode::Linear)
            kj["om"] = InterpModeToString(k.outgoingMode);
        // Influence is only meaningful when the side is a Bezier variant.
        if (k.incomingMode == InterpMode::Bezier ||
            k.incomingMode == InterpMode::ContinuousBezier ||
            k.incomingMode == InterpMode::AutoBezier) {
            kj["ii"] = k.inInfluence;
            kj["is"] = WriteSpeedT(k.inSpeed);
        }
        if (k.outgoingMode == InterpMode::Bezier ||
            k.outgoingMode == InterpMode::ContinuousBezier ||
            k.outgoingMode == InterpMode::AutoBezier) {
            kj["oi"] = k.outInfluence;
            kj["os"] = WriteSpeedT(k.outSpeed);
        }
        if (k.roving) kj["r"] = true;
        keys.push_back(std::move(kj));
    }
    out["keys"] = std::move(keys);
    return out;
}

template <typename T>
AnimatedProperty<T> ReadAnimatedProperty(const json& j, const T& fallbackStatic) {
    AnimatedProperty<T> p;
    p.staticValue      = fallbackStatic;
    p.stopwatchEnabled = false;
    if (!j.is_object()) return p;
    if (j.contains("sw"))     p.stopwatchEnabled = j["sw"].get<bool>();
    if (j.contains("static")) p.staticValue      = ReadValueT<T>(j["static"]);
    if (j.contains("keys") && j["keys"].is_array()) {
        // First pass: read everything, deferring the legacy converter until we
        // know each key's neighbors (so segment spans are correct).
        struct RawKey { Keyframe<T> k; Task54Legacy<T> lg; };
        std::vector<RawKey> raws;
        raws.reserve(j["keys"].size());
        for (const auto& kj : j["keys"]) {
            RawKey rk;
            rk.k.time  = kj.value("t", 0.0f);
            if (kj.contains("v")) rk.k.value = ReadValueT<T>(kj["v"]);
            // Modes (both old and new schemas use "im"/"om").
            if (kj.contains("im") && kj["im"].is_string())
                rk.k.incomingMode = InterpModeFromString(kj["im"].get<std::string>());
            if (kj.contains("om") && kj["om"].is_string())
                rk.k.outgoingMode = InterpModeFromString(kj["om"].get<std::string>());
            // New-schema AE-native fields.
            if (kj.contains("ii") && kj["ii"].is_number()) rk.k.inInfluence  = kj["ii"].get<float>();
            if (kj.contains("oi") && kj["oi"].is_number()) rk.k.outInfluence = kj["oi"].get<float>();
            if (kj.contains("is")) rk.k.inSpeed  = ReadSpeedT<T>(kj["is"]);
            if (kj.contains("os")) rk.k.outSpeed = ReadSpeedT<T>(kj["os"]);
            if (kj.contains("r"))  rk.k.roving   = kj["r"].get<bool>();
            // Task 5.4 legacy fields (raw (time_offset, value_offset) tangents).
            if (kj.contains("it") && kj["it"].is_number()) { rk.lg.inTimeOffset  = kj["it"].get<float>(); rk.lg.hasAny = true; }
            if (kj.contains("ot") && kj["ot"].is_number()) { rk.lg.outTimeOffset = kj["ot"].get<float>(); rk.lg.hasAny = true; }
            if (kj.contains("iv")) { rk.lg.inValueOffset  = ReadValueT<T>(kj["iv"]); rk.lg.hasAny = true; }
            if (kj.contains("ov")) { rk.lg.outValueOffset = ReadValueT<T>(kj["ov"]); rk.lg.hasAny = true; }
            raws.push_back(std::move(rk));
        }
        // Second pass: apply legacy converter using neighbor times as spans.
        for (size_t i = 0; i < raws.size(); ++i) {
            const float leftSpan  = (i > 0)
                ? raws[i].k.time - raws[i - 1].k.time
                : 1.0f;
            const float rightSpan = (i + 1 < raws.size())
                ? raws[i + 1].k.time - raws[i].k.time
                : 1.0f;
            ApplyTask54Legacy(raws[i].k, raws[i].lg, leftSpan, rightSpan);
            p.keyframes.push_back(raws[i].k);
        }
    }
    return p;
}

// ---- ShapeType <-> string ---------------------------------------------------
const char* ShapeTypeToString(ShapeType t) {
    switch (t) {
        case ShapeType::Rectangle:  return "Rectangle";
        case ShapeType::Ellipse:    return "Ellipse";
        case ShapeType::CustomPath: return "CustomPath";
        case ShapeType::Camera:     return "Camera";
        case ShapeType::Null:       return "Null";
        case ShapeType::Text:       return "Text";   // Task 5.9
    }
    return "Rectangle";
}
ShapeType ShapeTypeFromString(const std::string& s) {
    if (s == "Ellipse")    return ShapeType::Ellipse;
    if (s == "CustomPath") return ShapeType::CustomPath;
    if (s == "Camera")     return ShapeType::Camera;
    if (s == "Null")       return ShapeType::Null;
    if (s == "Text")       return ShapeType::Text;   // Task 5.9
    return ShapeType::Rectangle;
}

// ---- EffectType <-> string --------------------------------------------------
const char* EffectTypeToString(EffectType t) {
    switch (t) {
        case EffectType::MotionTile:            return "MotionTile";
        case EffectType::DirectionalMotionBlur: return "DirectionalMotionBlur";
        case EffectType::ChromaticAberration:   return "ChromaticAberration";
        case EffectType::BlendMode:             return "BlendMode";
        case EffectType::COUNT:                 return "MotionTile";
    }
    return "MotionTile";
}
EffectType EffectTypeFromString(const std::string& s) {
    if (s == "DirectionalMotionBlur") return EffectType::DirectionalMotionBlur;
    if (s == "ChromaticAberration")   return EffectType::ChromaticAberration;
    if (s == "BlendMode")             return EffectType::BlendMode;
    return EffectType::MotionTile;
}

// Task 5.10: per-layer BlendMode <-> string. Same enum as EffectType's
// BlendMode effect (Effect.h) — one BlendMode type in the codebase.
const char* BlendModeToString(BlendMode b) {
    switch (b) {
        case BlendMode::Normal:     return "Normal";
        case BlendMode::Additive:   return "Additive";
        case BlendMode::Multiply:   return "Multiply";
        case BlendMode::Screen:     return "Screen";
        case BlendMode::Overlay:    return "Overlay";
        case BlendMode::ColorDodge: return "ColorDodge";
    }
    return "Normal";
}
BlendMode BlendModeFromString(const std::string& s) {
    if (s == "Additive")   return BlendMode::Additive;
    if (s == "Multiply")   return BlendMode::Multiply;
    if (s == "Screen")     return BlendMode::Screen;
    if (s == "Overlay")    return BlendMode::Overlay;
    if (s == "ColorDodge") return BlendMode::ColorDodge;
    return BlendMode::Normal;
}

// ---- Effect <-> JSON --------------------------------------------------------
json WriteEffect(const Effect& e) {
    json out;
    out["id"]          = e.id;
    out["type"]        = EffectTypeToString(e.type);
    out["enabled"]     = e.enabled;
    out["displayName"] = e.displayName;
    out["p0"] = json::array({ e.params.p0[0], e.params.p0[1], e.params.p0[2], e.params.p0[3] });
    out["p1"] = json::array({ e.params.p1[0], e.params.p1[1], e.params.p1[2], e.params.p1[3] });
    out["p2"] = json::array({ e.params.p2[0], e.params.p2[1], e.params.p2[2], e.params.p2[3] });
    out["p3"] = json::array({ e.params.p3[0], e.params.p3[1], e.params.p3[2], e.params.p3[3] });
    return out;
}

Effect ReadEffect(const json& j) {
    Effect e;
    e.id          = j.value("id", 0);
    e.type        = EffectTypeFromString(j.value("type", std::string("MotionTile")));
    e.enabled     = j.value("enabled", true);
    e.displayName = j.value("displayName", std::string("Effect"));
    auto readParam = [&](const char* key, float (&dst)[4]) {
        if (!j.contains(key) || !j[key].is_array()) return;
        for (size_t i = 0; i < 4 && i < j[key].size(); ++i) {
            if (j[key][i].is_number()) dst[i] = j[key][i].get<float>();
        }
    };
    readParam("p0", e.params.p0);
    readParam("p1", e.params.p1);
    readParam("p2", e.params.p2);
    readParam("p3", e.params.p3);
    return e;
}

// ---- Transform <-> JSON -----------------------------------------------------
json WriteTransform(const Transform& t) {
    json out;
    out["position"]    = WriteAnimatedProperty(t.position);
    out["rotation"]    = WriteAnimatedProperty(t.rotation);
    out["scale"]       = WriteAnimatedProperty(t.scale);
    out["anchorPoint"] = WriteAnimatedProperty(t.anchorPoint);
    out["sizePixels"]  = WriteAnimatedProperty(t.sizePixels);
    out["opacity"]     = WriteAnimatedProperty(t.opacity);
    return out;
}
void ReadTransform(const json& j, Transform& t) {
    if (!j.is_object()) return;
    if (j.contains("position"))    t.position    = ReadAnimatedProperty<Vec3>(j["position"],    Vec3(0, 0, 0));
    if (j.contains("rotation"))    t.rotation    = ReadAnimatedProperty<Vec3>(j["rotation"],    Vec3(0, 0, 0));
    if (j.contains("scale"))       t.scale       = ReadAnimatedProperty<Vec3>(j["scale"],       Vec3(1, 1, 1));
    if (j.contains("anchorPoint")) t.anchorPoint = ReadAnimatedProperty<Vec2>(j["anchorPoint"], Vec2(0.5f, 0.5f));
    if (j.contains("sizePixels"))  t.sizePixels  = ReadAnimatedProperty<Vec2>(j["sizePixels"],  Vec2(200.0f, 120.0f));
    if (j.contains("opacity"))     t.opacity     = ReadAnimatedProperty<float>(j["opacity"],    1.0f);
}

// ---- Layer <-> JSON ---------------------------------------------------------
// fillColor is IM_COL32-packed as unsigned int. Serialize as a hex string like
// "0xFF00B4FF" so it survives roundtrip cleanly and reads well in a text editor.
std::string FillColorToHex(unsigned int c) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << c;
    return oss.str();
}
unsigned int FillColorFromHex(const std::string& s, unsigned int fallback) {
    try {
        if (s.size() > 2 && (s[1] == 'x' || s[1] == 'X'))
            return (unsigned int)std::stoul(s.substr(2), nullptr, 16);
        return (unsigned int)std::stoul(s, nullptr, 16);
    } catch (...) { return fallback; }
}

json WriteLayer(const Layer& L) {
    json out;
    out["id"]            = L.id;
    out["parentId"]      = L.parentId;
    out["name"]          = L.name;
    out["type"]          = ShapeTypeToString(L.type);
    out["isVisible"]     = L.isVisible;
    out["isSolo"]        = L.isSolo;
    out["isLocked"]      = L.isLocked;
    out["is3D"]          = L.is3D;
    out["stickToCamera"] = L.stickToCamera;
    out["fillColor"]     = FillColorToHex(L.fillColor);
    // Task 5.7: stroke + rounded corners. Emit unconditionally so users
    // hand-editing a .pmge see the schema; defaults keep the payload tiny.
    out["strokeColor"]   = FillColorToHex(L.strokeColor);
    out["strokeWidth"]   = L.strokeWidth;
    out["cornerRadius"]  = L.cornerRadius;
    // Task 5.10: trim + blend. Only emit non-default values so pre-5.10
    // untrimmed unblended layers save byte-identical to before.
    if (L.inPoint  != 0.0f)               out["inPoint"]  = L.inPoint;
    if (L.outPoint >= 0.0f)               out["outPoint"] = L.outPoint; // sentinel -1 = omit
    if (L.blend    != BlendMode::Normal)  out["blend"]    = BlendModeToString(L.blend);
    // Task 5.9: TextProps — only meaningful when type == Text, but emit
    // unconditionally when non-default so a manual retype in a text editor
    // preserves the block. Cheap; text data is tiny.
    if (L.type == ShapeType::Text ||
        L.textProps.text != "Text" ||
        L.textProps.fontFamily != "Segoe UI" ||
        L.textProps.fontSize != 72.0f ||
        L.textProps.fontWeight != 400 ||
        L.textProps.italic ||
        L.textProps.alignment != 0) {
        json tp;
        tp["text"]       = L.textProps.text;
        tp["fontFamily"] = L.textProps.fontFamily;
        tp["fontSize"]   = L.textProps.fontSize;
        tp["fontWeight"] = L.textProps.fontWeight;
        tp["italic"]     = L.textProps.italic;
        tp["alignment"]  = L.textProps.alignment;
        out["textProps"] = std::move(tp);
    }
    out["transform"]     = WriteTransform(L.transform);
    json effects = json::array();
    for (const auto& e : L.effects) effects.push_back(WriteEffect(e));
    out["effects"] = std::move(effects);
    out["nextEffectId"] = L.nextEffectId;
    return out;
}

Layer ReadLayer(const json& j) {
    Layer L;
    L.id            = j.value("id", 0);
    L.parentId      = j.value("parentId", -1);
    L.name          = j.value("name", std::string("Layer"));
    L.type          = ShapeTypeFromString(j.value("type", std::string("Rectangle")));
    L.isVisible     = j.value("isVisible", true);
    L.isSolo        = j.value("isSolo", false);
    L.isLocked      = j.value("isLocked", false);
    L.is3D          = j.value("is3D", false);
    L.stickToCamera = j.value("stickToCamera", false);
    L.fillColor     = FillColorFromHex(j.value("fillColor", std::string("0xFFCCCC00")), 0xFFCCCC00u);
    // Task 5.7: stroke + corner radius are optional (pre-5.7 files miss
    // them entirely) — defaults reproduce the old no-stroke sharp-corner look.
    L.strokeColor   = FillColorFromHex(j.value("strokeColor", std::string("0xFF000000")), 0xFF000000u);
    L.strokeWidth   = j.value("strokeWidth",  0.0f);
    L.cornerRadius  = j.value("cornerRadius", 0.0f);
    // Task 5.10: trim + blend. Missing => defaults reproduce pre-5.10
    // behavior (visible entire comp, Normal blend). Zero migration cost.
    L.inPoint       = j.value("inPoint",     0.0f);
    L.outPoint      = j.value("outPoint",   -1.0f);   // sentinel
    L.blend         = BlendModeFromString(j.value("blend", std::string("Normal")));
    // Task 5.9: TextProps. Missing on pre-5.9 files => defaults; on a non-
    // Text layer with only defaults, ignored downstream anyway.
    if (j.contains("textProps") && j["textProps"].is_object()) {
        const auto& tp = j["textProps"];
        L.textProps.text       = tp.value("text",       std::string("Text"));
        L.textProps.fontFamily = tp.value("fontFamily", std::string("Segoe UI"));
        L.textProps.fontSize   = tp.value("fontSize",   72.0f);
        L.textProps.fontWeight = tp.value("fontWeight", 400);
        L.textProps.italic     = tp.value("italic",     false);
        L.textProps.alignment  = tp.value("alignment",  0);
    }
    if (j.contains("transform")) ReadTransform(j["transform"], L.transform);
    if (j.contains("effects") && j["effects"].is_array()) {
        for (const auto& ej : j["effects"]) L.effects.push_back(ReadEffect(ej));
    }
    L.nextEffectId = j.value("nextEffectId", (int)(L.effects.size() + 1));
    return L;
}

// ---- Camera <-> JSON --------------------------------------------------------
json WriteCamera(const Camera& c) {
    json out;
    out["position"]      = WriteVec3(c.position);
    out["target"]        = WriteVec3(c.target);
    out["rotation"]      = WriteVec3(c.rotation);
    out["up"]            = WriteVec3(c.up);
    out["fov"]           = c.fov;
    out["nearZ"]         = c.nearZ;
    out["farZ"]          = c.farZ;
    out["zoom"]          = c.zoom;
    out["useTargetMode"] = c.useTargetMode;
    return out;
}
void ReadCamera(const json& j, Camera& c) {
    if (!j.is_object()) return;
    if (j.contains("position"))      c.position      = ReadVec3(j["position"],      c.position);
    if (j.contains("target"))        c.target        = ReadVec3(j["target"],        c.target);
    if (j.contains("rotation"))      c.rotation      = ReadVec3(j["rotation"],      c.rotation);
    if (j.contains("up"))            c.up            = ReadVec3(j["up"],            c.up);
    c.fov            = j.value("fov",           c.fov);
    c.nearZ          = j.value("nearZ",         c.nearZ);
    c.farZ           = j.value("farZ",          c.farZ);
    c.zoom           = j.value("zoom",          c.zoom);
    c.useTargetMode  = j.value("useTargetMode", c.useTargetMode);
}

// ---- AnimationEngine (comp clock + slingshot bezier) <-> JSON --------------
json WriteAnim(const AnimationEngine& a) {
    json out;
    out["duration"]    = a.duration;
    out["currentTime"] = a.currentTime;
    out["isPlaying"]   = a.isPlaying;
    out["isLooping"]   = a.isLooping;
    json curve;
    curve["p0"] = WriteVec2(a.currentCurve.P0);
    curve["p1"] = WriteVec2(a.currentCurve.P1);
    curve["p2"] = WriteVec2(a.currentCurve.P2);
    curve["p3"] = WriteVec2(a.currentCurve.P3);
    out["slingshotCurve"] = curve;
    return out;
}
void ReadAnim(const json& j, AnimationEngine& a) {
    if (!j.is_object()) return;
    a.duration    = j.value("duration",    a.duration);
    a.currentTime = j.value("currentTime", a.currentTime);
    a.isPlaying   = j.value("isPlaying",   a.isPlaying);
    a.isLooping   = j.value("isLooping",   a.isLooping);
    if (j.contains("slingshotCurve") && j["slingshotCurve"].is_object()) {
        const auto& c = j["slingshotCurve"];
        if (c.contains("p0")) a.currentCurve.P0 = ReadVec2(c["p0"], a.currentCurve.P0);
        if (c.contains("p1")) a.currentCurve.P1 = ReadVec2(c["p1"], a.currentCurve.P1);
        if (c.contains("p2")) a.currentCurve.P2 = ReadVec2(c["p2"], a.currentCurve.P2);
        if (c.contains("p3")) a.currentCurve.P3 = ReadVec2(c["p3"], a.currentCurve.P3);
    }
}

} // anonymous namespace

// =============================================================================
// Core (de)serialization — file and string versions both go through these.
// Task 5.3 split: extracted from the file-based API so UndoStack can round-trip
// AppState to std::string without touching the disk.
// =============================================================================
namespace {

// Serialize an AppState into a fully-formed nlohmann::json root object.
// Never throws. Returns true on success (currently always true; kept as a
// bool for symmetry with the reader).
bool AppStateToJson(const AppState& state, json& outRoot, std::string* outError) {
    if (!state.layerManager || !state.camera || !state.animEngine) {
        if (outError) *outError = "AppStateToJson: missing state pointer";
        return false;
    }

    outRoot = json{};
    outRoot["pmge_version"] = kPmgeFormatVersion;

    json comp;
    comp["width"]          = state.compositionWidth;
    comp["height"]         = state.compositionHeight;
    comp["cameraStyle"]    = (state.cameraStyleInt == 1) ? "AlightMotion" : "AfterEffects";
    comp["show3DFeatures"] = state.show3DFeatures;
    // Task 5.6: comp-wide framerate + background color. Whatever value the
    // user set (including a custom 25 or 29.97) is written verbatim — no
    // snap to the preset list. Reader restores it as-is.
    comp["fps"]            = state.compositionFps;
    comp["bgColor"]        = json::array({ state.bgColor[0], state.bgColor[1],
                                           state.bgColor[2], state.bgColor[3] });
    outRoot["composition"] = std::move(comp);

    outRoot["camera"]    = WriteCamera(*state.camera);
    outRoot["animation"] = WriteAnim(*state.animEngine);

    json layersArr = json::array();
    for (const auto& L : state.layerManager->Layers()) {
        layersArr.push_back(WriteLayer(L));
    }
    outRoot["layers"]      = std::move(layersArr);
    outRoot["nextLayerId"] = state.layerManager->GetNextId();
    // Task 5.6-fix-2: persist which layer is currently selected so undo/redo
    // (and file reload) don't reset the user's focus to the front-most layer.
    // Root-level field, not inside "composition" — selection is editor state,
    // not a comp property. Missing on read => front-layer fallback still fires.
    outRoot["selectedLayerId"] = state.layerManager->GetSelectedId();
    return true;
}

// Inverse of AppStateToJson. Wipes state.layerManager before repopulating so
// there's no leakage from whatever was in memory.
bool JsonToAppState(const json& root, AppState& state, std::string* outError) {
    if (!state.layerManager || !state.camera || !state.animEngine) {
        if (outError) *outError = "JsonToAppState: missing state pointer";
        return false;
    }

    const int fileVersion = root.value("pmge_version", 0);
    if (fileVersion > kPmgeFormatVersion) {
        std::cerr << "[JsonToAppState] warning: file version " << fileVersion
                  << " is newer than app version " << kPmgeFormatVersion
                  << "; attempting best-effort load." << std::endl;
    }

    if (root.contains("composition") && root["composition"].is_object()) {
        const auto& comp = root["composition"];
        state.compositionWidth  = comp.value("width",  state.compositionWidth);
        state.compositionHeight = comp.value("height", state.compositionHeight);
        std::string style       = comp.value("cameraStyle", std::string("AfterEffects"));
        state.cameraStyleInt    = (style == "AlightMotion") ? 1 : 0;
        state.show3DFeatures    = comp.value("show3DFeatures", state.show3DFeatures);
        // Task 5.6: fps + bgColor. Missing => keep defaults from AppState.
        state.compositionFps    = comp.value("fps", state.compositionFps);
        if (comp.contains("bgColor") && comp["bgColor"].is_array() &&
            comp["bgColor"].size() >= 4) {
            for (int i = 0; i < 4; ++i) {
                if (comp["bgColor"][i].is_number()) {
                    state.bgColor[i] = comp["bgColor"][i].get<float>();
                }
            }
        }
    }
    if (root.contains("camera"))    ReadCamera(root["camera"], *state.camera);
    if (root.contains("animation")) ReadAnim(root["animation"], *state.animEngine);

    state.layerManager->Clear();
    int maxId = 0;
    if (root.contains("layers") && root["layers"].is_array()) {
        for (const auto& lj : root["layers"]) {
            Layer L = ReadLayer(lj);
            if (L.id > maxId) maxId = L.id;
            state.layerManager->Layers().push_back(std::move(L));
        }
    }
    const int nextId = std::max(maxId + 1, root.value("nextLayerId", maxId + 1));
    state.layerManager->SetNextId(nextId);
    // Force idToIndex cache rebuild (LayerManager only rebuilds on Add/Delete).
    // Piggyback via a dummy Add+Delete. One-time cost per load; negligible.
    const int dummy = state.layerManager->AddLayer(ShapeType::Null, "___pmge_load_dummy___");
    state.layerManager->DeleteLayerById(dummy);

    // Task 5.6-fix-2: restore the saved selection BEFORE the front-layer
    // fallback fires. Guarded by GetLayerById so a stale ID (e.g. the
    // selected layer was deleted between save and load, or the snapshot
    // captured a layer that undo has since removed) falls through cleanly
    // instead of leaving selectedLayerId pointing at nothing.
    if (root.contains("selectedLayerId") && root["selectedLayerId"].is_number()) {
        const int savedSel = root["selectedLayerId"].get<int>();
        if (savedSel >= 0 && state.layerManager->GetLayerById(savedSel) != nullptr) {
            state.layerManager->SetSelectedId(savedSel);
        }
    }
    if (state.layerManager->GetSelectedId() < 0 && !state.layerManager->Layers().empty()) {
        state.layerManager->SetSelectedId(state.layerManager->Layers().front().id);
    }
    return true;
}

} // namespace

// =============================================================================
// Public API
// =============================================================================
bool SaveProjectToString(const AppState& state, std::string& outJson, std::string* outError) {
    json root;
    if (!AppStateToJson(state, root, outError)) return false;
    try {
        // dump(-1) = compact for undo snapshots (smaller in memory, still valid JSON).
        // File saves use pretty-printed dump(2) for readability -- see SaveProject below.
        outJson = root.dump(-1);
        return true;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Save-to-string exception: ") + e.what();
        return false;
    }
}

bool LoadProjectFromString(AppState& state, const std::string& inJson, std::string* outError) {
    json root;
    try {
        root = json::parse(inJson);
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Parse error: ") + e.what();
        return false;
    }
    return JsonToAppState(root, state, outError);
}

bool SaveProject(const AppState& state, const std::string& path, std::string* outError) {
    json root;
    if (!AppStateToJson(state, root, outError)) return false;
    try {
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) {
            if (outError) *outError = "Could not open path for writing: " + path;
            return false;
        }
        // Pretty-printed for on-disk .pmge files (2-space indent, git-diff friendly).
        f << std::setw(2) << root;
        return true;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Save exception: ") + e.what();
        return false;
    }
}

bool LoadProject(AppState& state, const std::string& path, std::string* outError) {
    json root;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            if (outError) *outError = "Could not open file: " + path;
            return false;
        }
        f >> root;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Parse error: ") + e.what();
        return false;
    }
    return JsonToAppState(root, state, outError);
}
