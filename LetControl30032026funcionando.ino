#include <WiFi.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_bt.h"

// ===================== CONFIGURAÇÕES - ALTERE AQUI =====================
#define SENSOR_PIN        2
#define DEVICE_ID         "dispositivo_01"
#define FIREBASE_HOST     "letcontrol-database-default-rtdb.firebaseio.com"
#define FATOR_SENSOR      7.5
#define INTERVALO_CALCULO 1000
#define INTERVALO_ENVIO   5000
#define USER_ID           "0KhRCEJoRYWGPLIloEMIW0eTh4I2"

// ===================== OBJETOS =====================
BluetoothSerial bluetooth;
Preferences preferencias;

// ===================== CREDENCIAIS =====================
String rede         = "";
String senha        = "";
String user         = "";
String incomingData = "";

// ===================== SENSOR =====================
volatile int pulsos   = 0;
float litrosPorMinuto = 0.0;
float totalLitros     = 0.0;
bool  aguaFluindo     = false;

// ===================== TEMPO =====================
unsigned long ultimoCalculo = 0;
unsigned long ultimoEnvio   = 0;

// ===================== PROTÓTIPOS =====================
void processarDados(String data);
void conectarWifi();

// ================================================================
//  BLUETOOTH
// ================================================================

String extrairValor(String data, String prefixo) {
  int inicio = data.indexOf(prefixo);
  if (inicio == -1) return "";
  inicio += prefixo.length();
  int fim = data.indexOf(";", inicio);
  if (fim == -1) fim = data.length();
  return data.substring(inicio, fim);
}

void processarDados(String data) {
  data.trim();
  Serial.println("Recebido: " + data);

  if (data.indexOf("rede=") == -1 || data.indexOf("senha=") == -1 || data.indexOf("user=") == -1) {
    bluetooth.println("FORMATO_INVALIDO");
    return;
  }

  rede  = extrairValor(data, "rede=");
  senha = extrairValor(data, "senha=");
  user  = extrairValor(data, "user=");

  if (rede == "" || senha == "" || user == "") {
    bluetooth.println("CAMPO_VAZIO");
    return;
  }

  preferencias.begin("cred", false);
  preferencias.putString("rede",  rede);
  preferencias.putString("senha", senha);
  preferencias.putString("user",  user);
  preferencias.end();

  bluetooth.println("CREDENCIAIS_SALVAS");
  conectarWifi();
}

void verificarBluetooth() {
  while (bluetooth.available()) {
    char c = bluetooth.read();
    if (c == '\n') {
      processarDados(incomingData);
      incomingData = "";
    } else {
      incomingData += c;
    }
  }
}

// ================================================================
//  WI-FI
// ================================================================

void sincronizarNTP() {
  Serial.println("Sincronizando horario NTP...");
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int tentativas = 0;
  while (!getLocalTime(&timeinfo) && tentativas < 10) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nHorario sincronizado!");
  } else {
    Serial.println("\nFalha ao sincronizar horario.");
  }
}

void conectarWifi() {
  WiFi.begin(rede.c_str(), senha.c_str());
  Serial.print("Conectando ao Wi-Fi");

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.isConnected()) {
    Serial.println("\nWi-Fi conectado! IP: " + WiFi.localIP().toString());
    bluetooth.println("WIFI_CONECTADO:" + WiFi.localIP().toString());
    delay(1000); // Aguarda app receber a mensagem

    // Desliga Bluetooth para liberar RAM para o SSL
    bluetooth.end();
    btStop();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    Serial.println("Bluetooth desligado!");
    Serial.println("Heap livre: " + String(ESP.getFreeHeap()) + " bytes");

    sincronizarNTP();
  } else {
    Serial.println("\nFalha ao conectar.");
    bluetooth.println("WIFI_ERRO");
  }
}

// ================================================================
//  FIREBASE
// ================================================================

void enviarParaFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  String ano = "2026", mes = "03", dia = "30";

  if (getLocalTime(&timeinfo)) {
    char bufAno[5], bufMes[3], bufDia[3];
    strftime(bufAno, sizeof(bufAno), "%Y", &timeinfo);
    strftime(bufMes, sizeof(bufMes), "%m", &timeinfo);
    strftime(bufDia, sizeof(bufDia), "%d", &timeinfo);
    ano = String(bufAno);
    mes = String(bufMes);
    dia = String(bufDia);
  }

  String url = "https://" + String(FIREBASE_HOST) +
               "/daily/" + String(USER_ID) +
               "/" + ano + "/" + mes + "/" + dia +
               ".json";

  Serial.println("URL: " + url);
  Serial.println("Heap antes do SSL: " + String(ESP.getFreeHeap()) + " bytes");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);

  bool iniciou = http.begin(client, url);
  Serial.println("HTTP begin: " + String(iniciou ? "ok" : "falhou"));
  if (!iniciou) return;

  http.addHeader("Content-Type", "application/json");

  String json = String(totalLitros, 3);
  int httpCode = http.PUT(json);
  Serial.println("HTTP code: " + String(httpCode));

  if (httpCode == 200) {
    Serial.println("Firebase atualizado! Total: " + String(totalLitros, 3) + "L");
  } else if (httpCode > 0) {
    Serial.println("Resposta: " + http.getString());
  } else {
    Serial.println("Erro: " + http.errorToString(httpCode));
  }

  http.end();
}

// ================================================================
//  SENSOR
// ================================================================

void IRAM_ATTR contarPulso() {
  pulsos++;
}

void calcularFluxo() {
  detachInterrupt(digitalPinToInterrupt(SENSOR_PIN));
  int pulsosCopia = pulsos;
  pulsos = 0;
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), contarPulso, FALLING);

  litrosPorMinuto = (pulsosCopia / FATOR_SENSOR) * 60.0;
  totalLitros    += litrosPorMinuto / 60.0;
  aguaFluindo     = (litrosPorMinuto > 0.1);

  Serial.printf("%.2f L/min | Total: %.3f L\n", litrosPorMinuto, totalLitros);
}

// ================================================================
//  SETUP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Iniciando...");

  bluetooth.begin("AquaMonitor");
  Serial.println("Bluetooth: AquaMonitor");

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), contarPulso, FALLING);

  preferencias.begin("cred", true);
  rede  = preferencias.getString("rede",  "");
  senha = preferencias.getString("senha", "");
  user  = preferencias.getString("user",  "");
  preferencias.end();

  if (rede != "" && senha != "") {
    Serial.println("Credenciais salvas! Conectando...");
    conectarWifi();
  } else {
    Serial.println("Aguardando credenciais via Bluetooth...");
    bluetooth.println("AGUARDANDO_CREDENCIAIS");
    while (rede == "" || senha == "" || user == "") {
      verificarBluetooth();
    }
  }
}

// ================================================================
//  LOOP
// ================================================================

void loop() {
  verificarBluetooth();

  if (millis() - ultimoCalculo >= INTERVALO_CALCULO) {
    calcularFluxo();
    ultimoCalculo = millis();
  }

  if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    if (WiFi.isConnected()) {
      enviarParaFirebase();
    } else {
      Serial.println("Sem Wi-Fi, reconectando...");
      WiFi.reconnect();
    }
    ultimoEnvio = millis();
  }
}