# easysearch

## Overview
`easysearch` is a Win32 desktop application built with Visual Studio, RC resources, WebView2, and vcpkg manifest mode.
It searches nearby places from OpenStreetMap / Overpass, shows results in a ListView, embeds a map with WebView2, and supports CSV / XLSX export.

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

## Common issues
- If dependencies are missing, verify `VCPKG_ROOT` and the vcpkg project settings in Visual Studio.
- If WebView2 does not initialize, install the Microsoft Edge WebView2 Runtime.
- If Overpass returns `429` or `504`, retry later because the public endpoint may be busy.