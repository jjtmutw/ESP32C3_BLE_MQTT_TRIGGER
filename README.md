# ESP32-C3 三按鈕 BLE HID -> MQTT 操作手冊

這個專案是給帶 `0.42"` OLED 的 `ESP32-C3` 板子使用。

功能是：

- 同時管理 3 顆藍牙按鈕
- 每顆按鈕用 BLE HID 方式連線
- 偵測按鍵按下事件後送出 MQTT
- 可透過 Portal 設定 Wi‑Fi、MQTT、`device_id`、三顆按鈕 MAC

---

## 1. 功能說明

目前裝置支援：

- `button1`
- `button2`
- `button3`

每顆按鈕都對應一個 BLE MAC address。

當某顆按鈕被按下時，ESP32-C3 會送出：

- Topic：`jj/ble/button/<device_id>`
- Payload：`{"buttonX":"pressed"}`

例如你的預設設定：

- `device_id = oWvWIn0N`

那麼：

- `button1` 按下時：
  - Topic：`jj/ble/button/oWvWIn0N`
  - Payload：`{"button1":"pressed"}`
- `button2` 按下時：
  - Topic：`jj/ble/button/oWvWIn0N`
  - Payload：`{"button2":"pressed"}`
- `button3` 按下時：
  - Topic：`jj/ble/button/oWvWIn0N`
  - Payload：`{"button3":"pressed"}`

---

## 2. 進入 Portal

進入 Portal 有兩種方式：

1. 開機時按住板上的 `BOOT` 鍵
2. 開機後長按 `BOOT` 約 4 秒

進入後，裝置會建立一個 Wi‑Fi AP：

- SSID：`ESP32C3-BLE-MQTT`

用手機或電腦連上這個 AP，然後打開：

- `http://192.168.4.1`

就能進入設定頁面。

注意：

- 現在手動進 Portal 時，裝置會強制停留在 Portal，不會立刻跳回工作模式

---

## 3. Portal 可設定項目

Portal 裡可以設定：

- `MQTT Host`
- `MQTT Port`
- `MQTT User`
- `MQTT Pass`
- `Device ID`
- `button1 MAC`
- `button2 MAC`
- `button3 MAC`
- `Cooldown ms`

各欄位用途如下：

### MQTT Host / Port / User / Pass

MQTT Broker 連線資訊。

例如：

- Host：`broker.emqx.io`
- Port：`1883`

如果 Broker 不需要帳號密碼，`MQTT User` 和 `MQTT Pass` 可以留空。

### Device ID

用來組成 MQTT topic。

例如：

- `device_id = oWvWIn0N`

則 Topic 會變成：

- `jj/ble/button/oWvWIn0N`

### button1 MAC / button2 MAC / button3 MAC

這三個欄位分別對應三顆藍牙按鈕的 MAC address。

例如：

- `button1 MAC = 2A0798103C4E`
- `button2 MAC = AABBCCDDEEFF`
- `button3 MAC = 112233445566`

建議格式：

- 直接填連續 12 碼十六進位字元
- 不要加 `:`
- 例如填 `2A0798103C4E`

### Cooldown ms

按鍵防連發時間，單位是毫秒。

例如：

- `400`

意思是同一顆按鈕在 400ms 內不重複送 MQTT。

---

## 4. MQTT Topic 與 Payload 規則

### Topic 規則

Topic 格式固定為：

```text
jj/ble/button/<device_id>
```

例如：

```text
jj/ble/button/oWvWIn0N
```

### Payload 規則

按鈕按下時送出：

```json
{"button1":"pressed"}
```

或：

```json
{"button2":"pressed"}
```

或：

```json
{"button3":"pressed"}
```

---

## 5. 使用流程

### 第一次使用

1. 燒錄韌體到 ESP32-C3
2. 開機進 Portal
3. 設定 Wi‑Fi
4. 設定 MQTT Broker
5. 設定 `device_id`
6. 填入三顆藍牙按鈕的 MAC
7. 儲存

儲存後，裝置會：

- 連上 Wi‑Fi
- 連上 MQTT
- 掃描指定的三顆 BLE HID 裝置
- 主動與它們建立連線
- 訂閱 HID report

### 正常工作時

當某顆藍牙按鈕按下時：

1. ESP32 收到該按鈕的 HID report
2. 判斷為按下事件
3. 發送對應 MQTT payload

放開按鍵時，ESP32 會收到 release report，並清除按鍵 latch，等待下一次按壓。

---

## 6. 如何取得藍牙按鈕 MAC

你可以用以下方式找按鈕的 MAC：

- 用目前韌體的序列埠 log
- 用手機 BLE Scanner App
- 用電腦藍牙掃描工具

如果序列埠中看到：

```text
BLE detail (target-watch)
mac=2A0798103C4E name=AB Shutter3
```

那麼這顆按鈕的 MAC 就是：

```text
2A0798103C4E
```

把它填進 Portal 對應的 `button1 MAC`、`button2 MAC` 或 `button3 MAC` 即可。

---

## 7. 序列埠除錯資訊

目前序列埠會顯示的重點資訊包括：

- Wi‑Fi 連線狀態
- MQTT 連線狀態
- Portal 進入與儲存
- 指定按鈕是否被掃描到
- BLE 連線成功或失敗
- HID subscribe 成功或失敗
- HID report 資料內容
- MQTT publish 成功或失敗

常見訊息說明：

### `BLE target seen`

代表掃描到你設定的目標按鈕。

### `BLE connected`

代表已成功連上該藍牙按鈕。

### `HID subscribe OK`

代表已成功訂閱該按鈕的 HID characteristic。

### `HID report notify uuid=0x2a4d len=2 data=0200`

代表收到按鍵資料。

以你目前的快門按鈕為例：

- `0200`：按下
- `0000`：放開

### `MQTT publish start`

代表即將送出 MQTT。

### `MQTT connected: YES`

代表已成功連上 MQTT Broker。

---

## 8. 常見問題

### Q1. 進 Portal 後一下就跳回工作模式

舊版會這樣，因為 `autoConnect()` 會直接回到舊 Wi‑Fi。

目前新版已修正，手動進 Portal 時會強制停留在 Portal。

### Q2. 看得到按鈕，但按下沒有送 MQTT

請檢查：

- 該按鈕 MAC 是否填對
- Wi‑Fi 是否已連上
- MQTT 是否已連上
- HID subscribe 是否成功

可從序列埠觀察：

- 是否有 `BLE connected`
- 是否有 `HID subscribe OK`
- 是否有 `HID report`

### Q3. 一按就送很多次

目前程式已經用：

- HID press latch
- `Cooldown ms`

來防止重複送出。

若仍有重複，可把 `Cooldown ms` 調大一些，例如：

- `600`
- `800`

### Q4. 三顆按鈕可以用同一個 Topic 嗎

可以。

目前就是共用同一個 Topic，只用不同 Payload 區分：

- `{"button1":"pressed"}`
- `{"button2":"pressed"}`
- `{"button3":"pressed"}`

---

## 9. 建置與燒錄

```powershell
cd ESP32C3_BLE_MQTT_TRIGGER
pio run
pio run -t upload --upload-port COM40
pio device monitor -b 115200 -p COM40
```

---

## 10. 目前版本摘要

目前版本已支援：

- 3 顆 BLE HID 按鈕同時管理
- Portal 設定三顆按鈕 MAC
- `device_id` 可設定
- MQTT topic 自動組成 `jj/ble/button/<device_id>`
- payload 依按鈕自動變成 `{"buttonX":"pressed"}`
- 進 Portal 不會秒退
- 序列埠輸出已精簡，保留重點除錯資訊

