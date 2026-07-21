#include "AlightXmlImporter.h"

#include <fstream>
#include <sstream>
#include <cctype>
#include <cstdlib>

namespace {

// Find the value of an attribute inside a single tag body like:
//   keyframe time="0.5" value="1.2" curve="0.4,0.0 0.6,1.0"
// Returns true and fills `out` if found; returns false if the attribute is
// missing. Handles both single and double quotes.
bool FindAttr(const std::string& tagBody, const std::string& name, std::string& out) {
    // Search for `name` followed by optional whitespace and =
    size_t pos = 0;
    while (pos < tagBody.size()) {
        pos = tagBody.find(name, pos);
        if (pos == std::string::npos) return false;
        // Make sure it's a whole-word match (previous char is space/start,
        // next-after-name is =, space, or end).
        const bool leftOk  = (pos == 0) || std::isspace((unsigned char)tagBody[pos - 1]);
        const size_t after = pos + name.size();
        const bool rightOk = (after < tagBody.size()) &&
                             (tagBody[after] == '=' || std::isspace((unsigned char)tagBody[after]));
        if (!leftOk || !rightOk) { ++pos; continue; }

        // Skip whitespace and '='
        size_t p = after;
        while (p < tagBody.size() && std::isspace((unsigned char)tagBody[p])) ++p;
        if (p >= tagBody.size() || tagBody[p] != '=') { ++pos; continue; }
        ++p;
        while (p < tagBody.size() && std::isspace((unsigned char)tagBody[p])) ++p;
        if (p >= tagBody.size()) return false;

        // Expect a quote
        const char q = tagBody[p];
        if (q != '"' && q != '\'') return false;
        const size_t start = ++p;
        const size_t end   = tagBody.find(q, start);
        if (end == std::string::npos) return false;
        out = tagBody.substr(start, end - start);
        return true;
    }
    return false;
}

float ParseFloatOr(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    char* endp = nullptr;
    const double d = std::strtod(s.c_str(), &endp);
    if (endp == s.c_str()) return fallback;
    return (float)d;
}

// Parse an Alight-Motion-style curve attribute value like:
//    "0.42,0.0 0.58,1.0"       (P1 and P2 separated by whitespace)
// Returns true on success and fills p1/p2. If only one pair is present,
// mirrors it into both slots. Extra tokens are ignored.
bool ParseCurveAttr(const std::string& value, Vec2& p1, Vec2& p2) {
    // Tokenize on whitespace, then split each token on comma.
    std::vector<std::pair<float, float>> pairs;
    std::istringstream iss(value);
    std::string tok;
    while (iss >> tok) {
        const size_t comma = tok.find(',');
        if (comma == std::string::npos) continue;
        const float x = ParseFloatOr(tok.substr(0, comma), 0.0f);
        const float y = ParseFloatOr(tok.substr(comma + 1), 0.0f);
        pairs.emplace_back(x, y);
    }
    if (pairs.empty()) return false;
    p1 = Vec2(pairs[0].first, pairs[0].second);
    p2 = (pairs.size() >= 2)
             ? Vec2(pairs[1].first, pairs[1].second)
             : p1;
    return true;
}

} // namespace

std::vector<ParsedKeyframe> AlightXmlImporter::ImportKeyframesFromString(const std::string& xml) {
    lastError_.clear();
    std::vector<ParsedKeyframe> out;

    // Walk the string looking for <keyframe ... /> or <keyframe ...></keyframe>
    // Extract the tag body between < and > and hand it off to attribute parsing.
    // Defensive: cap the number of keyframes we parse so a malicious XML
    // file can't hang us with 10^9 tags.
    constexpr size_t kMaxKeys = 100000;
    size_t pos = 0;
    while (pos < xml.size() && out.size() < kMaxKeys) {
        const size_t lt = xml.find("<keyframe", pos);
        if (lt == std::string::npos) break;
        // Make sure the tag name ends properly (space or slash or >).
        const size_t nameEnd = lt + 9; // length of "<keyframe"
        if (nameEnd >= xml.size()) break;
        const char c = xml[nameEnd];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '/' && c != '>') {
            pos = nameEnd; continue;
        }
        const size_t gt = xml.find('>', nameEnd);
        if (gt == std::string::npos) break;
        const std::string body = xml.substr(nameEnd, gt - nameEnd);
        pos = gt + 1;

        ParsedKeyframe kf;
        std::string s;
        if (FindAttr(body, "time",  s)) kf.time  = ParseFloatOr(s, 0.0f);
        if (FindAttr(body, "value", s)) kf.value = ParseFloatOr(s, 0.0f);
        if (FindAttr(body, "curve", s)) {
            kf.hasCurve = ParseCurveAttr(s, kf.p1, kf.p2);
        }
        out.push_back(kf);
    }
    if (out.empty()) {
        lastError_ = "No <keyframe .../> elements found in input.";
    }
    return out;
}

std::vector<ParsedKeyframe> AlightXmlImporter::ImportKeyframesFromFile(const std::string& path) {
    lastError_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        lastError_ = "Could not open file: " + path;
        return {};
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return ImportKeyframesFromString(oss.str());
}
