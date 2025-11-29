# 🖥️ ESP32 PC Power Controller

Ez a projekt egy **ESP32** mikrokontrollerrel megvalósított távvezérlő, amely lehetővé teszi egy asztali számítógép ki/bekapcsolását Wi-Fi-n keresztül.

A vezérlés egy beépített webes felhasználói felületen (Web UI) és WebSocket-en keresztül történik, valamint támogatja az automata időzítést is (Auto ON/OFF).

---

## 🚀 Előkészületek és Telepítés

### Hardver
* **ESP32** fejlesztői lap (pl. NodeMCU-32S vagy DevKit V1)
* **Relé modul** (a PC "Power" gombjának szimulálására).
* **Optocsatoló vagy feszültségosztó** az 5V-os (vagy 3.3V-os) PC állapotjelző LED (pl. Power LED) feszültségének leolvasásához (ADC bemenet).

### Szoftver Követelmények
1.  **Arduino IDE** (vagy PlatformIO)
2.  **ESP32 Board Support Package** telepítése az Arduino IDE-be.
3.  **Szükséges Könyvtárak:**
    * `WiFi.h` (Beépített)
    * `WebServer.h`
    * `WebSocketsServer.h`
    * `Preferences.h`
    * `time.h` (Beépített)

---

## 🔑 1. Konfiguráció: `secrets.h` Létrehozása (KÖTELEZŐ!)

A projekt a Wi-Fi hitelesítő adataidat egy külön, elrejtett fájlból, a **`secrets.h`**-ból tölti be. Ez a fájl nem szerepel a verziókövetésben (ki van zárva a `.gitignore` segítségével) a biztonság érdekében.

### Lépések

1.  **Hozd létre** a projekt főmappájában (ahol az `pc_power_controller.ino` is található) a **`secrets.h`** nevű fájlt.
2.  **Illesszd be** a fájlba a következő tartalmat, és cseréld ki az értékeket a saját Wi-Fi hálózatod adataira:

    ```cpp
    // secrets.h
    // WIFI hitelesítő adatok. Kérlek, NE töltsd fel ezt a fájlt Git/nyilvános repository-ba!

    #define WIFI_SSID "A_TE_WIFI_HALOZATOD_NEVE"
    #define WIFI_PASS "A_TE_WIFI_JELSZAVAD"
    ```

---

## ⚙️ 2. Fő Kód Beállítása (`pc_power_controller.ino`)

A `pc_power_controller.ino` fájl elején ellenőrizd a hardver specifikus beállításokat:

### Hardware Beállítások

| Konstans | Érték | Leírás |
| :--- | :--- | :--- |
| `#define RELAY_PIN` | `25` | A GPIO pin, amely a relé modult vezérli (a PC "Power" gombjára kötve). |
| `#define LED_INPUT` | `34` | Az ADC-képes GPIO pin, amely a PC állapotjelző LED (pl. Power LED) feszültségét olvassa (feszültségosztón keresztül). |
| `ADC_THRESHOLD` | `800` | Az analóg érték küszöb (0-4095), amely felett a PC-t **BEKAPCSOLT** állapotúnak tekinti. Kalibráld az értékedhez. |
| `PULSE_MS` | `1000` | A relé behúzó/pulzus időtartama (ms). |

### Időzóna

A kód Magyarországra (`CET-1CEST`) van beállítva automatikus nyári időszámítás (DST) kezeléssel.

* `const char* TIMEZONE = "CET-1CEST,M3.4.0/2,M10.4.0/3";`

---

## 🔌 3. Bekötési Diagram

Az ESP32 és a PC alaplap közötti alapvető bekötés a következő (az ESP32-nek közös GND-vel kell rendelkeznie a PC-vel):

1.  **Relé kimenet (NC, NO vagy COM):**
    * A relé NO/NC kimenete a PC alaplapján lévő **Power SW** tűkhöz csatlakozik (párhuzamosan a ház gombjával).
2.  **PC állapot (ADC bemenet):**
    * A PC alaplapján lévő **Power LED** tűk kimenete csatlakozik az ESP32 **LED\_INPUT** pinhez, **megfelelő feszültségosztón** keresztül, hogy a feszültség ne haladja meg az ESP32 bemeneti feszültségét (általában 3.3V). 

---

## 🌐 Használat

1.  **Fordítás és feltöltés** az ESP32-re az Arduino IDE-n keresztül.
2.  **Keresd meg** az ESP32 IP-címét a soros monitoron, miután sikeresen csatlakozott a Wi-Fi-hez.
3.  **Nyiss meg** egy webböngészőt, és lépj a megtalált IP-címre (pl. `http://192.168.1.100`).

A weboldal automatikusan megpróbál WebSocket-en keresztül csatlakozni az élő állapotfrissítésekhez.

---

## 🛠️ API Végpontok (Haladóknak)

A vezérlés HTTP API-n keresztül is lehetséges:

| Metódus | Végpont | Funkció | Elvárt Body | Válasz (JSON) |
| :--- | :--- | :--- | :--- | :--- |
| `GET` | `/state` | Lekéri az aktuális állapotot és időzítést. | N/A | `{ "power": true/false, "wifi": "ok", "autoOn": "HH:MM", ... }` |
| `POST` | `/press` | Szimulálja a PC gombnyomását (relé pulzus). | `{}` | Visszaadja az új állapotot. |
| `POST` | `/setAutoOn` | Beállítja az automata bekapcsolás idejét. | `{"time": "08:30"}` | Visszaadja az új állapotot. |
| `POST` | `/setAutoOff` | Beállítja az automata kikapcsolás idejét. | `{"time": "23:00"}` | Visszaadja az új állapotot. |
