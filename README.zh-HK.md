# ESP32-S3 Network MultiTool

[English](README.md) | **繁體中文**

一塊 ESP32-S3 N16R8 開發板，同時負責三項家用網絡工作，全部透過 [ntfy](https://ntfy.sh)
向你的手機回報及接收指令：

| 功能 | 用途 | 來源 |
|---|---|---|
| **廣告攔截** | 類似 Pi-hole 的 DNS 攔截，覆蓋網絡上所有裝置，上游以 DNS-over-TLS 加密 | ESP32 AdBlocker 1.0.3 |
| **網絡喚醒（Wake-on-LAN）** | 在任何地方喚醒你的電腦：ntfy 把指令帶進來，開發板在區域網絡內發出魔術封包 | ESP32 WOL-NTFY 2.0.1 |
| **網站監察** | 以 HTTP(S) 或 ping 檢查最多 30 個網站及裝置，停止運作或恢復時發出通知，並每日發送摘要 | SiteCheck 2.0.0 |

每項功能都可以在設定頁關閉；關閉的功能不佔記憶體，亦會忽略相關指令。

> **狀態：1.0.0-rc1。** 已能編譯並通過 457 項主機端測試，但合併後的韌體尚未在硬件上完成
> 48 小時耐久測試（見 [PLAN.md](PLAN.md) 第 5 階段）。在此之前可能仍有未完善之處。

`sekret status` 的回覆示例（數字僅供參考）：

```
ESP32 Network MultiTool v1.0.0-rc1
ip 192.168.0.254 (static)
gw 192.168.0.1
wifi home (-57 dBm)
settings http://192.168.0.254
up 2h 5m, last reset: daily reboot
time 2026-11-23 06:05 PST
next reboot 04:00 PST
ntfy connected
heap 104 KB, psram 132 KB free

-- ad blocker --
upstream DNS-over-TLS, connected to one.one.one.one; fallback 1.1.1.1 / 1.0.0.1
blocklist 236,195 domains, downloaded 3h 5m ago (from flash)
blocking on
queries 4,812, blocked 1,377 (28%), cached 1,906
list refresh 03:00

-- wake-on-lan --
computers 3/16

-- site monitor --
12 sites: 11 up, 1 down; next check in 21 min; digest 08:00
  DOWN nas
```

## 硬件

- **ESP32-S3 N16R8**：16 MB 快閃記憶體、8 MB 八線 PSRAM。必須有 PSRAM：攔截清單、DNS 快取、
  查詢記錄、ntfy 待發訊息匣和網站清單都放在裡面。已在 ESP32-S3-DevKitC-1 類型的開發板上測試。
- **2.4 GHz WiFi。** ESP32 沒有 5 GHz 無線電。
- **固定 IP 地址**（靜態 IP，或在路由器做 DHCP 保留）：使用廣告攔截時必須，因為所有裝置都會
  把 DNS 查詢發給它。
- **與要喚醒的電腦位於同一網段。** 魔術封包是廣播，不會穿過路由器或 VLAN。

## 快速開始

### 1. 燒錄

| 檔案 | 適用情況 |
|---|---|
| `multitool-v1.0.0-rc1-esp32s3-n16r8.bin` | 首次安裝，經開發板的 **UART** 接口或 USB 轉序列埠轉接器連接 |
| `multitool-v1.0.0-rc1-esp32s3-n16r8-usbcdc.bin` | 首次安裝，經開發板的**原生 USB** 接口連接 |
| `…-app.bin`／`…-usbcdc-app.bin` | 更新已在運行 MultiTool **或 ESP32 AdBlocker** 的開發板，保留設定及攔截清單 |

```bash
pip install esptool
```

```bash
esptool --port /dev/cu.usbserial-XXXX --baud 921600 write-flash 0x0 multitool-v1.0.0-rc1-esp32s3-n16r8.bin
```

更新指令、連接埠名稱、校驗碼及自行編譯的方法，請參閱 [flash.md](flash.md)（英文）。

### 2. 設定

1. 用手機連接 WiFi 網絡 **`MultiTool-Setup-XXXX`**。**請先關閉流動數據**，否則手機可能在
   頁面載入前，就因這個網絡沒有互聯網而自行斷開。
2. 設定頁會自動彈出；如沒有，請開啟 **http://192.168.4.1**。
3. 填寫 WiFi、ntfy 主題和兩個密碼，然後按 **Save & restart**。

| 部分 | 欄位 |
|---|---|
| WiFi | 網絡（從即時掃描中選取）、密碼、裝置名稱（`multitool` → http://multitool.local） |
| 功能 | 廣告攔截、網絡喚醒、網站監察：開或關 |
| 地址 | 靜態 IP、閘道、子網路遮罩；三項留空即使用 DHCP |
| 上游 DNS | 一般伺服器、DNS-over-TLS 及其伺服器、區域名稱交給路由器、IPv6 |
| 攔截清單 | 最多 4 個網址、每日更新時間、子域名攔截、停用 Firefox／iCloud 繞過功能 |
| 每日重新啟動 | 開／關、時間、時區 |
| 網站監察 | 檢查間隔（5–1440 分鐘）、每日摘要時間 |
| 狀態燈 | GPIO（0 = 不使用）、WS2812 或單色、亮度 |
| ntfy | 伺服器、連接埠、TLS、主題、可選的警報主題、可選的登入資料或權杖 |
| 密碼 | ntfy 指令密語，以及網頁管理密碼（用戶名 `admin`） |

**請用一個又長又隨機的主題名稱。** 在公開的 ntfy.sh 上，主題名稱是唯一阻止陌生人讀取你訊息的
東西；密語則阻止他們發送指令。

### 3. 開機回報

主題會收到 `MultiTool online` 及上面的狀態報告。如開啟了廣告攔截，首次開機時攔截清單會在
背景下載（預設清單約需 20 秒），完成後會再收到一則訊息。

### 4. 把網絡指向它（廣告攔截）

在路由器的 DHCP 設定中，把開發板的 IP 設為**唯一**的 DNS 伺服器。如果另加 `8.8.8.8` 之類的
第二個伺服器，裝置就能繞過攔截。更改前，可先在電腦上測試：

```bash
dig @192.168.0.254 doubleclick.net
```

回覆 `0.0.0.0` 即代表正在攔截。

## ntfy 指令

每個指令都以你的密語開頭：`sekret wake desktop`。沒有正確密語的訊息完全不會得到回覆，任何
回覆亦不會重複密語。未設定密語前，會更改狀態的指令一律拒絕。

### 整部裝置

| 指令 | 別名 | 用途 |
|---|---|---|
| `help` | `?`、`commands` | 已開啟功能的指令列表 |
| `status` | `stat` | 上面的狀態報告 |
| `reboot` | `restart` | 立即重新啟動 |
| `factoryreset` | | 清除設定，重新啟動進入設定模式；保留攔截清單、電腦及網站 |
| `factoryreset all` | | 同時忘記已儲存的電腦及監察中的網站 |

### 廣告攔截

| 指令 | 別名 | 用途 |
|---|---|---|
| `stats` | | 統計數字及最近 10 個被攔截的名稱 |
| `top` | | 最常被攔截的名稱及最活躍的裝置 |
| `check <域名>` | | 是否被攔截，以及由哪一項攔截 |
| `block <域名>` | | 加入你自己的攔截清單（域名必須能解析，以防打錯字） |
| `unblock <域名>` | `allow` | 放行，即使上層域名被攔截 |
| `on`／`off` | | 暫停攔截，直至 `on` 或下次重新啟動 |
| `reload` | `refresh` | 立即重新下載攔截清單 |
| `clearcustom` | | 忘記所有你自訂的攔截及放行 |

### 網絡喚醒

| 指令 | 別名 | 用途 |
|---|---|---|
| `wake <名稱>` | `wol <名稱>` | 發送魔術封包；`wake all` 喚醒所有電腦 |
| `list` | `ls`、`wol list` | 已儲存的電腦 |
| `wol add <名稱> <mac>` | `wol save` | 記住一部電腦（最多 16 部） |
| `wol rm <名稱>` | `remove`、`del`、`delete`、`forget` | 忘記一部電腦 |

MAC 可寫成 `aa:bb:cc:dd:ee:ff`、`aa-bb-cc-dd-ee-ff` 或 `aabbccddeeff`。名稱最多 20 個字元，
只可用 `a-z 0-9 _ -`，不分大小寫，且不能是指令字（`list`、`all` 等）。魔術封包沒有任何回應，
所以「已發送」只代表已發出，不代表電腦已醒來。

### 網站監察

| 指令 | 別名 | 用途 |
|---|---|---|
| `site list` | `ls`、`status` | 每個網站及其狀態，以及下次檢查的時間 |
| `site add <名稱> <目標>` | `save` | 監察一個網站（最多 30 個） |
| `site add A\|目標1;B\|目標2` | | 一次加入多個；名稱可包含空格 |
| `site rm <名稱>` | `remove`、`del`、`delete` | 停止監察；`site rm A;B` 可一次移除多個 |
| `site check` | | 立即檢查所有網站 |

**目標**的寫法：

| 你輸入 | 檢查方式 |
|---|---|
| `192.168.1.50` | ping，發 2 次，任何一次有回應即算正常 |
| `http://nas.lan:8080/health` 或 `https://…` | 完全按該網址檢查 |
| `immich.example.com`、`nas` | `https://<名稱>` |

HTTP 及 HTTPS 檢查先發 `HEAD`，失敗再發 `GET`，**2xx 及 3xx 一律算正常**。不會跟隨轉址，亦
不檢查憑證（憑證過期仍算正常）。區域名稱（`nas`、`printer.lan`、`*.home.arpa`）會先在你的
本地 DNS 記錄中查找，然後再問路由器。

SiteCheck 原本的寫法在 `site` 之後仍然可用：`site ADD|Name|target;…` 及
`site add | Name | target`。單獨發送 `add` 或 `remove`（前面沒有 `wol` 或 `site`）會收到回覆，
問你想改哪一個清單。

### 網站警報

| 事件 | 標題 | 優先度 |
|---|---|---|
| 網站停止運作（或首次檢查已停止運作） | `DOWN: <名稱>` | 4（高） |
| 網站恢復 | `Recovered: <名稱>` | 3 |
| 每日摘要 | `Daily status` 或 `Daily status - 2 down` | 2；如有網站停止運作則為 3 |

狀態沒有變化就不會發訊息。開機或新加入後首次檢查正常的網站，亦不會通知。在設定頁填寫**警報
主題**，就可把這些訊息（只有這些）發到另一個主題，讓手機用不同鈴聲提示。

## 網頁

用瀏覽器開啟開發板的 IP 或 http://multitool.local，以 `admin` 登入。已關閉功能的頁面會隱藏。

| 頁面 | 內容 |
|---|---|
| **Status**（狀態） | 狀態報告；廣告攔截的檢查、攔截、放行、重新下載、開／關；重新啟動 |
| **Dashboard**（儀表板） | DNS 總數、24 小時圖表、最常攔截／放行的域名、最活躍裝置 |
| **Query log**（查詢記錄） | 最近 6,000 個 DNS 查詢，可篩選，每行可一鍵攔截／放行 |
| **Local DNS**（本地 DNS） | 你自己的「IP 名稱」記錄（網站監察亦會使用） |
| **Wake**（喚醒） | 已儲存的電腦，各有 Wake 及 Forget 按鈕、Wake all、新增表格 |
| **Sites**（網站） | 每個網站的狀態、持續時間、最近結果；立即檢查；新增／移除 |
| **Settings**（設定） | 設定表格的所有項目 |

## 狀態燈

ESP32-S3-DevKitC-1 v1.0 的板載 WS2812 在 **GPIO 48**，v1.1 則在 **GPIO 38**。

| 顏色 | 閃動方式 | 意思 |
|---|---|---|
| 黃 | 慢閃 | 設定模式，或 WiFi 斷線 |
| 綠 | 長亮 | 正常運作（開啟廣告攔截時：正在攔截） |
| 藍 | 中速閃 | 正在下載攔截清單 |
| 紫 | 快速閃 | 上次下載攔截清單時連不上互聯網 |
| 青 | 極慢閃 | 上次下載失敗；仍以舊清單攔截 |
| 紅 | 快閃 | 完全沒有攔截清單 |

關閉廣告攔截時，只會用到黃及綠。

## 如何放進同一塊開發板

三個原有韌體各自運行時都綽綽有餘。合在一起，限制不在快閃記憶體（4 MB 應用程式空間只用了
1.4 MB），亦不在 PSRAM，而在 S3 的**內部 RAM**：每個 TLS 連線的緩衝區都必須放在這裡（預先
編譯的 Arduino 核心已固定如此）。廣告攔截運行時約有 100 KB 可用，而每個 TLS 連線需要
35–45 KB。因此：

- **一次只做一個 TLS 握手。** 一個小閘門（`tlsgate.cpp`）把所有握手排隊：ntfy 串流、
  DNS-over-TLS、ntfy 訊息、攔截清單下載及 HTTPS 網站檢查。
- **DNS-over-TLS 會讓路**：短暫連線需要空間時，以及每日約 20 秒的攔截清單下載期間，查詢會
  暫時改用一般 DNS。
- **訊息排隊發送。** ntfy 訊息先放進 PSRAM 中有 16 格的待發訊息匣，由主迴圈逐一發送，最多
  重試三次。即使網站檢查很慢，指令回覆最多只會延遲一次發送的時間。
- **網站檢查在獨立的低優先度工作中進行**，一次一個，優先度低於 DNS 工作。在 03:00 攔截清單
  下載期間的 HTTPS 檢查會等下載完成。
- **網絡插槽（socket）：** 同時最多使用核心固定 16 個中的 14 個。

完整設計及插槽數量見 [PLAN.md](PLAN.md)（英文）。

## 廣告攔截說明

ESP32 AdBlocker 說明文件的內容仍然適用，重點如下：

- **清單：** 最多合併 4 份清單（HOSTS、Adblock `||domain^` 或純域名格式），合共約 250,000 個
  域名。超出部分會被截斷，載入訊息會顯示 `TRUNCATED`。以下為 2026 年 11 月的大小（每日都會
  變動）：

  | 清單 | 域名數目 | 備註 |
  |---|---|---|
  | **Hagezi Multi PRO**（預設）`…/adblock/pro.txt` | 約 236,000 | 攔截力強，誤攔少；可在開發板上完整載入 |
  | Hagezi Multi PRO++ `…/adblock/pro.plus.txt` | 約 249,000 | 更進取，偶爾令網站出錯；接近上限 |
  | Hagezi Multi NORMAL `…/adblock/multi.txt` | 約 180,000 | 較溫和 |
  | StevenBlack unified `https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts` | 約 76,000 | 保守 |
  | Hagezi 加密 DNS 繞過清單 `…/adblock/doh.txt` | 約 3,600 | 作為第二行，與以上任何一份同時使用 |

  `…` 即 `https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest`。建議：PRO 加上
  `doh.txt`。
- **配對：** 子域名隨上層域名一併攔截（Hagezi 清單需要此功能）；最具體的項目優先，所以在
  `example.com` 被攔截時，`unblock cdn.example.com` 仍然有效；能識破以 CNAME 隱藏的追蹤器；
  開頭的 `www.` 會被忽略。
- **區域名稱**（`printer`、`*.lan`、`*.home`、`*.home.arpa`、`*.internal`、私人地址的反向
  查詢）交由路由器處理；`.local` 永不轉發。
- **任何 DNS 攔截器都擋不住的：** YouTube 廣告、Facebook／Instagram／TikTok 的贊助帖文，以及
  由網站自己域名提供的廣告。智能電視、應用程式和遊戲的分別最明顯。
- **要堵塞的繞過途徑：** 關閉 Chrome 的「使用安全 DNS」，或設為「使用你目前的服務供應商」
  （Firefox 的同類功能及 iCloud 私密轉送會自動停用）；Android 的「私人 DNS」保持「自動」；如
  路由器支援，封鎖除開發板以外所有裝置的對外 53 及 853 埠。上表的 Hagezi `doh.txt` 清單會
  攔截公開的加密 DNS 伺服器。
- **容量：** 約 1–10 人或 50–80 部裝置。可用 `tools/dnsload.py` 量度（只可在你自己的網絡上
  使用）。

## 每日排程（本地時間）

| 時間 | 工作 |
|---|---|
| 03:00 | 更新攔截清單（約 20 秒；DoT 及 HTTPS 網站檢查會等候） |
| 04:00 | 每日重新啟動（DNS 中斷數秒；攔截清單約 2 秒從快閃記憶體還原） |
| 08:00 | 網站每日摘要 |
| 每 30 分鐘 | 檢查所有網站，首次在開發板上線 30 秒後進行 |

全部可以更改。重新啟動時間請避開 01:00–03:00：夏令時間轉換會跳過或重複這個小時。

## 日後更改設定

| 情況 | 做法 |
|---|---|
| 開發板在線 | 用瀏覽器開啟它的 IP（或 http://multitool.local），以 `admin` 登入 |
| WiFi 斷線 | 它會開啟 `MultiTool-Setup-XXXX`（http://192.168.4.1，需管理員登入），並每 10 秒重試連線 |
| 搬屋，或忘記管理密碼 | 按 **EN** 後放開，再**按住 BOOT** 2 秒：設定頁會開啟，無需登入 |
| 身處任何地方 | 透過 ntfy 發送 `<密語> factoryreset` |

如連不上 WiFi，序列埠記錄（115200 baud）會列出範圍內所有 2.4 GHz 網絡，並指出你的網絡是否
在其中。

## 由獨立韌體轉過來

| 原本使用 | 可保留的內容 |
|---|---|
| 同一塊開發板上的 **ESP32 AdBlocker** | 全部保留：燒錄 `-app.bin` 即可。設定、攔截清單、自訂清單及本地 DNS 記錄都會保留，新設定則使用預設值（所有功能開啟） |
| **ESP32 WOL-NTFY** | 用 `wol add <名稱> <mac>` 重新加入電腦，每部一則訊息 |
| **SiteCheck** | 一則訊息重新加入所有網站：`site add Name1\|target1;Name2\|target2;…`（即你以前用 `ADD\|…` 發給它的清單）。如你有它的 `/hosts.json`，同樣格式放在 `/sites.json` 亦可直接讀取 |

指令密語、管理密碼及 ntfy 主題只需設定一次，三項功能共用。

## 編譯

使用 [arduino-cli](https://arduino.github.io/arduino-cli/)，配合 ESP32 核心 3.1.1 或以上
（以 3.3.10 編譯）及兩個程式庫：

```bash
arduino-cli core install esp32:esp32
```

```bash
arduino-cli lib install ArduinoJson ESPping
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=custom" --export-binaries MultiTool
```

使用原生 USB 接口的開發板，請在 FQBN 加上 `,CDCOnBoot=cdc`。`PartitionScheme=custom` 會使用
[`MultiTool/partitions.csv`](MultiTool/partitions.csv)：4 MB 應用程式空間及 12 MB LittleFS
（與 ESP32 AdBlocker 相同，所以 AdBlocker 開發板可直接升級）。不支援無線（OTA）更新。

### 測試

457 項原生測試無需開發板，涵蓋：DNS 封包處理、攔截清單解析、設定及其驗證、跨越夏令時間轉換
的重新啟動時間、狀態燈、本地 DNS、網絡喚醒的儲存、網站監察的目標規則／網址解析／警報規則／
摘要時間／儲存格式，以及每項功能開啟及關閉時的所有 ntfy 指令（包括密語檢查）。

```bash
./test/run.sh
```

測試需要 ArduinoJson 的原始碼，會在 `~/Documents/Arduino/libraries/ArduinoJson/src` 或
`$ARDUINOJSON_SRC` 尋找。

### 原始碼概覽

| 檔案 | 用途 |
|---|---|
| `MultiTool.ino` | 開機流程；設定／後備／在線狀態；PSRAM 分配次序 |
| `commands.cpp`、`cmdutil.h` | ntfy 指令分派：密語檢查、help、status、reboot、factoryreset |
| `adblock_cmd.cpp`、`wol_cmd.cpp`、`site_cmd.cpp` | 各功能的指令 |
| `tlsgate.cpp` | 一次一個 TLS 握手；要求 DoT 讓路 |
| `ntfy.cpp` | ntfy 串流及待發訊息匣 |
| `settings.cpp`、`config.h` | NVS 中的設定；可調參數 |
| `webconfig.cpp` | 設定頁及所有網頁；mDNS |
| `scheduler.cpp`、`schedule.h` | 每日重新啟動、每日更新攔截清單、重啟原因 |
| `led.cpp`、`ledstate.h` | 狀態燈 |
| `sinkhole.cpp`、`dnsmsg.h`、`dnscache.cpp`、`dot.cpp` | DNS 伺服器、封包格式、快取、DNS-over-TLS |
| `blocklist.cpp`、`blparse.h`、`querylog.cpp`、`localdns.cpp`、`localrec.h` | 攔截清單、查詢記錄、本地 DNS |
| `wol.cpp`、`wolstore.cpp` | 魔術封包；已儲存的電腦 |
| `sitelogic.h`、`sitestore.cpp`、`sitecheck.cpp` | 網站規則；網站清單；檢查工作、警報及摘要 |
| `tools/dnsload.py` | DNS 負載測試工具（在你的電腦上運行） |

## 鳴謝及授權

- 攔截清單引擎及原始 DNS 回應程式來自
  [s60sc/ESP32_AdBlocker](https://github.com/s60sc/ESP32_AdBlocker)（s60sc 及 dateno1），以
  AGPL-3.0 授權。因此合併後的韌體亦以 **[AGPL-3.0](LICENSE)** 發佈。如你以網絡服務形式運行
  修改版本，必須向其用戶提供原始碼。
- 網絡喚醒及網站監察的程式碼來自 ESP32 WOL-NTFY 及 SiteCheck，兩者均以 Apache-2.0 授權。相關
  聲明見 [NOTICE](NOTICE)，授權全文見 [LICENSE-APACHE-2.0.txt](LICENSE-APACHE-2.0.txt)。
- 預設攔截清單：[Hagezi DNS blocklists](https://github.com/hagezi/dns-blocklists)。Ping：
  [ESPping](https://github.com/dvarrel/ESPping)。

如兩個版本有出入，以[英文版](README.md)為準。
