#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// ============ НАСТРОЙКИ WIFI ============
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";

// ============ НАСТРОЙКИ ДАТЧИКА NPK ============
#define RXD2 16
#define TXD2 17
HardwareSerial npkSerial(2);

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============
WebServer server(80);
WiFiUDP udp;
const int DISCOVERY_PORT = 12345;
unsigned long lastDiscoveryTime = 0;
const unsigned long DISCOVERY_INTERVAL = 30000;
String receiverIP = "";
bool receiverFound = false;
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 5000;

// Данные с датчика NPK (БЕЗ ТЕМПЕРАТУРЫ)
float nitrogen = 0.0;
float phosphorus = 0.0;
float potassium = 0.0;
float humidity = 0.0;  // Влажность почвы
float ec = 0.0;       // электропроводность

// Данные с контроллера (сервоприводы)
bool servoNOpen = false;
bool servoPOpen = false;
bool servoKOpen = false;
unsigned long servoNOpenSeconds = 0;
unsigned long servoPOpenSeconds = 0;
unsigned long servoKOpenSeconds = 0;

bool useTestData = false;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 10000;

void setup() {
  Serial.begin(115200);
  npkSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("\n🌱 ДАТЧИК ПОЧВЫ С ВЕБ-ИНТЕРФЕЙСОМ");
  Serial.println("==========================================");

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
    useTestData = true;
  }

  setupWebServer();
  server.begin();
  Serial.println("🌐 Веб-сервер запущен на порту 80");

  // Инициализация UDP
  udp.begin(DISCOVERY_PORT);
  Serial.println("📡 UDP клиент запущен на порту " + String(DISCOVERY_PORT));

  broadcastDiscoveryRequest();
  lastDiscoveryTime = millis();

  if (!useTestData) {
    readNPKSensor();
  } else {
    generateTestData();
  }
}

void loop() {
  server.handleClient();

  // Обработка входящих UDP-пакетов
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;
      String response = String(incomingPacket);
      Serial.print("📨 UDP пакет от ");
      Serial.print(udp.remoteIP());
      Serial.print(": ");
      Serial.println(response);

      // ✅ ИСПРАВЛЕНО: убрана проверка colonPos — IP уже известен из пакета!
      if (response.startsWith("ESP32_DISCOVERY_RESPONSE:")) {
        receiverIP = udp.remoteIP().toString();  // ✅ Берём IP напрямую из пакета
        receiverFound = true;
        lastPingTime = millis();
        Serial.println("✅ Контроллер найден! IP: " + receiverIP);
      }
      else if (response.startsWith("PING_FROM_CONTROLLER:")) {
        udp.beginPacket(udp.remoteIP(), DISCOVERY_PORT);
        udp.print("PING_RESPONSE_FROM_SENSOR:" + WiFi.macAddress());
        udp.endPacket();
        Serial.println("📤 Отправлен ответ на пинг контроллеру");
      }
    }
  }

  // Периодический поиск контроллера
  if (millis() - lastDiscoveryTime > DISCOVERY_INTERVAL) {
    broadcastDiscoveryRequest();
    lastDiscoveryTime = millis();
  }

  // Отправка данных датчика контроллеру
  if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
    if (!useTestData) {
      readNPKSensor();
    } else {
      generateTestData();
    }
    lastSensorRead = millis();
    if (receiverFound && receiverIP != "") {
      sendDataToReceiver();
      getControllerData();
    }
  }
}

// Функция пересылки команды на контроллер
void forwardToController(String servo, String action) {
  if (!receiverFound || receiverIP == "") {
    server.send(404, "application/json", "{\"error\":\"Контроллер не найден\"}");
    return;
  }
  
  WiFiClient client;
  if (!client.connect(receiverIP.c_str(), 80)) {
    server.send(503, "application/json", "{\"error\":\"Не удалось подключиться к контроллеру\"}");
    return;
  }

  String url = "/servos/";
  if (servo == "all") {
    url += "all/close";
  } else {
    url += servo + "/" + action;
  }

  // ✅ ИСПРАВЛЕНО: правильный формат HTTP с \r\n
  client.print("GET " + url + " HTTP/1.1\r\n");
  client.print("Host: " + receiverIP + "\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");  // Пустая строка-разделитель ОБЯЗАТЕЛЬНА

  // Ждём ответа с таймаутом
  unsigned long timeout = millis();
  while (!client.available() && (millis() - timeout < 2000)) {
    delay(1);
  }

  // Читаем и игнорируем ответ (чтобы освободить буфер)
  while (client.available()) client.read();
  client.stop();

  // Обновляем состояние после задержки (даём время на выполнение)
  delay(100);
  getControllerData();

  server.send(200, "application/json", 
    "{\"message\":\"Сервопривод " + servo + " " + action + " успешно\"}");
}

// ============ ЧТЕНИЕ ДАННЫХ С ДАТЧИКА NPK (БЕЗ ТЕМПЕРАТУРЫ) ============
void readNPKSensor() {
  byte request[8] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x07, 0x64, 0x1C};
  npkSerial.write(request, 8);
  delay(200);
  int bytesAvailable = npkSerial.available();
  if (bytesAvailable >= 19) {
    byte response[19];
    npkSerial.readBytes(response, 19);
    if (response[0] == 0x01 && response[1] == 0x03) {
      nitrogen = (response[3] << 8 | response[4]) / 10.0;
      phosphorus = (response[5] << 8 | response[6]) / 10.0;
      potassium = (response[7] << 8 | response[8]) / 10.0;
      // ПРОПУСКАЕМ ТЕМПЕРАТУРУ (байты 9-10)
      humidity = (response[11] << 8 | response[12]) / 10.0;  // БЕЗ ТЕМПЕРАТУРЫ
      // Пропускаем байты 13-14
      ec = (response[15] << 8 | response[16]) / 10.0;
      Serial.println("=== 📊 Данные с датчика NPK ===");
      Serial.printf("   Азот (N): %.1f mg/kg\n", nitrogen);
      Serial.printf("   Фосфор (P): %.1f mg/kg\n", phosphorus);
      Serial.printf("   Калий (K): %.1f mg/kg\n", potassium);
      Serial.printf("   Влажность почвы: %.1f %%\n", humidity);  // БЕЗ ТЕМПЕРАТУРЫ
      Serial.printf("   EC: %.1f mS/cm\n", ec);
      Serial.println("===============================");
    } else {
      Serial.println("⚠️ Некорректный ответ от датчика NPK");
      useTestData = true;
    }
  } else {
    Serial.println("⚠️ Недостаточно данных от датчика NPK");
    useTestData = true;
  }
  while (npkSerial.available()) npkSerial.read();
}

// ============ ГЕНЕРАЦИЯ ТЕСТОВЫХ ДАННЫХ (БЕЗ ТЕМПЕРАТУРЫ) ============
void generateTestData() {
  nitrogen = random(150, 350) / 10.0;
  phosphorus = random(100, 250) / 10.0;
  potassium = random(200, 400) / 10.0;
  humidity = random(300, 700) / 10.0;  // БЕЗ ТЕМПЕРАТУРЫ
  ec = random(10, 50) / 10.0;
  Serial.println("⚠️ Использую тестовые данные (без температуры)");
}

// ============ ОТПРАВКА ДАННЫХ КОНТРОЛЛЕРУ (БЕЗ ТЕМПЕРАТУРЫ) ============
void sendDataToReceiver() {
  if (!receiverFound || receiverIP == "") return;
  WiFiClient client;
  if (client.connect(receiverIP.c_str(), 8888)) {
    String json = "{\"sender\":\"" + WiFi.localIP().toString() +
                  "\",\"mac\":\"" + WiFi.macAddress() +
                  "\",\"nitrogen\":" + String(nitrogen, 1) +
                  ",\"phosphorus\":" + String(phosphorus, 1) +
                  ",\"potassium\":" + String(potassium, 1) +
                  ",\"humidity\":" + String(humidity, 1) +  // БЕЗ ТЕМПЕРАТУРЫ
                  ",\"ec\":" + String(ec, 1) +
                  ",\"timestamp\":" + String(millis()) +
                  "}";
    client.println("POST /data HTTP/1.1");
    client.println("Host: " + receiverIP);
    client.println("Content-Type: application/json");
    client.println("Content-Length: " + String(json.length()));
    client.println("Connection: close");
    client.println();
    client.print(json);
    client.stop();
    Serial.println("📤 Данные отправлены контроллеру (" + receiverIP + ":8888)");
  } else {
    Serial.println("❌ Не удалось подключиться к контроллеру на порту 8888");
    receiverFound = false;
  }
}

// ============ ПОЛУЧЕНИЕ ДАННЫХ С КОНТРОЛЛЕРА ============
void getControllerData() {
  if (!receiverFound || receiverIP == "") return;
  WiFiClient client;
  if (client.connect(receiverIP.c_str(), 80)) {
    client.println("GET /data HTTP/1.1");
    client.println("Host: " + receiverIP);
    client.println("Connection: close");
    client.println();
    unsigned long timeout = millis();
    while (client.available() == 0) {
      if (millis() - timeout > 5000) {
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
      if (doc.containsKey("servos")) {
        servoNOpen = doc["servos"]["N"]["open"];
        servoPOpen = doc["servos"]["P"]["open"];
        servoKOpen = doc["servos"]["K"]["open"];
        servoNOpenSeconds = doc["servos"]["N"]["open_seconds"];
        servoPOpenSeconds = doc["servos"]["P"]["open_seconds"];
        servoKOpenSeconds = doc["servos"]["K"]["open_seconds"];
        Serial.println("🔄 Получены данные с контроллера:");
        Serial.println("   Сервоприводы: N=" + String(servoNOpen) +
                       ", P=" + String(servoPOpen) +
                       ", K=" + String(servoKOpen));
      }
    }
  }
}

// ============ ШИРОКОВЕЩАТЕЛЬНЫЙ ЗАПРОС НА ОБНАРУЖЕНИЕ ============
void broadcastDiscoveryRequest() {
  // ✅ ИСПРАВЛЕНО: используем универсальный broadcast для совместимости с iOS
  IPAddress broadcastIP(255, 255, 255, 255);
  String request = "ESP32_DISCOVERY_REQUEST:" + WiFi.localIP().toString();
  udp.beginPacket(broadcastIP, DISCOVERY_PORT);
  udp.print(request);
  udp.endPacket();
  Serial.println("📤 Отправлен широковещательный запрос на порт " + String(DISCOVERY_PORT));
}

// ============ НАСТРОЙКА ВЕБ-СЕРВЕРА ============
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
.header h1 { font-size: 2.5rem; margin-bottom: 10px; color: #4CAF50; }
.header p { opacity: 0.8; }
.status { display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); padding: 15px; border-radius: 10px; margin-bottom: 25px; flex-wrap: wrap; gap: 10px; }
.status-item { text-align: center; min-width: 150px; }
.status-label { font-size: 0.9rem; opacity: 0.8; margin-bottom: 5px; }
.status-value { font-size: 1.2rem; font-weight: bold; }
.status-value.online { color: #4CAF50; }
.status-value.offline { color: #f44336; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 20px; margin-bottom: 30px; }
.data-card { background: rgba(0, 30, 60, 0.7); border-radius: 15px; padding: 20px; text-align: center; box-shadow: 0 6px 15px rgba(0,0,0,0.3); transition: transform 0.3s ease; border-left: 4px solid #4CAF50; }
.data-card:hover { transform: translateY(-5px); }
.data-card.nitrogen { border-left-color: #2196F3; }
.data-card.phosphorus { border-left-color: #FF9800; }
.data-card.potassium { border-left-color: #9C27B0; }
.data-card.humidity { border-left-color: #2196F3; }
.data-card.ec { border-left-color: #FFEB3B; }
.servo-card { background: rgba(40, 20, 0, 0.7); border-radius: 15px; padding: 20px; text-align: center; box-shadow: 0 6px 15px rgba(0,0,0,0.3); transition: transform 0.3s ease; border-left: 4px solid #FF9800; }
.servo-card:hover { transform: translateY(-5px); }
.label { font-size: 0.95rem; margin-bottom: 8px; opacity: 0.9; }
.value { font-size: 2.2rem; font-weight: bold; margin: 5px 0; }
.unit { font-size: 0.9rem; opacity: 0.7; }
.controls { background: rgba(0,0,0,0.3); padding: 25px; border-radius: 15px; text-align: center; margin-bottom: 30px; }
.btn { background: #4CAF50; color: white; border: none; padding: 12px 25px; font-size: 1.1rem; border-radius: 8px; cursor: pointer; margin: 10px; transition: all 0.3s ease; font-weight: bold; display: inline-flex; align-items: center; gap: 8px; }
.btn:hover { background: #45a049; transform: scale(1.05); }
.btn.discover { background: #2196F3; }
.btn.discover:hover { background: #1976D2; }
.btn.offline { background: #f44336; }
.btn.offline:hover { background: #d32f2f; }
.btn.secondary { background: #6c757d; }
.btn.secondary:hover { background: #5a6268; }
.footer { text-align: center; margin-top: 30px; padding: 15px; font-size: 0.9rem; opacity: 0.7; border-top: 1px solid rgba(255,255,255,0.1); }
@media (max-width: 768px) {
.grid { grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); }
.header h1 { font-size: 2rem; }
.value { font-size: 1.8rem; }
}
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
<div class="status-label">Статус WiFi</div>
<div class="status-value online" id="wifiStatus">Онлайн</div>
</div>
<div class="status-item">
<div class="status-label">IP датчика</div>
<div class="status-value" id="ipAddress">-</div>
</div>
<div class="status-item">
<div class="status-label">Контроллер</div>
<div class="status-value offline" id="receiverStatus">Не найден</div>
</div>
<div class="status-item">
<div class="status-label">Обновлено</div>
<div class="status-value" id="lastUpdate">-</div>
</div>
</div>
<h2>📊 Данные с датчика почвы</h2>
<div class="grid" id="dataGrid">
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
<div class="label">Электропроводность</div>
<div class="value" id="ec">0.0</div>
<div class="unit">mS/cm</div>
</div>
</div>
<h2>⚙️ Управление сервоприводами</h2>
<div class="grid">
<div class="servo-card">
<div class="label">Азот (N)</div>
<div class="value" id="servoN">🔒</div>
<div class="unit" id="servoNTime">Закрыт</div>
<button class="btn" onclick="toggleServo('n')">🔄 Переключить</button>
</div>
<div class="servo-card">
<div class="label">Фосфор (P)</div>
<div class="value" id="servoP">🔒</div>
<div class="unit" id="servoPTime">Закрыт</div>
<button class="btn" onclick="toggleServo('p')">🔄 Переключить</button>
</div>
<div class="servo-card">
<div class="label">Калий (K)</div>
<div class="value" id="servoK">🔒</div>
<div class="unit" id="servoKTime">Закрыт</div>
<button class="btn" onclick="toggleServo('k')">🔄 Переключить</button>
</div>
</div>
<div class="controls">
<button class="btn discover" onclick="discoverReceiver()">🔍 Найти контроллер</button>
<button class="btn" onclick="refreshData()">🔄 Обновить данные</button>
<button class="btn secondary" onclick="closeAllServos()">🔒 Закрыть все</button>
<button class="btn offline" onclick="toggleTestData()">🧪 Тестовые данные</button>
</div>
<div class="footer">
<p>ESP32 Датчик почвы | NPK + Влажность + Электропроводность</p>
<p id="debugInfo" style="font-size: 0.8rem; margin-top: 5px;"></p>
</div>
</div>
<script>
let useTestData = false;
let autoUpdateInterval;
function updateUI(data) {
document.getElementById('nitrogen').textContent = data.nitrogen.toFixed(1);
document.getElementById('phosphorus').textContent = data.phosphorus.toFixed(1);
document.getElementById('potassium').textContent = data.potassium.toFixed(1);
document.getElementById('humidity').textContent = data.humidity.toFixed(1);
document.getElementById('ec').textContent = data.ec.toFixed(1);
document.getElementById('ipAddress').textContent = data.ip || '-';
const receiverStatus = document.getElementById('receiverStatus');
if (data.receiverFound) {
receiverStatus.textContent = 'Найден';
receiverStatus.className = 'status-value online';
} else {
receiverStatus.textContent = 'Не найден';
receiverStatus.className = 'status-value offline';
}
updateServoUI('N', data.servoNOpen, data.servoNOpenSeconds);
updateServoUI('P', data.servoPOpen, data.servoPOpenSeconds);
updateServoUI('K', data.servoKOpen, data.servoKOpenSeconds);
document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
document.getElementById('wifiStatus').textContent = data.wifiConnected ? 'Онлайн' : 'Оффлайн';
document.getElementById('wifiStatus').className = data.wifiConnected ? 'status-value online' : 'status-value offline';
}
function updateServoUI(servo, isOpen, seconds) {
const valueEl = document.getElementById('servo' + servo);
const timeEl = document.getElementById('servo' + servo + 'Time');
if (isOpen) {
valueEl.textContent = '✅';
valueEl.style.color = '#4CAF50';
timeEl.textContent = 'Открыт ' + seconds + ' сек';
} else {
valueEl.textContent = '🔒';
valueEl.style.color = '#9E9E9E';
timeEl.textContent = 'Закрыт';
}
}
function refreshData() {
fetch('/data')
.then(response => response.json())
.then(data => updateUI(data))
.catch(err => {
console.error('Ошибка:', err);
document.getElementById('debugInfo').textContent = 'Ошибка загрузки данных: ' + err;
});
}
function discoverReceiver() {
fetch('/discover')
.then(response => response.json())
.then(data => {
alert(data.message || 'Поиск запущен');
setTimeout(refreshData, 2000);
});
}
function toggleTestData() {
useTestData = !useTestData;
fetch(useTestData ? '/test-on' : '/test-off')
.then(response => response.json())
.then(data => {
alert(data.message || 'Режим изменён');
refreshData();
});
}
function toggleServo(servo) {
const isOpen = document.getElementById('servo' + servo.toUpperCase()).textContent === '✅';
const action = isOpen ? 'close' : 'open';
fetch('/servo/' + servo + '/' + action)
.then(response => response.json())
.then(data => {
alert(data.message || 'Выполнено');
refreshData();
})
.catch(err => {
alert('Ошибка: ' + err);
});
}
function closeAllServos() {
if (confirm('Закрыть все сервоприводы?')) {
fetch('/servo/all/close')
.then(response => response.json())
.then(data => {
alert(data.message || 'Выполнено');
refreshData();
})
.catch(err => {
alert('Ошибка: ' + err);
});
}
}
document.addEventListener('DOMContentLoaded', function() {
refreshData();
autoUpdateInterval = setInterval(refreshData, 5000);
});
</script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/data", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    doc["nitrogen"] = nitrogen;
    doc["phosphorus"] = phosphorus;
    doc["potassium"] = potassium;
    doc["humidity"] = humidity;
    doc["ec"] = ec;
    doc["ip"] = WiFi.localIP().toString();
    doc["receiverFound"] = receiverFound;
    doc["receiverIP"] = receiverIP;
    doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    doc["servoNOpen"] = servoNOpen;
    doc["servoPOpen"] = servoPOpen;
    doc["servoKOpen"] = servoKOpen;
    doc["servoNOpenSeconds"] = servoNOpenSeconds;
    doc["servoPOpenSeconds"] = servoPOpenSeconds;
    doc["servoKOpenSeconds"] = servoKOpenSeconds;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on("/discover", HTTP_GET, []() {
    broadcastDiscoveryRequest();
    lastDiscoveryTime = millis();
    String json = "{\"message\":\"Поиск контроллера запущен\"}";
    server.send(200, "application/json", json);
  });

  server.on("/test-on", HTTP_GET, []() {
    useTestData = true;
    generateTestData();
    String json = "{\"message\":\"Тестовые данные ВКЛЮЧЕНЫ (без температуры)\"}";
    server.send(200, "application/json", json);
  });

  server.on("/test-off", HTTP_GET, []() {
    useTestData = false;
    readNPKSensor();
    String json = "{\"message\":\"Тестовые данные ВЫКЛЮЧЕНЫ (без температуры)\"}";
    server.send(200, "application/json", json);
  });

  // 🔧 ЯВНЫЕ ОБРАБОТЧИКИ УПРАВЛЕНИЯ СЕРВОПРИВОДАМИ (вместо /servo/:servo/:action)
  server.on("/servo/n/open", HTTP_GET, []() { forwardToController("n", "open"); });
  server.on("/servo/n/close", HTTP_GET, []() { forwardToController("n", "close"); });
  server.on("/servo/p/open", HTTP_GET, []() { forwardToController("p", "open"); });
  server.on("/servo/p/close", HTTP_GET, []() { forwardToController("p", "close"); });
  server.on("/servo/k/open", HTTP_GET, []() { forwardToController("k", "open"); });
  server.on("/servo/k/close", HTTP_GET, []() { forwardToController("k", "close"); });
  server.on("/servo/all/close", HTTP_GET, []() { forwardToController("all", "close"); });
}