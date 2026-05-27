/*
 المكونات المطلوبة:
  - ESP32 Dev Board
  - ACS712 (5A أو 20A أو 30A) أو SCT-013
  - مقسّم جهد (voltage divider) لقياس الجهد — أو ZMPT101B
  - مقاومة burden لـ SCT-013: 33Ω
  - مكثفات 10µF

 التوصيلات:
  ACS712 OUT  → GPIO34 (ADC1_CH6)
  ZMPT101B    → GPIO35 (ADC1_CH7)  [اختياري]
  LED Status  → GPIO2

 المكتبات المطلوبة (ثبّتها من Library Manager):
  - ArduinoJson  (Benoit Blanchon)
  - ESPAsyncWebServer
  - AsyncTCP
  - EmonLib  (للـ SCT-013)
  وبعد رفع الكود هتخشي علي السيريل منتور وانسخي الip وحطيه في الاعدادت بتاعت الموقع
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <math.h>

// ─── إعدادات الشبكة ───────────────────────────────────────
const char* ssid     = "YOUR_WIFI_SSID";      // الـ WiFi هنا
const char* password = "YOUR_WIFI_PASSWORD";  // وكلمة السر هنا

// ─── Pins ────────────────────────────────────────────────
#define CURRENT_PIN  34    // ACS712 أو SCT-013
#define VOLTAGE_PIN  35    // ZMPT101B (لو مش موجود هيفترض 220V)
#define LED_PIN       2    

// ─── معايرة الحساسية ──────────────────────────────────────
// ACS712-5A  → 185 mV/A
// ACS712-20A → 100 mV/A
// ACS712-30A → 66  mV/A
#define ACS712_SENSITIVITY  185.0  
#define ACS712_ZERO_OFFSET  2048    
#define ADC_VREF            3.3    
#define ADC_RESOLUTION      4096   

#define SAMPLES_PER_CYCLE   200     
#define NOMINAL_VOLTAGE     220.0   
#define FREQUENCY           50      

struct Settings {
  float maxPower;       // أقصى طاقة مسموحة (W)
  float minVoltage;     // أدنى جهد مسموح (V)
  float maxVoltage;     // أقصى جهد مسموح (V)
  int   numPeople;      // عدد الأفراد
  int   numDevices;     // عدد الأجهزة
  bool  theftDetection; // كشف السرقة
  bool  notifications;  // إشعارات
  char  reserved[20];   // احتياطي
};

Settings cfg = { 3000, 190, 250, 4, 8, true, true };

// ─── هيكل البيانات اللحظية ──────────────────────────────
struct PowerData {
  float voltage;        // V
  float current;        // A
  float power;          // W
  float energy;         // kWh (تراكمي)
  float powerFactor;    // معامل القدرة
  bool  powerOn;        // هل الكهربا موجودة؟
  unsigned long uptime; // وقت التشغيل بالثواني
  String season;        // الفصل المتوقع
  String aiInsight;     // تحليل AI
};

PowerData data;

// ─── Web Server ──────────────────────────────────────────
WebServer server(80);

//-----------------------------------------------
struct Alert {
  String type;      // danger / warning / info
  String title;
  String message;
  unsigned long ts; // timestamp
};
Alert alertHistory[20];
int alertCount = 0;

// ─── متغيرات  ──────────────────────────────────────
unsigned long lastMeasure   = 0;
unsigned long lastEnergy    = 0;
unsigned long lastTheftCheck = 0;
float energyAccumulator     = 0;
float prevEnergy            = 0;
bool outageMode             = false;
unsigned long outageStart   = 0;

// ══════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n⚡ مراقب الكهرباء الذكي يبدأ...");

  pinMode(LED_PIN, OUTPUT);
  analogReadResolution(12);        
  analogSetAttenuation(ADC_11db);   

  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);
  if (cfg.maxPower < 100 || cfg.maxPower > 20000) {
    cfg = { 3000, 190, 250, 4, 8, true, true };
    saveSettings();
    Serial.println("⚙️ إعدادات افتراضية محملة");
  }


  connectWiFi();

  setupRoutes();

  server.begin();
  Serial.println("🌐 Web Server يعمل على: http://" + WiFi.localIP().toString());

  data.powerOn  = true;
  data.energy   = 0;
  data.season   = detectSeason();
  data.aiInsight = "جارٍ التحليل...";

  blinkLED(3); 
}

// ══════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  unsigned long now = millis();

  // قياس كل 2 ثانية
  if (now - lastMeasure >= 2000) {
    lastMeasure = now;
    measurePower();
    checkAlerts();
    updateAIInsight();
    data.uptime = now / 1000;
    printSerial();
  }

  // حفظ الطاقة المتراكمة كل دقيقة
  if (now - lastEnergy >= 60000) {
    lastEnergy = now;
    data.energy += (data.power / 1000.0) / 60.0; // kWh
  }

  // كشف السرقة كل 30 ثانية
  if (cfg.theftDetection && now - lastTheftCheck >= 30000) {
    lastTheftCheck = now;
    detectTheft();
  }

  digitalWrite(LED_PIN, (now / 500) % 2);
}

// ══════════════════════════════════════════════════════════
//  قياس الطاقة
// ══════════════════════════════════════════════════════════
void measurePower() {
  long sumSq = 0;
  int  rawCurrent;
  int  minRaw = 4096, maxRaw = 0;

  for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
    rawCurrent = analogRead(CURRENT_PIN);
    int offset = rawCurrent - ACS712_ZERO_OFFSET;
    sumSq += (long)offset * offset;
    if (rawCurrent < minRaw) minRaw = rawCurrent;
    if (rawCurrent > maxRaw) maxRaw = rawCurrent;
    delayMicroseconds(100);
  }

  // RMS Current
  float rmsRaw = sqrt((float)sumSq / SAMPLES_PER_CYCLE);
  float voltage_rms_raw = rmsRaw * (ADC_VREF / ADC_RESOLUTION);
  data.current = voltage_rms_raw / (ACS712_SENSITIVITY / 1000.0);

  if (data.current < 0.05) data.current = 0.0;


  data.voltage = NOMINAL_VOLTAGE; 

  // ─── حساب الطاقة ───
  data.power       = data.voltage * data.current;
  data.powerFactor = 0.85; 
  data.powerOn     = (data.current > 0.1 || data.voltage > 100);

  if (!data.powerOn && !outageMode) {
    outageMode  = true;
    outageStart = millis();
    addAlert("danger", "⚡ انقطاع الكهرباء", "تم رصد انقطاع الكهرباء في " + getTimeStr());
    Serial.println("🔴 انقطاع الكهرباء!");
  } else if (data.powerOn && outageMode) {
    outageMode = false;
    unsigned long duration = (millis() - outageStart) / 1000;
    String msg = "عادت الكهرباء بعد انقطاع " + String(duration) + " ثانية";
    addAlert("info", "🟢 عودة الكهرباء", msg);
    Serial.println("🟢 " + msg);
  }
}

// ══════════════════════════════════════════════════════════
//  كشف السرقة
// ══════════════════════════════════════════════════════════
void detectTheft() {

  static float prevPower = 0;
  if (prevPower > 0 && data.power > prevPower * 1.4 && data.power > 500) {
    addAlert("warning", "🔍 استهلاك مشبوه",
      "زيادة مفاجئة " + String(int((data.power/prevPower-1)*100)) +
      "% — احتمال سرقة كهرباء أو جهاز شغّل فجأة");
  }
  prevPower = data.power;
}

// ══════════════════════════════════════════════════════════
//  فحص حدود التنبيه
// ══════════════════════════════════════════════════════════
void checkAlerts() {
  if (data.power > cfg.maxPower) {
    addAlert("danger", "⚠️ استهلاك مرتفع جداً",
      "الطاقة اللحظية " + String(data.power) + "W تتخطى الحد " + String(cfg.maxPower) + "W");
  }
  if (data.voltage > 0 && data.voltage < cfg.minVoltage) {
    addAlert("warning", "⚡ جهد منخفض",
      "الجهد " + String(data.voltage) + "V أقل من " + String(cfg.minVoltage) + "V");
  }
  if (data.voltage > cfg.maxVoltage) {
    addAlert("danger", "⚡ جهد مرتفع خطر",
      "الجهد " + String(data.voltage) + "V أعلى من " + String(cfg.maxVoltage) + "V");
  }
}

// ══════════════════════════════════════════════════════════
//  تحليل AI مبسط
// ══════════════════════════════════════════════════════════
void updateAIInsight() {
  float perPerson = data.power / max(cfg.numPeople, 1);

  if (!data.powerOn) {
    data.aiInsight = "الكهرباء منقطعة — لا يوجد استهلاك";
  } else if (data.power > cfg.maxPower * 0.9) {
    data.aiInsight = "استهلاك مرتفع جداً — تحقق من الأجهزة الكبيرة";
  } else if (data.current < 0.1) {
    data.aiInsight = "لا يوجد أحد في المنزل أو الأجهزة مطفأة";
  } else if (perPerson > 500) {
    data.aiInsight = "استهلاك " + String(int(perPerson)) + "W لكل فرد — أعلى من المتوسط";
  } else {
    data.aiInsight = "استهلاك طبيعي " + String(int(data.power)) + "W لـ " +
                     String(cfg.numPeople) + " أفراد — " + data.season;
  }
}

// ══════════════════════════════════════════════════════════
//  ربط الـ Web Server
// ══════════════════════════════════════════════════════════
void setupRoutes() {

  // ─── البيانات اللحظية ───
  server.on("/api/data", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["voltage"]     = round(data.voltage * 10) / 10.0;
    doc["current"]     = round(data.current * 100) / 100.0;
    doc["power"]       = round(data.power);
    doc["energy"]      = round(data.energy * 100) / 100.0;
    doc["powerFactor"] = data.powerFactor;
    doc["powerOn"]     = data.powerOn;
    doc["uptime"]      = data.uptime;
    doc["season"]      = data.season;
    doc["aiInsight"]   = data.aiInsight;
    doc["freeHeap"]    = ESP.getFreeHeap();
    doc["rssi"]        = WiFi.RSSI();

    String json;
    serializeJson(doc, json);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  // ─── التنبيهات ───
  server.on("/api/alerts", HTTP_GET, []() {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.createNestedArray("alerts");
    for (int i = 0; i < min(alertCount, 20); i++) {
      JsonObject a = arr.createNestedObject();
      a["type"]    = alertHistory[i].type;
      a["title"]   = alertHistory[i].title;
      a["message"] = alertHistory[i].message;
      a["ts"]      = alertHistory[i].ts;
    }
    String json;
    serializeJson(doc, json);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  server.on("/api/settings", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["maxPower"]       = cfg.maxPower;
    doc["minVoltage"]     = cfg.minVoltage;
    doc["maxVoltage"]     = cfg.maxVoltage;
    doc["numPeople"]      = cfg.numPeople;
    doc["numDevices"]     = cfg.numDevices;
    doc["theftDetection"] = cfg.theftDetection;
    doc["notifications"]  = cfg.notifications;
    String json;
    serializeJson(doc, json);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  // ─── الإعدادات POST ───
  server.on("/api/settings", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"no body\"}");
      return;
    }
    StaticJsonDocument<256> doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc["maxPower"])       cfg.maxPower       = doc["maxPower"];
    if (doc["minVoltage"])     cfg.minVoltage     = doc["minVoltage"];
    if (doc["maxVoltage"])     cfg.maxVoltage     = doc["maxVoltage"];
    if (doc["numPeople"])      cfg.numPeople      = doc["numPeople"];
    if (doc["numDevices"])     cfg.numDevices     = doc["numDevices"];
    if (doc.containsKey("theftDetection")) cfg.theftDetection = doc["theftDetection"];
    saveSettings();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // ─── إعادة الضبط ───
  server.on("/api/reset-energy", HTTP_POST, []() {
    data.energy = 0;
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"status\":\"reset\"}");
  });

  // ─── Health Check ───
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain",
      "⚡ ESP32 Electricity Monitor OK\nIP: " + WiFi.localIP().toString() +
      "\nUptime: " + String(millis()/1000) + "s");
  });

  // CORS preflight
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(204);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
}

// ══════════════════════════════════════════════════════════
//  دوال مساعدة
// ══════════════════════════════════════════════════════════

void connectWiFi() {
  Serial.print("📶 جارٍ الاتصال بـ WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ متصل! IP: " + WiFi.localIP().toString());
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("\n❌ فشل الاتصال — هيشتغل بدون WiFi");
  }
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void addAlert(String type, String title, String msg) {

  if (alertCount > 0 && alertHistory[0].title == title) return;

 
  for (int i = min(alertCount, 19); i > 0; i--) {
    alertHistory[i] = alertHistory[i-1];
  }
  alertHistory[0] = { type, title, msg, millis() / 1000 };
  if (alertCount < 20) alertCount++;

  Serial.println("[ALERT " + type + "] " + title + ": " + msg);
}

String detectSeason() {

  return "ربيع";
}

String getTimeStr() {
  unsigned long s = millis() / 1000;
  return String(s/3600) + ":" + String((s%3600)/60) + ":" + String(s%60);
}

void blinkLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void printSerial() {
  Serial.printf("V:%.1fV | I:%.2fA | P:%.0fW | E:%.3fkWh | %s | %s\n",
    data.voltage, data.current, data.power, data.energy,
    data.powerOn ? "ON" : "OFF",
    data.aiInsight.c_str());
}
