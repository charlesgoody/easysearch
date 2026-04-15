#pragma once
#include "search_common.h"
#include <string>
#include <vector>

// Search for places inside a named administrative area.
//
// areaName may be:
//   - a single-level name:  "台北市", "大安區", "Taipei City"
//   - a compound CJK name:  "新北市蘆洲區", "台北市大安區"
//     In this case the function automatically splits the string at the
//     city/county boundary and issues a hierarchical Overpass query.
//
// Search strategy (stops as soon as results are found):
//   1. Direct single-area match  (name / name:zh / name:en)
//   2. Nested parent+child query (for compound CJK names)
//   3. Child-area-only fallback  (if parent lookup fails)
bool SearchByAreaName(const std::string& areaName, int presetIndex,
                      std::vector<PlaceItem>& out);
