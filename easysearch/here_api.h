#pragma once
#include "search_common.h"
#include <string>
#include <vector>
#include <windows.h>

// ── Registry-backed HERE API key storage ─────────────────────────────────────
std::wstring GetHereApiKey();
void         SetHereApiKey(const std::wstring& key);

// ── Settings dialog ───────────────────────────────────────────────────────────
// Shows a modal dialog that lets the user enter/clear their HERE API key.
// Parent window handle must be the main dialog.
void ShowSettingsDialog(HWND hParent);

// ── HERE Places enrichment ────────────────────────────────────────────────────
//
// WM message posted back to the result window when one item finishes enrichment.
//   wParam = item index (int)
//   lParam = heap-allocated PlaceItem* with enriched fields; window proc owns it
static constexpr UINT WM_ITEM_ENRICHED = WM_APP + 5;

// Launch background enrichment for all items in `items`.
// For each item that has a lat/lon, a HERE Discover query is issued.
// If a match is found within kMaxEnrichDistM and the phone differs, the window
// is notified via WM_ITEM_ENRICHED.
// Safe to call with an empty key – it becomes a no-op.
void LaunchHereEnrichment(HWND resultWnd,
                          const std::vector<PlaceItem>& items,
                          const std::string& apiKey);
