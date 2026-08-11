#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

#define DHT_PIN          6
#define DHTTYPE          DHT11
#define LDR_PIN          7
#define POT_PIN          4
#define LED_IRRIGA_PIN   10
#define BUZZER_PIN       5
#define SDA_PIN          8
#define SCL_PIN          9

DHT dht(DHT_PIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com";

WiFiClient espMqttClient;
PubSubClient clientMQTT(espMqttClient);

const char* TELEGRAM_TOKEN   = "8675430274:AAHI4KF7EOQPyIQfqzSMipgaZQVZz6YfTuM";
const char* TELEGRAM_CHAT_ID = "8801014782";

const float UMIDADE_MOFO_LIMITE = 85.0;
const float TEMP_ALERTA_LIMITE  = 30.0;

bool alertaMofoAtivo = false;
bool alertaTempAtivo = false;

// Variáveis de Estado
float g_temp = 25.0, g_umid = 60.0;
int   g_luz = 80, g_agua = 56;
bool  g_irrigando = false;

unsigned long tempoUltimoEnvio = 0;
unsigned long ultimoPollingTelegram = 0;
long lastUpdateId = 0;

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, msg);
  if (!err) {
    if (doc.containsKey("temp")) g_temp = doc["temp"];
    if (doc.containsKey("umid")) g_umid = doc["umid"];
    if (doc.containsKey("luz"))  g_luz  = doc["luz"];
    if (doc.containsKey("agua")) g_agua = doc["agua"];
    if (doc.containsKey("bomba")) g_irrigando = doc["bomba"];
  }
}

void reconectarMQTT() {
  while (!clientMQTT.connected()) {
    String clientId = "ESP32Estufa-" + String(random(0xffff), HEX);
    if (clientMQTT.connect(clientId.c_str())) {
      clientMQTT.subscribe("estufa/senai/comando");
    } else {
      delay(500);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Conectando...");

  dht.begin();
  pinMode(LED_IRRIGA_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  clientMQTT.setServer(mqtt_server, 1883);
  clientMQTT.setCallback(mqttCallback);

  lcd.clear();
  lcd.print("Sistema OK!");
  delay(1000);
}

void loop() {
  if (!clientMQTT.connected()) {
    reconectarMQTT();
  }
  clientMQTT.loop();

  // 1. Ler sensores do Wokwi se nenhum slider manual foi movido
  float t_lida = dht.readTemperature();
  float u_lida = dht.readHumidity();
  int pot_val  = analogRead(POT_PIN);
  int ldr_val  = analogRead(LDR_PIN);

  if (!isnan(t_lida)) g_temp = t_lida;
  if (!isnan(u_lida)) g_umid = u_lida;
  g_agua = map(pot_val, 0, 4095, 0, 100);
  g_luz  = map(ldr_val, 0, 4095, 0, 100);

  // 2. Regras de Atuadores
  bool riscoMofo = (g_umid > UMIDADE_MOFO_LIMITE);
  digitalWrite(LED_IRRIGA_PIN, (g_irrigando && !riscoMofo) ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, (g_temp > TEMP_ALERTA_LIMITE) ? HIGH : LOW);

  // 3. Atualizar Display LCD
  lcd.setCursor(0, 0);
  lcd.print("T:" + String(g_temp, 1) + "C U:" + String(g_umid, 0) + "%   ");
  lcd.setCursor(0, 1);
  if (riscoMofo) {
    lcd.print("MOFO! Irrig OFF ");
  } else {
    lcd.print("L:" + String(g_luz) + "% Agua:" + String(g_agua) + "%   ");
  }

  // 4. Enviar dados para o site a cada 2s
  if (millis() - tempoUltimoEnvio > 2000) {
    tempoUltimoEnvio = millis();

    DynamicJsonDocument doc(256);
    doc["temp"]  = g_temp;
    doc["umid"]  = g_umid;
    doc["luz"]   = g_luz;
    doc["agua"]  = g_agua;
    doc["bomba"] = g_irrigando;

    String jsonBuffer;
    serializeJson(doc, jsonBuffer);
    clientMQTT.publish("estufa/senai/dados", jsonBuffer.c_str());
  }

  // 5. Alertas do Telegram
  if (riscoMofo && !alertaMofoAtivo) {
    alertaMofoAtivo = true;
    enviarTelegram("🍄 ALERTA MOFO: Umidade em " + String(g_umid, 0) + "%");
  } else if (!riscoMofo) alertaMofoAtivo = false;

  if (g_temp > TEMP_ALERTA_LIMITE && !alertaTempAtivo) {
    alertaTempAtivo = true;
    enviarTelegram("🌡️ ALERTA TEMP: " + String(g_temp, 1) + "°C");
  } else if (g_temp <= TEMP_ALERTA_LIMITE) alertaTempAtivo = false;

  if (millis() - ultimoPollingTelegram > 3000) {
    ultimoPollingTelegram = millis();
    verificarComandosTelegram();
  }

  delay(100);
}

void verificarComandosTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=0";

  if (https.begin(secureClient, url)) {
    if (https.GET() == 200) {
      String payload = https.getString();
      DynamicJsonDocument doc(4096);
      if (!deserializeJson(doc, payload)) {
        JsonArray results = doc["result"].as<JsonArray>();
        for (JsonObject update : results) {
          long updateId = update["update_id"];
          if (updateId > lastUpdateId) lastUpdateId = updateId;

          String texto = update["message"]["text"].as<String>();
          String chatId = String((long)update["message"]["chat"]["id"]);
          texto.trim();

          if (texto == "/relatorio" || texto == "/status") {
            enviarRelatorio(chatId);
          } else if (texto == "/start") {
            enviarTelegramPara(chatId, "🌱 Bot da Estufa SENAI Ativo!\nComandos:\n/relatorio");
          }
        }
      }
    }
    https.end();
  }
}

void enviarRelatorio(String chatId) {
  String status = g_irrigando ? "🟢 Irrigando" : "🔴 Parada";
  String mofo = (g_umid > UMIDADE_MOFO_LIMITE) ? "⚠️ RISCO DE MOFO" : "✅ Normal";

  String msg = "📊 *Relatório da Estufa*\n"
               "🌡️ Temp: " + String(g_temp, 1) + "°C\n" +
               "💧 Umid: " + String(g_umid, 0) + "%\n" +
               "☀️ Luz: " + String(g_luz) + "%\n" +
               "🪣 Água: " + String(g_agua) + "%\n" +
               "🚿 Bomba: " + status + "\n" +
               "🍄 Status: " + mofo;

  enviarTelegramPara(chatId, msg);
}

void enviarTelegram(String mensagem) {
  enviarTelegramPara(String(TELEGRAM_CHAT_ID), mensagem);
}

void enviarTelegramPara(String chatId, String mensagem) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + chatId + "&text=" + urlEncode(mensagem);

  if (https.begin(secureClient, url)) {
    https.GET();
    https.end();
  }
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) encoded += c;
    else {
      char code0 = (c >> 4) & 0xf;
      char code1 = c & 0xf;
      encoded += '%';
      encoded += (code0 > 9 ? code0 - 10 + 'A' : code0 + '0');
      encoded += (code1 > 9 ? code1 - 10 + 'A' : code1 + '0');
    }
  }
  return encoded;
}