#pragma once
#include "search_common.h"
#include <vector>

// Build a decreasing radius schedule to try when the initial radius yields no
// results (e.g. 1500 m → 800 → 400 → 200).
std::vector<int> BuildRadiusPlan(double radius);

// Search within a fixed radius around (lat, lon).
// Tries progressively smaller radii until results are found.
bool SearchByRadius(double lat, double lon, double radius,
                    int presetIndex, std::vector<PlaceItem>& out);

// Search within the current map bounding box.
bool SearchByBBox(double south, double west, double north, double east,
                  int presetIndex, std::vector<PlaceItem>& out);
