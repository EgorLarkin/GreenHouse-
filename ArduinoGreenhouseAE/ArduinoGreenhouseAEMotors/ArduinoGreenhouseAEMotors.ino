#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
#include <HardwareSerial.h>

// ========== НАСТРОЙКИ СЕТИ ==========
const char* ssid = "iPhone 16 pro";
const char* password = "13243546";
const int serverPort = 8888;
const int discoveryPort = 12345;
WebServer server(80);
WiFiServer dataServer(serverPort);
WiFiUDP udp;

// ========== НАСТРОЙКИ ДАТЧИКА NPK (UART2) - как в первом коде ==========
// ✅ Схема подключения изменена под первый код:
#define NPK_RX_PIN 19       // Пин RX ESP32 -> RO преобразователя
#define NPK_TX_PIN 14       // Пин TX ESP32 -> DI преобразователя
#define NPK_DE_PIN 18       // Пин Driver Enable (Разделен)
#define NPK_RE_PIN 4        // Пин Receiver Enable (Разделен)
HardwareSerial npkSerial(2);

// ========== НАСТРОЙКИ СЕРВОПРИВОДОВ ==========
#define SERVO_N_PIN 23
#define SERVO_P_PIN 26
#define SERVO_K_PIN 32
Servo servoN;
Servo servoP;
Servo servoK;
#define SERVO_CLOSED_ANGLE 0
#define SERVO_OPEN_ANGLE 90
#define MIN_OPEN_TIME 1000      // 1 секунда
#define MAX_OPEN_TIME 60000     // 60 секунд
#define SERVO_CHECK_INTERVAL 30000
#define DATA_TIMEOUT_INTERVAL 300000

// ========== ПОРОГОВЫЕ ЗНАЧЕНИЯ ==========
float N_THRESHOLD_MIN = 20.0;
float P_THRESHOLD_MIN = 15.0;
float K_THRESHOLD_MIN = 30.0;
#define HYSTERESIS 2.0

// ========== СОСТОЯНИЕ СИСТЕМЫ ==========
bool autoMode = true;
bool servoNOpen = false;
bool servoPOpen = false;
bool servoKOpen = false;
unsigned long servoNOpenedAt = 0;
unsigned long servoPOpenedAt = 0;
unsigned long servoKOpenedAt = 0;
unsigned long servoNCloseAt = 0;
unsigned long servoPCloseAt = 0;
unsigned long servoKCloseAt = 0;

// ========== ДАННЫЕ С ДАТЧИКА ==========
struct SensorData {
  float nitrogen = 0.0;
  float phosphorus = 0.0;
  float potassium = 0.0;
  float humidity = 0.0;
  float ec = 0.0;
  unsigned long receivedAt = 0;
};
SensorData lastData;
bool dataTimeout = false;
unsigned long lastServoCheck = 0;
unsigned long lastAnnounceTime = 0;
const unsigned long ANNOUNCE_INTERVAL = 10000;

// ========== ПРОТОТИПЫ ФУНКЦИЙ ==========
uint16_t calculateCRC16(const byte *data, uint16_t length);
void readNPKSensorDirect();

// ========== CRC16 (как в первом коде) ==========
uint16_t calculateCRC16(const byte *data, uint16_t length) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; } 
      else { crc >>= 1; }
    }
  }
  return crc;
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // как в первом коде
  
  Serial.println("\n═══════════════════════════════");
  Serial.println("🚀 Контроллер удобрений - ЗАПУСК");
  Serial.println("═══════════════════════════════");
  
  // 1. Инициализация серво
  initServos();
  
  // 2. Инициализация датчика NPK - как в первом коде
  pinMode(NPK_DE_PIN, OUTPUT);
  pinMode(NPK_RE_PIN, OUTPUT);
  digitalWrite(NPK_DE_PIN, LOW);
  digitalWrite(NPK_RE_PIN, LOW);
  npkSerial.begin(9600, SERIAL_8N1, NPK_RX_PIN, NPK_TX_PIN);
  Serial.println("✅ UART2: 9600 бод (RX=19, TX=14, DE=18, RE=4)");
  
  // 3. Сеть и серверы
  connectToWiFi();
  setupWebServer();
  setupUDP();
  dataServer.begin();
  Serial.println("✅ Сервер данных: порт " + String(serverPort));
  
  closeAllServos();
  Serial.println("═══════════════════════════════\n");
}

void loop() {
  server.handleClient();
  checkIncomingData();
  handleUDP();
  
  // Чтение датчика каждые 5 секунд (как в первом коде)
  static unsigned long lastReadTime = 0;
  if (millis() - lastReadTime > 5000) {
    readNPKSensorDirect();
    lastReadTime = millis();
    yield();  // критично для ESP32
  }
  
  checkAndCorrectSubstances();
  checkDataTimeout();
  checkMaxOpenTime();
  checkManualOpenTimer();
  broadcastControllerAnnounce();
  
  yield();  // стабильность ESP32
}

// ========== ИСПРАВЛЕННАЯ ФУНКЦИЯ ЧТЕНИЯ (как в первом коде) ==========
void readNPKSensorDirect() {
  static int attempt = 0;
  attempt++;
  
  Serial.printf("\n[Попытка #%d]\n", attempt);
  
  // Очистка буфера датчика
  while (npkSerial.available()) npkSerial.read();
  delay(50);
  
  // Запрос Modbus
  byte request[8] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x07, 0, 0};
  uint16_t crc = calculateCRC16(request, 6);
  request[6] = crc & 0xFF;          // low byte
  request[7] = (crc >> 8) & 0xFF;   // high byte
  
  // Переключение на передачу
  digitalWrite(NPK_DE_PIN, HIGH);
  digitalWrite(NPK_RE_PIN, HIGH);
  delayMicroseconds(150);  // как в первом коде
  
  npkSerial.write(request, 8);
  npkSerial.flush();
  
  // Переключение на приём
  digitalWrite(NPK_DE_PIN, LOW);
  digitalWrite(NPK_RE_PIN, LOW);
  delayMicroseconds(150);
  
  // Ожидание ответа (300 мс как в первом коде)
  unsigned long start = millis();
  while (npkSerial.available() < 19 && millis() - start < 300) {
    delay(1);
    yield();
  }
  
  int available = npkSerial.available();
  Serial.printf("📥 Ответ: %d байт (%d мс)\n", available, millis()-start);
  
  if (available >= 19) {
    byte resp[19];
    npkSerial.readBytes(resp, 19);
    
    if (resp[0] == 0x01 && resp[1] == 0x03) {
      uint16_t crcRecv = (resp[18]<<8) | resp[17];
      uint16_t crcCalc = calculateCRC16(resp, 17);
      
      if (crcRecv == crcCalc) {
        // ✅ Успешный ответ - парсим данные
        lastData.nitrogen = (resp[3]<<8 | resp[4]) / 10.0;
        lastData.phosphorus = (resp[5]<<8 | resp[6]) / 10.0;
        lastData.potassium = (resp[7]<<8 | resp[8]) / 10.0;
        lastData.humidity = (resp[11]<<8 | resp[12]) / 10.0;
        lastData.ec = (resp[15]<<8 | resp[16]) / 10.0;
        lastData.receivedAt = millis();
        dataTimeout = false;
        
        Serial.printf("✅ N=%.1f P=%.1f K=%.1f H=%.1f EC=%.1f\n", 
          lastData.nitrogen, lastData.phosphorus, lastData.potassium,
          lastData.humidity, lastData.ec);
          
      } else {
        Serial.printf("❌ CRC: recv=0x%04X calc=0x%04X\n", crcRecv, crcCalc);
        dataTimeout = true;
      }
    } else {
      Serial.printf("❌ Заголовок: 0x%02X 0x%02X\n", resp[0], resp[1]);
      dataTimeout = true;
    }
  } else if (available > 0) {
    Serial.println("⚠️ Частичный ответ");
    while (npkSerial.available()) npkSerial.read();
    dataTimeout = true;
  } else {
    Serial.println("❌ Нет ответа от датчика");
    dataTimeout = true;
  }
  
  // Очистка буфера
  while (npkSerial.available()) npkSerial.read();
}

// ========== СЕРВОПРИВОДЫ (без изменений) ==========
void initServos() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  servoN.attach(SERVO_N_PIN, 500, 2400);
  servoP.attach(SERVO_P_PIN, 500, 2400);
  servoK.attach(SERVO_K_PIN, 500, 2400);
  Serial.println("✅ Сервоприводы: N=25, P=26, K=27");
}

void openServoN(unsigned long durationMs = MIN_OPEN_TIME) {
  if (!servoNOpen) {
    servoN.write(SERVO_OPEN_ANGLE);
    servoNOpen = true;
    servoNOpenedAt = millis();
    servoNCloseAt = millis() + durationMs;
    Serial.println("🔓 N открыт на " + String(durationMs/1000.0, 2) + " сек");
  }
}

void openServoP(unsigned long durationMs = MIN_OPEN_TIME) {
  if (!servoPOpen) {
    servoP.write(SERVO_OPEN_ANGLE);
    servoPOpen = true;
    servoPOpenedAt = millis();
    servoPCloseAt = millis() + durationMs;
    Serial.println("🔓 P открыт на " + String(durationMs/1000.0, 2) + " сек");
  }
}

void openServoK(unsigned long durationMs = MIN_OPEN_TIME) {
  if (!servoKOpen) {
    servoK.write(SERVO_OPEN_ANGLE);
    servoKOpen = true;
    servoKOpenedAt = millis();
    servoKCloseAt = millis() + durationMs;
    Serial.println("🔓 K открыт на " + String(durationMs/1000.0, 2) + " сек");
  }
}

void closeServoN() {
  if (servoNOpen && (millis() - servoNOpenedAt >= MIN_OPEN_TIME)) {
    servoN.write(SERVO_CLOSED_ANGLE);
    servoNOpen = false;
    servoNCloseAt = 0;
    Serial.println("🔒 N закрыт");
  }
}

void closeServoP() {
  if (servoPOpen && (millis() - servoPOpenedAt >= MIN_OPEN_TIME)) {
    servoP.write(SERVO_CLOSED_ANGLE);
    servoPOpen = false;
    servoPCloseAt = 0;
    Serial.println("🔒 P закрыт");
  }
}

void closeServoK() {
  if (servoKOpen && (millis() - servoKOpenedAt >= MIN_OPEN_TIME)) {
    servoK.write(SERVO_CLOSED_ANGLE);
    servoKOpen = false;
    servoKCloseAt = 0;
    Serial.println("🔒 K закрыт");
  }
}

void closeAllServos() {
  closeServoN(); 
  closeServoP(); 
  closeServoK();
}

void checkManualOpenTimer() {
  unsigned long now = millis();
  if (servoNOpen && now >= servoNCloseAt) closeServoN();
  if (servoPOpen && now >= servoPCloseAt) closeServoP();
  if (servoKOpen && now >= servoKCloseAt) closeServoK();
}

void checkMaxOpenTime() {
  unsigned long now = millis();
  if (servoNOpen && (now - servoNOpenedAt > MAX_OPEN_TIME)) { 
    servoN.write(SERVO_CLOSED_ANGLE); 
    servoNOpen = false; 
    servoNCloseAt = 0; 
  }
  if (servoPOpen && (now - servoPOpenedAt > MAX_OPEN_TIME)) { 
    servoP.write(SERVO_CLOSED_ANGLE); 
    servoPOpen = false; 
    servoPCloseAt = 0; 
  }
  if (servoKOpen && (now - servoKOpenedAt > MAX_OPEN_TIME)) { 
    servoK.write(SERVO_CLOSED_ANGLE); 
    servoKOpen = false; 
    servoKCloseAt = 0; 
  }
}

// ========== АВТОМАТИКА (без изменений) ==========
void checkAndCorrectSubstances() {
  if (millis() - lastServoCheck > SERVO_CHECK_INTERVAL) {
    if (dataTimeout) {
      Serial.println("⚠️ Таймаут данных, закрываем серво");
      closeAllServos();
    } else {
      Serial.printf("📊 Проверка: N=%.1f, P=%.1f, K=%.1f\n", 
        lastData.nitrogen, lastData.phosphorus, lastData.potassium);
      if (autoMode) {
        if (lastData.nitrogen < N_THRESHOLD_MIN && !servoNOpen) openServoN();
        else if (lastData.nitrogen >= (N_THRESHOLD_MIN + HYSTERESIS) && servoNOpen) closeServoN();
        
        if (lastData.phosphorus < P_THRESHOLD_MIN && !servoPOpen) openServoP();
        else if (lastData.phosphorus >= (P_THRESHOLD_MIN + HYSTERESIS) && servoPOpen) closeServoP();
        
        if (lastData.potassium < K_THRESHOLD_MIN && !servoKOpen) openServoK();
        else if (lastData.potassium >= (K_THRESHOLD_MIN + HYSTERESIS) && servoKOpen) closeServoK();
      }
    }
    lastServoCheck = millis();
  }
}

void checkDataTimeout() {
  if (lastData.receivedAt > 0 && millis() - lastData.receivedAt > DATA_TIMEOUT_INTERVAL) {
    if (!dataTimeout) { 
      dataTimeout = true; 
      Serial.println("⚠️ ТАЙМАУТ ДАННЫХ!"); 
    }
  } else if (dataTimeout) { 
    dataTimeout = false; 
    Serial.println("✅ Данные актуальны"); 
  }
}

// ========== СЕТЬ (без изменений) ==========
void connectToWiFi() {
  Serial.print("📶 WiFi: ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { 
    delay(500); 
    Serial.print("."); 
    attempts++; 
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi подключен! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ Ошибка WiFi");
  }
}

void setupUDP() {
  udp.begin(discoveryPort);
  Serial.println("📡 UDP порт: " + String(discoveryPort));
}

void handleUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;
      String request = String(incomingPacket);
      if (request.startsWith("ESP32_DISCOVERY_REQUEST:")) {
        udp.beginPacket(udp.remoteIP(), discoveryPort);
        udp.print("ESP32_DISCOVERY_RESPONSE:" + WiFi.localIP().toString());
        udp.endPacket();
      }
    }
  }
}

void broadcastControllerAnnounce() {
  if (millis() - lastAnnounceTime > ANNOUNCE_INTERVAL) {
    IPAddress broadcastIP(255, 255, 255, 255);
    udp.beginPacket(broadcastIP, discoveryPort);
    udp.print("CONTROLLER_ANNOUNCE:" + WiFi.localIP().toString());
    udp.endPacket();
    lastAnnounceTime = millis();
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
      client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOK");
    } else {
      client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nController Ready");
    }
    delay(10);
    client.stop();
  }
}

// ========== ВЕБ-СЕРВЕР (без изменений) ==========
void setupWebServer() {
  server.on("/data", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    unsigned long now = millis();
    doc["nitrogen"] = lastData.nitrogen;
    doc["phosphorus"] = lastData.phosphorus;
    doc["potassium"] = lastData.potassium;
    doc["humidity"] = lastData.humidity;
    doc["ec"] = lastData.ec;
    doc["autoMode"] = autoMode;
    doc["servos"]["N"]["open"] = servoNOpen;
    doc["servos"]["N"]["remaining_seconds"] = (servoNCloseAt > now) ? (servoNCloseAt - now) / 1000.0 : 0.0;
    doc["servos"]["P"]["open"] = servoPOpen;
    doc["servos"]["P"]["remaining_seconds"] = (servoPCloseAt > now) ? (servoPCloseAt - now) / 1000.0 : 0.0;
    doc["servos"]["K"]["open"] = servoKOpen;
    doc["servos"]["K"]["remaining_seconds"] = (servoKCloseAt > now) ? (servoKCloseAt - now) / 1000.0 : 0.0;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  server.on("/setmode", HTTP_GET, []() {
    String mode = server.arg("mode");
    autoMode = (mode == "auto");
    server.send(200, "text/plain", autoMode ? "Auto" : "Manual");
  });
  
  server.on("/servos/n/open", HTTP_GET, []() {
    float d = server.hasArg("duration") ? server.arg("duration").toFloat() : 5.0;
    openServoN((unsigned long)(d * 1000));
    server.send(200, "text/plain", "N Open");
  });
  server.on("/servos/n/close", HTTP_GET, []() { 
    closeServoN(); 
    server.send(200, "text/plain", "N Close"); 
  });
  
  server.on("/servos/p/open", HTTP_GET, []() {
    float d = server.hasArg("duration") ? server.arg("duration").toFloat() : 5.0;
    openServoP((unsigned long)(d * 1000));
    server.send(200, "text/plain", "P Open");
  });
  server.on("/servos/p/close", HTTP_GET, []() { 
    closeServoP(); 
    server.send(200, "text/plain", "P Close"); 
  });
  
  server.on("/servos/k/open", HTTP_GET, []() {
    float d = server.hasArg("duration") ? server.arg("duration").toFloat() : 5.0;
    openServoK((unsigned long)(d * 1000));
    server.send(200, "text/plain", "K Open");
  });
  server.on("/servos/k/close", HTTP_GET, []() { 
    closeServoK(); 
    server.send(200, "text/plain", "K Close"); 
  });
  
  server.on("/servos/all/close", HTTP_GET, []() { 
    closeAllServos(); 
    server.send(200, "text/plain", "All Closed"); 
  });
  
  server.begin();
  Serial.println("✅ API сервер запущен (порт 80)");
}