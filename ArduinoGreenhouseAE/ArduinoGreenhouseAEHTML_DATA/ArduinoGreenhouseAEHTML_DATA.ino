#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>

// ============ НАСТРОЙКИ WIFI ============
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============
WebServer server(80);
WiFiUDP udp;
const int DISCOVERY_PORT = 12345;
unsigned long lastDiscoveryTime = 0;
const unsigned long DISCOVERY_INTERVAL = 30000;
String controllerIP = "";
bool controllerFound = false;
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 5000;

// Данные (получаем от контроллера)
float nitrogen = 0.0;
float phosphorus = 0.0;
float potassium = 0.0;
float humidity = 0.0;
float ec = 0.0;
bool servoNOpen = false;
bool servoPOpen = false;
bool servoKOpen = false;
float servoNRemainingSeconds = 0;
float servoPRemainingSeconds = 0;
float servoKRemainingSeconds = 0;
bool autoMode = true;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 10000;

void setup() {
  Serial.begin(115200);
  Serial.println("\n🌱 ВЕБ-ИНТЕРФЕЙС ТЕПЛИЦЫ");
  Serial.println("==========================================");
  Serial.println("⚠️ Датчик НЕ подключен к этой плате!");
  Serial.println("✅ Данные получаются от контроллера по WiFi\n");

  WiFi.begin(ssid, password);
  Serial.print("📶 Подключение к WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ WiFi подключён. IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("🌐 Веб-интерфейс: http://" + WiFi.localIP().toString());
  } else {
    Serial.println("❌ Не удалось подключиться к WiFi");
  }
  
  setupWebServer();
  server.begin();
  Serial.println("🌐 Веб-сервер запущен на порту 80");
  
  udp.begin(DISCOVERY_PORT);
  Serial.println("📡 UDP клиент запущен");
  broadcastDiscoveryRequest();
  lastDiscoveryTime = millis();
}

void loop() {
  server.handleClient();
  
  // Обработка UDP (поиск контроллера)
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;
      String response = String(incomingPacket);
      if (response.startsWith("CONTROLLER_ANNOUNCE:")) {
        controllerIP = udp.remoteIP().toString();
        controllerFound = true;
        lastPingTime = millis();
        Serial.println("✅ Контроллер найден: " + controllerIP);
      }
      else if (response.startsWith("PING_FROM_CONTROLLER:")) {
        udp.beginPacket(udp.remoteIP(), DISCOVERY_PORT);
        udp.print("PING_RESPONSE_FROM_INTERFACE:" + WiFi.macAddress());
        udp.endPacket();
      }
    }
  }
  
  // Поиск контроллера
  if (millis() - lastDiscoveryTime > DISCOVERY_INTERVAL) {
    broadcastDiscoveryRequest();
    lastDiscoveryTime = millis();
  }
  
  // Пинг контроллера
  if (controllerFound && millis() - lastPingTime > PING_INTERVAL) {
    IPAddress broadcastIP(255, 255, 255, 255);
    udp.beginPacket(broadcastIP, DISCOVERY_PORT);
    udp.print("PING_FROM_INTERFACE:" + WiFi.macAddress());
    udp.endPacket();
    lastPingTime = millis();
  }
  
  // Запрос данных у контроллера
  if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
    if (controllerFound && controllerIP != "") {
      getControllerData();
    }
    lastSensorRead = millis();
  }
}

void broadcastDiscoveryRequest() {
  IPAddress broadcastIP(255, 255, 255, 255);
  udp.beginPacket(broadcastIP, DISCOVERY_PORT);
  udp.print("ESP32_DISCOVERY_REQUEST:" + WiFi.localIP().toString());
  udp.endPacket();
}

// ✅ ЗАПРОС ДАННЫХ У КОНТРОЛЛЕРА
void getControllerData() {
  if (!controllerFound || controllerIP == "") return;
  
  WiFiClient client;
  if (client.connect(controllerIP.c_str(), 80)) {
    client.println("GET /data HTTP/1.1");
    client.println("Host: " + controllerIP);
    client.println("Connection: close");
    client.println();
    
    unsigned long timeout = millis();
    while (client.available() == 0) {
      if (millis() - timeout > 3000) {
        client.stop();
        return;
      }
    }
    
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") break;
    }
    
    String body = client.readString();
    client.stop();
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (!error) {
      nitrogen = doc["nitrogen"];
      phosphorus = doc["phosphorus"];
      potassium = doc["potassium"];
      humidity = doc["humidity"];
      ec = doc["ec"];
      autoMode = doc["autoMode"];
      servoNOpen = doc["servos"]["N"]["open"];
      servoPOpen = doc["servos"]["P"]["open"];
      servoKOpen = doc["servos"]["K"]["open"];
      servoNRemainingSeconds = doc["servos"]["N"]["remaining_seconds"];
      servoPRemainingSeconds = doc["servos"]["P"]["remaining_seconds"];
      servoKRemainingSeconds = doc["servos"]["K"]["remaining_seconds"];
      Serial.println("🔄 Данные с контроллера получены");
    }
  }
}

void sendCommandToController(String url) {
  if (!controllerFound || controllerIP == "") {
    server.send(404, "application/json", "{\"error\":\"Контроллер не найден\"}");
    return;
  }
  WiFiClient client;
  if (client.connect(controllerIP.c_str(), 80)) {
    client.print("GET " + url + " HTTP/1.1\r\n");
    client.print("Host: " + controllerIP + "\r\n");
    client.print("Connection: close\r\n\r\n");
    unsigned long timeout = millis();
    while (!client.available() && (millis() - timeout < 2000)) delay(1);
    while (client.available()) client.read();
    client.stop();
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>🌱 Умная теплица</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; }
body { background: linear-gradient(135deg, #1a2a6c, #2c3e50); color: #fff; padding: 20px; min-height: 100vh; }
.container { max-width: 1200px; margin: 0 auto; }
.header { text-align: center; padding: 20px 0; border-bottom: 2px solid #4CAF50; margin-bottom: 30px; }
.header h1 { font-size: 2.5rem; color: #4CAF50; margin-bottom: 10px; }
.status { display: flex; justify-content: space-around; background: rgba(0,0,0,0.3); padding: 15px; border-radius: 10px; margin-bottom: 25px; flex-wrap: wrap; gap: 10px; }
.status-item { text-align: center; min-width: 120px; }
.status-label { font-size: 0.85rem; opacity: 0.8; }
.status-value { font-size: 1.1rem; font-weight: bold; }
.status-value.online { color: #4CAF50; }
.status-value.offline { color: #f44336; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }
.data-card { background: rgba(0,30,60,0.7); border-radius: 15px; padding: 20px; text-align: center; border-left: 4px solid #4CAF50; }
.data-card.nitrogen { border-left-color: #2196F3; }
.data-card.phosphorus { border-left-color: #FF9800; }
.data-card.potassium { border-left-color: #9C27B0; }
.data-card.humidity { border-left-color: #00BCD4; }
.data-card.ec { border-left-color: #FFEB3B; }
.label { font-size: 0.9rem; opacity: 0.9; margin-bottom: 8px; }
.value { font-size: 2rem; font-weight: bold; }
.unit { font-size: 0.85rem; opacity: 0.7; }
.servo-card { background: rgba(40,20,0,0.7); border-radius: 15px; padding: 20px; text-align: center; border-left: 4px solid #FF9800; }
.servo-card .value { font-size: 2.2rem; margin: 5px 0; }
.btn { background: #4CAF50; color: white; border: none; padding: 12px 25px; font-size: 1rem; border-radius: 8px; cursor: pointer; margin: 8px; transition: all 0.3s; }
.btn:hover { background: #45a049; transform: scale(1.05); }
.btn.secondary { background: #6c757d; }
.btn.offline { background: #f44336; }
.controls { background: rgba(0,0,0,0.3); padding: 20px; border-radius: 15px; text-align: center; margin-bottom: 30px; }
.mode-control { background: rgba(30,30,30,0.7); padding: 20px; border-radius: 10px; margin-bottom: 20px; text-align: center; }
.switch { position: relative; display: inline-block; width: 60px; height: 34px; }
.switch input { opacity: 0; width: 0; height: 0; }
.slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #4CAF50; transition: .4s; border-radius: 34px; }
.slider:before { position: absolute; content: ""; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
input:checked + .slider { background-color: #FF9800; }
input:checked + .slider:before { transform: translateX(26px); }
.duration-input { display: flex; align-items: center; justify-content: center; gap: 5px; margin: 10px 0; }
.duration-input input { background: rgba(255,255,255,0.1); border: 2px solid #FF9800; color: white; text-align: center; width: 80px; padding: 5px; border-radius: 5px; }
.countdown { color: #FF9800; font-weight: bold; }
.footer { text-align: center; margin-top: 30px; opacity: 0.7; font-size: 0.9rem; }
</style>
</head>
<body>
<div class="container">
<div class="header">
<h1>🌱 Умная теплица</h1>
<p>Мониторинг и управление удобрениями</p>
</div>
<div class="status">
<div class="status-item">
<div class="status-label">WiFi</div>
<div class="status-value online" id="wifiStatus">Онлайн</div>
</div>
<div class="status-item">
<div class="status-label">Контроллер</div>
<div class="status-value offline" id="controllerStatus">Не найден</div>
</div>
<div class="status-item">
<div class="status-label">Режим</div>
<div class="status-value" id="modeStatus">-</div>
</div>
<div class="status-item">
<div class="status-label">Обновлено</div>
<div class="status-value" id="lastUpdate">-</div>
</div>
</div>
<div class="mode-control">
<h3 style="color: #4CAF50; margin-bottom: 15px;">⚙️ Режим работы</h3>
<label class="switch">
<input type="checkbox" id="modeToggle" onchange="toggleMode()">
<span class="slider"></span>
</label>
<span id="modeText" style="margin-left: 15px; font-weight: bold;">Автоматический</span>
</div>
<h2>📊 Данные с датчика</h2>
<div class="grid">
<div class="data-card nitrogen">
<div class="label">Азот (N)</div>
<div class="value" id="nitrogen">0.0</div>
<div class="unit">мг/кг</div>
</div>
<div class="data-card phosphorus">
<div class="label">Фосфор (P)</div>
<div class="value" id="phosphorus">0.0</div>
<div class="unit">мг/кг</div>
</div>
<div class="data-card potassium">
<div class="label">Калий (K)</div>
<div class="value" id="potassium">0.0</div>
<div class="unit">мг/кг</div>
</div>
<div class="data-card humidity">
<div class="label">Влажность</div>
<div class="value" id="humidity">0.0</div>
<div class="unit">%</div>
</div>
<div class="data-card ec">
<div class="label">EC</div>
<div class="value" id="ec">0.0</div>
<div class="unit">mS/cm</div>
</div>
</div>
<h2>⚙️ Сервоприводы</h2>
<div class="grid">
<div class="servo-card">
<div class="label">Азот (N)</div>
<div class="value" id="servoN">🔒</div>
<div class="unit" id="servoNTime">Закрыт</div>
<div class="duration-input" id="durationNContainer" style="display: none;">
<input type="number" id="durationN" value="5" step="0.5" min="1" max="60">
<span>сек</span>
</div>
<button class="btn" onclick="toggleServo('n')">🔄</button>
</div>
<div class="servo-card">
<div class="label">Фосфор (P)</div>
<div class="value" id="servoP">🔒</div>
<div class="unit" id="servoPTime">Закрыт</div>
<div class="duration-input" id="durationPContainer" style="display: none;">
<input type="number" id="durationP" value="5" step="0.5" min="1" max="60">
<span>сек</span>
</div>
<button class="btn" onclick="toggleServo('p')">🔄</button>
</div>
<div class="servo-card">
<div class="label">Калий (K)</div>
<div class="value" id="servoK">🔒</div>
<div class="unit" id="servoKTime">Закрыт</div>
<div class="duration-input" id="durationKContainer" style="display: none;">
<input type="number" id="durationK" value="5" step="0.5" min="1" max="60">
<span>сек</span>
</div>
<button class="btn" onclick="toggleServo('k')">🔄</button>
</div>
</div>
<div class="controls">
<button class="btn" onclick="refreshData()">🔄 Обновить</button>
<button class="btn secondary" onclick="closeAllServos()">🔒 Закрыть все</button>
<button class="btn offline" onclick="discoverController()">🔍 Найти контроллер</button>
</div>
<div class="footer">
<p>ESP32 Веб-интерфейс | <span id="debugInfo"></span></p>
</div>
</div>
<script>
let autoUpdateInterval;
let countdownInterval;
let autoMode = true;
function updateUI(data) {
document.getElementById('nitrogen').textContent = data.nitrogen.toFixed(1);
document.getElementById('phosphorus').textContent = data.phosphorus.toFixed(1);
document.getElementById('potassium').textContent = data.potassium.toFixed(1);
document.getElementById('humidity').textContent = data.humidity.toFixed(1);
document.getElementById('ec').textContent = data.ec.toFixed(1);
const ctrlStatus = document.getElementById('controllerStatus');
ctrlStatus.textContent = data.controllerFound ? 'Найден' : 'Не найден';
ctrlStatus.className = data.controllerFound ? 'status-value online' : 'status-value offline';
const modeStatus = document.getElementById('modeStatus');
modeStatus.textContent = data.autoMode ? 'Авто' : 'Ручной';
document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
updateServoUI('N', data.servoNOpen, data.servos?.N?.remaining_seconds || 0);
updateServoUI('P', data.servoPOpen, data.servos?.P?.remaining_seconds || 0);
updateServoUI('K', data.servoKOpen, data.servos?.K?.remaining_seconds || 0);
document.getElementById('modeToggle').checked = !data.autoMode;
document.getElementById('modeText').textContent = data.autoMode ? 'Автоматический' : 'Ручной';
autoMode = data.autoMode;
}
function updateServoUI(servo, isOpen, remainingSeconds) {
const valueEl = document.getElementById('servo' + servo);
const timeEl = document.getElementById('servo' + servo + 'Time');
const durationContainer = document.getElementById('duration' + servo + 'Container');
if (isOpen) {
valueEl.textContent = '✅';
valueEl.style.color = '#4CAF50';
if (remainingSeconds > 0) {
timeEl.innerHTML = '<span class="countdown">⏰ ' + remainingSeconds.toFixed(2) + ' сек</span>';
} else {
timeEl.textContent = 'Открыт';
}
if (durationContainer) durationContainer.style.display = 'none';
} else {
valueEl.textContent = '🔒';
valueEl.style.color = '#9E9E9E';
timeEl.textContent = 'Закрыт';
if (durationContainer) {
durationContainer.style.display = autoMode ? 'none' : 'flex';
}
}
}
function refreshData() {
fetch('/api-data')
.then(r => r.json())
.then(data => updateUI(data))
.catch(err => console.error('Ошибка:', err));
}
function discoverController() {
fetch('/discover').then(() => setTimeout(refreshData, 2000));
}
function toggleServo(servo) {
const isOpen = document.getElementById('servo' + servo.toUpperCase()).textContent === '✅';
if (isOpen) {
fetch('/servo/' + servo + '/close').then(() => refreshData());
} else {
const duration = document.getElementById('duration' + servo.toUpperCase()).value;
fetch('/servo/' + servo + '/open?duration=' + duration).then(() => refreshData());
}
}
function closeAllServos() {
if (confirm('Закрыть все?')) {
fetch('/servo/all/close').then(() => refreshData());
}
}
function toggleMode() {
const isManual = document.getElementById('modeToggle').checked;
fetch('/setmode?mode=' + (isManual ? 'manual' : 'auto')).then(() => refreshData());
}
function startCountdown() {
countdownInterval = setInterval(() => {
['N', 'P', 'K'].forEach(s => {
const el = document.getElementById('servo' + s + 'Time');
if (el && el.querySelector('.countdown')) {
const txt = el.querySelector('.countdown').textContent;
const match = txt.match(/([\d.]+)/);
if (match) {
let val = parseFloat(match[1]) - 0.1;
if (val > 0) {
el.querySelector('.countdown').textContent = '⏰ ' + val.toFixed(2) + ' сек';
}
}
}
});
}, 100);
}
document.addEventListener('DOMContentLoaded', function() {
refreshData();
autoUpdateInterval = setInterval(refreshData, 1000);
startCountdown();
});
</script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/api-data", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    doc["nitrogen"] = nitrogen;
    doc["phosphorus"] = phosphorus;
    doc["potassium"] = potassium;
    doc["humidity"] = humidity;
    doc["ec"] = ec;
    doc["autoMode"] = autoMode;
    doc["controllerFound"] = controllerFound;
    doc["servos"]["N"]["open"] = servoNOpen;
    doc["servos"]["N"]["remaining_seconds"] = servoNRemainingSeconds;
    doc["servos"]["P"]["open"] = servoPOpen;
    doc["servos"]["P"]["remaining_seconds"] = servoPRemainingSeconds;
    doc["servos"]["K"]["open"] = servoKOpen;
    doc["servos"]["K"]["remaining_seconds"] = servoKRemainingSeconds;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on("/discover", HTTP_GET, []() {
    broadcastDiscoveryRequest();
    server.send(200, "text/plain", "Поиск запущен");
  });

  server.on("/setmode", HTTP_GET, []() {
    String mode = server.arg("mode");
    sendCommandToController("/setmode?mode=" + mode);
    server.send(200, "text/plain", "Режим: " + mode);
  });

  server.on("/servo/n/open", HTTP_GET, []() {
    String duration = server.hasArg("duration") ? server.arg("duration") : "5";
    sendCommandToController("/servos/n/open?duration=" + duration);
    server.send(200, "text/plain", "N открыт");
  });
  server.on("/servo/n/close", HTTP_GET, []() {
    sendCommandToController("/servos/n/close");
    server.send(200, "text/plain", "N закрыт");
  });
  server.on("/servo/p/open", HTTP_GET, []() {
    String duration = server.hasArg("duration") ? server.arg("duration") : "5";
    sendCommandToController("/servos/p/open?duration=" + duration);
    server.send(200, "text/plain", "P открыт");
  });
  server.on("/servo/p/close", HTTP_GET, []() {
    sendCommandToController("/servos/p/close");
    server.send(200, "text/plain", "P закрыт");
  });
  server.on("/servo/k/open", HTTP_GET, []() {
    String duration = server.hasArg("duration") ? server.arg("duration") : "5";
    sendCommandToController("/servos/k/open?duration=" + duration);
    server.send(200, "text/plain", "K открыт");
  });
  server.on("/servo/k/close", HTTP_GET, []() {
    sendCommandToController("/servos/k/close");
    server.send(200, "text/plain", "K закрыт");
  });
  server.on("/servo/all/close", HTTP_GET, []() {
    sendCommandToController("/servos/all/close");
    server.send(200, "text/plain", "Все закрыты");
  });
}