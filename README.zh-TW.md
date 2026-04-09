# easysearch

## 專案說明
`easysearch` 是一個使用 Visual Studio、Win32、RC 資源、WebView2 與 vcpkg manifest mode 的桌面工具。
它會透過 OpenStreetMap / Overpass 查詢附近店家，結果顯示在 ListView，地圖用 WebView2 內嵌，並支援匯出 CSV / XLSX。

English README 請見 [README.md](./README.md)。

## 開發需求
- Windows 10 / 11
- Visual Studio 2022，並安裝 Desktop development with C++
- Git
- 本機已安裝 vcpkg
- 電腦需安裝 WebView2 Runtime

## 設定方式
1. 先 clone 專案。
2. 確認 `VCPKG_ROOT` 指到本機 vcpkg 路徑。
3. 用 Visual Studio 2022 開啟 `easysearch.sln`。
4. 如有需要，到 Project Properties 確認：
   - `vcpkg` -> `Use Vcpkg` = `Yes`
   - `vcpkg` -> `Use Vcpkg Manifest` = `Yes`
5. 直接 Build Solution。

## 建置方式
- Startup project：`easysearch`
- 建議平台：`x64`
- 常用組態：`Debug` 或 `Release`
- 在 Visual Studio 使用 **Build Solution** 建置

## 常見問題
- 如果相依套件抓不到，先檢查 `VCPKG_ROOT` 與 Visual Studio 內的 vcpkg 設定。
- 如果 WebView2 無法初始化，請安裝 Microsoft Edge WebView2 Runtime。
- 如果 Overpass 回傳 `429` 或 `504`，通常是 public endpoint 忙碌，稍後再試即可。