#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <xlsxwriter.h>
#include <WebView2.h>
#include "resource.h"
#include "place_types.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "xlsxwriter.lib")
#pragma comment(lib, "WebView2Loader.dll.lib")

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

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

static const char* kOverpassEndpoints[] = {
    "https://overpass-api.de/api/interpreter",
    "https://lz4.overpass-api.de/api/interpreter",
    "https://z.overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://overpass.nchc.org.tw/api/interpreter"
};
static constexpr int kOverpassEndpointCount = sizeof(kOverpassEndpoints) / sizeof(kOverpassEndpoints[0]);

static HWND g_hDlg = nullptr;
static HWND g_hDebug = nullptr;
static HWND g_hMapHost = nullptr;
static std::vector<PlaceItem> g_items;

struct LayoutState {
    bool ready = false;
    int baseClientW = 0;
    int baseClientH = 0;
    int topMargin = 0;
    int leftMargin = 0;
    int rightMargin = 0;
    int gapX = 0;
    int contentTop = 0;
    int bottomMargin = 0;
    int toolbarHeight = 0;
    int debugWidth = 0;
    SIZE minTrack = { 800, 450 };
};

static LayoutState g_layout;
static double g_bboxSouth = 0.0, g_bboxWest = 0.0, g_bboxNorth = 0.0, g_bboxEast = 0.0;
static bool g_hasMapArea = false;
static bool g_mapReady = false;
static bool g_updatingCombo = false;
static int g_lastPresetIndex = 0;
static DWORD g_uiThreadId = 0;
static bool g_areaMode = true;  // true = area-name mode (primary), false = lat/lon mode
static HFONT g_hDlgFont = nullptr;
static ComPtr<ICoreWebView2Controller> g_webController;
static ComPtr<ICoreWebView2> g_webView;

static constexpr UINT WM_SEARCH_DONE = WM_APP + 1;
static constexpr UINT WM_LOG_MSG     = WM_APP + 2;

static std::string Trim(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string W2U(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::vector<char> buf(n);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, buf.data(), n, nullptr, nullptr);
    return std::string(buf.data());
}

static std::wstring U2W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::vector<wchar_t> buf(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), n);
    return std::wstring(buf.data());
}

static std::wstring ToLowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) { return (wchar_t)towlower(ch); });
    return s;
}

static bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = ToLowerW(haystack);
    std::wstring n = ToLowerW(needle);
    return h.find(n) != std::wstring::npos;
}

static void LogMsg(const std::wstring& msg) {
    if (!g_hDlg) return;
    if (g_uiThreadId && GetCurrentThreadId() != g_uiThreadId) {
        PostMessageW(g_hDlg, WM_LOG_MSG, 0, (LPARAM) new std::wstring(msg));
        return;
    }
    if (!g_hDebug) return;
    int len = GetWindowTextLengthW(g_hDebug);
    SendMessageW(g_hDebug, EM_SETSEL, len, len);
    SendMessageW(g_hDebug, EM_REPLACESEL, FALSE, (LPARAM)(msg + L"\r\n").c_str());
}

static void LogHresult(const wchar_t* prefix, HRESULT hr) {
    wchar_t buf[256]{};
    swprintf_s(buf, L"%s HRESULT=0x%08X", prefix, static_cast<unsigned>(hr));
    LogMsg(buf);
}

static void SetMapFallbackText(const wchar_t* text) {
    if (!g_hMapHost) return;
    SetWindowTextW(g_hMapHost, text);
}

static std::wstring GetDlgItemTextString(HWND hwnd, int id) {
    HWND h = GetDlgItem(hwnd, id);
    int len = GetWindowTextLengthW(h);
    if (len < 0) return L"";
    std::vector<wchar_t> buf(len + 1, L'\0');
    GetWindowTextW(h, buf.data(), (int)buf.size());
    return std::wstring(buf.data());
}

static void SetNumberEdit(HWND hwnd, int id, double value) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.6f", value);
    SetDlgItemTextW(hwnd, id, buf);
}

static RECT GetChildRectInClient(HWND parent, int id) {
    RECT rc{};
    HWND h = GetDlgItem(parent, id);
    GetWindowRect(h, &rc);
    MapWindowPoints(nullptr, parent, reinterpret_cast<LPPOINT>(&rc), 2);
    return rc;
}

static void CaptureLayout(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    RECT rcMap   = GetChildRectInClient(hwnd, IDC_MAPHOST);
    RECT rcDebug = GetChildRectInClient(hwnd, IDC_DEBUG);
    RECT rcBtnSearch = GetChildRectInClient(hwnd, IDC_BTN_SEARCH);

    g_layout.baseClientW  = client.right - client.left;
    g_layout.baseClientH  = client.bottom - client.top;
    g_layout.topMargin    = rcBtnSearch.top;
    g_layout.leftMargin   = rcMap.left;
    g_layout.rightMargin  = client.right - rcDebug.right;
    g_layout.gapX         = rcDebug.left - rcMap.right;
    g_layout.contentTop   = rcMap.top;
    g_layout.bottomMargin = client.bottom - rcDebug.bottom;
    g_layout.toolbarHeight = rcMap.top - g_layout.topMargin;
    g_layout.debugWidth   = rcDebug.right - rcDebug.left;

    g_layout.ready = true;
}

static void ResizeWebView();

static void LayoutControls(HWND hwnd) {
    if (!g_layout.ready) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;

    int debugW = g_layout.debugWidth;
    int debugX = cx - g_layout.rightMargin - debugW;
    int leftW = debugX - g_layout.gapX - g_layout.leftMargin;
    if (leftW < 420) {
        leftW = 420;
        debugX = g_layout.leftMargin + leftW + g_layout.gapX;
        debugW = max(240, cx - debugX - g_layout.rightMargin);
    }

    int contentTop = g_layout.contentTop;
    int contentH = cy - contentTop - g_layout.bottomMargin;
    if (contentH < 260) contentH = 260;

    MoveWindow(GetDlgItem(hwnd, IDC_MAPHOST), g_layout.leftMargin, contentTop, leftW, contentH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_DEBUG),   debugX, contentTop, debugW, contentH, TRUE);

    ResizeWebView();
}

static size_t CurlWriteCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* str = static_cast<std::string*>(userdata);
    str->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static void AppendPart(std::string& out, const std::string& part, const char* sep = ", ") {
    std::string t = Trim(part);
    if (t.empty()) return;
    if (!out.empty()) out += sep;
    out += t;
}

static std::string GetTag(const json& tags, const char* k1, const char* k2 = nullptr) {
    if (tags.contains(k1) && tags[k1].is_string()) return Trim(tags[k1].get<std::string>());
    if (k2 && tags.contains(k2) && tags[k2].is_string()) return Trim(tags[k2].get<std::string>());
    return {};
}

static std::string BuildDetailedAddress(const json& tags) {
    if (tags.contains("addr:full") && tags["addr:full"].is_string()) return tags["addr:full"].get<std::string>();
    std::string address;
    if (tags.contains("addr:postcode")) AppendPart(address, tags["addr:postcode"].get<std::string>(), " ");
    if (tags.contains("addr:country")) AppendPart(address, tags["addr:country"].get<std::string>(), " ");
    if (tags.contains("addr:state")) AppendPart(address, tags["addr:state"].get<std::string>());
    if (tags.contains("addr:province")) AppendPart(address, tags["addr:province"].get<std::string>());
    if (tags.contains("addr:city")) AppendPart(address, tags["addr:city"].get<std::string>());
    if (tags.contains("addr:district")) AppendPart(address, tags["addr:district"].get<std::string>());
    if (tags.contains("addr:subdistrict")) AppendPart(address, tags["addr:subdistrict"].get<std::string>());
    if (tags.contains("addr:suburb")) AppendPart(address, tags["addr:suburb"].get<std::string>());
    if (tags.contains("addr:place")) AppendPart(address, tags["addr:place"].get<std::string>());
    if (tags.contains("addr:street")) AppendPart(address, tags["addr:street"].get<std::string>());
    if (tags.contains("addr:housenumber")) AppendPart(address, tags["addr:housenumber"].get<std::string>(), " ");
    if (address.empty()) {
        if (tags.contains("contact:street")) AppendPart(address, tags["contact:street"].get<std::string>());
        if (tags.contains("contact:city")) AppendPart(address, tags["contact:city"].get<std::string>());
    }
    return address;
}

static bool ParsePlacesFromJson(const std::string& resp, const std::wstring& categoryLabel, std::vector<PlaceItem>& out) {
    try {
        json j = json::parse(resp);
        if (!j.contains("elements") || !j["elements"].is_array()) {
            LogMsg(L"invalid response: missing elements[]");
            return false;
        }

        out.clear();
        for (auto& e : j["elements"]) {
            if (!e.contains("tags")) continue;
            const auto& t = e["tags"];
            PlaceItem p;
            p.name = GetTag(t, "name");
            if (p.name.empty()) continue;
            p.category = W2U(categoryLabel);
            p.brand = GetTag(t, "brand", "operator");
            p.cuisine = GetTag(t, "cuisine");
            p.address = BuildDetailedAddress(t);
            p.phone = GetTag(t, "contact:phone", "phone");
            p.website = GetTag(t, "contact:website", "website");
            p.openingHours = GetTag(t, "opening_hours");
            p.email = GetTag(t, "contact:email", "email");

            if (e.contains("lat") && e.contains("lon")) {
                p.lat = e["lat"].get<double>();
                p.lon = e["lon"].get<double>();
            }
            else if (e.contains("center")) {
                p.lat = e["center"].value("lat", 0.0);
                p.lon = e["center"].value("lon", 0.0);
            }
            out.push_back(std::move(p));
        }
        return true;
    }
    catch (const std::exception& ex) {
        LogMsg(L"json parse error");
        LogMsg(U2W(std::string(ex.what())));
        return false;
    }
}

static bool PerformOverpassPost(const std::string& endpoint, const std::string& query, std::string& resp, long& httpCode, std::string& contentType, CURLcode& rc) {
    resp.clear();
    httpCode = 0;
    contentType.clear();

    CURL* esc = curl_easy_init();
    if (!esc) {
        rc = CURLE_FAILED_INIT;
        return false;
    }
    char* enc = curl_easy_escape(esc, query.c_str(), (int)query.size());
    if (!enc) {
        curl_easy_cleanup(esc);
        rc = CURLE_FAILED_INIT;
        return false;
    }
    std::string postBody = "data=" + std::string(enc);
    curl_free(enc);
    curl_easy_cleanup(esc);

    CURL* c = curl_easy_init();
    if (!c) {
        rc = CURLE_FAILED_INIT;
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");

    curl_easy_setopt(c, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, postBody.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)postBody.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "easysearch/1.1");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 35L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);

    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
    char* ct = nullptr;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) contentType = ct;

    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

static bool RunOverpassQuery(const std::string& query, const std::wstring& categoryLabel, std::vector<PlaceItem>& out) {
    for (int endpointIndex = 0; endpointIndex < kOverpassEndpointCount; ++endpointIndex) {
        const std::string endpoint = kOverpassEndpoints[endpointIndex];
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::string resp;
            std::string contentType;
            long httpCode = 0;
            CURLcode rc = CURLE_OK;

            LogMsg(L"endpoint = " + U2W(endpoint));
            LogMsg(L"attempt = " + std::to_wstring(attempt + 1));
            bool ok = PerformOverpassPost(endpoint, query, resp, httpCode, contentType, rc);

            if (!ok) {
                LogMsg(L"network error");
                LogMsg(U2W(curl_easy_strerror(rc)));
            }
            else {
                LogMsg(L"HTTP status = " + std::to_wstring(httpCode));
                if (!contentType.empty()) LogMsg(L"content-type = " + U2W(contentType));
                std::string preview = resp.substr(0, 500);
                LogMsg(L"response preview:");
                LogMsg(U2W(preview));

                if (httpCode == 200 && ParsePlacesFromJson(resp, categoryLabel, out)) {
                    return true;
                }
            }

            if (attempt < 2) {
                DWORD backoffMs = (attempt == 0) ? 2000 : 5000;
                LogMsg(L"backoff ms = " + std::to_wstring(backoffMs));
                Sleep(backoffMs);
            }
        }
    }
    return false;
}

static std::vector<int> BuildRadiusPlan(double radius) {
    int start = (radius > 0.0) ? (int)(radius + 0.5) : 1500;
    std::vector<int> plan;
    auto addUnique = [&](int r) {
        if (r <= 0) return;
        if (std::find(plan.begin(), plan.end(), r) == plan.end()) plan.push_back(r);
        };
    addUnique(start);
    if (start > 800) addUnique(800);
    if (start > 400) addUnique(400);
    if (start > 200) addUnique(200);
    return plan;
}

static bool SearchByRadius(double lat, double lon, double radius, int presetIndex, std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    auto plan = BuildRadiusPlan(radius);
    for (int r : plan) {
        LogMsg(L"radius try = " + std::to_wstring(r));
        std::ostringstream q;
        q << "[out:json][timeout:20];(";
        q << "node(around:" << r << "," << lat << "," << lon << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << "way(around:" << r << "," << lat << "," << lon << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << "relation(around:" << r << "," << lat << "," << lon << ")[\"" << preset.key << "\"=\"" << preset.value << "\"];";
        q << ");out center tags;";
        if (RunOverpassQuery(q.str(), preset.label, out)) return true;
    }
    return false;
}

static bool SearchByBBox(double south, double west, double north, double east, int presetIndex, std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    std::ostringstream q;
    q << "[out:json][timeout:20];(";
    q << "node[\"" << preset.key << "\"=\"" << preset.value << "\"](" << south << "," << west << "," << north << "," << east << ");";
    q << "way[\"" << preset.key << "\"=\"" << preset.value << "\"](" << south << "," << west << "," << north << "," << east << ");";
    q << "relation[\"" << preset.key << "\"=\"" << preset.value << "\"](" << south << "," << west << "," << north << "," << east << ");";
    q << ");out center tags;";
    return RunOverpassQuery(q.str(), preset.label, out);
}

// Search by administrative area name (e.g. "台北市", "大安區", "Taipei City").
// Tries name → name:zh → name:en in sequence to handle both Chinese and English input.
static bool SearchByAreaName(const std::string& areaName, int presetIndex, std::vector<PlaceItem>& out) {
    const auto& preset = kPlaceTypes[presetIndex];
    static const char* kNameTags[] = { "name", "name:zh", "name:en" };

    for (const char* tag : kNameTags) {
        std::ostringstream q;
        q << "[out:json][timeout:30];";
        q << "area[\"" << tag << "\"=\"" << areaName << "\"]->.a;";
        q << "(";
        q << "node[\"" << preset.key << "\"=\"" << preset.value << "\"](area.a);";
        q << "way[\"" << preset.key << "\"=\"" << preset.value << "\"](area.a);";
        q << "relation[\"" << preset.key << "\"=\"" << preset.value << "\"](area.a);";
        q << ");out center tags;";

        LogMsg(L"Area search: [" + U2W(tag) + L"=\"" + U2W(areaName) + L"\"]");
        bool ok = RunOverpassQuery(q.str(), preset.label, out);
        if (ok && !out.empty()) return true;
        if (ok && out.empty()) LogMsg(L"No results, trying next name tag...");
    }
    return false;
}

struct SearchContext {
    HWND hwnd;
    bool byBBox       = false;
    bool byAreaName   = false;
    double lat = 0, lon = 0, radius = 0;
    double south = 0, west = 0, north = 0, east = 0;
    std::string areaName;
    int presetIndex = 0;
    SYSTEMTIME startTime{};
    bool succeeded = false;
    std::vector<PlaceItem> result;
};

static DWORD WINAPI SearchThreadProc(LPVOID lpParam) {
    auto* ctx = static_cast<SearchContext*>(lpParam);
    if (ctx->byAreaName)
        ctx->succeeded = SearchByAreaName(ctx->areaName, ctx->presetIndex, ctx->result);
    else if (ctx->byBBox)
        ctx->succeeded = SearchByBBox(ctx->south, ctx->west, ctx->north, ctx->east, ctx->presetIndex, ctx->result);
    else
        ctx->succeeded = SearchByRadius(ctx->lat, ctx->lon, ctx->radius, ctx->presetIndex, ctx->result);
    PostMessageW(ctx->hwnd, WM_SEARCH_DONE, 0, (LPARAM)ctx);
    return 0;
}

static std::wstring MakeExportFilename(int presetIndex, const SYSTEMTIME& st, const wchar_t* ext) {
    int hour12 = st.wHour % 12;
    if (hour12 == 0) hour12 = 12;
    const wchar_t* ampm = (st.wHour < 12) ? L"am" : L"pm";
    std::wstring label = (presetIndex >= 0 && presetIndex < kPlaceTypeCount)
        ? std::wstring(kPlaceTypes[presetIndex].label) : L"unknown";
    std::transform(label.begin(), label.end(), label.begin(),
        [](wchar_t ch) { return (wchar_t)towlower(ch); });
    std::replace(label.begin(), label.end(), L' ', L'_');
    wchar_t buf[260]{};
    swprintf_s(buf, L"%02d_%02d_%04d_%02d_%02d%s_%s%s",
        st.wMonth, st.wDay, st.wYear,
        hour12, st.wMinute, ampm,
        label.c_str(), ext);
    return buf;
}

static void InitListColumns(HWND hList) {
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.cx = 140; c.pszText = (LPWSTR)L"Name"; ListView_InsertColumn(hList, 0, &c);
    c.cx = 90;  c.pszText = (LPWSTR)L"Category"; ListView_InsertColumn(hList, 1, &c);
    c.cx = 90;  c.pszText = (LPWSTR)L"Brand"; ListView_InsertColumn(hList, 2, &c);
    c.cx = 90;  c.pszText = (LPWSTR)L"Cuisine"; ListView_InsertColumn(hList, 3, &c);
    c.cx = 180; c.pszText = (LPWSTR)L"Address"; ListView_InsertColumn(hList, 4, &c);
    c.cx = 110; c.pszText = (LPWSTR)L"Phone"; ListView_InsertColumn(hList, 5, &c);
    c.cx = 140; c.pszText = (LPWSTR)L"Website"; ListView_InsertColumn(hList, 6, &c);
    c.cx = 110; c.pszText = (LPWSTR)L"Opening Hours"; ListView_InsertColumn(hList, 7, &c);
    c.cx = 120; c.pszText = (LPWSTR)L"Email"; ListView_InsertColumn(hList, 8, &c);
    c.cx = 85;  c.pszText = (LPWSTR)L"Lat"; ListView_InsertColumn(hList, 9, &c);
    c.cx = 85;  c.pszText = (LPWSTR)L"Lon"; ListView_InsertColumn(hList, 10, &c);
}

static void UpdateListView(HWND hList, const std::vector<PlaceItem>& items) {
    ListView_DeleteAllItems(hList);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& p = items[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring wname = U2W(p.name);
        it.pszText = (LPWSTR)wname.c_str();
        ListView_InsertItem(hList, &it);
        std::wstring wcat = U2W(p.category);
        std::wstring wbrand = U2W(p.brand);
        std::wstring wcuisine = U2W(p.cuisine);
        std::wstring waddr = U2W(p.address);
        std::wstring wphone = U2W(p.phone);
        std::wstring wweb = U2W(p.website);
        std::wstring whours = U2W(p.openingHours);
        std::wstring wemail = U2W(p.email);
        wchar_t buf[64]{};
        ListView_SetItemText(hList, (int)i, 1, (LPWSTR)wcat.c_str());
        ListView_SetItemText(hList, (int)i, 2, (LPWSTR)wbrand.c_str());
        ListView_SetItemText(hList, (int)i, 3, (LPWSTR)wcuisine.c_str());
        ListView_SetItemText(hList, (int)i, 4, (LPWSTR)waddr.c_str());
        ListView_SetItemText(hList, (int)i, 5, (LPWSTR)wphone.c_str());
        ListView_SetItemText(hList, (int)i, 6, (LPWSTR)wweb.c_str());
        ListView_SetItemText(hList, (int)i, 7, (LPWSTR)whours.c_str());
        ListView_SetItemText(hList, (int)i, 8, (LPWSTR)wemail.c_str());
        swprintf_s(buf, L"%.6f", p.lat); ListView_SetItemText(hList, (int)i, 9, buf);
        swprintf_s(buf, L"%.6f", p.lon); ListView_SetItemText(hList, (int)i, 10, buf);
    }
}

static std::string CsvEscape(const std::string& s) {
    std::string out = s;
    size_t pos = 0;
    while ((pos = out.find('"', pos)) != std::string::npos) {
        out.insert(pos, 1, '"');
        pos += 2;
    }
    return '"' + out + '"';
}

static bool ExportCSVUtf8Bom(const std::wstring& path, const std::vector<PlaceItem>& items) {
    FILE* fp = nullptr;
    errno_t err = _wfopen_s(&fp, path.c_str(), L"wb");
    if (err != 0 || !fp) return false;
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, fp);
    std::string header = "Name,Category,Brand,Cuisine,Address,Phone,Website,OpeningHours,Email,Lat,Lon\r\n";
    fwrite(header.data(), 1, header.size(), fp);
    for (const auto& p : items) {
        std::ostringstream row;
        row << CsvEscape(p.name) << ','
            << CsvEscape(p.category) << ','
            << CsvEscape(p.brand) << ','
            << CsvEscape(p.cuisine) << ','
            << CsvEscape(p.address) << ','
            << CsvEscape(p.phone) << ','
            << CsvEscape(p.website) << ','
            << CsvEscape(p.openingHours) << ','
            << CsvEscape(p.email) << ','
            << p.lat << ',' << p.lon << "\r\n";
        auto s = row.str();
        fwrite(s.data(), 1, s.size(), fp);
    }
    fclose(fp);
    return true;
}

static bool ExportXLSX(const std::wstring& path, const std::vector<PlaceItem>& items) {
    std::string utf8Path = W2U(path);
    lxw_workbook* workbook = workbook_new(utf8Path.c_str());
    if (!workbook) return false;
    lxw_worksheet* ws = workbook_add_worksheet(workbook, "places");
    worksheet_write_string(ws, 0, 0, "Name", nullptr);
    worksheet_write_string(ws, 0, 1, "Category", nullptr);
    worksheet_write_string(ws, 0, 2, "Brand", nullptr);
    worksheet_write_string(ws, 0, 3, "Cuisine", nullptr);
    worksheet_write_string(ws, 0, 4, "Address", nullptr);
    worksheet_write_string(ws, 0, 5, "Phone", nullptr);
    worksheet_write_string(ws, 0, 6, "Website", nullptr);
    worksheet_write_string(ws, 0, 7, "Opening Hours", nullptr);
    worksheet_write_string(ws, 0, 8, "Email", nullptr);
    worksheet_write_string(ws, 0, 9, "Lat", nullptr);
    worksheet_write_string(ws, 0, 10, "Lon", nullptr);
    worksheet_set_column(ws, 0, 0, 24, nullptr);
    worksheet_set_column(ws, 1, 3, 16, nullptr);
    worksheet_set_column(ws, 4, 4, 42, nullptr);
    worksheet_set_column(ws, 5, 8, 22, nullptr);
    worksheet_set_column(ws, 9, 10, 14, nullptr);
    for (lxw_row_t i = 0; i < (lxw_row_t)items.size(); ++i) {
        const auto& p = items[i];
        worksheet_write_string(ws, i + 1, 0, p.name.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 1, p.category.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 2, p.brand.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 3, p.cuisine.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 4, p.address.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 5, p.phone.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 6, p.website.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 7, p.openingHours.c_str(), nullptr);
        worksheet_write_string(ws, i + 1, 8, p.email.c_str(), nullptr);
        worksheet_write_number(ws, i + 1, 9, p.lat, nullptr);
        worksheet_write_number(ws, i + 1, 10, p.lon, nullptr);
    }
    return workbook_close(workbook) == LXW_NO_ERROR;
}

static bool PresetMatchesQuery(int index, const std::wstring& query) {
    if (query.empty()) return true;
    const auto& p = kPlaceTypes[index];
    if (ContainsNoCase(p.label, query)) return true;
    if (ContainsNoCase(U2W(p.key), query)) return true;
    if (ContainsNoCase(U2W(p.value), query)) return true;
    return false;
}

static void PopulateTypeCombo(HWND hwnd, const std::wstring& filterText) {
    HWND hCombo = GetDlgItem(hwnd, IDC_TYPE_COMBO);
    g_updatingCombo = true;
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kPlaceTypeCount; ++i) {
        if (!PresetMatchesQuery(i, filterText)) continue;
        int row = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)kPlaceTypes[i].label);
        SendMessageW(hCombo, CB_SETITEMDATA, row, (LPARAM)i);
    }
    if (SendMessageW(hCombo, CB_GETCOUNT, 0, 0) > 0) SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    SetWindowTextW(hCombo, filterText.c_str());
    SendMessageW(hCombo, CB_SETEDITSEL, 0, MAKELPARAM((WORD)filterText.size(), (WORD)filterText.size()));
    if (!filterText.empty()) SendMessageW(hCombo, CB_SHOWDROPDOWN, TRUE, 0);
    g_updatingCombo = false;
}

static void SelectPresetByActualIndex(HWND hwnd, int actualIndex) {
    HWND hCombo = GetDlgItem(hwnd, IDC_TYPE_COMBO);
    int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        LRESULT data = SendMessageW(hCombo, CB_GETITEMDATA, i, 0);
        if ((int)data == actualIndex) {
            SendMessageW(hCombo, CB_SETCURSEL, i, 0);
            SetWindowTextW(hCombo, kPlaceTypes[actualIndex].label);
            return;
        }
    }
}

static int FindPresetIndexByLabel(const wchar_t* label) {
    for (int i = 0; i < kPlaceTypeCount; ++i) {
        if (_wcsicmp(kPlaceTypes[i].label, label) == 0) return i;
    }
    return -1;
}

static int GetSelectedPresetIndex(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_TYPE_COMBO);
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) {
        LRESULT idx = SendMessageW(hCombo, CB_GETITEMDATA, sel, 0);
        if (idx >= 0 && idx < kPlaceTypeCount) return (int)idx;
    }

    std::wstring typed = GetDlgItemTextString(hwnd, IDC_TYPE_COMBO);
    if (typed.empty()) return -1;

    for (int i = 0; i < kPlaceTypeCount; ++i) {
        if (ToLowerW(kPlaceTypes[i].label) == ToLowerW(typed)) return i;
    }
    for (int i = 0; i < kPlaceTypeCount; ++i) {
        if (PresetMatchesQuery(i, typed)) return i;
    }
    return -1;
}

static void InitTypeCombo(HWND hwnd) {
    PopulateTypeCombo(hwnd, L"");
    int restaurant = FindPresetIndexByLabel(L"Restaurant");
    if (restaurant >= 0) SelectPresetByActualIndex(hwnd, restaurant);
}

// IDs to show/hide when toggling search mode.
static const int kAreaModeControls[]   = { IDC_AREA_STATIC, IDC_AREA_NAME, IDC_BTN_SEARCH_AREA_NAME };
static const int kLatLonModeControls[] = { IDC_LAT_STATIC, IDC_LAT, IDC_LON_STATIC, IDC_LON,
                                            IDC_RADIUS_STATIC, IDC_RADIUS, IDC_BTN_SEARCH, IDC_BTN_AREA };

static void SetSearchMode(HWND hwnd, bool areaMode) {
    g_areaMode = areaMode;

    for (int id : kAreaModeControls)
        ShowWindow(GetDlgItem(hwnd, id), areaMode ? SW_SHOW : SW_HIDE);
    for (int id : kLatLonModeControls)
        ShowWindow(GetDlgItem(hwnd, id), areaMode ? SW_HIDE : SW_SHOW);

    // Restore IDC_BTN_AREA enabled state when returning to lat/lon mode
    if (!areaMode)
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_AREA), (g_mapReady && g_hasMapArea) ? TRUE : FALSE);

    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_TOGGLE_MODE),
                   areaMode ? L"[ Lat/Lon ]" : L"[ Area ]");
}

static std::wstring GetMapHtml() {
    return LR"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
html, body, #map { height:100%; width:100%; margin:0; padding:0; overflow:hidden; }
#crosshair { position:absolute; left:50%; top:50%; width:18px; height:18px; margin-left:-9px; margin-top:-9px; pointer-events:none; z-index:1000; }
#crosshair:before, #crosshair:after { content:''; position:absolute; background:#d00; }
#crosshair:before { left:8px; top:0; width:2px; height:18px; }
#crosshair:after { left:0; top:8px; width:18px; height:2px; }
#tip { position:absolute; left:10px; top:10px; background:rgba(255,255,255,.92); padding:6px 8px; border-radius:6px; z-index:1000; font:12px sans-serif; }
</style>
</head>
<body>
<div id="map"></div>
<div id="crosshair"></div>
<div id="tip">Drag map, then click Search Area or Search Center</div>
<script>
const map = L.map('map', { zoomControl:true, dragging:true }).setView([25.0330, 121.5654], 14);
L.tileLayer('https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png', {
    maxZoom: 20,
    subdomains: 'abcd',
    attribution: '&copy; OpenStreetMap contributors &copy; CARTO'
}).addTo(map);
function postState() {
    const c = map.getCenter();
    const b = map.getBounds();
    const msg = {
        type: 'mapMoved',
        lat: c.lat,
        lon: c.lng,
        south: b.getSouth(),
        west: b.getWest(),
        north: b.getNorth(),
        east: b.getEast(),
        zoom: map.getZoom()
    };
    if (window.chrome && window.chrome.webview) {
        chrome.webview.postMessage(JSON.stringify(msg));
    }
}
map.on('moveend', postState);
window.setCenterFromHost = function(lat, lon, zoom) {
    map.setView([lat, lon], zoom || map.getZoom());
};
postState();
</script>
</body>
</html>)HTML";
}

static void ResizeWebView() {
    if (!g_webController || !g_hMapHost) return;
    RECT rc{};
    GetClientRect(g_hMapHost, &rc);
    g_webController->put_Bounds(rc);
}

static void DisableMapFeatures() {
    g_mapReady = false;
    g_hasMapArea = false;
    if (g_hDlg) EnableWindow(GetDlgItem(g_hDlg, IDC_BTN_AREA), FALSE);
    SetMapFallbackText(L"Map unavailable. Install WebView2 Runtime or fix loader.\r\nYou can still search by Lat/Lon.");
}

static void InitWebView(HWND hwnd) {
    g_hMapHost = GetDlgItem(hwnd, IDC_MAPHOST);
    DisableMapFeatures();

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT hrEnv, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hrEnv) || !env) {
                    LogHresult(L"CreateEnvironment failed.", hrEnv);
                    DisableMapFeatures();
                    return hrEnv;
                }
                return env->CreateCoreWebView2Controller(
                    g_hMapHost,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT hrCtl, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hrCtl) || !controller) {
                                LogHresult(L"CreateController failed.", hrCtl);
                                DisableMapFeatures();
                                return hrCtl;
                            }
                            g_webController = controller;
                            g_webController->get_CoreWebView2(&g_webView);
                            ResizeWebView();
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webView->get_Settings(&settings)) && settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                            }
                            g_webView->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        if (FAILED(args->TryGetWebMessageAsString(&msg)) || !msg) return S_OK;
                                        std::wstring wmsg(msg);
                                        CoTaskMemFree(msg);
                                        try {
                                            json j = json::parse(W2U(wmsg));
                                            if (j.value("type", "") == "mapMoved") {
                                                double lat = j.value("lat", 0.0);
                                                double lon = j.value("lon", 0.0);
                                                g_bboxSouth = j.value("south", 0.0);
                                                g_bboxWest = j.value("west", 0.0);
                                                g_bboxNorth = j.value("north", 0.0);
                                                g_bboxEast = j.value("east", 0.0);
                                                g_hasMapArea = true;
                                                if (g_hDlg) {
                                                    SetNumberEdit(g_hDlg, IDC_LAT, lat);
                                                    SetNumberEdit(g_hDlg, IDC_LON, lon);
                                                    EnableWindow(GetDlgItem(g_hDlg, IDC_BTN_AREA), TRUE);
                                                }
                                            }
                                        }
                                        catch (...) {
                                        }
                                        return S_OK;
                                    }).Get(),
                                        nullptr);
                            g_webView->NavigateToString(GetMapHtml().c_str());
                            g_mapReady = true;
                            LogMsg(L"Map ready.");
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(hr)) {
        LogHresult(L"CreateCoreWebView2EnvironmentWithOptions returned.", hr);
        DisableMapFeatures();
    }
}

static constexpr int IDC_RES_CSV  = 1;
static constexpr int IDC_RES_XLS  = 2;
static constexpr int IDC_RES_LIST = 3;
static constexpr int RES_TOOLBAR_H = 34;

struct ResultWindow {
    std::vector<PlaceItem> items;
    int presetIndex = 0;
    SYSTEMTIME searchTime{};
    HWND hList = nullptr;
};

static LRESULT CALLBACK ResultWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* rw = static_cast<ResultWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);
        HINSTANCE hInst = cs->hInstance;
        HWND hCsvBtn = CreateWindowExW(0, L"BUTTON", L"Export CSV",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            4, 4, 110, 26, hwnd, (HMENU)(UINT_PTR)IDC_RES_CSV, hInst, nullptr);
        HWND hXlsBtn = CreateWindowExW(0, L"BUTTON", L"Export Excel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 4, 110, 26, hwnd, (HMENU)(UINT_PTR)IDC_RES_XLS, hInst, nullptr);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, RES_TOOLBAR_H, rc.right, rc.bottom - RES_TOOLBAR_H,
            hwnd, (HMENU)(UINT_PTR)IDC_RES_LIST, hInst, nullptr);
        if (g_hDlgFont) {
            SendMessageW(hCsvBtn, WM_SETFONT, (WPARAM)g_hDlgFont, FALSE);
            SendMessageW(hXlsBtn, WM_SETFONT, (WPARAM)g_hDlgFont, FALSE);
            SendMessageW(hList,   WM_SETFONT, (WPARAM)g_hDlgFont, FALSE);
        }
        InitListColumns(hList);
        UpdateListView(hList, rw->items);
        rw->hList = hList;
        return 0;
    }
    case WM_SIZE: {
        auto* rw = reinterpret_cast<ResultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (rw && rw->hList) {
            int cx = LOWORD(lParam), cy = HIWORD(lParam);
            MoveWindow(rw->hList, 0, RES_TOOLBAR_H, cx, cy - RES_TOOLBAR_H, TRUE);
        }
        return 0;
    }
    case WM_COMMAND: {
        auto* rw = reinterpret_cast<ResultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!rw) break;
        if (LOWORD(wParam) == IDC_RES_CSV) {
            std::wstring name = MakeExportFilename(rw->presetIndex, rw->searchTime, L".csv");
            if (ExportCSVUtf8Bom(name, rw->items))
                MessageBoxW(hwnd, (L"Exported: " + name).c_str(), L"Export CSV", MB_OK);
            else
                MessageBoxW(hwnd, L"Export failed", L"Export CSV", MB_OK | MB_ICONERROR);
        } else if (LOWORD(wParam) == IDC_RES_XLS) {
            std::wstring name = MakeExportFilename(rw->presetIndex, rw->searchTime, L".xlsx");
            if (ExportXLSX(name, rw->items))
                MessageBoxW(hwnd, (L"Exported: " + name).c_str(), L"Export Excel", MB_OK);
            else
                MessageBoxW(hwnd, L"Export failed", L"Export Excel", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_DESTROY: {
        auto* rw = reinterpret_cast<ResultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        delete rw;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CreateResultWindow(HWND hOwner, SearchContext* ctx) {
    const SYSTEMTIME& st = ctx->startTime;
    int hour12 = st.wHour % 12;
    if (hour12 == 0) hour12 = 12;
    const wchar_t* ampm = (st.wHour < 12) ? L"am" : L"pm";

    std::wstring title = kPlaceTypes[ctx->presetIndex].label;
    title += ctx->byBBox ? L" \u2014 Search Area" : L" \u2014 Search Center";
    wchar_t dateBuf[64]{};
    swprintf_s(dateBuf, L"  %02d/%02d/%04d %02d:%02d%s",
        st.wMonth, st.wDay, st.wYear, hour12, st.wMinute, ampm);
    title += dateBuf;
    title += L"  (" + std::to_wstring(ctx->result.size()) + L" results)";

    auto* rw = new ResultWindow{};
    rw->items = std::move(ctx->result);
    rw->presetIndex = ctx->presetIndex;
    rw->searchTime = ctx->startTime;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"EasySearchResult",
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 480,
        nullptr, nullptr, GetModuleHandleW(nullptr), rw
    );
    if (hwnd) ShowWindow(hwnd, SW_SHOW);
    else delete rw;
}

INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {

        HICON hBig = (HICON)LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_APPICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            0
        );

        HICON hSmall = (HICON)LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_APPICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            0
        );

        if (hBig) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        }
        if (hSmall) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
        }

        g_hDlg = hwnd;
        g_uiThreadId = GetCurrentThreadId();
        g_hDlgFont = (HFONT)SendMessageW(GetDlgItem(hwnd, IDC_BTN_SEARCH), WM_GETFONT, 0, 0);
        g_hDebug = GetDlgItem(hwnd, IDC_DEBUG);
        SetDlgItemTextW(hwnd, IDC_LAT, L"25.033000");
        SetDlgItemTextW(hwnd, IDC_LON, L"121.565400");
        SetDlgItemTextW(hwnd, IDC_RADIUS, L"1500");
        InitTypeCombo(hwnd);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CSV), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_XLS), FALSE);
        InitWebView(hwnd);
        CaptureLayout(hwnd);
        SetSearchMode(hwnd, true);  // start in area-name mode (primary)
        LogMsg(L"Ready. Enter an area name (e.g. \u53f0\u5317\u5e02, \u5927\u5b89\u5340) and press Search by Area.");
        return TRUE;
    }
    case WM_SIZE:
        LayoutControls(hwnd);
        return TRUE;
    case WM_GETMINMAXINFO: {
        if (g_layout.ready) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = g_layout.minTrack.cx;
            mmi->ptMinTrackSize.y = g_layout.minTrack.cy;
        }
        return TRUE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_TYPE_COMBO:
            if (HIWORD(wParam) == CBN_EDITCHANGE && !g_updatingCombo) {
                PopulateTypeCombo(hwnd, GetDlgItemTextString(hwnd, IDC_TYPE_COMBO));
                return TRUE;
            }
            return TRUE;
        case IDC_BTN_SEARCH: {
            int presetIndex = GetSelectedPresetIndex(hwnd);
            if (presetIndex < 0) {
                LogMsg(L"preset not found");
                return TRUE;
            }
            double lat = _wtof(GetDlgItemTextString(hwnd, IDC_LAT).c_str());
            double lon = _wtof(GetDlgItemTextString(hwnd, IDC_LON).c_str());
            double radius = _wtof(GetDlgItemTextString(hwnd, IDC_RADIUS).c_str());
            LogMsg(L"Searching by center: " + std::wstring(kPlaceTypes[presetIndex].label));
            {
                auto* ctx = new SearchContext{};
                ctx->hwnd = hwnd;
                ctx->byBBox = false;
                ctx->lat = lat; ctx->lon = lon; ctx->radius = radius;
                ctx->presetIndex = presetIndex;
                GetLocalTime(&ctx->startTime);
                HANDLE hThread = CreateThread(nullptr, 0, SearchThreadProc, ctx, 0, nullptr);
                if (hThread) CloseHandle(hThread);
                else { delete ctx; LogMsg(L"failed to start search thread"); }
            }
            return TRUE;
        }
        case IDC_BTN_AREA: {
            if (!g_mapReady || !g_hasMapArea) {
                LogMsg(L"map area not ready");
                return TRUE;
            }
            int presetIndex = GetSelectedPresetIndex(hwnd);
            if (presetIndex < 0) {
                LogMsg(L"preset not found");
                return TRUE;
            }
            LogMsg(L"Searching visible area: " + std::wstring(kPlaceTypes[presetIndex].label));
            {
                auto* ctx = new SearchContext{};
                ctx->hwnd = hwnd;
                ctx->byBBox = true;
                ctx->south = g_bboxSouth; ctx->west = g_bboxWest;
                ctx->north = g_bboxNorth; ctx->east = g_bboxEast;
                ctx->presetIndex = presetIndex;
                GetLocalTime(&ctx->startTime);
                HANDLE hThread = CreateThread(nullptr, 0, SearchThreadProc, ctx, 0, nullptr);
                if (hThread) CloseHandle(hThread);
                else { delete ctx; LogMsg(L"failed to start search thread"); }
            }
            return TRUE;
        }
        case IDC_BTN_CSV: {
            SYSTEMTIME st{}; GetLocalTime(&st);
            std::wstring csvName = MakeExportFilename(g_lastPresetIndex, st, L".csv");
            if (ExportCSVUtf8Bom(csvName, g_items)) LogMsg(L"CSV exported: " + csvName);
            else LogMsg(L"CSV export failed");
            return TRUE;
        }
        case IDC_BTN_XLS: {
            SYSTEMTIME st{}; GetLocalTime(&st);
            std::wstring xlsName = MakeExportFilename(g_lastPresetIndex, st, L".xlsx");
            if (ExportXLSX(xlsName, g_items)) LogMsg(L"Excel exported: " + xlsName);
            else LogMsg(L"Excel export failed");
            return TRUE;
        }
        case IDC_BTN_SEARCH_AREA_NAME: {
            int presetIndex = GetSelectedPresetIndex(hwnd);
            if (presetIndex < 0) { LogMsg(L"Please select a place type first."); return TRUE; }
            std::wstring wAreaName = GetDlgItemTextString(hwnd, IDC_AREA_NAME);
            std::string areaName = W2U(wAreaName);
            if (areaName.empty()) { LogMsg(L"Please enter an area name."); return TRUE; }
            LogMsg(L"Searching area: \"" + wAreaName + L"\" / " + kPlaceTypes[presetIndex].label);
            auto* ctx = new SearchContext{};
            ctx->hwnd = hwnd;
            ctx->byAreaName = true;
            ctx->areaName = areaName;
            ctx->presetIndex = presetIndex;
            GetLocalTime(&ctx->startTime);
            HANDLE hThread = CreateThread(nullptr, 0, SearchThreadProc, ctx, 0, nullptr);
            if (hThread) CloseHandle(hThread);
            else { delete ctx; LogMsg(L"failed to start search thread"); }
            return TRUE;
        }
        case IDC_BTN_TOGGLE_MODE:
            SetSearchMode(hwnd, !g_areaMode);
            return TRUE;
        }
        break;
    }
    case WM_SEARCH_DONE: {
        auto* ctx = reinterpret_cast<SearchContext*>(lParam);
        if (ctx->succeeded) {
            std::wstring modeLabel = ctx->byAreaName
                ? (L"area \"" + U2W(ctx->areaName) + L"\"")
                : (ctx->byBBox ? L"bbox" : L"center");
            LogMsg(modeLabel + L" search count = " + std::to_wstring(ctx->result.size()));
            CreateResultWindow(hwnd, ctx);
        } else {
            LogMsg(L"search failed");
        }
        delete ctx;
        return TRUE;
    }
    case WM_LOG_MSG: {
        auto* msg = reinterpret_cast<std::wstring*>(lParam);
        if (g_hDebug && msg) {
            int len = GetWindowTextLengthW(g_hDebug);
            SendMessageW(g_hDebug, EM_SETSEL, len, len);
            SendMessageW(g_hDebug, EM_REPLACESEL, FALSE, (LPARAM)(*msg + L"\r\n").c_str());
        }
        delete msg;
        return TRUE;
    }
    case WM_CLOSE:
        EndDialog(hwnd, 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) return 1;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ResultWindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"EasySearchResult";
    RegisterClassExW(&wc);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    INT_PTR ret = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAINDIALOG), nullptr, DlgProc, 0);
    curl_global_cleanup();

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return (ret == -1) ? 1 : 0;
}

