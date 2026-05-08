#include "neotimer.h"
#include <PubSubClient.h>
#include "IPAddress.h"
#include "ESPEncrypt.h"

// ================= TIMERS =================
Neotimer publishTimer = Neotimer(15000);
Neotimer checkMqtt    = Neotimer(5000);

// ================= MODEM =================
#define TINY_GSM_MODEM_A7670
#define TINY_GSM_RX_BUFFER 1024

#include <TinyGsmClient.h>

#define SerialAT  Serial1

// ================= REDE =================
const char APN[]      = "timbrasil.br";
const char GPRS_USER[] = "tim";
const char GPRS_PASS[] = "tim";

// ================= MQTT =================
#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT   1883
#define MQTT_TOPIC  "Teste"
#define DEVICE_ID   "ihancis"

const char *MQTT_USER = "lite";
const char *MQTT_PASS = "lite123";

// ================= PINOS =================
#define MODEM_TX    17    // pino tx do esp = 17, vai no RX do mod
#define MODEM_RX    16
#define MODEM_PWRKEY 4

// ================= OBJETOS =================
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);
PubSubClient mqtt(gsmClient);

// ================= CRYPTO =================
#define AES_KEY "7cc7f46dd4a82b10b23388c7eda6379b"
ESPEncrypt crypto(AES_KEY);


// =========================================================
void powerOnModem()
{
  Serial.println("\n[GSM] Powering modem...");

  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);

  delay(10000); // tempo para boot completo

  Serial.print("[GSM] Testing AT: ");
  while (!modem.testAT(1000)) {
    Serial.print(".");
  }
  Serial.println(" OK");
}

// =========================================================
void configModem()
{

  // Força LTE + IPv4
  modem.sendAT("+CNMP=38");
  modem.waitResponse();

  modem.sendAT("+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
  modem.waitResponse();

  Serial.print("[GSM] Waiting network...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(" FAIL");
    ESP.restart();
  }
  Serial.println(" OK");

  Serial.print("[GPRS] Connecting...");
  if (!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
    Serial.println(" FAIL");
    ESP.restart();
  }
  Serial.println(" OK");

  Serial.print("[IP] ");
  Serial.println(modem.getLocalIP());

  // ================= TESTE TCP =================
  Serial.println("[TEST] TCP...");

  if (gsmClient.connect("broker.hivemq.com", 1883)) {
      Serial.println("[TEST] TCP OK");
      gsmClient.stop(); // IMPORTANTÍSSIMO liberar o socket
  } else {
      Serial.println("[TEST] TCP FAIL");
  }
}

// =========================================================
bool mqttConnect()
{
  if (mqtt.connected()) return true;

  Serial.println("[MQTT] Connecting...");

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(30);

  String clientId = String(DEVICE_ID) + "_" + String(random(0xffff), HEX);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("[MQTT] Connected");
    return true;
  }

  Serial.print("[MQTT] Failed rc=");
  Serial.println(mqtt.state());
  return false;
}

// =========================================================
bool checkConn()
{
  if (!modem.isNetworkConnected()) {
    Serial.println("[GSM] Reconnecting network...");
    if (!modem.waitForNetwork(30000L)) return false;
  }

  if (!modem.isGprsConnected()) {
    Serial.println("[GPRS] Reconnecting...");
    if (!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) return false;
  }

  if (!mqtt.connected()) {
    return mqttConnect();
  }

  return true;
}

void pwrKeyPulse() 
{
  Serial.println("[GSM] Sending PWRKEY pulse...");
  pinMode(MODEM_PWRKEY, OUTPUT);
  
  // Garante que comece em HIGH (assumindo lógica Active-Low comum nesses módulos)
  digitalWrite(MODEM_PWRKEY, HIGH); 
  delay(500);
  
  // Pulso para ligar: Puxa para LOW por 1.5 segundos
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1500); 
  
  // Retorna para HIGH e aguarda a estabilização do sistema
  digitalWrite(MODEM_PWRKEY, HIGH);
  Serial.println("[GSM] Pulse finished. Waiting for boot...");
  delay(8000); // O A7670 é um computador Linux rodando internamente, ele demora para subir a UART
}

/************************************************************************
 * @brief Esvazia o buffer de recepção (RX) do ESP32.
 ***********************************************************************/
void flushModemUART() {
    while (SerialAT.available()) {
        SerialAT.read();
    }
}

/************************************************************************
 * @brief Envia comando AT e espera resposta específica
 * @param ..
 ***********************************************************************/
bool sendATAndCheck(const char* cmd, const char* expected_response, uint32_t timeout_ms) {
    flushModemUART();
    modem.sendAT(cmd);
    
    // Ao passar const char*, o compilador seleciona a versão de 'match' da TinyGSM
    // e não a versão de 'capture' que causou o erro de compilação.
    return (modem.waitResponse(timeout_ms, expected_response) == 1);
}

/************************************************************************
 * @brief Inicializa o módulo GSM A7670
 * @param ..
 ***********************************************************************/
bool initModem()
{
  // 1. Tenta falar com o modem (caso já esteja ligado)
  if (!sendATAndCheck("", "OK", 1000)) {
    Serial.println("[GSM] No response. Triggering hardware boot...");
    pwrKeyPulse(); // Chama a função de pulso longo
  }

  // 2. Sincronia de baud rate
  bool synced = false;
  for (int i = 0; i < 10; i++) {
    if (sendATAndCheck("", "OK", 1000)) {
      synced = true;
      break;
    }
    delay(500);
  }

  if (!synced) return false;

  // REALIZA CONFIGURAÇÃO DO COMPORTAMENTO DO MÓDULO:

  sendATAndCheck("E0", "OK", 2000);        // Echo Off

  sendATAndCheck("+CMEE=2", "OK", 2000);   // Erros verbais
  if (!sendATAndCheck("+CFUN=1", "OK", 10000)) {
    Serial.println("[GSM] Critical: RF could not start.");
    return false;
  }
  // Desabilita URCs de fuso horário que podem sujar a UART do ESP32
  sendATAndCheck("+CTZR=0", "OK", 2000);
  sendATAndCheck("+CTZU=1", "OK", 2000);

  // ---------------------------------------------------------
  // ETAPA 4: Verificação rigorosa do SIM Card
  // ---------------------------------------------------------
  bool simReady = false;
  
  // O SIM card exige tempo para ser inicializado pela baseband LTE
  for (uint32_t start = millis(); millis() - start < 10000;) 
  {
    flushModemUART();
    
    // Envia AT+CPIN? e aguarda "+CPIN: READY"
    if (sendATAndCheck("+CPIN?", "+CPIN: READY", 1000)) {
        // É preciso limpar o "OK" que vem logo após o "+CPIN: READY"
        modem.waitResponse(); 
        simReady = true;
        break;
    }
    delay(1000); 
  }

  if (!simReady) {
    Serial.println("[GSM INIT] SIM ERROR");
    return false;
  }

  Serial.println("[GSM INIT] ALL OK");
  return true;
}

// =========================================================
void setup()
{
  Serial.begin(115200);

  // 1. LIGANDO MODEM GSM
  Serial.println("[SETUP GSM] ---------------");
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  initModem();
  configModem();

  // 2. CONFIGURANDO CONEXÃO C/ BROKER MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqttConnect();

  // 3. SETANDO TIMERS.
  publishTimer.start();
  checkMqtt.start();
}

// =========================================================
void loop()
{
  if (publishTimer.repeat()) {
    Serial.println("Na teoria é pra publicar agora!");
  }
  mqtt.loop();
}