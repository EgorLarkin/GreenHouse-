#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>

// ========== НАСТРОЙКИ СЕТИ ==========
const char* ssid = "WIFI_SSID";
const char* password = "PASSWORD";

// ========== НАСТРОЙКИ СЕРВЕРА ==========
const int serverPort = 8888;
const int discoveryPort = 12345;
WebServer server(80);
WiFiServer dataServer(serverPort);
WiFiUDP udp;

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
  float ph;
  float ec;
  unsigned long receivedAt;
};

SensorData lastData;
bool newDataAvailable = false;
String lastSenderIP = "";

// ========== НАСТРОЙКИ ХРАНЕНИЯ ==========
#define MAX_HISTORY 100
SensorData dataHistory[MAX_HISTORY];
int historyIndex = 0;

// ========== ФУНКЦИИ ==========

void setup() {
  Serial.begin(115200);
  
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
}

void loop() {
  // Обработка веб-запросов
  server.handleClient();
  
  // Проверка входящих данных от отправителя
  checkIncomingData();
  
  // Обработка UDP запросов
  handleUDP();
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
    Serial.print("📡 IP адрес: ");
    Serial.println(WiFi.localIP());
    Serial.print("🔑 MAC адрес: ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("\n❌ Не удалось подключиться к WiFi!");
  }
}

// ========== НАСТРОЙКА UDP ==========
void setupUDP() {
  udp.begin(discoveryPort);
  Serial.println("📡 UDP сервер запущен на порту " + String(discoveryPort));
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
      }
    }
  }
}

// ========== ПРОВЕРКА ВХОДЯЩИХ ДАННЫХ ==========
void checkIncomingData() {
  WiFiClient client = dataServer.available();
  
  if (client) {
    Serial.println("🔌 Новое подключение от " + client.remoteIP().toString());
    
    // Читаем запрос
    String request = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        request += c;
        
        // Конец заголовков HTTP
        if (request.endsWith("\r\n\r\n")) {
          break;
        }
      }
    }
    
    // Проверяем тип запроса
    if (request.indexOf("POST /data") >= 0) {
      processDataRequest(client, request);
    } else {
      // Простой HTTP ответ
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("ESP32 Data Receiver готов к работе!");
      client.println("Используйте POST /data для отправки данных");
    }
    
    delay(10);
    client.stop();
    Serial.println("🔌 Клиент отключен");
  }
}

// ========== ОБРАБОТКА ЗАПРОСА ДАННЫХ ==========
void processDataRequest(WiFiClient& client, String& headers) {
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
  
  // Сохраняем данные
  lastData.senderIP = doc["sender"].as<String>();
  lastData.senderMAC = doc["mac"].as<String>();
  lastData.timestamp = doc["timestamp"];
  lastData.nitrogen = doc["nitrogen"];
  lastData.phosphorus = doc["phosphorus"];
  lastData.potassium = doc["potassium"];
  lastData.temperature = doc["temperature"];
  lastData.humidity = doc["humidity"];
  lastData.ph = doc["ph"];
  lastData.ec = doc["ec"];
  lastData.receivedAt = millis();
  
  // Сохраняем в историю
  dataHistory[historyIndex] = lastData;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  
  lastSenderIP = client.remoteIP().toString();
  newDataAvailable = true;
  
  // Выводим в консоль
  Serial.println("✅ Данные получены и сохранены:");
  Serial.println("   Отправитель: " + lastData.senderIP);
  Serial.printf("   Азот (N): %.1f mg/kg\n", lastData.nitrogen);
  Serial.printf("   Фосфор (P): %.1f mg/kg\n", lastData.phosphorus);
  Serial.printf("   Калий (K): %.1f mg/kg\n", lastData.potassium);
  Serial.printf("   Температура: %.1f °C\n", lastData.temperature);
  Serial.printf("   Влажность: %.1f %%\n", lastData.humidity);
  Serial.printf("   pH: %.1f\n", lastData.ph);
  Serial.printf("   EC: %.1f mS/cm\n", lastData.ec);
  Serial.println("   Время получения: " + String(millis()));
  
  // Отправляем ответ
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.println("{\"status\":\"received\",\"timestamp\":" + String(millis()) + "}");
}

// ========== НАСТРОЙКА ВЕБ-СЕРВЕРА ==========
void setupWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, []() {
    String response = "ESP32 Data Receiver\n";
    response += "IP: " + WiFi.localIP().toString() + "\n";
    response += "Data Port: " + String(serverPort) + "\n";
    response += "Last Sender: " + lastSenderIP + "\n";
    response += "Last Update: " + String(lastData.receivedAt) + "\n\n";
    response += "Endpoints:\n";
    response += "  GET /data - последние данные\n";
    response += "  GET /history - история\n";
    response += "  GET /status - статус\n";
    response += "  GET /ping - проверка связи\n";
    server.send(200, "text/plain", response);
  });
  
  // Получение последних данных
  server.on("/data", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    
    doc["status"] = "ok";
    doc["last_update"] = lastData.receivedAt;
    doc["sender_ip"] = lastData.senderIP;
    doc["sender_mac"] = lastData.senderMAC;
    doc["timestamp"] = lastData.timestamp;
    doc["nitrogen"] = lastData.nitrogen;
    doc["phosphorus"] = lastData.phosphorus;
    doc["potassium"] = lastData.potassium;
    doc["temperature"] = lastData.temperature;
    doc["humidity"] = lastData.humidity;
    doc["ph"] = lastData.ph;
    doc["ec"] = lastData.ec;
    doc["received_at"] = lastData.receivedAt;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  // Получение истории
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
        entry["ph"] = dataHistory[i].ph;
        entry["ec"] = dataHistory[i].ec;
        entry["received_at"] = dataHistory[i].receivedAt;
      }
    }
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  // Статус системы
  server.on("/status", HTTP_GET, []() {
    DynamicJsonDocument doc(512);
    
    doc["status"] = "online";
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["ssid"] = WiFi.SSID();
    doc["data_port"] = serverPort;
    doc["last_sender"] = lastSenderIP;
    doc["last_update"] = lastData.receivedAt;
    doc["new_data_available"] = newDataAvailable;
    doc["history_count"] = historyIndex;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  
  // Проверка связи
  server.on("/ping", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 Data Receiver работает!");
  });
  
  // Очистка истории
  server.on("/clear", HTTP_GET, []() {
    memset(dataHistory, 0, sizeof(dataHistory));
    historyIndex = 0;
    server.send(200, "text/plain", "История очищена");
  });
  
  // Ручная установка данных (для тестирования)
  server.on("/test", HTTP_GET, []() {
    lastData.senderIP = "192.168.1.100";
    lastData.senderMAC = "AA:BB:CC:DD:EE:FF";
    lastData.timestamp = millis();
    lastData.nitrogen = 25.5;
    lastData.phosphorus = 18.3;
    lastData.potassium = 42.7;
    lastData.temperature = 22.5;
    lastData.humidity = 65.8;
    lastData.ph = 6.8;
    lastData.ec = 2.3;
    lastData.receivedAt = millis();
    newDataAvailable = true;
    
    server.send(200, "text/plain", "Тестовые данные установлены");
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