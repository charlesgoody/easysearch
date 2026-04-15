# easysearch

## Overview
`easysearch` is a Win32 desktop application built with Visual Studio, RC resources, WebView2, and vcpkg manifest mode.
It searches nearby places from OpenStreetMap / Overpass, shows results in a ListView with an embedded Leaflet map, and supports CSV / XLSX export from the result window.

中文說明請見 [README.zh-TW.md](./README.zh-TW.md).

## Requirements
- Windows 10/11
- Visual Studio 2022 with Desktop development with C++
- Git
- A local vcpkg installation
- WebView2 Runtime installed on the machine

## Setup
1. Clone the repository.
2. Make sure `VCPKG_ROOT` points to your local vcpkg folder.
3. Open `easysearch.sln` in Visual Studio 2022.
4. If needed, open Project Properties and confirm:
   - `vcpkg` -> `Use Vcpkg` = `Yes`
   - `vcpkg` -> `Use Vcpkg Manifest` = `Yes`
5. Build the solution.

## Build
- Startup project: `easysearch`
- Recommended platform: `x64`
- Typical configurations: `Debug` or `Release`
- Build from Visual Studio using **Build Solution**

---

## HERE Places API — Phone & Detail Enrichment (Optional)

Easysearch can optionally enrich search results (phone, website, opening hours) using
the **HERE Geocoding & Search API**.  This is a free tier and requires no credit card.

### Why is this needed?

OpenStreetMap data (used by Overpass) is community-maintained.  Phone numbers for many
businesses are simply not recorded in OSM.  HERE Places has independent coverage and
fills in the gaps automatically in the background after a search completes.

### Step 1 — Get a free HERE API key

1. Go to <https://developer.here.com> and click **Get started for free**.
2. Sign up with your email address (no credit card required).
3. After signing in, open the **Projects** tab and create a new project.
4. Inside the project, click **Create API key**.
5. Copy the generated API key (a long alphanumeric string).

> **Free tier limits (as of 2025):** 1,000 free transactions per day.  Each enriched
> result counts as one transaction.  For typical use this limit is never reached.

### Step 2 — Enter the key in Easysearch

1. Launch Easysearch.
2. Click the **Settings** button in the toolbar.
3. Paste your API key into the text box and click **Save**.

The key is stored in the Windows registry under
`HKCU\Software\Easysearch\HereApiKey` and is never transmitted anywhere other than
the HERE API endpoint.

### Step 3 — Run a search

After a search completes, the result window opens immediately with the data from
OpenStreetMap.  If a HERE key is configured, a background enrichment process starts
automatically: up to 4 parallel threads query HERE for each result and update the
phone / website / opening-hours columns in real time — no need to re-run the search.

To disable enrichment, open Settings and clear the key field, then click Save.

---

## Common issues
- If dependencies are missing, verify `VCPKG_ROOT` and the vcpkg project settings in Visual Studio.
- If WebView2 does not initialize, install the Microsoft Edge WebView2 Runtime.
- If Overpass returns `429` or `504`, retry later because the public endpoint may be busy.
- If HERE enrichment shows no phone numbers, verify the API key in Settings and check that the key has not exceeded its daily quota on the HERE developer dashboard.
