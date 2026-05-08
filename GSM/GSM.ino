/*************************************************************************************
 *  @file GSM.ino
 *  @author Filipe Ihancis <filipeihancist@gmail.com>
 *  @brief Conexão GSM estável no ESP32
 *  @details Modem Bk-A7670 (andglobal)
 *  @warning
 *************************************************************************************/

// Bibliotecas ************************************************************************
#include "neotimer.h"
#include <PubSubClient.h>
#include "IPAddress.h"
#include "ESPEncrypt.h"

// Timers ******************************************************************************
Neotimer publishTimer   = Neotimer(15000);        // Timer para publicações MQTT (15 seg)
Neotimer checkConnTimer = Neotimer(10000);         // Timer para checar com. MQTT/GPRS

// Modem GSM ****************************************************************************
#define TINY_GSM_MODEM_A7670                // Modelo Modem GSM
#define SIM_TIM                             // Definição do modelo do chip/SIM (TIM/VIVO)
#define TINY_GSM_RX_BUFFER 1024             // Buffer interno GSM (uart, process etc)
#include <TinyGsmClient.h>                  // Biblioteca base da RTOS GSM
#define SerialAT  Serial1                   // Define Serial para comandos AT

// Definições da Rede LTE ****************************************************************
#ifdef SIM_TIM
const char APN[]        = "timbrasil.br";
const char GPRS_USER[]  = "tim";
const char GPRS_PASS[]  = "tim";
#endif
#ifdef SIM_VIVO
const char APN[]        = "zap.vivo.com.br";
const char GPRS_USER[]  = "vivo";
const char GPRS_PASS[]  = "vivo";
#endif

// Definições MQTT *************************************************************************
#define MQTT_SERVER           "broker.hivemq.com"     // Host Broker Público (pré DNS)
#define MQTT_PORT             1883                    // Porta do Broker (1883 - porta padrão)
#define MQTT_TOPIC            "Teste"                 // Tópico para publicações MQTT
#define DEVICE_ID             "ihancis"               // ID Base do dispositivo para conexão MQTT
#define KEEP_ALIVE_MQTT       20
#define SOCKET_TIMEOUT_MQTT   40

// Pinagem GSM ******************************************************************************
#define MODEM_TX      17                // pino tx do esp = 17, vai no RX do mod
#define MODEM_RX      16
#define MODEM_PWRKEY  4                 // Pino Power Key
//#define MODEM_SLEEP   2                 // Pino Sleep

// Definições GSM ****************************************************************************
#define BAUD_RATE           115200      // Velocidade comunicação UART AT
#define STAB_TIME_GSM       8000        // Tempo de estabilização após hardware boot modem
#define PULSE_TIME_PWRKEY   1500        // Tempo do pulso para hardware boot modem
#define SYNC_ATTEMPTS       10          // Tentativas de sincronia de Baud Rate UART

// Instância dos objetos **********************************************************************
TinyGsm modem(SerialAT);                // Obj. modem, que utiliza SerialAT para com. UART
TinyGsmClient gsmClient(modem);         // Cliente GSM para criar socket/conexão mqtt
PubSubClient mqtt(gsmClient);           // Obj. para manipulação conexão MQTT

// Definições de criptografia *****************************************************************
#define AES_KEY "7cc7f46dd4a82b10b23388c7eda6379b"      // Chave criptográfica AES 128 bits
ESPEncrypt crypto(AES_KEY);                             // Obj, que implementa AES-GCM-128bits



/**
 * @brief Implementa delay não bloqueante
 */
void intDelay(uint32_t time)
{
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < time) {
    // Enquanto estiver "bloqueado", mantém ESP vivo
    yield();
  }
}

/**
 * @brief Esvazia o buffer de recepção (RX) UART (AT) do ESP32.
 */
void flushModemUART() {
  // Enquanto estiver disponível, realiza leitura do 'lixo'/mensagens antes do próximo comando
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

/**
 * @brief Inicializa NET e GPRS
 */
bool modemConnect()
{
  /*
  // Força contexto PDP
  modem.sendAT("+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
  modem.waitResponse();
  */

  Serial.print("[GSM NET] Waiting Network: ");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(" FAIL");
    return false;
  }
  Serial.println(" OK");

  Serial.print("[GPRS] Connecting APN: ");
  if (!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
    Serial.println(" FAIL");
    return false;
  }
  Serial.println(" OK");
  checkIP();
  return true;
}

/**
 * @brief Exibe valor do IP atribuído ao PDP
 */
void checkIP() {
  Serial.print("[GSM] IP: ");
  Serial.println(modem.getLocalIP());
}

/**
 * @brief Testa TCP
 */
bool checkTCP()
{
  Serial.print("[TCP] Testing TCP: ");
  if (gsmClient.connect("broker.hivemq.com", 1883)) {
      Serial.println("OK");
      gsmClient.stop(); // IMPORTANTÍSSIMO liberar o socket
      return true;
  } else {
      Serial.println("FAIL");
      return false;
  }
}

/**
 * @brief Envia comando AT e aguarda resposta específica dentro de um timeout
 */
bool sendATAndCheck(const char* cmd, const char* expected_response, uint32_t timeout_ms)
{
  // Antes de executar comando AT, limpa a UART para realizar leitura correta
  flushModemUART();

  // Envia comando AT ao Modem
  modem.sendAT(cmd);

  // Aguarda resposta de acordo com timeout selecionado
  return (modem.waitResponse(timeout_ms, expected_response) == 1);
}

/**
 * @brief Hardware boot via pino PWRKEY
 */
void pwrKeyPulse()
{
  Serial.println("[GSM] Pulso PWRKEY");
  pinMode(MODEM_PWRKEY, OUTPUT);

  digitalWrite(MODEM_PWRKEY, HIGH); // Garante que comece em HIGH (assumindo lógica Active-Low)
  intDelay(500);
  digitalWrite(MODEM_PWRKEY, LOW);
  intDelay(PULSE_TIME_PWRKEY);      // Pulso para ligar: Puxa para LOW por 1.5 segundos

  // Retorna para HIGH e aguarda a estabilização do sistema
  digitalWrite(MODEM_PWRKEY, HIGH);

  Serial.println("[GSM] Pulse finished. Waiting for boot...");

  // Tempo de boot
  intDelay(STAB_TIME_GSM);
}

/**
 * @brief Inicializa módulo GSM e configurações padrão
 */
bool initModem()
{
  // Tempo de estabilização de energia GSM
  intDelay(2000);

  Serial.println("[GSM] " + modem.getModemInfo());

  // 1. Tenta realizar comunicação com Modem
  if (!sendATAndCheck("", "OK", 1000)) {
    Serial.println("[GSM] Dont response. Triggering hardware boot");
    pwrKeyPulse();
  }

  // 2. Realiza Sincronia de BAUD RATE (importante para validação AT)
  bool synced = false;
  for (int i = 0; i < SYNC_ATTEMPTS; i++) {
    if (sendATAndCheck("", "OK", 1000)) {
      Serial.println("[GSM] Responding (synced)");
      synced = true;
      break;
    }
    intDelay(500);
  }
  if (!synced) {
    Serial.println("[GSM] Error: BAUD RATE UART Dont synced.");
    return false;
  }

  // Configuração do comportamento GSM:
  sendATAndCheck("E0", "OK", 2000);         // Defino echo = off
  sendATAndCheck("+CMEE=2", "OK", 2000);    // Define erros detalhados
  sendATAndCheck("+CNMP=38", "OK", 2000);   // Define LTE only

  // Define modo de funcionalidade máxima
  if (!sendATAndCheck("+CFUN=1", "OK", 10000)) {
    Serial.println("[GSM] Erro crítico: rádio não está configurado");
    return false;
  }

  // Desabilita URCs de fuso horário que podem sujar a UART do ESP32
  sendATAndCheck("+CTZR=0", "OK", 2000);
  sendATAndCheck("+CTZU=1", "OK", 2000);

  // ---------------------------------------------------------
  // ETAPA 4: Verificação rigorosa do cartão SIM
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
    intDelay(1000);
  }

  if (!simReady) {
    Serial.println("[GSM INIT] SIM ERROR");
    return false;
  }

  Serial.println("[GSM INIT] All OK");
  return true;
}

/**
 * @brief Realiza conexão MQTT
 */
bool mqttConnect()
{
  // Se já está conectado, não realizada nenhuma operação
  if (mqtt.connected()) return true;

  Serial.print("[MQTT] Connecting: ");

  // Configurações base MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);       // Seta Servidor e porta do Broker
  mqtt.setBufferSize(TINY_GSM_RX_BUFFER);       // Define buffer MQTT (importante analisar)
  mqtt.setKeepAlive(KEEP_ALIVE_MQTT);           // Configura keepAlive do socket
  mqtt.setSocketTimeout(SOCKET_TIMEOUT_MQTT);   // Timeout de operações mqtt

  // Cria um ID de cliente randômico
  String CLIENT_ID = String(DEVICE_ID) + "_" + String(random(0xffff), HEX);

  // Realiza conexão com o Broker MQTT
  if (mqtt.connect(CLIENT_ID.c_str())) {
    Serial.println("Connected");
    return true;
  }

  // Informa o erro caso não consiga se conectar
  Serial.print(" Failed "); Serial.println(mqtt.state());
  return false;
}

/**
 * @brief Setup / Inicialização do programa
 */
void setup()
{
  // Inicializa Serial
  Serial.begin(BAUD_RATE);
  SerialAT.begin(BAUD_RATE, SERIAL_8N1, MODEM_RX, MODEM_TX);

  // 1. LIGANDO MODEM GSM

  Serial.println("[SETUP GSM] ---------------");

  if(initModem()) {
    if (modemConnect()) {
      mqttConnect();
    }
  }

  // 4. SETANDO TIMERS.
  publishTimer.start();
  checkConnTimer.start();
}

/**
 * @brief Loop/task única infinita
 */
void loop()
{
  // Mantém MQTT vivo
  if(mqtt.connected()) mqtt.loop();

  if (publishTimer.repeat()) {
    Serial.println("[MQTT] Publicando dados");
    // vamos implementar ainda (fase de testes, apenas!)
  }

  if(checkConnTimer.repeat()) {

    if(!modem.isNetworkConnected() || !mqtt.connected()) {
      Serial.println("[WATCHDOG NETWORK] Conexão perdida");
      if(modemConnect()) {
        mqttConnect();
      }
    }

  }

}
