#include "search_latlon.h"
#include "place_types.h"
#include <sstream>
#include <algorithm>

std::vector<int> BuildRadiusPlan(double radius) {
    int start = (radius > 0.0) ? (int)(radius + 0.5) : 1500;
    std::vector<int> plan;
    auto addUnique = [&](int r) {
        if (r <= 0) return;
        if (std::find(plan.begin(), plan.end(), r) == plan.end()) plan.push_back(r);
    };
    addUnique(start);
    if (start > 800)  addUnique(800);
    if (start > 400)  addUnique(400);
    if (start > 200)  addUnique(200);
    return plan;
}

bool SearchByRadius(double lat, double lon, double radius,
                    int presetIndex, std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    auto plan = BuildRadiusPlan(radius);
    for (int r : plan) {
        LogMsg(L"radius try = " + std::to_wstring(r));
        std::ostringstream q;
        q << "[out:json][timeout:20];(";
        q << "node(around:" << r << "," << lat << "," << lon
          << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << "way(around:" << r << "," << lat << "," << lon
          << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << "relation(around:" << r << "," << lat << "," << lon
          << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << ");out center tags;";
        if (RunOverpassQuery(q.str(), preset.label, out)) return true;
    }
    return false;
}

bool SearchByBBox(double south, double west, double north, double east,
                  int presetIndex, std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    std::ostringstream q;
    q << "[out:json][timeout:20];(";
    q << "node[\"" << preset.key << "\"=\"" << preset.value << "\"]"
      << "(" << south << "," << west << "," << north << "," << east << ");";
    q << "way[\"" << preset.key << "\"=\"" << preset.value << "\"]"
      << "(" << south << "," << west << "," << north << "," << east << ");";
    q << "relation[\"" << preset.key << "\"=\"" << preset.value << "\"]"
      << "(" << south << "," << west << "," << north << "," << east << ");";
    q << ");out center tags;";
    return RunOverpassQuery(q.str(), preset.label, out);
}
