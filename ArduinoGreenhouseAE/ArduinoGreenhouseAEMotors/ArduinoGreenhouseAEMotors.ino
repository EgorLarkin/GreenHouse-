#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>

// ========== НАСТРОЙКИ СЕТИ ==========
const char* ssid = "MTS_GPON_1BC5";
const char* password = "UiY2VDYurp";
=======
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";
>>>>>>> 7b6ccc4 (рабочая система)

// ========== НАСТРОЙКИ СЕРВЕРА ==========
const int serverPort = 8888;
const int discoveryPort = 12345;
WebServer server(80);
WiFiServer dataServer(serverPort);
WiFiUDP udp;

// ========== НАСТРОЙКИ СЕРВОПРИВОДОВ ==========
#define SERVO_N_PIN 25
#define SERVO_P_PIN 26
#define SERVO_K_PIN 27
Servo servoN;  // Сервопривод для азота
Servo servoP;  // Сервопривод для фосфора
Servo servoK;  // Сервопривод для калия

// Углы сервоприводов
#define SERVO_CLOSED_ANGLE 0      // Закрыто (не подача)
#define SERVO_OPEN_ANGLE 90       // Открыто (подача)

// ТАЙМЕРЫ
#define SERVO_CHECK_INTERVAL 30000      // Проверка каждые 30 секунд
#define DATA_TIMEOUT_INTERVAL 300000    // Таймаут данных - 5 минут
#define MIN_OPEN_TIME 60000             // Минимальное время открытия - 1 минута
#define MAX_OPEN_TIME 300000            // Максимальное время открытия - 5 минут

// Пороговые значения веществ (мг/кг)
#define N_THRESHOLD_MIN 20.0
#define P_THRESHOLD_MIN 15.0
#define K_THRESHOLD_MIN 30.0

// Гистерезис для предотвращения частого переключения
#define HYSTERESIS 2.0  // +/- 2.0 мг/кг

// Состояния сервоприводов
bool servoNOpen = false;
bool servoPOpen = false;
bool servoKOpen = false;

// Время последнего открытия
unsigned long servoNOpenedAt = 0;
unsigned long servoPOpenedAt = 0;
unsigned long servoKOpenedAt = 0;

// ========== ПЕРЕМЕННЫЕ ДЛЯ ДАННЫХ ==========
struct SensorData {
  String senderIP;
  String senderMAC;
  unsigned long timestamp;
  float nitrogen;
  float phosphorus;
  float potassium;
  float temperature;
  float humidity;
  // float ph;  // УДАЛЕНО - датчик не измеряет pH!
  float ec;
  unsigned long receivedAt;
};

SensorData lastData;
bool newDataAvailable = false;
String lastSenderIP = "";
unsigned long lastDataReceived = 0;

// ========== НАСТРОЙКИ ХРАНЕНИЯ ==========
#define MAX_HISTORY 100
SensorData dataHistory[MAX_HISTORY];
int historyIndex = 0;

// ========== ТАЙМЕРЫ ==========
unsigned long lastServoCheck = 0;
bool dataTimeout = false;

// ========== ФЛАГИ ==========
bool sensorConnected = false;

// ========== ФУНКЦИИ ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("    КОНТРОЛЛЕР УДОБРЕНИЙ ESP32");
  Serial.println("========================================\n");
  
  // Инициализация сервоприводов
  initServos();
  
  // Подключение к WiFi
  connectToWiFi();
  
  // Настройка веб-сервера
  setupWebServer();
  
  // Запуск UDP для обнаружения
  setupUDP();
  
  // Запуск сервера для приема данных
  dataServer.begin();
  Serial.println("✅ Сервер данных запущен на порту " + String(serverPort));
  
  // Инициализация истории
  memset(dataHistory, 0, sizeof(dataHistory));
  
  // Изначально закрываем все сервоприводы
  closeAllServos();
  
  // Инициализация данных
  lastData.nitrogen = 0;
  lastData.phosphorus = 0;
  lastData.potassium = 0;
  lastData.receivedAt = 0;
  
  Serial.println("⏰ Настроены интервалы:");
  Serial.println("  - Проверка веществ: каждые " + String(SERVO_CHECK_INTERVAL/1000) + " сек");
  Serial.println("  - Таймаут данных: " + String(DATA_TIMEOUT_INTERVAL/1000) + " сек");
  Serial.println("  - Минимальное время открытия: " + String(MIN_OPEN_TIME/1000) + " сек");
  Serial.println("\n🔍 Ожидание подключения датчика...");
  Serial.println("📡 Отправьте PING на порт 12345 для проверки связи");
}

void loop() {
  // Обработка веб-запросов
  server.handleClient();
  
  // Проверка входящих данных от датчика
  checkIncomingData();
  
  // Обработка UDP запросов
  handleUDP();
  
  // Проверка и коррекция уровня веществ (реже)
  checkAndCorrectSubstances();
  
  // Проверка таймаута данных (реже)
  checkDataTimeout();
  
  // Проверка максимального времени открытия
  checkMaxOpenTime();
}

// ========== ИНИЦИАЛИЗАЦИЯ СЕРВОПРИВОДОВ ==========
void initServos() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  
  servoN.attach(SERVO_N_PIN, 500, 2400);
  servoP.attach(SERVO_P_PIN, 500, 2400);
  servoK.attach(SERVO_K_PIN, 500, 2400);
  
  Serial.println("✅ Сервоприводы инициализированы на пинах:");
  Serial.println("  - Азот (N): GPIO" + String(SERVO_N_PIN));
  Serial.println("  - Фосфор (P): GPIO" + String(SERVO_P_PIN));
  Serial.println("  - Калий (K): GPIO" + String(SERVO_K_PIN));
}

// ========== УПРАВЛЕНИЕ СЕРВОПРИВОДАМИ ==========
void openServoN() {
  if (!servoNOpen) {
    servoN.write(SERVO_OPEN_ANGLE);
    servoNOpen = true;
    servoNOpenedAt = millis();
    Serial.println("🔓 Сервопривод АЗОТА открыт");
    logServoAction("N", "OPEN", lastData.nitrogen);
  }
}

void closeServoN() {
  if (servoNOpen) {
    // Проверяем минимальное время открытия
    if (millis() - servoNOpenedAt < MIN_OPEN_TIME) {
      Serial.println("⏳ Сервопривод N: слишком рано для закрытия (" +
        String((MIN_OPEN_TIME - (millis() - servoNOpenedAt))/1000) +
        " сек осталось)");
      return;
    }
    servoN.write(SERVO_CLOSED_ANGLE);
    servoNOpen = false;
    Serial.println("🔒 Сервопривод АЗОТА закрыт");
    logServoAction("N", "CLOSE", lastData.nitrogen);
  }
}

void openServoP() {
  if (!servoPOpen) {
    servoP.write(SERVO_OPEN_ANGLE);
    servoPOpen = true;
    servoPOpenedAt = millis();
    Serial.println("🔓 Сервопривод ФОСФОРА открыт");
    logServoAction("P", "OPEN", lastData.phosphorus);
  }
}

void closeServoP() {
  if (servoPOpen) {
    // Проверяем минимальное время открытия
    if (millis() - servoPOpenedAt < MIN_OPEN_TIME) {
      Serial.println("⏳ Сервопривод P: слишком рано для закрытия (" +
        String((MIN_OPEN_TIME - (millis() - servoPOpenedAt))/1000) +
        " сек осталось)");
      return;
    }
    servoP.write(SERVO_CLOSED_ANGLE);
    servoPOpen = false;
    Serial.println("🔒 Сервопривод ФОСФОРА закрыт");
    logServoAction("P", "CLOSE", lastData.phosphorus);
  }
}

void openServoK() {
  if (!servoKOpen) {
    servoK.write(SERVO_OPEN_ANGLE);
    servoKOpen = true;
    servoKOpenedAt = millis();
    Serial.println("🔓 Сервопривод КАЛИЯ открыт");
    logServoAction("K", "OPEN", lastData.potassium);
  }
}

void closeServoK() {
  if (servoKOpen) {
    // Проверяем минимальное время открытия
    if (millis() - servoKOpenedAt < MIN_OPEN_TIME) {
      Serial.println("⏳ Сервопривод K: слишком рано для закрытия (" +
        String((MIN_OPEN_TIME - (millis() - servoKOpenedAt))/1000) +
        " сек осталось)");
      return;
    }
    servoK.write(SERVO_CLOSED_ANGLE);
    servoKOpen = false;
    Serial.println("🔒 Сервопривод КАЛИЯ закрыт");
    logServoAction("K", "CLOSE", lastData.potassium);
  }
}

void closeAllServos() {
  closeServoN();
  closeServoP();
  closeServoK();
  Serial.println("🔒 Все сервоприводы закрыты");
}

void logServoAction(String element, String action, float value) {
  Serial.println("📝 [" + String(millis()/1000) + "с] " + element +
    " сервопривод: " + action + " | Значение: " + String(value) +
    " мг/кг | Порог: " + getThreshold(element));
}

String getThreshold(String element) {
  if (element == "N") return String(N_THRESHOLD_MIN + HYSTERESIS);
  if (element == "P") return String(P_THRESHOLD_MIN + HYSTERESIS);
  if (element == "K") return String(K_THRESHOLD_MIN + HYSTERESIS);
  return "0";
}

// ========== ПРОВЕРКА И КОРРЕКЦИЯ ВЕЩЕСТВ ==========
void checkAndCorrectSubstances() {
  if (millis() - lastServoCheck > SERVO_CHECK_INTERVAL) {
    Serial.println("\n⏰ [" + String(millis()/1000) + "с] Проверка уровня веществ...");
    
    // Проверяем, есть ли актуальные данные
    if (dataTimeout || !sensorConnected) {
      Serial.println("⚠️ Нет актуальных данных от датчика, закрываем все сервоприводы");
      closeAllServos();
    } else {
      // Выводим текущие значения
      Serial.printf("  📊 Текущие значения: N=%.1f, P=%.1f, K=%.1f мг/кг\n",
        lastData.nitrogen, lastData.phosphorus, lastData.potassium);
      Serial.printf("  🎯 Пороги: N<%.1f, P<%.1f, K<%.1f мг/кг\n",
        N_THRESHOLD_MIN, P_THRESHOLD_MIN, K_THRESHOLD_MIN);
      
      // Коррекция азота с гистерезисом
      if (lastData.nitrogen < N_THRESHOLD_MIN && !servoNOpen) {
        Serial.println("  ⬇️ Уровень азота ниже порога, открываем...");
        openServoN();
      } else if (lastData.nitrogen >= (N_THRESHOLD_MIN + HYSTERESIS) && servoNOpen) {
        Serial.println("  ⬆️ Уровень азота восстановлен, закрываем...");
        closeServoN();
      }
      
      // Коррекция фосфора с гистерезисом
      if (lastData.phosphorus < P_THRESHOLD_MIN && !servoPOpen) {
        Serial.println("  ⬇️ Уровень фосфора ниже порога, открываем...");
        openServoP();
      } else if (lastData.phosphorus >= (P_THRESHOLD_MIN + HYSTERESIS) && servoPOpen) {
        Serial.println("  ⬆️ Уровень фосфора восстановлен, закрываем...");
        closeServoP();
      }
      
      // Коррекция калия с гистерезисом
      if (lastData.potassium < K_THRESHOLD_MIN && !servoKOpen) {
        Serial.println("  ⬇️ Уровень калия ниже порога, открываем...");
        openServoK();
      } else if (lastData.potassium >= (K_THRESHOLD_MIN + HYSTERESIS) && servoKOpen) {
        Serial.println("  ⬆️ Уровень калия восстановлен, закрываем...");
        closeServoK();
      }
    }
    
    // Выводим текущее состояние
    Serial.println("  ⚙️ Состояние сервоприводов: " +
      String(servoNOpen ? "N(откр)" : "N(закр)") + ", " +
      String(servoPOpen ? "P(откр)" : "P(закр)") + ", " +
      String(servoKOpen ? "K(откр)" : "K(закр)"));
    
    lastServoCheck = millis();
    Serial.println("⏳ Следующая проверка через " + String(SERVO_CHECK_INTERVAL/1000) + " секунд");
  }
}

// ========== ПРОВЕРКА ТАЙМАУТА ДАННЫХ ==========
void checkDataTimeout() {
  // Если данных нет больше 5 минут - считаем таймаут
  if (lastData.receivedAt > 0 && millis() - lastData.receivedAt > DATA_TIMEOUT_INTERVAL) {
    if (!dataTimeout) {
      dataTimeout = true;
      sensorConnected = false;
      Serial.println("⚠️ [" + String(millis()/1000) + "с] ТАЙМАУТ ДАННЫХ! " +
        "Последние данные получены более " +
        String(DATA_TIMEOUT_INTERVAL/60000) + " минут назад");
    }
  } else if (dataTimeout && millis() - lastData.receivedAt <= DATA_TIMEOUT_INTERVAL) {
    dataTimeout = false;
    sensorConnected = true;
    Serial.println("✅ [" + String(millis()/1000) + "с] Данные снова актуальны");
  }
}

// ========== ПРОВЕРКА МАКСИМАЛЬНОГО ВРЕМЕНИ ОТКРЫТИЯ ==========
void checkMaxOpenTime() {
  unsigned long now = millis();
  
  // Проверяем азот
  if (servoNOpen && (now - servoNOpenedAt > MAX_OPEN_TIME)) {
    Serial.println("⏰ [" + String(now/1000) + "с] ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ N!");
    Serial.println("   Закрываю сервопривод для безопасности");
    servoN.write(SERVO_CLOSED_ANGLE);
    servoNOpen = false;
  }
  
  // Проверяем фосфор
  if (servoPOpen && (now - servoPOpenedAt > MAX_OPEN_TIME)) {
    Serial.println("⏰ [" + String(now/1000) + "с] ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ P!");
    Serial.println("   Закрываю сервопривод для безопасности");
    servoP.write(SERVO_CLOSED_ANGLE);
    servoPOpen = false;
  }
  
  // Проверяем калий
  if (servoKOpen && (now - servoKOpenedAt > MAX_OPEN_TIME)) {
    Serial.println("⏰ [" + String(now/1000) + "с] ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ K!");
    Serial.println("   Закрываю сервопривод для безопасности");
    servoK.write(SERVO_CLOSED_ANGLE);
    servoKOpen = false;
  }
}

// ========== ПОДКЛЮЧЕНИЕ К WIFI ==========
void connectToWiFi() {
  Serial.print("📶 Подключение к WiFi: ");
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
    Serial.print("📡 IP адрес контроллера: ");
    Serial.println(WiFi.localIP());
    Serial.print("🔑 MAC адрес: ");
    Serial.println(WiFi.macAddress());
    Serial.println("🌐 Веб-интерфейс доступен по: http://" + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ Не удалось подключиться к WiFi!");
  }
}

// ========== НАСТРОЙКА UDP ==========
void setupUDP() {
  udp.begin(discoveryPort);
  Serial.println("📡 UDP сервер запущен на порту " + String(discoveryPort));
  Serial.println("   Готов к приему запросов на обнаружение");
}

// ========== ОБРАБОТКА UDP ЗАПРОСОВ ==========
void handleUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;
      String request = String(incomingPacket);
      Serial.print("📨 UDP запрос от ");
      Serial.print(udp.remoteIP());
      Serial.print(": ");
      Serial.println(request);
      
      if (request.startsWith("ESP32_DISCOVERY_REQUEST:")) {
        // Отправляем ответ
        udp.beginPacket(udp.remoteIP(), discoveryPort);
        String response = "ESP32_DISCOVERY_RESPONSE:esp32-receiver:" + WiFi.macAddress();
        udp.print(response);
        udp.endPacket();
        Serial.println("✅ Отправлен UDP ответ на обнаружение");
        
        // Отправляем дополнительный пинг для проверки связи
        udp.beginPacket(udp.remoteIP(), discoveryPort);
        udp.print("PING_FROM_CONTROLLER:" + WiFi.macAddress());
        udp.endPacket();
        Serial.println("📤 Отправлен PING датчику");
      }
      
      // Обработка ответа на пинг
      if (request.startsWith("PING_RESPONSE_FROM_SENSOR:")) {
        Serial.println("✅ Получен ответ на PING от датчика");
        sensorConnected = true;
      }
    }
  }
}

// ========== ПРОВЕРКА ВХОДЯЩИХ ДАННЫХ ==========
void checkIncomingData() {
  WiFiClient client = dataServer.available();
  if (client) {
    Serial.println("\n🔌 Новое подключение от " + client.remoteIP().toString());
    
    // Читаем весь запрос
    String request = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        request += c;
        // Конец запроса (пустая строка после заголовков)
        if (request.endsWith("\r\n\r\n")) {
          break;
        }
      }
    }
    
    // Проверяем тип запроса
    if (request.indexOf("POST /data") >= 0) {
      processDataRequest(client, request);  // ИСПРАВЛЕНО: передаём только заголовки
    } else {
      // Простой HTTP ответ
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("ESP32 Controller готов к работе!");
      client.println("Используйте POST /data для отправки данных");
      client.println("IP контроллера: " + WiFi.localIP().toString());
      client.println("Порт: " + String(serverPort));
    }
    delay(10);
    client.stop();
    Serial.println("🔌 Клиент отключен");
  }
}

// ========== ОБРАБОТКА ЗАПРОСА ДАННЫХ ==========
void processDataRequest(WiFiClient& client, String headers) {  // ИСПРАВЛЕНО: принимаем по значению
  // Ищем Content-Length
  int contentLength = 0;
  int contentLengthIndex = headers.indexOf("Content-Length: ");
  if (contentLengthIndex >= 0) {
    contentLength = headers.substring(contentLengthIndex + 16).toInt();
  }
  
  Serial.print("📦 Ожидаю данные, размер: ");
  Serial.println(contentLength);
  
  // Читаем тело запроса
  String body = "";
  unsigned long startTime = millis();
  while (body.length() < contentLength && millis() - startTime < 5000) {
    if (client.available()) {
      body += (char)client.read();
    }
  }
  
  Serial.println("📥 Полученные данные: " + body);
  
  // Парсим JSON
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    Serial.print("❌ Ошибка парсинга JSON: ");
    Serial.println(error.c_str());
    client.println("HTTP/1.1 400 Bad Request");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();
    client.println("{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  // Сохраняем данные (совместимость с обоими форматами)
  lastData.senderIP = client.remoteIP().toString();
  
  // Проверяем оба возможных формата
  if (doc.containsKey("sender")) {
    lastData.senderIP = doc["sender"].as<String>();
  }
  if (doc.containsKey("mac")) {
    lastData.senderMAC = doc["mac"].as<String>();
  } else {
    lastData.senderMAC = "unknown";
  }
  
  lastData.timestamp = doc.containsKey("timestamp") ? doc["timestamp"] : millis();
  lastData.nitrogen = doc["nitrogen"];
  lastData.phosphorus = doc["phosphorus"];
  lastData.potassium = doc["potassium"];
  lastData.temperature = doc.containsKey("temperature") ? doc["temperature"] : 0.0;
  lastData.humidity = doc.containsKey("humidity") ? doc["humidity"] : 0.0;
  // lastData.ph = doc.containsKey("ph") ? doc["ph"] : 0.0;  // УДАЛЕНО!
  lastData.ec = doc.containsKey("ec") ? doc["ec"] : 0.0;
  lastData.receivedAt = millis();
  
  // Сбрасываем таймаут
  dataTimeout = false;
  sensorConnected = true;
  lastDataReceived = millis();
  
  // Сохраняем в историю
  dataHistory[historyIndex] = lastData;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  lastSenderIP = lastData.senderIP;
  newDataAvailable = true;
  
  // Выводим информацию (БЕЗ pH!)
  Serial.println("✅ Данные успешно получены от " + lastData.senderIP);
  Serial.printf("   📊 N:%.1f P:%.1f K:%.1f мг/кг\n",
                lastData.nitrogen, lastData.phosphorus, lastData.potassium);
  Serial.printf("   🌡️ T:%.1f°C | 💧 H:%.1f%% | ⚡ EC:%.1f mS/cm\n",  // БЕЗ pH!
                lastData.temperature, lastData.humidity, lastData.ec);
  
  // Отправляем успешный ответ
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.println("{\"status\":\"received\",\"timestamp\":" + String(millis()) + ",\"message\":\"Data received successfully\"}");
}

// ========== НАСТРОЙКА ВЕБ-СЕРВЕРА ==========
void setupWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Контроллер удобрений</title>
<style>
body {
font-family: Arial, sans-serif;
margin: 20px;
background: linear-gradient(135deg, #4CAF50 0%, #2E7D32 100%);
min-height: 100vh;
padding: 20px;
}
.container {
max-width: 1000px;
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
.status-card {
background: #f0f7ff;
padding: 20px;
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
border-left: 4px solid #4CAF50;
transition: transform 0.3s;
}
.data-card:hover {
transform: translateY(-5px);
box-shadow: 0 5px 15px rgba(0,0,0,0.1);
}
.servo-card {
background: #fff3e0;
padding: 15px;
border-radius: 10px;
margin: 10px 0;
border-left: 4px solid #FF9800;
}
.value {
font-size: 28px;
font-weight: bold;
color: #4CAF50;
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
.controls {
display: flex;
flex-wrap: wrap;
gap: 10px;
justify-content: center;
margin-top: 30px;
}
button {
background: #4CAF50;
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
background: #388E3C;
transform: scale(1.05);
}
button.secondary {
background: #6c757d;
}
button.secondary:hover {
background: #5a6268;
}
.connected { color: #4CAF50; }
.disconnected { color: #f44336; }
.open { color: #FF9800; }
.closed { color: #9E9E9E; }
</style>
</head>
<body>
<div class="container">
<h1>⚙️ Контроллер удобрений</h1>
<div class="subtitle">IP: )rawliteral" + WiFi.localIP().toString() + R"rawliteral( | Порт: )rawliteral" + String(serverPort) + R"rawliteral(</div>
<div class="status-card">
<strong>📡 Статус системы:</strong><br>
<span id="wifiStatus">WiFi: Проверка...</span><br>
<span id="sensorStatus">Датчик: Не подключен</span><br>
<span id="dataStatus">Данные: Ожидание...</span><br>
<span id="timeoutStatus">Таймаут: Нет</span>
</div>
<div id="connectionAlert" style="display: none; padding: 15px; background: #ffebee; color: #c62828; border-radius: 8px; margin: 20px 0; text-align: center;">
⚠️ ДАТЧИК НЕ ПОДКЛЮЧЕН! Проверьте соединение.
</div>
<h2>📊 Последние данные с датчика</h2>
<div class="data-grid" id="dataContainer">
<!-- Данные будут загружены через JavaScript -->
</div>
<h2>⚙️ Состояние сервоприводов</h2>
<div id="servosContainer">
<!-- Сервоприводы будут загружены через JavaScript -->
</div>
<h2>🎯 Настройки порогов</h2>
<div style="background: #e8f5e9; padding: 20px; border-radius: 10px;">
<div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 15px;">
<div>
<strong>Азот (N):</strong> <span id="nThreshold">20.0</span> мг/кг<br>
<small>Открывается при: < 20.0, закрывается при: ≥ 22.0</small>
</div>
<div>
<strong>Фосфор (P):</strong> <span id="pThreshold">15.0</span> мг/кг<br>
<small>Открывается при: < 15.0, закрывается при: ≥ 17.0</small>
</div>
<div>
<strong>Калий (K):</strong> <span id="kThreshold">30.0</span> мг/кг<br>
<small>Открывается при: < 30.0, закрывается при: ≥ 32.0</small>
</div>
</div>
</div>
<div class="controls">
<button onclick="updateData()">
<span>🔄</span> Обновить данные
</button>
<button onclick="closeAllServos()" class="secondary">
<span>🔒</span> Закрыть все
</button>
<button onclick="location.reload()">
<span>🔄</span> Обновить страницу
</button>
<button onclick="openDebug()">
<span>🐛</span> Отладка
</button>
</div>
<div style="margin-top: 30px; text-align: center; color: #666; font-size: 12px;">
Последнее обновление: <span id="lastUpdate">--</span><br>
<span id="updateInfo"></span>
</div>
</div>
<script>
let autoUpdateInterval;
function updateAllData() {
updateStatus();
updateSensorData();
updateServos();
}
function updateStatus() {
fetch('/status')
.then(response => response.json())
.then(data => {
// WiFi статус
document.getElementById('wifiStatus').innerHTML =
`WiFi: <span class="${data.wifiConnected ? 'connected' : 'disconnected'}">${data.wifiConnected ? '✓ Подключен' : '✗ Отключен'}</span>`;
// Статус датчика
const sensorStatus = document.getElementById('sensorStatus');
const secondsAgo = data.last_update_seconds_ago;
const isConnected = secondsAgo < 300; // 5 минут
if (isConnected) {
sensorStatus.innerHTML = `Датчик: <span class="connected">✓ Подключен (${secondsAgo} сек назад)</span>`;
document.getElementById('connectionAlert').style.display = 'none';
} else {
sensorStatus.innerHTML = `Датчик: <span class="disconnected">✗ Не подключен (${secondsAgo} сек назад)</span>`;
document.getElementById('connectionAlert').style.display = 'block';
}
// Статус данных
document.getElementById('dataStatus').innerHTML =
`Данные: ${data.new_data_available ? '<span class="connected">✓ Есть</span>' : '<span class="disconnected">✗ Нет</span>'}`;
// Таймаут
document.getElementById('timeoutStatus').innerHTML =
`Таймаут: ${data.data_timeout ? '<span class="disconnected">✓ Да</span>' : '<span class="connected">✗ Нет</span>'}`;
})
.catch(error => {
console.error('Ошибка получения статуса:', error);
});
}
function updateSensorData() {
fetch('/data')
.then(response => response.json())
.then(data => {
// Основные данные (БЕЗ pH!)
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
<div class="label">Электропроводность</div>
<div class="value">${data.ec.toFixed(1)}</div>
<div class="unit">mS/cm</div>
</div>
`;
// Обновляем информацию
const now = new Date();
document.getElementById('lastUpdate').textContent = now.toLocaleTimeString();
document.getElementById('updateInfo').textContent =
`Источник: ${data.sender_ip || 'неизвестен'} | Время получения: ${new Date(data.received_at).toLocaleTimeString()}`;
})
.catch(error => {
console.error('Ошибка получения данных:', error);
document.getElementById('dataContainer').innerHTML = '<div style="text-align: center; color: #f44336; padding: 20px;">Ошибка загрузки данных</div>';
});
}
function updateServos() {
fetch('/data')
.then(response => response.json())
.then(data => {
const servos = data.servos;
document.getElementById('servosContainer').innerHTML = `
<div class="servo-card">
<strong>Азот (N):</strong>
<span class="${servos.N.open ? 'open' : 'closed'}">
${servos.N.open ? '✅ ОТКРЫТ' : '🔒 ЗАКРЫТ'}
</span>
${servos.N.open ? ` (открыт ${servos.N.open_seconds} сек)` : ''}
<br>
<button onclick="controlServo('n', '${servos.N.open ? 'close' : 'open'}')" style="padding: 5px 10px; font-size: 12px; margin-top: 5px;">
${servos.N.open ? 'Закрыть' : 'Открыть'}
</button>
</div>
<div class="servo-card">
<strong>Фосфор (P):</strong>
<span class="${servos.P.open ? 'open' : 'closed'}">
${servos.P.open ? '✅ ОТКРЫТ' : '🔒 ЗАКРЫТ'}
</span>
${servos.P.open ? ` (открыт ${servos.P.open_seconds} сек)` : ''}
<br>
<button onclick="controlServo('p', '${servos.P.open ? 'close' : 'open'}')" style="padding: 5px 10px; font-size: 12px; margin-top: 5px;">
${servos.P.open ? 'Закрыть' : 'Открыть'}
</button>
</div>
<div class="servo-card">
<strong>Калий (K):</strong>
<span class="${servos.K.open ? 'open' : 'closed'}">
${servos.K.open ? '✅ ОТКРЫТ' : '🔒 ЗАКРЫТ'}
</span>
${servos.K.open ? ` (открыт ${servos.K.open_seconds} сек)` : ''}
<br>
<button onclick="controlServo('k', '${servos.K.open ? 'close' : 'open'}')" style="padding: 5px 10px; font-size: 12px; margin-top: 5px;">
${servos.K.open ? 'Закрыть' : 'Открыть'}
</button>
</div>
`;
});
}
function controlServo(servo, action) {
fetch(`/servos/${servo}/${action}`)
.then(response => response.text())
.then(result => {
alert(result);
updateServos();
});
}
function closeAllServos() {
if (confirm('Закрыть все сервоприводы?')) {
fetch('/servos/all/close')
.then(response => response.text())
.then(result => {
alert(result);
updateServos();
});
}
}
function updateData() {
updateAllData();
showNotification('Данные обновлены', 'success');
}
function openDebug() {
window.open('/debug', '_blank');
}
function showNotification(message, type) {
// Простое уведомление
alert(message);
}
// Запуск при загрузке
document.addEventListener('DOMContentLoaded', function() {
updateAllData();
autoUpdateInterval = setInterval(updateAllData, 5000); // Каждые 5 секунд
});
</script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  // Отладочная страница
  server.on("/debug", HTTP_GET, []() {
    String response = "<h1>Отладка контроллера</h1>";
    response += "<h3>Сетевая информация:</h3>";
    response += "<pre>";
    response += "IP контроллера: " + WiFi.localIP().toString() + "\n";
    response += "MAC: " + WiFi.macAddress() + "\n";
    response += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    response += "Порт данных: " + String(serverPort) + "\n";
    response += "Порт обнаружения: " + String(discoveryPort) + "\n";
    response += "</pre>";
    response += "<h3>Статус датчика:</h3>";
    response += "<pre>";
    response += "Подключен: " + String(sensorConnected ? "Да" : "Нет") + "\n";
    response += "Последний IP: " + lastSenderIP + "\n";
    response += "Последние данные: " + String((millis() - lastDataReceived)/1000) + " сек назад\n";
    response += "Таймаут: " + String(dataTimeout ? "Да" : "Нет") + "\n";
    response += "</pre>";
    response += "<h3>Команды:</h3>";
    response += "<a href='/ping'><button>Проверка связи</button></a> ";
    response += "<a href='/status'><button>Статус JSON</button></a> ";
    response += "<a href='/data'><button>Данные JSON</button></a> ";
    response += "<a href='/history'><button>История</button></a>";
    server.send(200, "text/html", response);
  });

  // Получение последних данных (JSON) — БЕЗ pH!
  server.on("/data", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    doc["status"] = "ok";
    doc["last_update"] = lastData.receivedAt;
    doc["seconds_ago"] = (millis() - lastData.receivedAt) / 1000;
    doc["sender_ip"] = lastData.senderIP;
    doc["sender_mac"] = lastData.senderMAC;
    doc["timestamp"] = lastData.timestamp;
    doc["nitrogen"] = lastData.nitrogen;
    doc["phosphorus"] = lastData.phosphorus;
    doc["potassium"] = lastData.potassium;
    doc["temperature"] = lastData.temperature;
    doc["humidity"] = lastData.humidity;
    // doc["ph"] = lastData.ph;  // УДАЛЕНО!
    doc["ec"] = lastData.ec;
    doc["received_at"] = lastData.receivedAt;
    // Информация о сервоприводах
    doc["servos"]["N"]["open"] = servoNOpen;
    doc["servos"]["N"]["angle"] = servoNOpen ? SERVO_OPEN_ANGLE : SERVO_CLOSED_ANGLE;
    doc["servos"]["N"]["open_seconds"] = servoNOpen ? (millis() - servoNOpenedAt) / 1000 : 0;
    doc["servos"]["P"]["open"] = servoPOpen;
    doc["servos"]["P"]["angle"] = servoPOpen ? SERVO_OPEN_ANGLE : SERVO_CLOSED_ANGLE;
    doc["servos"]["P"]["open_seconds"] = servoPOpen ? (millis() - servoPOpenedAt) / 1000 : 0;
    doc["servos"]["K"]["open"] = servoKOpen;
    doc["servos"]["K"]["angle"] = servoKOpen ? SERVO_OPEN_ANGLE : SERVO_CLOSED_ANGLE;
    doc["servos"]["K"]["open_seconds"] = servoKOpen ? (millis() - servoKOpenedAt) / 1000 : 0;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Статус системы (JSON)
  server.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(512);
    doc["status"] = "online";
    doc["uptime_seconds"] = millis() / 1000;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["ssid"] = WiFi.SSID();
    doc["data_port"] = serverPort;
    doc["discovery_port"] = discoveryPort;
    doc["last_sender"] = lastSenderIP;
    doc["last_update_seconds_ago"] = (millis() - lastData.receivedAt) / 1000;
    doc["new_data_available"] = newDataAvailable;
    doc["data_timeout"] = dataTimeout;
    doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    // Информация о сервоприводах
    doc["servos"]["N"]["open"] = servoNOpen;
    doc["servos"]["N"]["open_seconds"] = servoNOpen ? (millis() - servoNOpenedAt) / 1000 : 0;
    doc["servos"]["P"]["open"] = servoPOpen;
    doc["servos"]["P"]["open_seconds"] = servoPOpen ? (millis() - servoPOpenedAt) / 1000 : 0;
    doc["servos"]["K"]["open"] = servoKOpen;
    doc["servos"]["K"]["open_seconds"] = servoKOpen ? (millis() - servoKOpenedAt) / 1000 : 0;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // История данных — БЕЗ pH!
  server.on("/history", HTTP_GET, []() {
    DynamicJsonDocument doc(4096);
    JsonArray history = doc.createNestedArray("history");
    for (int i = 0; i < MAX_HISTORY; i++) {
      if (dataHistory[i].receivedAt > 0) {
        JsonObject entry = history.createNestedObject();
        entry["sender_ip"] = dataHistory[i].senderIP;
        entry["timestamp"] = dataHistory[i].timestamp;
        entry["nitrogen"] = dataHistory[i].nitrogen;
        entry["phosphorus"] = dataHistory[i].phosphorus;
        entry["potassium"] = dataHistory[i].potassium;
        entry["temperature"] = dataHistory[i].temperature;
        entry["humidity"] = dataHistory[i].humidity;
        // entry["ph"] = dataHistory[i].ph;  // УДАЛЕНО!
        entry["ec"] = dataHistory[i].ec;
        entry["received_at"] = dataHistory[i].receivedAt;
      }
    }
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Управление сервоприводами
  server.on("/servos/n/open", HTTP_GET, []() {
    openServoN();
    server.send(200, "text/plain", "Сервопривод N открыт");
  });
  server.on("/servos/n/close", HTTP_GET, []() {
    closeServoN();
    server.send(200, "text/plain", "Сервопривод N закрыт");
  });
  server.on("/servos/p/open", HTTP_GET, []() {
    openServoP();
    server.send(200, "text/plain", "Сервопривод P открыт");
  });
  server.on("/servos/p/close", HTTP_GET, []() {
    closeServoP();
    server.send(200, "text/plain", "Сервопривод P закрыт");
  });
  server.on("/servos/k/open", HTTP_GET, []() {
    openServoK();
    server.send(200, "text/plain", "Сервопривод K открыт");
  });
  server.on("/servos/k/close", HTTP_GET, []() {
    closeServoK();
    server.send(200, "text/plain", "Сервопривод K закрыт");
  });
  server.on("/servos/all/close", HTTP_GET, []() {
    closeAllServos();
    server.send(200, "text/plain", "Все сервоприводы закрыты");
  });

  // Проверка связи
  server.on("/ping", HTTP_GET, []() {
    String response = "ESP32 Controller работает! ";
    response += "IP: " + WiFi.localIP().toString();
    response += " | Время работы: " + String(millis()/1000) + " сек";
    server.send(200, "text/plain", response);
  });

  server.begin();
  Serial.println("✅ Веб-сервер запущен на порту 80");
}

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========
void printDataHistory() {
  Serial.println("\n📊 История данных:");
  Serial.println("======================");
  
  int count = 0;
  for (int i = 0; i < MAX_HISTORY; i++) {
    if (dataHistory[i].receivedAt > 0) {
      Serial.print("Запись ");
      Serial.print(count + 1);
      Serial.print(": От ");
      Serial.print(dataHistory[i].senderIP);
      Serial.print(" в ");
      Serial.print(dataHistory[i].receivedAt);
      Serial.print(" мс");
      Serial.printf(" N:%.1f P:%.1f K:%.1f", 
        dataHistory[i].nitrogen, 
        dataHistory[i].phosphorus, 
        dataHistory[i].potassium);
      Serial.println();
      count++;
    }
  }
  
  if (count == 0) {
    Serial.println("История пуста");
  } else {
    Serial.print("Всего записей: ");
    Serial.println(count);
  }
  Serial.println("======================");
}

// ========== ФУНКЦИЯ ДЛЯ ВНЕШНЕГО ДОСТУПА ==========
SensorData getLastSensorData() {
  return lastData;
}

bool isNewDataAvailable() {
  if (newDataAvailable) {
    newDataAvailable = false;
    return true;
  }
  return false;
}

void clearDataHistory() {
  memset(dataHistory, 0, sizeof(dataHistory));
  historyIndex = 0;
}
=======
  Serial.println("🌐 Доступ к интерфейсу: http://" + WiFi.localIP().toString());
}
>>>>>>> 7b6ccc4 (рабочая система)
