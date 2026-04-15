#include "here_api.h"
#include "search_common.h"
#include <windows.h>
#include <commctrl.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <cmath>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── HERE API constants ────────────────────────────────────────────────────────
static constexpr double  kMaxEnrichDistM = 120.0; // reject matches farther than this
static constexpr int     kEnrichWorkers  = 4;      // parallel enrichment threads
static constexpr DWORD   kHereTimeoutSec = 10;

// ── Registry helpers ──────────────────────────────────────────────────────────
static constexpr wchar_t kRegPath[] = L"Software\\Easysearch";
static constexpr wchar_t kRegKey[]  = L"HereApiKey";

std::wstring GetHereApiKey() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return {};
    wchar_t buf[512]{};
    DWORD sz = sizeof(buf);
    DWORD type = REG_SZ;
    LSTATUS s = RegQueryValueExW(hk, kRegKey, nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(hk);
    return (s == ERROR_SUCCESS) ? std::wstring(buf) : std::wstring{};
}

void SetHereApiKey(const std::wstring& key) {
    HKEY hk;
    RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr);
    RegSetValueExW(hk, kRegKey, 0, REG_SZ,
        (LPBYTE)key.c_str(), (DWORD)((key.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}

// ── Settings dialog (created in-code, no RC entry needed) ────────────────────

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowTextW(hwnd, L"Settings – HERE API Key");
        // Key label + edit
        HWND hLbl  = CreateWindowExW(0, L"STATIC",
            L"HERE API Key (leave blank to disable enrichment):",
            WS_CHILD|WS_VISIBLE, 12, 14, 430, 16, hwnd, nullptr,
            (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            12, 34, 430, 22, hwnd, (HMENU)101,
            (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
        HWND hNote = CreateWindowExW(0, L"STATIC",
            L"Get a free key at: developer.here.com  (Freemium plan, 1,000 req/day)",
            WS_CHILD|WS_VISIBLE, 12, 62, 430, 16, hwnd, nullptr,
            (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
        HWND hSave = CreateWindowExW(0, L"BUTTON", L"Save",
            WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            268, 92, 80, 26, hwnd, (HMENU)IDOK,
            (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
        HWND hCncl = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            360, 92, 80, 26, hwnd, (HMENU)IDCANCEL,
            (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
        (void)hLbl; (void)hNote; (void)hSave; (void)hCncl;

        // Pre-fill existing key
        std::wstring key = GetHereApiKey();
        SetWindowTextW(hEdit, key.c_str());
        SetFocus(hEdit);
        return FALSE; // we set focus manually
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            HWND hEdit = GetDlgItem(hwnd, 101);
            wchar_t buf[512]{};
            GetWindowTextW(hEdit, buf, 512);
            std::wstring key(buf);
            // Trim whitespace
            auto trim = [](std::wstring s) {
                s.erase(0, s.find_first_not_of(L" \t\r\n"));
                s.erase(s.find_last_not_of(L" \t\r\n") + 1);
                return s;
            };
            SetHereApiKey(trim(key));
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowSettingsDialog(HWND hParent) {
    // Build a minimal DLGTEMPLATE in memory
    struct {
        DLGTEMPLATE dt;
        WORD menu, cls, title;
    } tmpl{};
    tmpl.dt.style = DS_CENTER | DS_MODALFRAME | WS_CAPTION | WS_SYSMENU | WS_POPUP;
    tmpl.dt.cx = 295; // dialog units
    tmpl.dt.cy = 85;
    DialogBoxIndirectW(
        (HINSTANCE)GetWindowLongPtrW(hParent, GWLP_HINSTANCE),
        &tmpl.dt, hParent, SettingsDlgProc);
}

// ── HERE enrichment ───────────────────────────────────────────────────────────

static size_t CurlCb(void* ptr, size_t sz, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

static double Haversine(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0, DEG = 3.14159265358979323846 / 180.0;
    double dLat = (lat2 - lat1) * DEG, dLon = (lon2 - lon1) * DEG;
    double a = std::sin(dLat/2)*std::sin(dLat/2)
             + std::cos(lat1*DEG)*std::cos(lat2*DEG)
             * std::sin(dLon/2)*std::sin(dLon/2);
    return R * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

// Query HERE Discover API for one place. Returns true and fills out `enriched`
// phone/website if a close-enough match is found.
static bool QueryHere(const std::string& name, double lat, double lon,
                      const std::string& apiKey, PlaceItem& enriched) {
    // Build URL
    char url[1024]{};
    // URL-encode the name (basic: replace spaces with %20)
    std::string encName;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') {
            encName += (char)c;
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encName += buf;
        }
    }
    snprintf(url, sizeof(url),
        "https://discover.search.hereapi.com/v1/discover"
        "?at=%.6f,%.6f&q=%s&limit=1&lang=zh-TW&apiKey=%s",
        lat, lon, encName.c_str(), apiKey.c_str());

    CURL* c = curl_easy_init();
    if (!c) return false;

    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "easysearch/1.1");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)kHereTimeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(c);
    long httpCode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || httpCode != 200) return false;

    try {
        json j = json::parse(resp);
        auto& items = j["items"];
        if (!items.is_array() || items.empty()) return false;

        auto& it = items[0];
        // Distance check
        if (it.contains("distance")) {
            double dist = it["distance"].get<double>();
            if (dist > kMaxEnrichDistM) return false;
        } else if (it.contains("position")) {
            double rlat = it["position"]["lat"].get<double>();
            double rlon = it["position"]["lng"].get<double>();
            if (Haversine(lat, lon, rlat, rlon) > kMaxEnrichDistM) return false;
        }

        bool found = false;
        if (it.contains("contacts") && it["contacts"].is_array()
            && !it["contacts"].empty()) {
            auto& ct = it["contacts"][0];
            if (ct.contains("phone") && ct["phone"].is_array()
                && !ct["phone"].empty()) {
                enriched.phone = ct["phone"][0].value("value", "");
                if (!enriched.phone.empty()) found = true;
            }
            if (ct.contains("www") && ct["www"].is_array()
                && !ct["www"].empty()) {
                std::string w = ct["www"][0].value("value", "");
                if (!w.empty()) enriched.website = w;
            }
        }
        if (it.contains("openingHours") && it["openingHours"].is_array()
            && !it["openingHours"].empty()) {
            auto& oh = it["openingHours"][0];
            if (oh.contains("text") && oh["text"].is_array()
                && !oh["text"].empty()) {
                enriched.openingHours = oh["text"][0].get<std::string>();
            }
        }
        return found;
    } catch (...) {
        return false;
    }
}

// ── Shared context for the worker thread pool ─────────────────────────────────

struct EnrichCtx {
    HWND resultWnd;
    std::vector<PlaceItem> items; // snapshot at launch time
    std::string apiKey;
    std::atomic<int> next{0};     // next item index to process
};

static DWORD WINAPI EnrichWorker(LPVOID lpParam) {
    // Worker holds a raw pointer; context lifetime guaranteed by LaunchHereEnrichment
    auto* ctx = static_cast<EnrichCtx*>(lpParam);
    int total = (int)ctx->items.size();

    while (true) {
        int idx = ctx->next.fetch_add(1);
        if (idx >= total) break;

        const PlaceItem& src = ctx->items[idx];
        if (src.lat == 0.0 && src.lon == 0.0) continue;
        if (src.name.empty()) continue;

        PlaceItem enriched = src; // copy so we can fill in gaps
        if (QueryHere(src.name, src.lat, src.lon, ctx->apiKey, enriched)) {
            // Only post if we actually got something new
            if (!enriched.phone.empty() || !enriched.website.empty()) {
                auto* heap = new PlaceItem(enriched);
                if (!PostMessageW(ctx->resultWnd, WM_ITEM_ENRICHED,
                                  (WPARAM)idx, (LPARAM)heap)) {
                    delete heap; // window already closed
                }
            }
        }
    }
    return 0;
}

// Waiter thread – deletes EnrichCtx after all workers finish.
static DWORD WINAPI EnrichWaiter(LPVOID lpParam) {
    auto* data = static_cast<std::pair<std::vector<HANDLE>, EnrichCtx*>*>(lpParam);
    WaitForMultipleObjects((DWORD)data->first.size(),
                           data->first.data(), TRUE, INFINITE);
    for (HANDLE h : data->first) CloseHandle(h);
    delete data->second;   // safe: all workers done
    delete data;
    return 0;
}

void LaunchHereEnrichment(HWND resultWnd,
                          const std::vector<PlaceItem>& items,
                          const std::string& apiKey) {
    if (apiKey.empty() || items.empty()) return;

    auto* ctx = new EnrichCtx{ resultWnd, items, apiKey };

    int workers = min(kEnrichWorkers, (int)items.size());
    auto* waiterData = new std::pair<std::vector<HANDLE>, EnrichCtx*>();
    waiterData->second = ctx;

    for (int i = 0; i < workers; ++i) {
        HANDLE h = CreateThread(nullptr, 0, EnrichWorker, ctx, 0, nullptr);
        if (h) waiterData->first.push_back(h);
    }

    if (waiterData->first.empty()) {
        // No threads started – clean up immediately
        delete ctx;
        delete waiterData;
        return;
    }

    // Waiter thread cleans up ctx after all workers exit
    HANDLE hw = CreateThread(nullptr, 0, EnrichWaiter, waiterData, 0, nullptr);
    if (hw) CloseHandle(hw);
    else {
        // Fallback cleanup (waiter failed to start – leak is acceptable here)
        // Workers still hold ctx, waiter will never free it; log only.
        LogMsg(L"[HERE] waiter thread failed to start");
    }
}
