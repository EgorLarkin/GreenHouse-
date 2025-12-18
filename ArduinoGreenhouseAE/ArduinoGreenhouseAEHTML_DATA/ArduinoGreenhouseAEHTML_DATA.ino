#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>

// ========== НАСТРОЙКИ СЕТИ ==========
const char* ssid = "MTS_GPON_FF0C";
const char* password = "eA6hFtRk";

// ========== НАСТРОЙКИ ВТОРОЙ ESP32 ==========
String receiverIP = "";  // Будет определяться динамически
const int receiverPort = 8888;

// ========== НАСТРОЙКИ ПОСЛЕДОВАТЕЛЬНОГО ПОРТА ==========
#define RX_PIN 16  // GPIO16 для приема от датчика NPK
#define TX_PIN 17  // GPIO17 для передачи (если нужно)
HardwareSerial npkSerial(1);  // Используем UART1

// ========== ПЕРЕМЕННЫЕ ДЛЯ ДАННЫХ ==========
float nitrogen = 0.0;
float phosphorus = 0.0;
float potassium = 0.0;
float temperature = 0.0;
float humidity = 0.0;
float pH = 0.0;
float ec = 0.0;  // электропроводность

WebServer server(80);

// ========== ПРОТОКОЛ ОБЩЕНИЯ С ДАТЧИКОМ ==========
const byte requestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
const int frameLength = 8;

// ========== ФЛАГИ И СОСТОЯНИЯ ==========
bool receiverFound = false;
bool firstReadDone = false;

// ========== ФУНКЦИИ ==========

void setup() {
  Serial.begin(115200);
  
  // Инициализация последовательного порта для датчика NPK
  npkSerial.begin(4800, SERIAL_8N1, RX_PIN, TX_PIN);
  
  // Подключение к WiFi
  connectToWiFi();
  
  // Настройка маршрутов веб-сервера
  setupWebServer();
  
  // Первоначальное чтение данных с датчика
  readNPKSensor();
  firstReadDone = true;
}

void loop() {
  // Обработка веб-запросов - ДОЛЖНО БЫТЬ ПЕРВЫМ!
  server.handleClient();
  
  // Чтение данных с датчика каждые 5 секунд
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    readNPKSensor();
    lastRead = millis();
  }
}

// ========== ПОДКЛЮЧЕНИЕ К WIFI ==========
void connectToWiFi() {
  Serial.print("Подключение к WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi подключен!");
    Serial.println("========== ВАЖНАЯ ИНФОРМАЦИЯ ==========");
    Serial.print("🌐 Ваш IP адрес: ");
    Serial.println(WiFi.localIP());
    Serial.print("🔗 HTML интерфейс доступен по: ");
    Serial.print("http://");
    Serial.println(WiFi.localIP());
    Serial.println("=======================================");
    Serial.print("MAC адрес: ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("\n❌ Не удалось подключиться к WiFi!");
  }
}

// ========== ЧТЕНИЕ ДАННЫХ С ДАТЧИКА NPK ==========
void readNPKSensor() {
  Serial.println("📊 Чтение данных с датчика NPK...");
  
  // Очистка буфера
  while (npkSerial.available()) {
    npkSerial.read();
  }
  
  // Отправка запроса
  npkSerial.write(requestFrame, frameLength);
  delay(200);
  
  // Чтение ответа
  int bytesAvailable = npkSerial.available();
  Serial.print("📥 Доступно байт: ");
  Serial.println(bytesAvailable);
  
  if (bytesAvailable >= 19) {
    byte response[19];
    for (int i = 0; i < 19; i++) {
      response[i] = npkSerial.read();
    }
    
    // Парсинг данных
    if (response[0] == 0x01 && response[1] == 0x03) {
      nitrogen = (response[3] << 8 | response[4]) / 10.0;
      phosphorus = (response[5] << 8 | response[6]) / 10.0;
      potassium = (response[7] << 8 | response[8]) / 10.0;
      temperature = (response[9] << 8 | response[10]) / 10.0;
      humidity = (response[11] << 8 | response[12]) / 10.0;
      pH = (response[13] << 8 | response[14]) / 10.0;
      ec = (response[15] << 8 | response[16]) / 10.0;
      
      Serial.println("=== Данные с датчика NPK ===");
      Serial.printf("Азот (N): %.1f mg/kg\n", nitrogen);
      Serial.printf("Фосфор (P): %.1f mg/kg\n", phosphorus);
      Serial.printf("Калий (K): %.1f mg/kg\n", potassium);
      Serial.printf("Температура: %.1f °C\n", temperature);
      Serial.printf("Влажность: %.1f %%\n", humidity);
      Serial.printf("pH: %.1f\n", pH);
      Serial.printf("EC: %.1f mS/cm\n", ec);
      Serial.println("===========================");
    } else {
      Serial.println("❌ Неверный ответ датчика");
      generateTestData();
    }
  } else {
    Serial.println("❌ Недостаточно данных от датчика");
    generateTestData();
  }
}

// ========== ГЕНЕРАЦИЯ ТЕСТОВЫХ ДАННЫХ ==========
void generateTestData() {
  nitrogen = 25.5;
  phosphorus = 18.3;
  potassium = 42.7;
  temperature = 22.5;
  humidity = 65.8;
  pH = 6.8;
  ec = 2.3;
  
  Serial.println("⚠️ Использую тестовые данные");
}

// ========== НАСТРОЙКА ВЕБ-СЕРВЕРА ==========
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Датчик почвы NPK</title>
      <style>
        body { 
          font-family: Arial, sans-serif; 
          margin: 20px; 
          background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
          min-height: 100vh;
          padding: 20px;
        }
        .container { 
          max-width: 900px; 
          margin: 0 auto; 
          background: white; 
          padding: 30px; 
          border-radius: 15px;
          box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { 
          color: #333; 
          text-align: center;
          margin-bottom: 10px;
        }
        .subtitle {
          text-align: center;
          color: #666;
          margin-bottom: 30px;
        }
        .info-box {
          background: #f0f7ff;
          padding: 15px;
          border-radius: 10px;
          margin-bottom: 20px;
          border-left: 4px solid #2196F3;
        }
        .data-grid {
          display: grid;
          grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
          gap: 20px;
          margin: 30px 0;
        }
        .data-card {
          background: #f8f9fa;
          padding: 20px;
          border-radius: 10px;
          text-align: center;
          border-left: 4px solid #667eea;
          transition: transform 0.3s;
        }
        .data-card:hover {
          transform: translateY(-5px);
          box-shadow: 0 5px 15px rgba(0,0,0,0.1);
        }
        .value {
          font-size: 28px;
          font-weight: bold;
          color: #667eea;
          margin: 10px 0;
        }
        .unit {
          color: #666;
          font-size: 14px;
        }
        .label {
          font-weight: bold;
          color: #333;
          font-size: 16px;
        }
        .status {
          padding: 15px;
          border-radius: 8px;
          margin: 20px 0;
          font-weight: bold;
          display: flex;
          align-items: center;
          justify-content: space-between;
        }
        .status-success { background: #e8f5e9; color: #2e7d32; }
        .status-warning { background: #fff3e0; color: #ef6c00; }
        .status-error { background: #ffebee; color: #c62828; }
        .status-info { background: #e3f2fd; color: #1565c0; }
        .controls {
          display: flex;
          flex-wrap: wrap;
          gap: 10px;
          justify-content: center;
          margin-top: 30px;
        }
        button {
          background: #667eea;
          color: white;
          border: none;
          padding: 12px 25px;
          border-radius: 25px;
          font-size: 16px;
          cursor: pointer;
          transition: all 0.3s;
          display: flex;
          align-items: center;
          gap: 8px;
        }
        button:hover {
          background: #5a67d8;
          transform: scale(1.05);
        }
        button.secondary {
          background: #6c757d;
        }
        button.secondary:hover {
          background: #5a6268;
        }
        .timestamp {
          text-align: center;
          color: #666;
          margin-top: 20px;
          font-style: italic;
        }
        .receiver-status {
          display: inline-flex;
          align-items: center;
          gap: 8px;
        }
        .status-dot {
          width: 12px;
          height: 12px;
          border-radius: 50%;
          display: inline-block;
        }
        .dot-online { background: #4caf50; }
        .dot-offline { background: #f44336; }
        .dot-searching { background: #ff9800; animation: pulse 1s infinite; }
        @keyframes pulse {
          0% { opacity: 1; }
          50% { opacity: 0.5; }
          100% { opacity: 1; }
        }
      </style>
    </head>
    <body>
      <div class="container">
        <h1>🌱 Мониторинг почвы NPK</h1>
        <div class="subtitle">IP: )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</div>
        
        <div class="info-box">
          <strong>📡 Статус системы:</strong><br>
          WiFi: <span style="color: green;">✓ Подключен</span><br>
          Датчик: <span id="sensorStatus">Проверка...</span><br>
          Данные: <span id="dataStatus">Ожидание...</span>
        </div>
        
        <div id="statusMessage" class="status status-info" style="display: none;"></div>
        
        <div class="data-grid" id="dataContainer">
          <!-- Данные будут загружены через JavaScript -->
        </div>
        
        <div class="timestamp" id="timestamp">
          Последнее обновление: --
        </div>
        
        <div class="controls">
          <button onclick="updateData()">
            <span>🔄</span> Обновить данные
          </button>
          <button onclick="startAutoUpdate()">
            <span>▶️</span> Автообновление
          </button>
          <button onclick="stopAutoUpdate()" class="secondary">
            <span>⏹️</span> Стоп
          </button>
          <button onclick="location.reload()">
            <span>🔄</span> Обновить страницу
          </button>
        </div>
      </div>
      
      <script>
        let autoUpdateInterval;
        
        function updateSensorStatus() {
          const sensorStatus = document.getElementById('sensorStatus');
          const dataStatus = document.getElementById('dataStatus');
          
          fetch('/status')
            .then(response => response.json())
            .then(data => {
              if (data.sensorWorking) {
                sensorStatus.innerHTML = '<span style="color: green;">✓ Работает</span>';
              } else {
                sensorStatus.innerHTML = '<span style="color: red;">✗ Ошибка</span>';
              }
              
              if (data.dataAvailable) {
                dataStatus.innerHTML = '<span style="color: green;">✓ Есть данные</span>';
              } else {
                dataStatus.innerHTML = '<span style="color: orange;">⚠ Нет данных</span>';
              }
            })
            .catch(error => {
              sensorStatus.innerHTML = '<span style="color: red;">✗ Ошибка связи</span>';
              dataStatus.innerHTML = '<span style="color: red;">✗ Ошибка</span>';
            });
        }
        
        function formatData() {
          fetch('/data')
            .then(response => response.json())
            .then(data => {
              document.getElementById('dataContainer').innerHTML = `
                <div class="data-card">
                  <div class="label">Азот (N)</div>
                  <div class="value">${data.nitrogen.toFixed(1)}</div>
                  <div class="unit">mg/kg</div>
                </div>
                <div class="data-card">
                  <div class="label">Фосфор (P)</div>
                  <div class="value">${data.phosphorus.toFixed(1)}</div>
                  <div class="unit">mg/kg</div>
                </div>
                <div class="data-card">
                  <div class="label">Калий (K)</div>
                  <div class="value">${data.potassium.toFixed(1)}</div>
                  <div class="unit">mg/kg</div>
                </div>
                <div class="data-card">
                  <div class="label">Температура</div>
                  <div class="value">${data.temperature.toFixed(1)}</div>
                  <div class="unit">°C</div>
                </div>
                <div class="data-card">
                  <div class="label">Влажность</div>
                  <div class="value">${data.humidity.toFixed(1)}</div>
                  <div class="unit">%</div>
                </div>
                <div class="data-card">
                  <div class="label">pH почвы</div>
                  <div class="value">${data.ph.toFixed(1)}</div>
                  <div class="unit">pH</div>
                </div>
                <div class="data-card">
                  <div class="label">Электропроводность</div>
                  <div class="value">${data.ec.toFixed(1)}</div>
                  <div class="unit">mS/cm</div>
                </div>
              `;
              
              const now = new Date();
              document.getElementById('timestamp').innerHTML = 
                `Последнее обновление: ${now.toLocaleTimeString()}`;
                
              showStatus('Данные успешно обновлены!', 'success');
            })
            .catch(error => {
              console.error('Ошибка:', error);
              showStatus('Ошибка при получении данных', 'error');
            });
        }
        
        function updateData() {
          showStatus('Обновление данных...', 'loading');
          fetch('/update')
            .then(response => {
              if(response.ok) {
                formatData();
                updateSensorStatus();
              }
            });
        }
        
        function startAutoUpdate() {
          autoUpdateInterval = setInterval(updateData, 10000);
          showStatus('Автообновление запущено (каждые 10 сек)', 'info');
        }
        
        function stopAutoUpdate() {
          clearInterval(autoUpdateInterval);
          showStatus('Автообновление остановлено', 'info');
        }
        
        function showStatus(message, type) {
          const statusDiv = document.getElementById('statusMessage');
          statusDiv.textContent = message;
          statusDiv.className = 'status status-' + type;
          statusDiv.style.display = 'flex';
          
          setTimeout(() => {
            statusDiv.style.display = 'none';
          }, 3000);
        }
        
        // Загрузить данные при старте
        document.addEventListener('DOMContentLoaded', function() {
          formatData();
          updateSensorStatus();
          startAutoUpdate();
          
          // Периодическое обновление статуса
          setInterval(updateSensorStatus, 5000);
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
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["ph"] = pH;
    doc["ec"] = ec;
    doc["timestamp"] = millis();
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  server.on("/update", HTTP_GET, []() {
    readNPKSensor();
    server.send(200, "application/json", "{\"status\":\"updated\"}");
  });
  
  server.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(256);
    doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    doc["sensorWorking"] = (nitrogen > 0 || phosphorus > 0 || potassium > 0);
    doc["dataAvailable"] = firstReadDone;
    doc["localIP"] = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["nitrogen"] = nitrogen;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  server.on("/ping", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 Soil Sensor работает!");
  });
  
  server.on("/debug", HTTP_GET, []() {
    String debug = "=== Debug Info ===\n";
    debug += "WiFi Status: " + String(WiFi.status()) + "\n";
    debug += "IP: " + WiFi.localIP().toString() + "\n";
    debug += "SSID: " + WiFi.SSID() + "\n";
    debug += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    debug += "N: " + String(nitrogen) + "\n";
    debug += "P: " + String(phosphorus) + "\n";
    debug += "K: " + String(potassium) + "\n";
    debug += "First Read Done: " + String(firstReadDone) + "\n";
    debug += "================\n";
    
    server.send(200, "text/plain", debug);
  });
  
  server.begin();
  Serial.println("✅ HTTP сервер запущен на порту 80");
}