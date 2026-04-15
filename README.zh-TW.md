# easysearch

## 專案說明
`easysearch` 是一個使用 Visual Studio、Win32、RC 資源、WebView2 與 vcpkg manifest mode 的桌面工具。
透過 OpenStreetMap / Overpass 查詢附近店家，結果顯示在 ListView，內嵌 Leaflet 地圖，並支援從結果視窗匯出 CSV / XLSX。

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

---

## HERE Places API — 電話與詳細資料補齊（選用）

Easysearch 可選擇性地透過 **HERE Geocoding & Search API** 補充搜尋結果的電話、網站、營業時間等資料。
HERE 提供免費方案，**不需綁定信用卡**。

### 為什麼需要這個功能？

Overpass 使用的 OpenStreetMap 資料由社群人工維護，許多店家的電話號碼並未填入 OSM。
HERE Places 有獨立的資料覆蓋，可以在搜尋完成後於背景自動補充缺少的電話資訊。

### 第一步 — 申請免費 HERE API Key

1. 前往 <https://developer.here.com>，點選 **Get started for free**。
2. 用 Email 註冊帳號（不需信用卡）。
3. 登入後進入 **Projects** 頁籤，建立新專案。
4. 在專案內點選 **Create API key**。
5. 複製產生的 API Key（一串英數字組成的長字串）。

> **免費方案限制（截至 2025 年）：** 每日 1,000 次免費查詢。  
> 每筆補充結果消耗一次查詢額度，一般使用不會超過上限。

### 第二步 — 在 Easysearch 輸入 Key

1. 啟動 Easysearch。
2. 點選工具列的 **Settings** 按鈕。
3. 將 API Key 貼入文字欄位，點選 **Save**。

Key 會儲存在 Windows 登錄機碼 `HKCU\Software\Easysearch\HereApiKey`，不會傳送到 HERE API 以外的地方。

### 第三步 — 執行搜尋

搜尋完成後，結果視窗會立即開啟並顯示 OpenStreetMap 的資料。  
若已設定 HERE Key，背景補充程序會自動啟動：最多 4 個平行執行緒向 HERE 查詢每筆結果，  
並即時更新結果視窗的電話、網站、營業時間欄位，**不需重新搜尋**。

若要停用補充功能，開啟 Settings 清空 Key 欄位後按 Save 即可。

---

## 常見問題
- 相依套件抓不到：檢查 `VCPKG_ROOT` 與 Visual Studio 內的 vcpkg 設定。
- WebView2 無法初始化：請安裝 Microsoft Edge WebView2 Runtime。
- Overpass 回傳 `429` 或 `504`：public endpoint 目前忙碌，稍後再試。
- HERE 補充後電話仍空白：在 Settings 確認 Key 正確，並至 HERE 開發者後台確認今日額度未超過上限。
