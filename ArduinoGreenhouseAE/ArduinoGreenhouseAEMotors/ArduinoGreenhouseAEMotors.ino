#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>

// ========== НАСТРОЙКИ СЕТИ ==========
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";

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
Servo servoN;
Servo servoP;
Servo servoK;

#define SERVO_CLOSED_ANGLE 0
#define SERVO_OPEN_ANGLE 90
#define SERVO_CHECK_INTERVAL 30000
#define DATA_TIMEOUT_INTERVAL 300000
#define MIN_OPEN_TIME 60000
#define MAX_OPEN_TIME 300000
#define N_THRESHOLD_MIN 20.0
#define P_THRESHOLD_MIN 15.0
#define K_THRESHOLD_MIN 30.0
#define HYSTERESIS 2.0

bool servoNOpen = false;
bool servoPOpen = false;
bool servoKOpen = false;
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
  float humidity;  // УБРАНА ТЕМПЕРАТУРА
  float ec;
  unsigned long receivedAt;
};

SensorData lastData;
bool newDataAvailable = false;
String lastSenderIP = "";
unsigned long lastDataReceived = 0;

#define MAX_HISTORY 100
SensorData dataHistory[MAX_HISTORY];
int historyIndex = 0;

unsigned long lastServoCheck = 0;
bool dataTimeout = false;
bool sensorConnected = false;

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("    КОНТРОЛЛЕР УДОБРЕНИЙ ESP32");
  Serial.println("========================================\n");

  initServos();
  connectToWiFi();
  setupWebServer();
  setupUDP();
  dataServer.begin();
  Serial.println("✅ Сервер данных запущен на порту " + String(serverPort));

  memset(dataHistory, 0, sizeof(dataHistory));
  closeAllServos();
  lastData.nitrogen = 0;
  lastData.phosphorus = 0;
  lastData.potassium = 0;
  lastData.receivedAt = 0;

  Serial.println("⏰ Настроены интервалы:");
  Serial.println("  - Проверка веществ: каждые " + String(SERVO_CHECK_INTERVAL/1000) + " сек");
  Serial.println("  - Таймаут данных: " + String(DATA_TIMEOUT_INTERVAL/1000) + " сек");
  Serial.println("  - Минимальное время открытия: " + String(MIN_OPEN_TIME/1000) + " сек");
  Serial.println("\n🔍 Ожидание подключения датчика...");
}

void loop() {
  server.handleClient();
  checkIncomingData();
  handleUDP();
  checkAndCorrectSubstances();
  checkDataTimeout();
  checkMaxOpenTime();
}

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

void openServoN() {
  if (!servoNOpen) {
    servoN.write(SERVO_OPEN_ANGLE);
    servoNOpen = true;
    servoNOpenedAt = millis();
    Serial.println("🔓 Сервопривод АЗОТА открыт");
  }
}
void closeServoN() {
  if (servoNOpen) {
    if (millis() - servoNOpenedAt < MIN_OPEN_TIME) return;
    servoN.write(SERVO_CLOSED_ANGLE);
    servoNOpen = false;
    Serial.println("🔒 Сервопривод АЗОТА закрыт");
  }
}
void openServoP() {
  if (!servoPOpen) {
    servoP.write(SERVO_OPEN_ANGLE);
    servoPOpen = true;
    servoPOpenedAt = millis();
    Serial.println("🔓 Сервопривод ФОСФОРА открыт");
  }
}
void closeServoP() {
  if (servoPOpen) {
    if (millis() - servoPOpenedAt < MIN_OPEN_TIME) return;
    servoP.write(SERVO_CLOSED_ANGLE);
    servoPOpen = false;
    Serial.println("🔒 Сервопривод ФОСФОРА закрыт");
  }
}
void openServoK() {
  if (!servoKOpen) {
    servoK.write(SERVO_OPEN_ANGLE);
    servoKOpen = true;
    servoKOpenedAt = millis();
    Serial.println("🔓 Сервопривод КАЛИЯ открыт");
  }
}
void closeServoK() {
  if (servoKOpen) {
    if (millis() - servoKOpenedAt < MIN_OPEN_TIME) return;
    servoK.write(SERVO_CLOSED_ANGLE);
    servoKOpen = false;
    Serial.println("🔒 Сервопривод КАЛИЯ закрыт");
  }
}
void closeAllServos() {
  closeServoN();
  closeServoP();
  closeServoK();
  Serial.println("🔒 Все сервоприводы закрыты");
}

void checkAndCorrectSubstances() {
  if (millis() - lastServoCheck > SERVO_CHECK_INTERVAL) {
    Serial.println("\n⏰ Проверка уровня веществ...");
    if (dataTimeout || !sensorConnected) {
      Serial.println("⚠️ Нет актуальных данных, закрываем все сервоприводы");
      closeAllServos();
    } else {
      Serial.printf("  📊 Текущие значения: N=%.1f, P=%.1f, K=%.1f мг/кг | 💧 H:%.1f%% | ⚡ EC:%.1f\n",
                    lastData.nitrogen, lastData.phosphorus, lastData.potassium,
                    lastData.humidity, lastData.ec);
      if (lastData.nitrogen < N_THRESHOLD_MIN && !servoNOpen) openServoN();
      else if (lastData.nitrogen >= (N_THRESHOLD_MIN + HYSTERESIS) && servoNOpen) closeServoN();

      if (lastData.phosphorus < P_THRESHOLD_MIN && !servoPOpen) openServoP();
      else if (lastData.phosphorus >= (P_THRESHOLD_MIN + HYSTERESIS) && servoPOpen) closeServoP();

      if (lastData.potassium < K_THRESHOLD_MIN && !servoKOpen) openServoK();
      else if (lastData.potassium >= (K_THRESHOLD_MIN + HYSTERESIS) && servoKOpen) closeServoK();
    }
    lastServoCheck = millis();
  }
}

void checkDataTimeout() {
  if (lastData.receivedAt > 0 && millis() - lastData.receivedAt > DATA_TIMEOUT_INTERVAL) {
    if (!dataTimeout) {
      dataTimeout = true;
      sensorConnected = false;
      Serial.println("⚠️ ТАЙМАУТ ДАННЫХ! Последние данные получены более 5 минут назад");
    }
  } else if (dataTimeout && millis() - lastData.receivedAt <= DATA_TIMEOUT_INTERVAL) {
    dataTimeout = false;
    sensorConnected = true;
    Serial.println("✅ Данные снова актуальны");
  }
}

void checkMaxOpenTime() {
  unsigned long now = millis();
  if (servoNOpen && (now - servoNOpenedAt > MAX_OPEN_TIME)) {
    servoN.write(SERVO_CLOSED_ANGLE);
    servoNOpen = false;
    Serial.println("⏰ ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ N! Закрыто для безопасности");
  }
  if (servoPOpen && (now - servoPOpenedAt > MAX_OPEN_TIME)) {
    servoP.write(SERVO_CLOSED_ANGLE);
    servoPOpen = false;
    Serial.println("⏰ ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ P! Закрыто для безопасности");
  }
  if (servoKOpen && (now - servoKOpenedAt > MAX_OPEN_TIME)) {
    servoK.write(SERVO_CLOSED_ANGLE);
    servoKOpen = false;
    Serial.println("⏰ ПРЕВЫШЕНО МАКС. ВРЕМЯ ОТКРЫТИЯ K! Закрыто для безопасности");
  }
}

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
  } else {
    Serial.println("\n❌ Не удалось подключиться к WiFi!");
  }
}

void setupUDP() {
  udp.begin(discoveryPort);
  Serial.println("📡 UDP сервер запущен на порту " + String(discoveryPort));
}

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
        udp.beginPacket(udp.remoteIP(), discoveryPort);
        String response = "ESP32_DISCOVERY_RESPONSE:esp32-receiver:" + WiFi.macAddress();
        udp.print(response);
        udp.endPacket();
        Serial.println("✅ Отправлен ответ на обнаружение");

        // Отправляем пинг датчику
        udp.beginPacket(udp.remoteIP(), discoveryPort);
        udp.print("PING_FROM_CONTROLLER:" + WiFi.macAddress());
        udp.endPacket();
        Serial.println("📤 Отправлен PING датчику");
      }
      if (request.startsWith("PING_RESPONSE_FROM_SENSOR:")) {
        Serial.println("✅ Получен ответ на PING от датчика");
        sensorConnected = true;
      }
    }
  }
}

void checkIncomingData() {
  WiFiClient client = dataServer.available();
  if (client) {
    String request = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        request += c;
        if (request.endsWith("\r\n\r\n")) break;
      }
    }
    if (request.indexOf("POST /data") >= 0) {
      processDataRequest(client, request);
    } else {
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
  }
}

void processDataRequest(WiFiClient& client, String headers) {
  int contentLength = 0;
  int contentLengthIndex = headers.indexOf("Content-Length: ");
  if (contentLengthIndex >= 0) {
    contentLength = headers.substring(contentLengthIndex + 16).toInt();
  }

  String body = "";
  unsigned long startTime = millis();
  while (body.length() < contentLength && millis() - startTime < 5000) {
    if (client.available()) body += (char)client.read();
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    client.println("HTTP/1.1 400 Bad Request");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();
    client.println("{\"error\":\"Invalid JSON\"}");
    return;
  }

  lastData.senderIP = client.remoteIP().toString();
  if (doc.containsKey("sender")) lastData.senderIP = doc["sender"].as<String>();
  if (doc.containsKey("mac")) lastData.senderMAC = doc["mac"].as<String>();
  else lastData.senderMAC = "unknown";
  lastData.timestamp = doc.containsKey("timestamp") ? doc["timestamp"] : millis();
  lastData.nitrogen = doc["nitrogen"];
  lastData.phosphorus = doc["phosphorus"];
  lastData.potassium = doc["potassium"];
  lastData.humidity = doc.containsKey("humidity") ? doc["humidity"] : 0.0;  // БЕЗ ТЕМПЕРАТУРЫ
  lastData.ec = doc.containsKey("ec") ? doc["ec"] : 0.0;
  lastData.receivedAt = millis();

  dataTimeout = false;
  sensorConnected = true;
  lastDataReceived = millis();

  dataHistory[historyIndex] = lastData;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  lastSenderIP = lastData.senderIP;
  newDataAvailable = true;

  Serial.println("✅ Данные получены от " + lastData.senderIP);
  Serial.printf("   📊 N:%.1f P:%.1f K:%.1f мг/кг | 💧 H:%.1f%% | ⚡ EC:%.1f\n",
                lastData.nitrogen, lastData.phosphorus, lastData.potassium,
                lastData.humidity, lastData.ec);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.println("{\"status\":\"received\",\"timestamp\":" + String(millis()) + "}");
}

void setupWebServer() {
  // Только текстовый ответ на главной странице
  server.on("/", HTTP_GET, []() {
    String response = "ESP32 Контроллер удобрений\n";
    response += "=============================\n";
    response += "IP: " + WiFi.localIP().toString() + "\n";
    response += "Порт данных: " + String(serverPort) + "\n";
    response += "Порт обнаружения: " + String(discoveryPort) + "\n";
    response += "Доступные эндпоинты:\n";
    response += "  GET /status - Статус системы\n";
    response += "  GET /data - Последние данные с датчика и состояние сервоприводов\n";
    response += "  GET /ping - Проверка связи (возвращает 'pong')\n";
    response += "  GET /servos/[n|p|k]/[open|close] - Управление сервоприводами";
    server.send(200, "text/plain", response);
  });

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
    doc["humidity"] = lastData.humidity;  // БЕЗ ТЕМПЕРАТУРЫ
    doc["ec"] = lastData.ec;
    doc["received_at"] = lastData.receivedAt;
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
    doc["sensor_connected"] = sensorConnected;
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
  server.on("/ping", HTTP_GET, []() {
    String response = "pong";
    server.send(200, "text/plain", response);
  });

  server.begin();
  Serial.println("✅ API сервер запущен на порту 80");
}