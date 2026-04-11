# easysearch — Project Guide for Claude

## Project purpose

`easysearch` is a Windows desktop application that lets users search for nearby places (restaurants, hospitals, shops, etc.) from OpenStreetMap via the Overpass API. Results are shown in a Win32 ListView and on an embedded Leaflet map, and can be exported as CSV or Excel.

---

## Repository layout

```
easysearch/               ← Visual Studio solution root
├── easysearch.sln        ← VS 2022 solution file
├── vcpkg.json            ← vcpkg manifest (dependencies)
├── README.md / README.zh-TW.md
├── CLAUDE.md             ← this file
├── .github/workflows/
│   └── build.yml         ← GitHub Actions CI/CD
└── easysearch/           ← C++ project directory
    ├── main.cpp          ← entire application logic (~910 lines)
    ├── place_types.h     ← static table of searchable place presets
    ├── resource.h        ← Win32 resource ID constants
    ├── app.rc            ← Win32 resource script (dialog layout, icon)
    ├── title.ico         ← application icon
    └── easysearch.vcxproj / .vcxproj.filters / .vcxproj.user
```

Build output lands in `x64/Release/` (or `x64/Debug/`).

---

## Tech stack

| Layer | Technology |
|---|---|
| UI framework | Win32 dialog (`DialogBoxParamW`), Common Controls v6 |
| Embedded map | WebView2 (`ICoreWebView2` / `ICoreWebView2Controller`) |
| Map tiles/JS | Leaflet 1.9.4 loaded from unpkg CDN at runtime |
| HTTP client | libcurl (vcpkg) |
| JSON parser | nlohmann/json (vcpkg) |
| Excel export | libxlsxwriter (vcpkg) |
| Dependency mgmt | vcpkg manifest mode (`vcpkg.json`) |
| Build system | MSBuild via `easysearch.sln`, toolset v143 (VS 2022), Unicode |
| CI | GitHub Actions (`windows-latest`, MSBuild + vcpkg) |

---

## Source file details

### `main.cpp`

Single-file application. All functions are `static` file-scope helpers. Key sections in order:

#### Data model
- `PlaceItem` — struct holding name, category, brand, cuisine, address, phone, website, openingHours, email, lat, lon.
- `g_items` — global `std::vector<PlaceItem>` holding the last search result.

#### Global state
- `g_hDlg` — main dialog HWND.
- `g_hDebug` — HWND of the debug edit control (`IDC_DEBUG`).
- `g_hMapHost` — HWND of the static placeholder for WebView2 (`IDC_MAPHOST`).
- `g_webController` / `g_webView` — WebView2 COM smart pointers.
- `g_bboxSouth/West/North/East` — current map bounding box, updated via WebView2 message.
- `g_hasMapArea` / `g_mapReady` — guards for map-dependent buttons.
- `g_layout` / `LayoutState` — captures initial control geometry at startup; used to proportionally resize all controls on `WM_SIZE`.

#### String utilities
- `Trim`, `W2U` (wide→UTF-8), `U2W` (UTF-8→wide), `ToLowerW`, `ContainsNoCase`.

#### Layout system
- `CaptureLayout(hwnd)` — called once in `WM_INITDIALOG`; records all margins and ratios from the resource-defined positions.
- `LayoutControls(hwnd)` — called on every `WM_SIZE`; repositions map host, list view, and debug panel proportionally. Min-track enforced via `WM_GETMINMAXINFO`.
- `ResizeWebView()` — syncs the WebView2 controller bounds to the map host HWND.

#### Overpass query pipeline
1. `BuildRadiusPlan(radius)` — generates a decreasing list of radii to try if there are no results (e.g. 1500 → 800 → 400 → 200).
2. `SearchByRadius(lat, lon, radius, presetIndex)` — builds an Overpass QL query (`node/way/relation around:r`); calls `RunOverpassQuery`.
3. `SearchByBBox(south, west, north, east, presetIndex)` — builds a bbox-bounded Overpass QL query.
4. `RunOverpassQuery(query, categoryLabel)` — iterates over 5 Overpass mirror endpoints (`kOverpassEndpoints[]`), up to 3 attempts each with 2 s / 5 s back-off.
5. `PerformOverpassPost(endpoint, query, ...)` — executes a single libcurl POST (`data=<url-encoded-query>`); timeout 35 s, connect timeout 12 s.
6. `ParsePlacesFromJson(resp, categoryLabel)` — parses the Overpass JSON `elements[]` array into `g_items`; builds addresses from `addr:*` / `contact:*` tags.

#### ListView
- `InitListColumns` — inserts 11 columns: Name, Category, Brand, Cuisine, Address, Phone, Website, Opening Hours, Email, Lat, Lon.
- `UpdateListView` — populates the list from `g_items` after each successful query.

#### Export
- `ExportCSVUtf8Bom` — writes UTF-8 BOM + CSV rows to `places.csv` in the working directory.
- `ExportXLSX` — writes `places.xlsx` via libxlsxwriter with pre-set column widths.

#### Type combo
- Populated from `kPlaceTypes[]` (see below).
- Supports live filtering: typing in the combo calls `PopulateTypeCombo` which re-populates the dropdown matching label / key / value.
- `GetSelectedPresetIndex` resolves the selected item to a `kPlaceTypes` index via `CB_GETITEMDATA` or fuzzy label match fallback.

#### WebView2 map
- `GetMapHtml()` — returns a self-contained Leaflet HTML string (default center: Taipei, zoom 14; CARTO light tiles).
- `InitWebView(hwnd)` — async chain: `CreateCoreWebView2EnvironmentWithOptions` → `CreateCoreWebView2Controller` → `NavigateToString`.
- JS→C++ bridge: the Leaflet map posts a JSON message `{type:"mapMoved", lat, lon, south, west, north, east, zoom}` on every `moveend`. The `WebMessageReceived` handler updates `IDC_LAT`, `IDC_LON` and the global bbox.
- C++→JS bridge: `window.setCenterFromHost(lat, lon, zoom)` is callable via `ExecuteScript` (currently unused in this version).

#### Entry point and dialog
- `wWinMain` — initialises COM (`COINIT_APARTMENTTHREADED`), `InitCommonControlsEx` (ListView), `curl_global_init`, then runs `DialogBoxParamW`.
- `DlgProc` handles: `WM_INITDIALOG`, `WM_SIZE`, `WM_GETMINMAXINFO`, `WM_COMMAND` (search by center, search by area, export CSV, export XLSX), `WM_CLOSE`.
- Searches run **synchronously on the UI thread** (no worker thread); the UI freezes during HTTP requests. This is a known limitation.

---

### `place_types.h`

Defines `SearchType { label, key, value }` and the static array `kPlaceTypes[91]` covering:
- `amenity`: restaurants, cafes, hospitals, banks, parking, etc.
- `shop`: supermarkets, electronics, clothes, etc.
- `tourism`: hotels, museums, attractions.
- `leisure`: parks, gyms, stadiums.
- `office`: company, government, lawyer, etc.
- `healthcare`: hospitals, clinics, pharmacies.

---

### `resource.h` — resource ID constants

| Constant | ID | Control |
|---|---|---|
| `IDD_MAINDIALOG` | 101 | Main dialog |
| `IDC_LAT` | 1001 | Latitude edit |
| `IDC_LON` | 1002 | Longitude edit |
| `IDC_RADIUS` | 1003 | Radius edit (metres) |
| `IDC_TYPE_COMBO` | 1004 | Place-type combo box |
| `IDC_BTN_SEARCH` | 1005 | "Search Center" button |
| `IDC_BTN_AREA` | 1006 | "Search Area" button |
| `IDC_BTN_CSV` | 1007 | "Export CSV" button |
| `IDC_BTN_XLS` | 1008 | "Export Excel" button |
| `IDC_MAPHOST` | 1009 | Static placeholder hosting WebView2 |
| `IDC_LIST` | 1010 | ListView (results) |
| `IDC_DEBUG` | 1011 | Multi-line edit (log output) |
| `IDI_APPICON` | 201 | Application icon |

---

## Dependencies (`vcpkg.json`)

```json
{
  "dependencies": ["curl", "nlohmann-json", "libxlsxwriter", "webview2"],
  "builtin-baseline": "cb2981c4e03d421fa03b9bb5044cd1986180e7e4"
}
```

`webview2` is a vcpkg port that provides the `WebView2Loader.dll` import library and headers. The actual WebView2 Runtime must be installed separately on the user's machine.

---

## Build

### Local (Visual Studio 2022)
1. Ensure `VCPKG_ROOT` is set to your vcpkg directory.
2. Open `easysearch.sln`.
3. Confirm Project Properties → vcpkg → Use Vcpkg Manifest = Yes.
4. Build with **Build Solution** (`Ctrl+Shift+B`), platform `x64`, configuration `Release` or `Debug`.

### CI (GitHub Actions)
Workflow file: `.github/workflows/build.yml`
- Trigger: push to `main`, any tag `v*.*.*`, pull request, manual dispatch.
- Clones vcpkg fresh into `$USERPROFILE\vcpkg`, bootstraps it, integrates it.
- Builds with `msbuild` `/p:Configuration=Release /p:Platform=x64 /p:VcpkgEnableManifest=true`.
- Packages `easysearch.exe`, all `*.dll` from `x64/Release/`, and `title.ico` into `easysearch-win-x64.zip`.
- Uploads the zip as a build artifact on every run.
- Creates a GitHub Release (with auto-generated notes) only when a `v*.*.*` tag is pushed.

---

## Known limitations / areas to improve

- **Blocking UI thread**: `SearchByRadius` and `SearchByBBox` call `Sleep()` and do HTTP I/O on the UI thread. Moving search to a worker thread with `PostMessage` completion would fix freezing.
- **Overpass rate limits**: public mirrors return HTTP 429/504 under load; the retry logic helps but the 35 s timeout means the UI can stall for several minutes.
- **Export path**: CSV and XLSX are always written to the current working directory as `places.csv` / `places.xlsx` with no file dialog.
- **No marker rendering**: search results are not drawn as pins on the Leaflet map; only the map position/bbox is used for queries.
