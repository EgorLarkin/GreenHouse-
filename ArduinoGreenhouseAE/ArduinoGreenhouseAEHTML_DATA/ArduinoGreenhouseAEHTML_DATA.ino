#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// ============ НАСТРОЙКИ WIFI ============
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";

// ============ НАСТРОЙКИ ДАТЧИКА NPK ============
#define RXD2 16  // GPIO16 -> RX2 (приём от датчика)
#define TXD2 17  // GPIO17 -> TX2 (передача к датчику)
HardwareSerial npkSerial(2);

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============
WebServer server(80);
WiFiUDP udp;
const int DISCOVERY_PORT = 12345;
unsigned long lastDiscoveryTime = 0;
const unsigned long DISCOVERY_INTERVAL = 30000; // 30 сек

String receiverIP = "";  // IP контроллера удобрений
bool receiverFound = false;
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 5000; // 5 сек

// Данные с датчика NPK
float nitrogen = 0.0;
float phosphorus = 0.0;
float potassium = 0.0;
float temperature = 0.0;
float humidity = 0.0;
float ec = 0.0;  // электропроводность

bool useTestData = false;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 10000; // 10 сек

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  npkSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  Serial.println("\n🌱 ДАТЧИК ПОЧВЫ (NPK + влажность + EC)");
  Serial.println("==========================================");

  // Подключение к WiFi
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
  } else {
    Serial.println("❌ Не удалось подключиться к WiFi");
    useTestData = true;
  }

  // Запуск веб-сервера
  setupWebServer();
  server.begin();
  Serial.println("🌐 Веб-сервер запущен на порту 80");

  // Первый поиск контроллера
  broadcastDiscoveryRequest();
  lastDiscoveryTime = millis();

  // Первое чтение датчика
  if (!useTestData) {
    readNPKSensor();
  } else {
    generateTestData();
  }
}

// ============ ОСНОВНОЙ ЦИКЛ ============
void loop() {
  server.handleClient();

  // Периодический поиск контроллера
  if (millis() - lastDiscoveryTime > DISCOVERY_INTERVAL) {
    broadcastDiscoveryRequest();
    lastDiscoveryTime = millis();
  }

  // Отправка PING контроллеру (если найден)
  if (receiverFound && millis() - lastPingTime > PING_INTERVAL) {
    sendPingToReceiver();
    lastPingTime = millis();
  }

  // Периодическое чтение датчика
  if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
    if (!useTestData) {
      readNPKSensor();
    } else {
      generateTestData();
    }
    lastSensorRead = millis();

    // Отправка данных контроллеру
    if (receiverFound && receiverIP != "") {
      sendDataToReceiver();
    }
  }
}

// ============ ЧТЕНИЕ ДАННЫХ С ДАТЧИКА NPK ============
void readNPKSensor() {
  // Запрос 7 регистров (адрес 0x03, количество 0x07)
  byte request[8] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x07, 0x64, 0x1C};

  npkSerial.write(request, 8);
  delay(200);

  int bytesAvailable = npkSerial.available();
  if (bytesAvailable >= 19) {
    byte response[19];
    npkSerial.readBytes(response, 19);

    // Проверка CRC (простая)
    if (response[0] == 0x01 && response[1] == 0x03) {
      nitrogen = (response[3] << 8 | response[4]) / 10.0;
      phosphorus = (response[5] << 8 | response[6]) / 10.0;
      potassium = (response[7] << 8 | response[8]) / 10.0;
      temperature = (response[9] << 8 | response[10]) / 10.0;
      humidity = (response[11] << 8 | response[12]) / 10.0;
      ec = (response[15] << 8 | response[16]) / 10.0;

      Serial.println("=== 📊 Данные с датчика NPK ===");
      Serial.printf("   Азот (N): %.1f mg/kg\n", nitrogen);
      Serial.printf("   Фосфор (P): %.1f mg/kg\n", phosphorus);
      Serial.printf("   Калий (K): %.1f mg/kg\n", potassium);
      Serial.printf("   Температура: %.1f °C\n", temperature);
      Serial.printf("   Влажность почвы: %.1f %%\n", humidity);
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

  // Очистка буфера
  while (npkSerial.available()) npkSerial.read();
}

// ============ ГЕНЕРАЦИЯ ТЕСТОВЫХ ДАННЫХ ============
void generateTestData() {
  nitrogen = random(150, 350) / 10.0;
  phosphorus = random(100, 250) / 10.0;
  potassium = random(200, 400) / 10.0;
  temperature = random(180, 280) / 10.0;
  humidity = random(300, 700) / 10.0;
  ec = random(10, 50) / 10.0;
  Serial.println("⚠️ Использую тестовые данные");
}

// ============ ОТПРАВКА ДАННЫХ КОНТРОЛЛЕРУ ============
void sendDataToReceiver() {
  if (!receiverFound || receiverIP == "") return;

  WiFiClient client;
  if (client.connect(receiverIP.c_str(), 80)) {
    String json = "{\"senderIP\":\"" + WiFi.localIP().toString() + 
                  "\",\"senderMAC\":\"" + WiFi.macAddress() + 
                  "\",\"nitrogen\":" + String(nitrogen, 1) + 
                  ",\"phosphorus\":" + String(phosphorus, 1) + 
                  ",\"potassium\":" + String(potassium, 1) + 
                  ",\"temperature\":" + String(temperature, 1) + 
                  ",\"humidity\":" + String(humidity, 1) + 
                  ",\"ec\":" + String(ec, 1) + 
                  ",\"timestamp\":" + String(millis()) + 
                  "}";

    client.print("POST /data HTTP/1.1\r\n");
    client.print("Host: " + receiverIP + "\r\n");
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: " + String(json.length()) + "\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print(json);
    client.stop();
    Serial.println("📤 Данные отправлены контроллеру");
  } else {
    Serial.println("❌ Не удалось подключиться к контроллеру");
    receiverFound = false;
  }
}

// ============ ШИРОКОВЕЩАТЕЛЬНЫЙ ЗАПРОС НА ОБНАРУЖЕНИЕ ============
void broadcastDiscoveryRequest() {
  IPAddress broadcastIP = WiFi.localIP();
  broadcastIP[3] = 255;

  String request = "ESP32_DISCOVERY_REQUEST:" + WiFi.localIP().toString();
  udp.beginPacket(broadcastIP, DISCOVERY_PORT);
  udp.print(request);
  udp.endPacket();

  Serial.println("📤 Отправлен широковещательный запрос на порт " + String(DISCOVERY_PORT));
}

// ============ ОТПРАВКА PING КОНТРОЛЛЕРУ ============
void sendPingToReceiver() {
  if (!receiverFound || receiverIP == "") return;

  WiFiClient client;
  if (client.connect(receiverIP.c_str(), 80)) {
    client.print("GET /ping HTTP/1.1\r\n");
    client.print("Host: " + receiverIP + "\r\n");
    client.print("Connection: close\r\n\r\n");
    client.stop();
  } else {
    Serial.println("⚠️ Контроллер недоступен, сброшен статус");
    receiverFound = false;
  }
}

// ============ НАСТРОЙКА ВЕБ-СЕРВЕРА ============
void setupWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>🌱 Датчик почвы</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; }
    body { background: linear-gradient(135deg, #1a2a6c, #2c3e50); color: #fff; padding: 20px; min-height: 100vh; }
    .container { max-width: 1000px; margin: 0 auto; }
    .header { text-align: center; padding: 20px 0; border-bottom: 2px solid #4CAF50; margin-bottom: 30px; }
    .header h1 { font-size: 2.5rem; margin-bottom: 10px; color: #4CAF50; }
    .status { display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.3); padding: 15px; border-radius: 10px; margin-bottom: 25px; }
    .status-item { text-align: center; }
    .status-label { font-size: 0.9rem; opacity: 0.8; margin-bottom: 5px; }
    .status-value { font-size: 1.4rem; font-weight: bold; color: #FFD700; }
    .status-value.online { color: #4CAF50; }
    .status-value.offline { color: #f44336; }
    .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 20px; margin-bottom: 30px; }
    .data-card { background: rgba(0, 30, 60, 0.7); border-radius: 15px; padding: 20px; text-align: center; box-shadow: 0 6px 15px rgba(0,0,0,0.3); transition: transform 0.3s ease; border-left: 4px solid #4CAF50; }
    .data-card:hover { transform: translateY(-5px); }
    .data-card.nitrogen { border-left-color: #2196F3; }
    .data-card.phosphorus { border-left-color: #FF9800; }
    .data-card.potassium { border-left-color: #9C27B0; }
    .data-card.temperature { border-left-color: #FF5722; }
    .data-card.humidity { border-left-color: #2196F3; }
    .data-card.ec { border-left-color: #FFEB3B; }
    .label { font-size: 0.95rem; margin-bottom: 8px; opacity: 0.9; }
    .value { font-size: 2.2rem; font-weight: bold; margin: 5px 0; }
    .unit { font-size: 0.9rem; opacity: 0.7; }
    .controls { background: rgba(0,0,0,0.3); padding: 25px; border-radius: 15px; text-align: center; }
    .btn { background: #4CAF50; color: white; border: none; padding: 12px 25px; font-size: 1.1rem; border-radius: 8px; cursor: pointer; margin: 10px; transition: all 0.3s ease; font-weight: bold; }
    .btn:hover { background: #45a049; transform: scale(1.05); }
    .btn.discover { background: #2196F3; }
    .btn.discover:hover { background: #1976D2; }
    .btn.offline { background: #f44336; }
    .btn.offline:hover { background: #d32f2f; }
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
      <h1>🌱 Датчик почвы</h1>
      <p>Мониторинг питательных веществ и состояния почвы</p>
    </div>

    <div class="status">
      <div class="status-item">
        <div class="status-label">Статус WiFi</div>
        <div class="status-value online" id="wifiStatus">Онлайн</div>
      </div>
      <div class="status-item">
        <div class="status-label">IP адрес</div>
        <div class="status-value" id="ipAddress">-</div>
      </div>
      <div class="status-item">
        <div class="status-label">Контроллер</div>
        <div class="status-value offline" id="receiverStatus">Не найден</div>
      </div>
    </div>

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
      <div class="data-card temperature">
        <div class="label">Температура</div>
        <div class="value" id="temperature">0.0</div>
        <div class="unit">°C</div>
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

    <div class="controls">
      <button class="btn discover" onclick="discoverReceiver()">🔍 Найти контроллер</button>
      <button class="btn" onclick="refreshData()">🔄 Обновить данные</button>
      <button class="btn offline" onclick="useTestData(true)">🧪 Тестовые данные</button>
      <button class="btn" onclick="useTestData(false)">📡 Реальные данные</button>
    </div>

    <div class="footer">
      <p>ESP32 Датчик почвы | NPK + Влажность + Электропроводность</p>
    </div>
  </div>

  <script>
    function updateUI(data) {
      document.getElementById('nitrogen').textContent = data.nitrogen.toFixed(1);
      document.getElementById('phosphorus').textContent = data.phosphorus.toFixed(1);
      document.getElementById('potassium').textContent = data.potassium.toFixed(1);
      document.getElementById('temperature').textContent = data.temperature.toFixed(1);
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
    }

    function refreshData() {
      fetch('/data')
        .then(response => response.json())
        .then(data => updateUI(data))
        .catch(err => console.error('Ошибка:', err));
    }

    function discoverReceiver() {
      fetch('/discover')
        .then(response => response.json())
        .then(data => {
          alert(data.message || 'Поиск запущен');
          setTimeout(refreshData, 2000);
        });
    }

    function useTestData(enable) {
      fetch(enable ? '/test-on' : '/test-off')
        .then(response => response.json())
        .then(data => {
          alert(data.message || 'Режим изменён');
          refreshData();
        });
    }

    // Автообновление каждые 10 сек
    setInterval(refreshData, 10000);
    // Первое обновление
    refreshData();
  </script>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
  });

  // API: получение данных
  server.on("/data", HTTP_GET, []() {
    String json;
    DynamicJsonDocument doc(512);
    doc["nitrogen"] = nitrogen;
    doc["phosphorus"] = phosphorus;
    doc["potassium"] = potassium;
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["ec"] = ec;
    doc["ip"] = WiFi.localIP().toString();
    doc["receiverFound"] = receiverFound;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // API: запуск поиска контроллера
  server.on("/discover", HTTP_GET, []() {
    broadcastDiscoveryRequest();
    lastDiscoveryTime = millis();
    String json = "{\"message\":\"Поиск контроллера запущен\"}";
    server.send(200, "application/json", json);
  });

  // API: включить тестовые данные
  server.on("/test-on", HTTP_GET, []() {
    useTestData = true;
    generateTestData();
    String json = "{\"message\":\"Тестовые данные ВКЛЮЧЕНЫ\"}";
    server.send(200, "application/json", json);
  });

  // API: выключить тестовые данные
  server.on("/test-off", HTTP_GET, []() {
    useTestData = false;
    readNPKSensor();
    String json = "{\"message\":\"Тестовые данные ВЫКЛЮЧЕНЫ\"}";
    server.send(200, "application/json", json);
  });

  // UDP-приём для обнаружения
  udp.begin(DISCOVERY_PORT);
  server.onNotFound([]() {
    if (server.method() == HTTP_POST && server.uri() == "/discovery-response") {
      String body = server.arg("plain");
      if (body.startsWith("ESP32_DISCOVERY_RESPONSE:")) {
        String ip = body.substring(26);
        receiverIP = ip;
        receiverFound = true;
        lastPingTime = millis();
        Serial.println("✅ Контроллер найден!");
        Serial.println("   IP адрес контроллера: " + receiverIP);
        server.send(200, "text/plain", "OK");
        return;
      }
    }
    server.send(404, "text/plain", "Not found");
  });
}