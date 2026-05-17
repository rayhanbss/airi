#include <Arduino.h>
#include <RTClib.h>
#include <WiFi.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Pin
// ─────────────────────────────────────────────────────────────────────────────
#define PIN_SOIL_A  33
#define PIN_SOIL_B  34
#define PIN_SOIL_C  35
#define PIN_RELAY   23

// ─────────────────────────────────────────────────────────────────────────────
//  Konfigurasi Penyiraman — ubah di sini sesuai kebutuhan
// ─────────────────────────────────────────────────────────────────────────────
#define WATER_HOUR_MORNING      5           // jam penyiraman pagi  (05:00)
#define WATER_HOUR_EVENING     17           // jam penyiraman sore  (17:00)

#define WATER_DURATION_NORMAL_MS  (15UL * 60 * 1000)   // 15 menit — tanah kering
#define WATER_DURATION_MOIST_MS   (10UL * 60 * 1000)   // 10 menit — tanah sudah lembab

#define SOIL_MOIST_THRESHOLD   40           // % ke atas = lembab

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi / Server
// ─────────────────────────────────────────────────────────────────────────────
const char* ssid     = "AIRI";
const char* password = "123456789";

WiFiServer server(80);
RTC_DS3231  rtc;

// ─────────────────────────────────────────────────────────────────────────────
//  State penyiraman
// ─────────────────────────────────────────────────────────────────────────────
bool          isWatering    = false;
bool          manualTest    = false;  // true = relay dihidupkan manual, timer diabaikan
unsigned long waterStartMs  = 0;
unsigned long waterDuration = 0;
int           lastWateredDay  = -1;   // hari terakhir relay aktif (cegah trigger ulang)
int           lastWateredHour = -1;   // jam  terakhir relay aktif

// ─────────────────────────────────────────────────────────────────────────────
//  Fungsi utilitas
// ─────────────────────────────────────────────────────────────────────────────
int readSoilPercent() {
    int avg = (analogRead(PIN_SOIL_A) + analogRead(PIN_SOIL_B) + analogRead(PIN_SOIL_C)) / 3;
    int pct = 100 - (avg * 100 / 4095);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void setRelay(bool on) {
    digitalWrite(PIN_RELAY, on ? LOW : HIGH);   // relay active-LOW
    isWatering = on;
}

String formatTime(const DateTime& dt) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
    return String(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Logika jadwal penyiraman otomatis
//  Dipanggil setiap loop (termasuk saat SSE aktif)
// ─────────────────────────────────────────────────────────────────────────────
void checkSchedule() {
    // ── Jika sedang menyiram ────────────────────────────────────────────────────────
    if (isWatering) {
        if (manualTest) return;   // mode manual: relay tetap nyala, tidak ada timer
        if (millis() - waterStartMs >= waterDuration) {
            setRelay(false);
            Serial.printf("[Jadwal] Penyiraman selesai. Durasi: %lu menit.\n",
                          waterDuration / 60000UL);
        }
        return;     // jangan cek jadwal baru selagi relay masih aktif
    }

    // ── Cek apakah sekarang adalah jam penyiraman ─────────────────────────────
    DateTime now = rtc.now();
    int h   = now.hour();
    int day = now.day();

    bool isScheduledHour = (h == WATER_HOUR_MORNING || h == WATER_HOUR_EVENING);
    bool alreadyTriggered = (lastWateredDay == day && lastWateredHour == h);

    if (!isScheduledHour || alreadyTriggered) return;

    // ── Tentukan durasi berdasarkan kelembaban tanah ───────────────────────────
    int soil = readSoilPercent();
    bool soilMoist = (soil >= SOIL_MOIST_THRESHOLD);

    waterDuration = soilMoist ? WATER_DURATION_MOIST_MS : WATER_DURATION_NORMAL_MS;
    waterStartMs  = millis();
    setRelay(true);

    lastWateredDay  = day;
    lastWateredHour = h;

    Serial.printf("[Jadwal] Mulai penyiraman %s | Soil: %d%% (%s) | Durasi: %lu menit\n",
                  h < 12 ? "pagi" : "sore",
                  soil,
                  soilMoist ? "LEMBAB — durasi diperpendek" : "KERING — durasi normal",
                  waterDuration / 60000UL);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sinkronisasi waktu dari URL parameter
//  Contoh request: GET /settime?y=2025&mo=7&d=14&h=17&mi=30&s=0
// ─────────────────────────────────────────────────────────────────────────────
int extractParam(const String& req, const char* key) {
    String k = String(key) + "=";
    int idx = req.indexOf(k);
    if (idx < 0) return -1;
    idx += k.length();
    int end = req.indexOf('&', idx);
    if (end < 0) end = req.indexOf(' ', idx);
    if (end < 0) end = req.length();
    return req.substring(idx, end).toInt();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Toggle test penyiraman manual — ON/OFF tanpa durasi
// ─────────────────────────────────────────────────────────────────────────────
void handleTestWater(WiFiClient& client) {
    const char* state;
    if (manualTest) {
        // Sedang manual ON → matikan
        manualTest = false;
        setRelay(false);
        state = "off";
        Serial.println("[Test] Relay OFF (toggle).");
    } else if (!isWatering) {
        // Relay mati → nyalakan manual
        manualTest = true;
        setRelay(true);
        state = "on";
        Serial.println("[Test] Relay ON (toggle, tanpa durasi).");
    } else {
        // Penyiraman jadwal sedang berlangsung
        state = "auto";
        Serial.println("[Test] Diabaikan — penyiraman jadwal aktif.");
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.print("{\"state\":\"");
    client.print(state);
    client.print("\"}" );
}

void handleSetTime(const String& header, WiFiClient& client) {
    int y  = extractParam(header, "y");
    int mo = extractParam(header, "mo");
    int d  = extractParam(header, "d");
    int h  = extractParam(header, "h");
    int mi = extractParam(header, "mi");
    int s  = extractParam(header, "s");

    bool valid = (y >= 2020 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31
                  && h >= 0 && h <= 23 && mi >= 0 && mi <= 59);

    if (valid) {
        rtc.adjust(DateTime(y, mo, d, h, mi, (s < 0 ? 0 : s)));
        // reset state jadwal agar tidak bentrok dengan waktu baru
        lastWateredDay  = -1;
        lastWateredHour = -1;
        DateTime t = rtc.now();
        Serial.printf("[RTC] Waktu diperbarui → %02d/%02d/%04d %02d:%02d:%02d\n",
                      t.day(), t.month(), t.year(), t.hour(), t.minute(), t.second());
    } else {
        Serial.println("[RTC] Parameter /settime tidak valid.");
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.print(valid ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SSE payload  →  "HH:MM:SS|soilPct|relayState"
// ─────────────────────────────────────────────────────────────────────────────
String buildSSEData() {
    DateTime now = rtc.now();
    int soil = readSoilPercent();

    // format: time|soil|relayOn|manualTest
    char buf[40];
    snprintf(buf, sizeof(buf), "%s|%d|%d|%d",
             formatTime(now).c_str(),
             soil,
             isWatering  ? 1 : 0,
             manualTest  ? 1 : 0);
    return String(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Halaman web — hanya tampil Waktu & Kelembaban Tanah
// ─────────────────────────────────────────────────────────────────────────────
void sendHtmlPage(WiFiClient& client) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();

    client.println(F("<!DOCTYPE html><html lang='id'>"));
    client.println(F("<head>"));
    client.println(F("<meta charset='utf-8'>"));
    client.println(F("<meta name='viewport' content='width=device-width,initial-scale=1'>"));
    client.println(F("<link rel='icon' href='data:,'>"));
    client.println(F("<title>AIRI Monitor</title>"));
    client.println(F("<style>"));
    client.println(F("*{box-sizing:border-box;margin:0;padding:0}"));
    client.println(F("body{font-family:Arial,sans-serif;background:#f0fdf4;"
                     "min-height:100vh;display:flex;justify-content:center;align-items:center;}"));
    client.println(F(".card{background:#fff;border-radius:20px;padding:40px 32px;"
                     "box-shadow:0 6px 24px rgba(0,0,0,.1);width:92%;max-width:380px;text-align:center;}"));
    client.println(F("h1{color:#15803d;font-size:1.5rem;margin-bottom:28px;}"));
    client.println(F(".row{background:#f0fdf4;border-radius:12px;padding:20px 16px;margin:14px 0;}"));
    client.println(F(".lbl{font-size:.78rem;color:#6b7280;text-transform:uppercase;"
                     "letter-spacing:.06em;margin-bottom:6px;}"));
    client.println(F(".val{font-size:2.2rem;font-weight:700;color:#166534;}"));
    client.println(F(".relay-on {color:#15803d;font-size:.9rem;margin-top:6px;font-weight:600;}"));
    client.println(F(".relay-off{color:#9ca3af;font-size:.9rem;margin-top:6px;}"));
    client.println(F(".dot{display:inline-block;width:8px;height:8px;border-radius:50%;"
                     "background:#22c55e;margin-right:5px;animation:pulse 1.2s infinite;}"));
    client.println(F("@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"));
    client.println(F(".status{margin-top:18px;font-size:.78rem;color:#9ca3af;}"));
    client.println(F("</style>"));

    // JavaScript — terima SSE dan update DOM
    // es dibuat global agar syncTime() bisa menutupnya
    client.println(F("<script>"));
    client.println(F("var es=null;"));
    client.println(F("function startSSE(){"));
    client.println(F("  if(es){es.close();es=null;}"));
    client.println(F("  es=new EventSource('/events');"));
    client.println(F("  es.addEventListener('update',function(e){"));
    client.println(F("    var p=e.data.split('|');"));
    client.println(F("    document.getElementById('time').innerText=p[0]||'--:--:--';"));
    client.println(F("    document.getElementById('soil').innerText=(p[1]||'--')+'%';"));
    client.println(F("    var ri=document.getElementById('relay-info');"));
    client.println(F("    if(p[2]==='1'){"));
    client.println(F("      ri.className='relay-on';"));
    client.println(F("      ri.innerHTML='<span class=dot></span>Menyiram\u2026';"));
    client.println(F("    }else{"));
    client.println(F("      ri.className='relay-off';"));
    client.println(F("      ri.innerText='Standby';"));
    client.println(F("    }"));
    // sinkron tampilan tombol test dari SSE (p[3]=manualTest)
    client.println(F("    setTestBtn(p[3]==='1');"));
    client.println(F("    document.getElementById('status').innerText='\u25cf Live';"));
    client.println(F("  });"));
    client.println(F("  es.onerror=function(){document.getElementById('status').innerText='\u26a0 Menghubungkan ulang...';};"));
    client.println(F("}"));
    client.println(F("window.onload=startSSE;"));
    // ── Helper tampilan tombol test ──────────────────────────────────────────────
    client.println(F("function setTestBtn(on){"));
    client.println(F("  var b=document.getElementById('testBtn');"));
    client.println(F("  b.innerHTML=on?'&#9209; Stop Siram':'&#128167; Siram Manual';"));
    client.println(F("  b.style.background=on?'#dc2626':'#0369a1';"));
    client.println(F("}"));
    // ── Fungsi test siram (toggle) ─────────────────────────────────────────
    // Alur: tutup SSE → tunggu 1.5s → fetch /test → update tombol → reconnect SSE
    client.println(F("function testSiram(){"));
    client.println(F("  document.getElementById('testBtn').disabled=true;"));
    client.println(F("  document.getElementById('syncMsg').innerText='Menutup koneksi live...';"));
    client.println(F("  if(es){es.close();es=null;}"));
    client.println(F("  setTimeout(function(){"));
    client.println(F("    document.getElementById('syncMsg').innerText='';"));
    client.println(F("    fetch('/test',{signal:AbortSignal.timeout(5000)})"));
    client.println(F("    .then(function(r){return r.json();})"));
    client.println(F("    .then(function(j){"));
    client.println(F("      document.getElementById('testBtn').disabled=false;"));
    client.println(F("      if(j.state==='on'){"));
    client.println(F("        setTestBtn(true);"));
    client.println(F("        document.getElementById('syncMsg').innerText='\u25cf Relay ON — tekan lagi untuk mematikan';"));
    client.println(F("      }else if(j.state==='off'){"));
    client.println(F("        setTestBtn(false);"));
    client.println(F("        document.getElementById('syncMsg').innerText='\u25a0 Relay OFF';"));
    client.println(F("      }else{"));
    client.println(F("        document.getElementById('syncMsg').innerText='\u26a0 Penyiraman jadwal sedang aktif';"));
    client.println(F("      }"));
    client.println(F("      setTimeout(startSSE,800);"));
    client.println(F("    }).catch(function(){"));
    client.println(F("      document.getElementById('syncMsg').innerText='\u2717 Tidak ada respons.';"));
    client.println(F("      document.getElementById('testBtn').disabled=false;"));
    client.println(F("      startSSE();"));
    client.println(F("    });"));
    client.println(F("  },1500);"));
    client.println(F("}"));
    // ── Fungsi sync waktu ────────────────────────────────────────────────────
    // Alur: tutup SSE → tunggu server bebas (~1.5s) → fetch /settime → reconnect SSE
    client.println(F("function syncTime(){"));
    client.println(F("  document.getElementById('syncBtn').disabled=true;"));
    client.println(F("  document.getElementById('syncMsg').innerText='Menutup koneksi live...';"));
    client.println(F("  if(es){es.close();es=null;}"));
    client.println(F("  setTimeout(function(){"));
    client.println(F("    document.getElementById('syncMsg').innerText='Menyinkron...';"));
    client.println(F("    var n=new Date();"));
    client.println(F("    var url='/settime?y='+n.getFullYear()"));
    client.println(F("      +'&mo='+(n.getMonth()+1)"));
    client.println(F("      +'&d='+n.getDate()"));
    client.println(F("      +'&h='+n.getHours()"));
    client.println(F("      +'&mi='+n.getMinutes()"));
    client.println(F("      +'&s='+n.getSeconds();"));
    client.println(F("    fetch(url,{signal:AbortSignal.timeout(5000)})"));
    client.println(F("    .then(function(r){return r.json();})"));
    client.println(F("    .then(function(j){"));
    client.println(F("      document.getElementById('syncMsg').innerText=j.ok?'\u2713 Berhasil! Menghubungkan ulang...':'\u2717 Gagal sinkron.';"));
    client.println(F("      document.getElementById('syncBtn').disabled=false;"));
    client.println(F("      if(j.ok)setTimeout(startSSE,800);"));
    client.println(F("      else startSSE();"));
    client.println(F("    }).catch(function(e){"));
    client.println(F("      document.getElementById('syncMsg').innerText='\u2717 Timeout / tidak ada respons.';"));
    client.println(F("      document.getElementById('syncBtn').disabled=false;"));
    client.println(F("      startSSE();"));
    client.println(F("    });"));
    client.println(F("  },1500);"));
    client.println(F("}"));
    client.println(F("</script>"));
    client.println(F("</head><body>"));

    client.println(F("<div class='card'>"));
    client.println(F("<h1>&#127807; AIRI Monitor</h1>"));

    client.println(F("<div class='row'>"));
    client.println(F("  <div class='lbl'>Waktu</div>"));
    client.println(F("  <div class='val' id='time'>--:--:--</div>"));
    client.println(F("</div>"));

    client.println(F("<div class='row'>"));
    client.println(F("  <div class='lbl'>Kelembaban Tanah</div>"));
    client.println(F("  <div class='val' id='soil'>--%</div>"));
    client.println(F("  <div class='relay-off' id='relay-info'>Standby</div>"));
    client.println(F("</div>"));

    client.println(F("<div class='status' id='status'>Connecting...</div>"));

    // ── Tombol-tombol aksi ───────────────────────────────────────────────────
    client.println(F("<div style='margin-top:18px;display:flex;gap:10px;justify-content:center;flex-wrap:wrap;'>"));

    client.println(F("<button onclick='testSiram()' id='testBtn'"
                     " style='padding:10px 18px;background:#0369a1;"
                     "color:#fff;border:0;border-radius:10px;cursor:pointer;font-size:.9rem;'>"
                     "&#128167; Test Siram</button>"));

    client.println(F("<button onclick='syncTime()' id='syncBtn'"
                     " style='padding:10px 18px;background:#166534;"
                     "color:#fff;border:0;border-radius:10px;cursor:pointer;font-size:.9rem;'>"
                     "&#128337; Sync Waktu</button>"));
    client.println(F("</div>"));
    client.println(F("<div id='syncMsg' style='margin-top:8px;font-size:.75rem;color:#6b7280;'></div>"));

    client.println(F("</div>"));
    client.println(F("</body></html>"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  SSE helpers
// ─────────────────────────────────────────────────────────────────────────────
void sendSSEHeaders(WiFiClient& client) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println("Access-Control-Allow-Origin: *");
    client.println();
}

void sendSSEUpdate(WiFiClient& client) {
    client.print("event: update\r\n");
    client.print("data: ");
    client.print(buildSSEData());
    client.print("\r\n\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(PIN_RELAY, OUTPUT);
    digitalWrite(PIN_RELAY, HIGH);    // pastikan relay mati saat boot
    delay(1000);

    if (!rtc.begin()) {
        Serial.println("RTC tidak ditemukan! Periksa koneksi I2C.");
        while (1) delay(10);
    }
    if (rtc.lostPower()) {
        Serial.println("RTC kehilangan daya → set waktu dari kompilasi.");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    {
        DateTime t = rtc.now();
        Serial.printf("Waktu RTC: %02d/%02d/%04d %02d:%02d:%02d\n",
                      t.day(), t.month(), t.year(),
                      t.hour(), t.minute(), t.second());
    }

    WiFi.softAP(ssid, password);
    Serial.printf("AP aktif. IP: %s\n", WiFi.softAPIP().toString().c_str());
    server.begin();
    Serial.printf("Jadwal penyiraman: %02d:00 & %02d:00\n",
                  WATER_HOUR_MORNING, WATER_HOUR_EVENING);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop utama
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    checkSchedule();

    WiFiClient client = server.available();
    if (!client) {
        delay(10);
        return;
    }

    Serial.println("[HTTP] Klien baru terhubung.");
    String header = "";
    String currentLine = "";
    unsigned long startTime = millis();

    while (client.connected() && millis() - startTime < 5000) {
        if (!client.available()) { delay(1); continue; }

        char c = client.read();
        header += c;

        if (c == '\n') {
            if (currentLine.length() == 0) {
                // ── Test penyiraman manual ────────────────────────────────
                if (header.indexOf("GET /test") >= 0) {
                    handleTestWater(client);
                    break;
                }

                // ── Set waktu RTC dari browser ────────────────────────────
                if (header.indexOf("GET /settime") >= 0) {
                    handleSetTime(header, client);
                    break;
                }

                // ── SSE stream ────────────────────────────────────────────────
                if (header.indexOf("GET /events") >= 0) {
                    sendSSEHeaders(client);
                    while (client.connected()) {
                        checkSchedule();        // cek jadwal meski sedang streaming
                        sendSSEUpdate(client);
                        client.flush();
                        delay(1000);
                    }
                    break;
                }

                // ── Halaman utama ─────────────────────────────────────────────
                sendHtmlPage(client);
                break;

            } else {
                currentLine = "";
            }
        } else if (c != '\r') {
            currentLine += c;
        }
    }

    client.stop();
    Serial.println("[HTTP] Klien terputus.");
}
