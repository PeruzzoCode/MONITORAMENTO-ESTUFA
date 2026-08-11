#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

// ==========================================
// DEFINIÇÃO DE PINOS - ESP32-S3
// ==========================================
#define DHT_PIN          4   // Sensor DHT22
#define DHTTYPE          DHT22
#define LDR_PIN          5   // Sensor de Luz (ADC)
#define POT_PIN          6   // Potenciômetro Nível de Água (ADC)
#define LED_IRRIGA_PIN   7   // LED/Relé Irrigação
#define BUZZER_PIN       15  // Buzzer de Alerta
#define SDA_PIN          8   // Display LCD I2C SDA
#define SCL_PIN          9   // Display LCD I2C SCL

// ==========================================
// CONFIGURAÇÃO DE REDE E SERVIÇOS
// ==========================================
const char* ssid        = "NOME_DA_SUA_REDE_WIFI";
const char* password    = "SENHA_DO_SEU_WIFI";
const char* mqtt_server = "broker.hivemq.com";

const char* TELEGRAM_TOKEN   = "8675430274:AAHI4KF7EOQPyIQfqzSMipgaZQVZz6YfTuM";
const char* TELEGRAM_CHAT_ID = "8801014782";

// Limites e Alertas
const float UMIDADE_MOFO_LIMITE = 85.0;
const float TEMP_ALERTA_LIMITE  = 30.0;

DHT dht(DHT_PIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClient espMqttClient;
PubSubClient clientMQTT(espMqttClient);

// Variáveis de Estado Global
float g_temp = 0.0, g_umid = 0.0;
int   g_luz = 0, g_agua = 0;
bool  g_irrigando = false;

// Controle de Interrupção Remota (Override via Site)
unsigned long tempoUltimoComandoRemoto = 0;
const unsigned long TIMEOUT_REMOTO = 6000; // 6 segundos de tempo de resposta remota

unsigned long tempoUltimoEnvio = 0;
unsigned long ultimoPollingTelegram = 0;
long lastUpdateId = 0;

bool alertaMofoAtivo = false;
bool alertaTempAtivo = false;

// Função de Callback do MQTT (Recebe comandos do Dashboard)
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

    tempoUltimoComandoRemoto = millis();
  }
}

void reconectarMQTT() {
  while (!clientMQTT.connected()) {
    String clientId = "ESP32S3Estufa-" + String(random(0xffff), HEX);
    if (clientMQTT.connect(clientId.c_str())) {
      clientMQTT.subscribe("estufa/senai/comando");
    } else {
      delay(1000);
    }
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

void enviarTelegram(String mensagem) {
  enviarTelegramPara(String(TELEGRAM_CHAT_ID), mensagem);
}

void enviarRelatorio(String chatId) {
  String status = g_irrigando ? "🟢 Irrigando" : "🔴 Parada";
  String mofo = (g_umid > UMIDADE_MOFO_LIMITE) ? "⚠️ RISCO DE MOFO" : "✅ Normal";

  String msg = "📊 *Relatório ESP32-S3 Estufa*\n"
               "🌡️ Temp: " + String(g_temp, 1) + "°C\n" +
               "💧 Umid: " + String(g_umid, 0) + "%\n" +
               "☀️ Luz: " + String(g_luz) + "%\n" +
               "🪣 Água: " + String(g_agua) + "%\n" +
               "🚿 Bomba: " + status + "\n" +
               "🍄 Status: " + mofo;

  enviarTelegramPara(chatId, msg);
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
            enviarTelegramPara(chatId, "🌱 ESP32-S3 Estufa Conectado!\nUse /relatorio para verificar a estufa.");
          }
        }
      }
    }
    https.end();
  }
}

void setup() {
  Serial.begin(115200);

  // Configuração I2C para ESP32-S3
  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ESP32-S3 Estufa");
  lcd.setCursor(0, 1);
  lcd.print("Conectando WiFi");

  dht.begin();
  pinMode(LED_IRRIGA_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  clientMQTT.setServer(mqtt_server, 1883);
  clientMQTT.setCallback(mqttCallback);

  lcd.clear();
  lcd.print("Conectado!");
  delay(1000);
}

void loop() {
  if (!clientMQTT.connected()) {
    reconectarMQTT();
  }
  clientMQTT.loop();

  // Leitura dos Sensores Físicos (Caso não haja comando remoto ativo)
  if (millis() - tempoUltimoComandoRemoto > TIMEOUT_REMOTO) {
    float t_lida = dht.readTemperature();
    float u_lida = dht.readHumidity();
    int pot_val  = analogRead(POT_PIN);
    int ldr_val  = analogRead(LDR_PIN);

    if (!isnan(t_lida)) g_temp = t_lida;
    if (!isnan(u_lida)) g_umid = u_lida;
    
    // Leitura ADC do ESP32-S3 é de 12 bits (0 a 4095)
    g_agua = map(pot_val, 0, 4095, 0, 100);
    g_luz  = map(ldr_val, 0, 4095, 0, 100);
  }

  // Lógica de Atuação
  bool riscoMofo = (g_umid > UMIDADE_MOFO_LIMITE);
  digitalWrite(LED_IRRIGA_PIN, (g_irrigando && !riscoMofo) ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, (g_temp > TEMP_ALERTA_LIMITE) ? HIGH : LOW);

  // Atualização do LCD 16x2
  lcd.setCursor(0, 0);
  lcd.print("T:" + String(g_temp, 1) + "C U:" + String(g_umid, 0) + "%   ");
  lcd.setCursor(0, 1);
  if (riscoMofo) {
    lcd.print("MOFO! Irrig OFF ");
  } else {
    lcd.print("L:" + String(g_luz) + "% Agua:" + String(g_agua) + "%   ");
  }

  // Publicação de Dados via MQTT a cada 2 segundos
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

  // Notificações do Telegram
  if (riscoMofo && !alertaMofoAtivo) {
    alertaMofoAtivo = true;
    enviarTelegram("🍄 ALERTA ESP32-S3: Risco de Mofo! Umidade em " + String(g_umid, 0) + "%");
  } else if (!riscoMofo) alertaMofoAtivo = false;

  if (g_temp > TEMP_ALERTA_LIMITE && !alertaTempAtivo) {
    alertaTempAtivo = true;
    enviarTelegram("🌡️ ALERTA ESP32-S3: Temperatura Elevada! " + String(g_temp, 1) + "°C");
  } else if (g_temp <= TEMP_ALERTA_LIMITE) alertaTempAtivo = false;

  if (millis() - ultimoPollingTelegram > 3000) {
    ultimoPollingTelegram = millis();
    verificarComandosTelegram();
  }

  delay(100);
}