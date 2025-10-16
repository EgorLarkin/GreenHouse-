#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <FastBot.h>

// Настройки Wi-Fi
const char* ssid = "421";
const char* password = "dfmnprof";

// Настройки Telegram
#define BOT_TOKEN "8165033611:AAG3HWYMpE06sJ7CScbxcCndfd13Nu5wq4k"  // Токен вашего бота
#define CHAT_ID "2010765991"       // ID вашего чата

WiFiClient wifiClient;
FastBot bot(BOT_TOKEN, wifiClient);

// Настройки датчиков
#define DHTPIN 4         // Пин подключения DHT
#define DHTTYPE DHT22    // Тип датчика
DHT dht(DHTPIN, DHTTYPE);
const int soilMoisturePin = 34; // Пин подключения датчика влажности почвы

// Настройки емкостей
const int pumpPin = 13;          // Пин для насоса полива
const int fertilizerPin1 = 12;   // Пин для первого вещества
const int fertilizerPin2 = 14;   // Пин для второго вещества
const int fertilizerPin3 = 27;   // Пин для третьего вещества
const int fertilizerPin4 = 26;   // Пин для четвертого вещества
const int mixingTankPin = 25;    // Пин для насоса емкости смешивания

// Создаем объект веб-сервера на порту 80
WebServer server(80);

// Переменные для хранения данных
float humidity;
float temperature;
int soilMoisture;

// Функция для получения данных от датчиков
void readSensors() {
    humidity = dht.readHumidity();
    temperature = dht.readTemperature();
    soilMoisture = analogRead(soilMoisturePin);
}

// HTML страница
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<meta charset="UTF-8">
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Управление поливом</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f9; }
        h1 { color: #333; }
        .button {
            background-color: #4CAF50; /* Зеленый */
            border: none;
            color: white;
            padding: 15px 20px;
            text-align: center;
            text-decoration: none;
            display: inline-block;
            font-size: 16px;
            margin: 10px 2px;
            cursor: pointer;
            border-radius: 4px;
            transition: background-color 0.3s;
        }
        .button:hover { background-color: #45a049; }
        .status { font-size: 24px; margin-top: 20px; }
        .moisture, .humidity, .temperature { font-size: 24px; margin-top: 20px; }
    </style>
    <script>
        function executeCommand(command) {
            const form = document.getElementById('mixForm');
            const formData = new FormData(form);
            const params = new URLSearchParams(formData).toString();
            fetch('/' + command + '?' + params)
                .then(response => response.text())
                .then(text => {
                    document.getElementById('result').innerText = text;
                    updatePage();
                });
        }

        function updatePage() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('moisture').innerText = 'Влажность почвы: ' + data.soilMoisture;
                    document.getElementById('humidity').innerText = 'Влажность воздуха: ' + data.humidity; // Добавлено
                    document.getElementById('temperature').innerText = 'Температура воздуха: ' + data.temperature;
                    document.getElementById('status').innerText = 'Статус: ' + data.status;
                    document.getElementById('status').style.color = data.color;
                });
        }
    </script>
</head>
<body>
    <h1>Управление поливом растений</h1>
    <div class="moisture" id="moisture">Влажность почвы: %SOIL_MOISTURE%</div>
    <div class="humidity" id="humidity">Влажность воздуха: %HUMIDITY%</div> <!-- Добавлено -->
    <div class="temperature" id="temperature">Температура воздуха: %TEMPERATURE%</div>
    <div class="status" id="status" style="color: %COLOR%">Статус: %STATUS%</div>
    <button class="button" onclick="executeCommand('water')">Полив</button>
    <button class="button" onclick="executeCommand('fertilize1')">Подкормка 1</button>
    <button class="button" onclick="executeCommand('fertilize2')">Подкормка 2</button>
    <button class="button" onclick="executeCommand('fertilize3')">Подкормка 3</button>
    <button class="button" onclick="executeCommand('fertilize4')">Подкормка 4</button>

    <h2>Смешивание веществ</h2>
    <form id="mixForm">
        <label for="fertilizer1">Подкормка 1:</label>
        <input type="number" id="fertilizer1" name="fertilizer1" min="0" value="0"><br>
        <label for="fertilizer2">Подкормка 2:</label>
        <input type="number" id="fertilizer2" name="fertilizer2" min="0" value="0"><br>
        <label for="fertilizer3">Подкормка 3:</label>
        <input type="number" id="fertilizer3" name="fertilizer3" min="0" value="0"><br>
        <label for="fertilizer4">Подкормка 4:</label>
        <input type="number" id="fertilizer4" name="fertilizer4" min="0" value="0"><br>
        <button type="button" class="button" onclick="executeCommand('mix')">Смешивание</button>
    </form>
    <div id="result"></div>
</body>
</html>
)rawliteral";
// Обработчики
void handleRoot() {
    readSensors();
    String page = htmlPage;
    page.replace("%SOIL_MOISTURE%", String(soilMoisture).c_str());
    page.replace("%HUMIDITY%", String(humidity).c_str()); // Замена на влажность воздуха
    page.replace("%TEMPERATURE%", String(temperature).c_str());
    String color = soilMoisture < 300 ? "red" : "green"; // Замените 300 на подходящее значение
    String status = soilMoisture < 300 ? "Сухо, нужно полить!" : "Влажно, все в порядке.";
    page.replace("%COLOR%", color);
    page.replace("%STATUS%", status);
    server.send(200, "text/html", page);
}

void handleStatus() {
    readSensors(); // Обновим данные с датчиков
    String jsonResponse = "{";
    jsonResponse += "\"soilMoisture\":" + String(soilMoisture) + ",";
    jsonResponse += "\"temperature\":" + String(temperature) + ",";
    jsonResponse += "\"humidity\":" + String(humidity) + ","; // Добавлено
    String color = soilMoisture < 300 ? "red" : "green"; // Замените 300 на подходящее значение
    String status = soilMoisture < 300 ? "Сухо, нужно полить!" : "Влажно, все в порядке.";
    jsonResponse += "\"status\":\"" + status + "\",";
    jsonResponse += "\"color\":\"" + color + "\"";
    jsonResponse += "}";
    server.send(200, "application/json", jsonResponse);
}

// Функция для полива до достижения 100% влажности
void waterUntilSaturation() {
    int targetMoistureLevel = 800; // Задайте целевой уровень по шкале вашего датчика
    digitalWrite(pumpPin, HIGH);  // Включаем насос
    Serial.println("Начат полив...");

    while (soilMoisture < targetMoistureLevel) {
        delay(1000);
        readSensors(); // Обновляем уровень влажности
        Serial.print("Текущая влажность: ");
        Serial.println(soilMoisture);
    }

    digitalWrite(pumpPin, LOW);  // Отключаем насос
    Serial.println("Полив завершен.");
}
void handleWater() {
    waterUntilSaturation();
    server.send(200, "text/plain", "Полив завершен до достижения 100% влажности.");
}

void handleFertilize1() {
    digitalWrite(fertilizerPin1, HIGH); // Включаем первое вещество
    delay(5000); // Длительность подачи (настройте)
    digitalWrite(fertilizerPin1, LOW); // Отключаем
    server.send(200, "text/plain", "Подкормка 1 завершена.");
}

void handleFertilize2() {
    digitalWrite(fertilizerPin2, HIGH); // Включаем второе вещество
    delay(5000); // Длительность подачи (настройте)
    digitalWrite(fertilizerPin2, LOW); // Отключаем
    server.send(200, "text/plain", "Подкормка 2 завершена.");
}

void handleFertilize3() {
    digitalWrite(fertilizerPin3, HIGH); // Включаем третье вещество
    delay(5000); // Длительность подачи (настройте)
    digitalWrite(fertilizerPin3, LOW); // Отключаем
    server.send(200, "text/plain", "Подкормка 3 завершена.");
}

void handleFertilize4() {
    digitalWrite(fertilizerPin4, HIGH); // Включаем четвертое вещество
    delay(5000); // Длительность подачи (настройте)
    digitalWrite(fertilizerPin4, LOW); // Отключаем
    server.send(200, "text/plain", "Подкормка 4 завершена.");
}

void handleMix() {
    // Получаем пропорции из запроса
    int fertilizer1 = server.arg("fertilizer1").toInt();
    int fertilizer2 = server.arg("fertilizer2").toInt();
    int fertilizer3 = server.arg("fertilizer3").toInt();
    int fertilizer4 = server.arg("fertilizer4").toInt();

    // Включаем каждый насос на указанное время (или задайте другую логику, основанную на пропорциях)
    if (fertilizer1 > 0) {
        digitalWrite(fertilizerPin1, HIGH);
        delay(fertilizer1 * 1000); // Секунды
        digitalWrite(fertilizerPin1, LOW);
    }
    if (fertilizer2 > 0) {
        digitalWrite(fertilizerPin2, HIGH);
        delay(fertilizer2 * 1000); // Секунды
        digitalWrite(fertilizerPin2, LOW);
    }
    if (fertilizer3 > 0) {
        digitalWrite(fertilizerPin3, HIGH);
        delay(fertilizer3 * 1000); // Секунды
        digitalWrite(fertilizerPin3, LOW);
    }
    if (fertilizer4 > 0) {
        digitalWrite(fertilizerPin4, HIGH);
        delay(fertilizer4 * 1000); // Секунды
        digitalWrite(fertilizerPin4, LOW);
    }

    server.send(200, "text/plain", "Смешивание завершено.");
}

void newMsg(FB_msg& msg) {
    String text = msg.text;
    Serial.println(text);
    if (text == String("/status")) {
        readSensors(); // Обновите данные датчиков перед отправкой
        bot.sendMessage("Влажность почвы: " + String(soilMoisture) +
                        "\nТемпература воздуха: " + String(temperature) +
                        "\nВлажность воздуха: " + String(humidity) + "\n", CHAT_ID);
        Serial.println("отправка: " + String("Влажность почвы: " + String(soilMoisture) +
                                            "\nТемпература воздуха: " + String(temperature) +
                                            "\nВлажность воздуха: " + String(humidity) + "\n"));
    } 
    else if (text == String("/water")) {
        bot.sendMessage("Полив начат.", CHAT_ID);
        waterUntilSaturation();
        bot.sendMessage("Полив завершен до достижения 100% влажности.", CHAT_ID);
    }
    else if (text == String("/fertilize1")) {
        handleFertilize1();
        bot.sendMessage("Подкормка 1 завершена.", CHAT_ID);
    } 
    else if (text == String("/fertilize2")) {
        handleFertilize2();
        bot.sendMessage("Подкормка 2 завершена.", CHAT_ID);
    } 
    else if (text == String("/fertilize3")) {
        handleFertilize3();
        bot.sendMessage("Подкормка 3 завершена.", CHAT_ID);
    } 
    else if (text == String("/fertilize4")) {
        handleFertilize4();
        bot.sendMessage("Подкормка 4 завершена.", CHAT_ID);
    } 
    else if (text.startsWith("/mix")) {
        // Извлекаем пропорции из текста
        int fertilizer1, fertilizer2, fertilizer3, fertilizer4;
        sscanf(text.c_str(), "/mix %d %d %d %d", &fertilizer1, &fertilizer2, &fertilizer3, &fertilizer4);

        // Используйте значения для смешивания
        digitalWrite(fertilizerPin1, HIGH);
        delay(fertilizer1 * 1000);
        digitalWrite(fertilizerPin1, LOW);

        digitalWrite(fertilizerPin2, HIGH);
        delay(fertilizer2 * 1000);
        digitalWrite(fertilizerPin2, LOW);
    
        digitalWrite(fertilizerPin3, HIGH);
        delay(fertilizer3 * 1000);
        digitalWrite(fertilizerPin3, LOW);
    
        digitalWrite(fertilizerPin4, HIGH);
        delay(fertilizer4 * 1000);
        digitalWrite(fertilizerPin4, LOW);
    
        bot.sendMessage("Смешивание завершено.", CHAT_ID);
    }
}
// Обработчик команды из Telegram

void setup() {
    Serial.begin(115200);
    dht.begin();
    pinMode(pumpPin, OUTPUT);
    pinMode(fertilizerPin1, OUTPUT);
    pinMode(fertilizerPin2, OUTPUT);
    pinMode(fertilizerPin3, OUTPUT);
    pinMode(fertilizerPin4, OUTPUT);
    pinMode(mixingTankPin, OUTPUT);
    digitalWrite(pumpPin, LOW);
    digitalWrite(fertilizerPin1, LOW);
    digitalWrite(fertilizerPin2, LOW);
    digitalWrite(fertilizerPin3, LOW);
    digitalWrite(fertilizerPin4, LOW);
    digitalWrite(mixingTankPin, LOW);
    bot.setChatID(CHAT_ID);
    bot.attach(newMsg);
    Serial.println("Бот готов к работе!");
//    bot.sendMessage("Бот готов к работе!", CHAT_ID);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Подключение к WiFi...");
    }
    Serial.println("Подключено к WiFi");
    Serial.println(WiFi.localIP());
    
    server.on("/", handleRoot);
    server.on("/water", handleWater);
    server.on("/fertilize1", handleFertilize1);
    server.on("/fertilize2", handleFertilize2);
    server.on("/fertilize3", handleFertilize3);
    server.on("/fertilize4", handleFertilize4);
    server.on("/mix", handleMix);
    server.on("/status", handleStatus);
    server.begin();
    Serial.println("Веб-сервер запущен");
}

void loop() {
    bot.tick();
    server.handleClient();  // Обработка команд из Telegram
    // Обновление данных о датчиках
    readSensors(); 
}
