#include "search_area.h"
#include "place_types.h"
#include <sstream>

// ── CJK area-name splitting ───────────────────────────────────────────────────

struct AreaParts {
    std::string parent; // UTF-8, e.g. "新北市"
    std::string child;  // UTF-8, e.g. "蘆洲區" — empty when no split found
};

// Detect compound CJK administrative names and split at the city/county
// boundary.  Examples:
//   "新北市蘆洲區" → { "新北市", "蘆洲區" }
//   "台北市大安區" → { "台北市", "大安區" }
//   "新竹縣竹東鎮" → { "新竹縣", "竹東鎮" }
//   "台北市"       → { "台北市", ""        }   (no split)
//
// Splits after the first occurrence of a level-1 suffix (市/縣/州) when
// additional characters follow it.
static AreaParts SplitCjkAreaName(const std::string& utf8) {
    std::wstring w = U2W(utf8);
    // Level-1 administrative suffixes
    static const wchar_t kL1[] = {
        L'\u5E02',  // 市
        L'\u7E23',  // 縣
        L'\u5DDE',  // 州
        L'\0'
    };
    for (size_t i = 0; i < w.size(); ++i) {
        for (const wchar_t* p = kL1; *p; ++p) {
            if (w[i] == *p && i + 1 < w.size()) {
                return { W2U(w.substr(0, i + 1)), W2U(w.substr(i + 1)) };
            }
        }
    }
    return { utf8, {} };
}

// ── Query builder helper ──────────────────────────────────────────────────────

static std::string MakeSingleAreaQuery(const char* nameTag,
                                       const std::string& areaName,
                                       const char* key, const char* value) {
    std::ostringstream q;
    q << "[out:json][timeout:30];";
    q << "area[\"" << nameTag << "\"=\"" << areaName << "\"]->.a;";
    q << "(";
    q << "node[\""     << key << "\"=\"" << value << "\"](area.a);";
    q << "way[\""      << key << "\"=\"" << value << "\"](area.a);";
    q << "relation[\"" << key << "\"=\"" << value << "\"](area.a);";
    q << ");out center tags;";
    return q.str();
}

static std::string MakeNestedAreaQuery(const char* parentTag,
                                       const std::string& parentName,
                                       const char* childTag,
                                       const std::string& childName,
                                       const char* key, const char* value) {
    std::ostringstream q;
    q << "[out:json][timeout:30];";
    q << "area[\"" << parentTag << "\"=\"" << parentName << "\"]->.parent;";
    q << "area[\"" << childTag  << "\"=\"" << childName  << "\"](area.parent)->.a;";
    q << "(";
    q << "node[\""     << key << "\"=\"" << value << "\"](area.a);";
    q << "way[\""      << key << "\"=\"" << value << "\"](area.a);";
    q << "relation[\"" << key << "\"=\"" << value << "\"](area.a);";
    q << ");out center tags;";
    return q.str();
}

// ── Public function ───────────────────────────────────────────────────────────

bool SearchByAreaName(const std::string& areaName, int presetIndex,
                      std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    const std::wstring wLabel = preset.label;
    const char* key   = preset.key;
    const char* value = preset.value;

    static const char* kNameTags[] = { "name", "name:zh", "name:en" };

    // ── Step 1: direct single-area match ─────────────────────────────────────
    for (const char* tag : kNameTags) {
        LogMsg(L"[area] direct: [" + U2W(tag) + L"=\"" + U2W(areaName) + L"\"]");
        if (RunOverpassQuery(MakeSingleAreaQuery(tag, areaName, key, value), wLabel, out)
                && !out.empty())
            return true;
        LogMsg(L"[area] no results, trying next strategy...");
    }

    // ── Step 2: compound CJK name → split → nested area query ────────────────
    AreaParts parts = SplitCjkAreaName(areaName);
    if (parts.child.empty()) return false;  // single-level name, nothing more to try

    LogMsg(L"[area] split → parent=\"" + U2W(parts.parent)
           + L"\"  child=\"" + U2W(parts.child) + L"\"");

    // Try the four most useful (parentTag, childTag) combinations.
    // "name" is the primary OSM tag for CJK regions; "name:zh" covers
    // cases where the primary tag holds romanised text.
    struct TagPair { const char* ptag; const char* ctag; };
    static const TagPair kNestedPairs[] = {
        { "name",    "name"    },
        { "name:zh", "name:zh" },
        { "name",    "name:zh" },
        { "name:zh", "name"    },
    };
    for (const auto& pair : kNestedPairs) {
        LogMsg(L"[area] nested: parent[" + U2W(pair.ptag) + L"]"
               + L" + child[" + U2W(pair.ctag) + L"]");
        if (RunOverpassQuery(
                MakeNestedAreaQuery(pair.ptag, parts.parent,
                                    pair.ctag, parts.child,
                                    key, value),
                wLabel, out)
                && !out.empty())
            return true;
        LogMsg(L"[area] no results, trying next tag combination...");
    }

    // ── Step 3: child-area-only fallback ─────────────────────────────────────
    // Useful when the parent area isn't indexed by Overpass or uses a
    // different administrative boundary level.
    LogMsg(L"[area] fallback: child-only \"" + U2W(parts.child) + L"\"");
    for (const char* tag : kNameTags) {
        LogMsg(L"[area] child-only: [" + U2W(tag) + L"=\"" + U2W(parts.child) + L"\"]");
        if (RunOverpassQuery(MakeSingleAreaQuery(tag, parts.child, key, value), wLabel, out)
                && !out.empty())
            return true;
    }

    return false;
}
