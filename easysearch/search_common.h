#pragma once
#include <string>
#include <vector>

// Shared data model used by all search translation units.
struct PlaceItem {
    std::string name;
    std::string category;
    std::string brand;
    std::string cuisine;
    std::string address;
    std::string phone;
    std::string website;
    std::string openingHours;
    std::string email;
    double lat = 0.0;
    double lon = 0.0;
};

// ── String utilities (defined in main.cpp) ───────────────────────────────────
std::string  W2U(const std::wstring& s);
std::wstring U2W(const std::string& s);

// ── Logging (defined in main.cpp) ────────────────────────────────────────────
void LogMsg(const std::wstring& msg);

// ── Overpass query runner (defined in main.cpp) ──────────────────────────────
// Tries all mirror endpoints with retries.
// Returns true if the HTTP response was 200 and parsed successfully.
// 'out' is always cleared before writing results.
bool RunOverpassQuery(const std::string& query,
                      const std::wstring& categoryLabel,
                      std::vector<PlaceItem>& out);
