/*************************************************************************************
 *  @file RobustGSM.ino
 *  @author Filipe Ihancis <filipeihancist@gmail.com>
 *  @brief Conexão GSM estável no ESP32 com robustez PDP , DNS etc
 *  @details Modem Bk-A7670 (andglobal)
 *************************************************************************************/

// Bibliotecas ************************************************************************
#include "neotimer.h"
#include <PubSubClient.h>
#include "IPAddress.h"
#include "ESPEncrypt.h"

// Timers ******************************************************************************
Neotimer publishTimer   = Neotimer(60000);        // Timer para publicações MQTT (15 seg)
Neotimer checkConnTimer = Neotimer(20000);        // Timer para checar com. MQTT/GPRS

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
#define MQTT_TOPIC            "ihancis/testeGSM"      // Tópico para publicações MQTT
#define DEVICE_ID             "ihancis"               // ID Base do dispositivo para conexão MQTT
#define KEEP_ALIVE_MQTT       50                      // Keep Alive MQTT
#define SOCKET_TIMEOUT_MQTT   30                      // Timeout MQTT

// Pinagem GSM ******************************************************************************
#define MODEM_TX      17                // pino tx do esp = 17, vai no RX do mod
#define MODEM_RX      16              
#define MODEM_PWRKEY  4                 // Pino Power Key
#define MODEM_SLEEP   18                 // Pino Sleep
#define DTR_SET_SLEEP 0                 // SETAR SLEEP MODEM VIA DTR PIN
#define DTR_SET_WAKE  1                 // SETAR WAKE MODEM VIA DTR PIN

// Variáveis de Controle de Falha ***************************************************************
uint8_t uartFailCount   = 0;            // Indica erros de comunicação UART (Comandos AT)
uint8_t stackFailCount  = 0;            // Indica erros de stack (Registro LTE etc)
int mqttFailCount       = 0;            // Indica quantidade de falhas de MQTT

// Definições GSM ****************************************************************************
#define BAUD_RATE           115200      // Velocidade comunicação UART AT
#define STAB_TIME_GSM       8000        // Tempo de estabilização após hardware boot modem (ms)
#define ENERGY_STAB_GSM     2000        // Tempo de estabilização de energia GSM (ms)
#define PULSE_TIME_PWRKEY   1500        // Tempo do pulso para hardware boot modem
#define SYNC_ATTEMPTS       10          // Tentativas de sincronia de Baud Rate UART
#define UART_CHECK_TIMEOUT  1000        // Timeout p/ confirmação UART (AT)
#define SIM_CHECK_TIMEOUT   10000       // Timeout p/ verificação do chip/SIM
#define MAX_UART_FAILS      3           // Máximo de falhas da UART antes de pulso no GSM
#define MAX_STACK_FAILS     3           // Máximo de falhas de Registro de Rede LTE
#define MAX_DNS_FAIL        3           // Máximo de falhas DNS permitidas


// Instâncias **********************************************************************************
TinyGsm modem(SerialAT);                // Obj. modem, que utiliza SerialAT para com. UART
TinyGsmClient gsmClient(modem);         // Cliente GSM para criar socket/conexão mqtt
PubSubClient mqtt(gsmClient);           // Obj. para manipulação conexão MQTT

// Definições de criptografia *****************************************************************
#define AES_KEY "7cc7f46dd4a82b10b23388c7eda6379b"      // Chave criptográfica AES 128 bits
ESPEncrypt crypto(AES_KEY);                             // Obj, que implementa AES-GCM-128bits



/*********************************************************************************************************************/
void intDelay(uint32_t time)
{
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < time) {
    // Enquanto estiver "bloqueado", mantém ESP vivo
    yield();
  }
}

/*********************************************************************************************************************/
void flushModemUART() 
{
  // Enquanto estiver disponível, realiza leitura do 'lixo'/mensagens antes do próximo comando
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

/*********************************************************************************************************************/
bool sendATAndCheck(const char* cmd, const char* expected_response, uint32_t timeout_ms)
{
  // Comando AT básico
  if (!cmd || cmd[0] == '\0') {
    SerialAT.print("AT\r\n");
  }
  else {
    modem.sendAT(cmd);
  }

  // Aguarda resposta de acordo com timeout selecionado
  return (modem.waitResponse(timeout_ms, expected_response) == 1);
}

/*********************************************************************************************************************/
String ATResponse(const char* cmd, uint32_t timeout = 3000)
{
  // flushModemUART();

  /*
  // Envia comando
  modem.sendAT(cmd);

  String response = "";
  uint32_t start = millis();

  while ((millis() - start) < timeout)
  {
    while (SerialAT.available())
    {
      char c = SerialAT.read();
      response += c;
    }

    // Se já recebeu OK ou ERROR, encerra
    if (response.indexOf("\r\nOK\r\n") != -1 ||
        response.indexOf("\r\nERROR\r\n") != -1)
    {
      break;
    }
    yield();
  }

  response.trim();
  return response;
  */
  modem.sendAT(cmd);
  String res;
  modem.waitResponse(timeout, res);
  res.trim();
  return res;
}

/*********************************************************************************************************************/
void modemDiag() {
  Serial.print("CFUN (RÁDIO): " + ATResponse("+CFUN?"));
  Serial.print("CPIN (SIM): " + ATResponse("+CPIN?"));
  Serial.print("CEREG (REGISTRO): " + ATResponse("+CEREG?"));
  Serial.print("CGREG (REGISTRO): " + ATResponse("+CGREG?"));
  Serial.print("CPSMS (POWER SAVING): " + ATResponse("+CPSMS?"));
  Serial.print("CSCLK (ECONOMIA,CLOCK): " + ATResponse("+CSCLK?"));
}

/*********************************************************************************************************************/
bool modemConnect()
{
  Serial.print("[GSM NET] Waiting Network: ");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(" FAIL");
    return false;
  }
  Serial.println(" Connected");

  Serial.print("[LTE] Connecting APN: ");
  if (!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
    Serial.println(" FAIL");
    return false;
  }
  Serial.println(" Connected");
  return true;
}

/*********************************************************************************************************************/
bool checkUART()
{
  // Teste simples de eco (verificar se está respondendo aos ATs)
  if (sendATAndCheck("", "OK", UART_CHECK_TIMEOUT)) {
    uartFailCount = 0;
    return true;
  }

  // Considera-se que UART falhou dentro do timeout
  uartFailCount++;
  Serial.printf("[UART] Timeout %d/%d: Tentando sincronia.\n", uartFailCount, MAX_UART_FAILS);
  
  // Tenta sincronizar enviando AT repetidamente
  for (int i = 0; i < 3; i++)
  {
    SerialAT.println();         // Envia enter para limpar buffer
    intDelay(100);              // Processamento do enter
    
    if (sendATAndCheck("", "OK", UART_CHECK_TIMEOUT)) {
      Serial.println("[UART] Sincronizado.");
      uartFailCount = 0;
      return true;
    }
  }
  // Verifica se ultrapassou limite de falhas da UART
  if(uartFailCount >= MAX_UART_FAILS) {
    Serial.println("[UART] Comunicação perdida. Reset de Hardware.");
    pwrKeyPulse();
    uartFailCount = 0;
  }
  return false;
}

/*********************************************************************************************************************/
bool checkIP() 
{
  // Obtém endereço de IP atribuído pelo PDP
  String ip = modem.getLocalIP();

  // Verifica se IP é válido (!=0)
  if(ip == "" || ip == "0.0.0.0") {
    Serial.print("[GSM] Invalid IP: " + ip);
    return false;
  }
  return true;
}

/*********************************************************************************************************************/
bool checkSignal()
{
  int csq = modem.getSignalQuality();

  if(csq==99 || csq <=0) {
    Serial.println("[GSM] RF Stack Inválida: CSQ " + String(csq));
    return false;
  }
  return true;
}

/*********************************************************************************************************************/
bool checkSIM()
{
  // Tenta por 10 segundos
  for (uint32_t start = millis(); millis() - start < SIM_CHECK_TIMEOUT; )
  {
    SimStatus status = modem.getSimStatus();
    if (status == SIM_READY) {
      return true;
    }
    // Caso o pino está em LOCK porém não há pino para retornar situação:
    if (status == SIM_LOCKED || status == SIM_ANTITHEFT_LOCKED || status == SIM_LOCKED) {
      if(status == SIM_LOCKED) Serial.println("[GSM] SIM is locked.. (continue)");
      return true;
    }
    intDelay(500); 
  }
  return false;
}

/*********************************************************************************************************************/
bool checkPDPContext()
{
  if(!sendATAndCheck("+CGACT?", ",1", 2000)) 
  {
    Serial.println("[RECOVERY] PDP Deactivated. Reactivating PDP.");

    // Tenta ativar diretamente o contexto PDP 1
    if(!sendATAndCheck("+CGACT=1,1", "OK", 5000)) return false;
  }
  return true;
}

/*********************************************************************************************************************/
bool hardReset()
{
  Serial.println("[GSM] Aplicando Reset (AT&F)");
  sendATAndCheck("&F", "OK", 3000); // Restaura padrões de fábrica na memória
  Serial.println("[GSM] Comandando Reinicialização (CRESET)...");
  
  // Envia o comando sem esperar o OK pela biblioteca para evitar conflito de buffer
  SerialAT.println("AT+CRESET");
  
  // Aguardamos o boot físico (o modem some e volta)
  // Durante esse tempo, a Netlight deve apagar e depois voltar a piscar
  uint32_t startBoot = millis();
  bool bootFinished = false;
  
  Serial.print("[GSM] Aguardando Boot");
  while(millis() - startBoot < 20000) { // Timeout de 20 seg
    Serial.print(".");
    // Tenta mandar um AT simples para ver se a UART voltou
    SerialAT.println("AT");
    if (SerialAT.find("OK")) {
      bootFinished = true;
      break;
    }
    intDelay(1000);
  }

  if (bootFinished) {
    Serial.println("\n[GSM] Modem voltou! Forçando rádio ativo...");
    // IMEDIATAMENTE desativa as economias de energia que podem ter vindo no chip
    sendATAndCheck("+CSCLK=0", "OK", 2000);
    sendATAndCheck("+CEDRXS=0", "OK", 2000);
    sendATAndCheck("+CFUN=1", "OK", 5000);
    return true;
  }
  
  Serial.println("\n[GSM] Erro: Modem não voltou do reset.");
  return false;
}

/*********************************************************************************************************************/
bool validateFullStack()
{
  // 0. Verifica UART & Hardware (mais crítico)
  if(!checkUART()) return false;

  // 1. Verifica Registro na Rede LTE
  if (!modem.isNetworkConnected())
  {
    Serial.println("[STACK] LTE Offline. Aguardando torre...");

    // Recovery: tenta reconectar
    if(!modem.waitForNetwork(15000L))
    {
      stackFailCount++;
      if(stackFailCount >= MAX_STACK_FAILS) {
        // Caso não consiga reconectar, reinicia o rádio
        Serial.println("[RECOVERY] Resetando rádio (CFUN 0->1)");
        sendATAndCheck("+CFUN=0", "OK", 5000);
        intDelay(2000);
        sendATAndCheck("+CFUN=1", "OK", 5000);
        stackFailCount = 0;
      }
      return false;
    }
  }

  // Checa IP e PDP (Camada de dados)
  if(!checkIP()) {
    Serial.print("[STACK] Conectando GPRS novamente: ");
    if(!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
      Serial.println("Falha");
      return false;
    }
    Serial.println("Conectado");
  }

  // Teste de Navegação Real (DNS)
  static uint8_t dnsError = 0;
  if (!sendATAndCheck("+CDNSGIP=\"google.com\"", "+CDNSGIP: 1", 4000))
  {
    dnsError++;
    Serial.printf("[STACK] DNS Warning %d\n", dnsError);

    // Reseta a interface NET caso erro seja persistente
    if (dnsError >= MAX_DNS_FAIL) 
    {
      Serial.println("[RECOVERY] DNS persistente falhou.");
      Serial.print("[RECOVERY] Resetando interface NET: ");
      sendATAndCheck("+NETCLOSE", "OK", 3000);
      intDelay(1000);
      sendATAndCheck("+NETOPEN", "OK", 5000);
      if(modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
        Serial.println("Reconectado.");
      } else {
        Serial.println("Falha");
      }
      dnsError = 0;
      return false;
    }
  } else {
    dnsError = 0;
  }

  stackFailCount = 0; 
  return true;
}

/*********************************************************************************************************************/
void pwrKeyPulse()
{
  Serial.println("[GSM] Pulso de Reset de Hardware (PWRKEY)");

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

/*********************************************************************************************************************/
bool initModem()
{
  // Limpa Serial AT
  flushModemUART();

  // Tempo de estabilização de energia GSM
  intDelay(ENERGY_STAB_GSM);

  Serial.println("[GSM] Inicializando detecção UART");
  Serial.print("[GSM] UART: ");

  // 1. Tenta realizar comunicação com Modem
  bool synced = false;
  for(int i = 0; i < SYNC_ATTEMPTS; i++) {
    if (sendATAndCheck("", "OK", 1000)) {
      Serial.println("Responding (synced)");
      synced = true;
      break;
    }
    intDelay(200);
  }

  if(!synced) 
  {
    Serial.println("Sem resposta. Triggering hardware boot");
    pwrKeyPulse();

    Serial.print("[GSM] UART: ");
    for(int i = 0; i < SYNC_ATTEMPTS; i++) {
      if (sendATAndCheck("", "OK", 1000)) {
        Serial.println("Responding (synced)");
        synced = true;
        break;
      }
      intDelay(200);
    }
  }
  
  if (!synced) {
    Serial.println("ERRO UART");
    return false;
  }

  // Configuração do comportamento GSM:
  //sendATAndCheck("+IPR=115200", "OK", 1000);  // Força BAUD RATE
  sendATAndCheck("E0", "OK", 2000);           // Defino echo = off
  sendATAndCheck("+CMEE=2", "OK", 2000);      // Define erros detalhados
  sendATAndCheck("+CNMP=38", "OK", 2000);     // Define LTE only
  intDelay(10);
  
  // Configurações de recuperação
  if(!sendATAndCheck("+CSCLK=0", "OK", 1000)) {
    Serial.println("[GSM] ERROR CSCLK=0");
  }
  if(!sendATAndCheck("+CEDRXS=0", "OK", 1000)) {
    Serial.println("[GSM] ERROR CEDRXS=0");// Mata o ciclo de 8 segundos
  }
  if(!sendATAndCheck("+CPSMS=0", "OK", 1000)) {
    Serial.println("[GSM] ERROR CPSMS=0");  // Desativa o Power Saving Mode (PSM) se estiver ativo
  }

  // Define modo de funcionalidade máxima
  //if (!sendATAndCheck("+CFUN=1,1", "OK", 10000)) {
  if (!sendATAndCheck("+CFUN=1", "OK", 10000)) {
    Serial.println("[GSM] Erro crítico: rádio não está configurado");
    return false;
  }

  intDelay(5000);

  // Desabilita URCs de fuso horário que podem sujar a UART do ESP32
  sendATAndCheck("+CTZR=0", "OK", 2000);
  intDelay(5);
  sendATAndCheck("+CTZU=1", "OK", 2000);

  if(!checkSIM()) {
    Serial.println("[GSM INIT] Falha ao inicializar modem. Problema no CHIP/SIM.");
    return false;
  }

  intDelay(10);
  modemDiag();

  return true;

  /*
  // Afere a qualidade do sinal
  if(checkSignal()) {
    Serial.println("[GSM] OK: Pronto para uso.\n");
    return true;
  }
  Serial.println("[GSM] FAIL (Aplicando Hard Reset)");
  hardReset();
  return false;
  */
}

/*********************************************************************************************************************/
bool sleepModem()
{
  Serial.print("[GSM] Entrando em Sleep (DTR High): ");

  if(!sendATAndCheck("+CSCLK=1", "OK", 2000)) {
    Serial.println(" CSCLK=1 ERROR.");
    return false;
  }

  // 3. Puxa o pino DTR (MODEM_SLEEP) autorizar o sleep
  digitalWrite(MODEM_SLEEP, DTR_SET_SLEEP);
  intDelay(100);

  Serial.println(" Modem em modo de baixo consumo.");
  Serial.println(ATResponse("+CSCLK?"));
  return true;
}

/*********************************************************************************************************************/
bool wakeModem()
{
  Serial.println("[GSM] Acordando modem (WAKE)");

  // 1. Puxa DTR para acordar a interface serial
  digitalWrite(MODEM_SLEEP, DTR_SET_WAKE);

  intDelay(500);
  flushModemUART();

  if(checkUART())
  {
    Serial.println("[GSM] Modem acordado. Pronto para reconectar.");
    return true;
  } else {
    Serial.println("[GSM] Erro ao acordar");
    return false;
  }
}

/*********************************************************************************************************************/
void mqttConfig() {
  // Configurações base MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);       // Seta Servidor e porta do Broker
  mqtt.setBufferSize(TINY_GSM_RX_BUFFER);       // Define buffer MQTT (importante analisar)
  mqtt.setKeepAlive(KEEP_ALIVE_MQTT);           // Configura keepAlive do socket
  mqtt.setSocketTimeout(SOCKET_TIMEOUT_MQTT);   // Timeout de operações mqtt
}

/*********************************************************************************************************************/
bool mqttConnect()
{
  // Se já está conectado, não realizada nenhuma operação
  if (mqtt.connected()) return true;

  Serial.print("[MQTT] Conectando: ");

  // Cria um ID de cliente randômico
  String CLIENT_ID = String(DEVICE_ID) + "_" + String(random(0xffff), HEX);

  // Realiza conexão com o Broker MQTT
  if (mqtt.connect(CLIENT_ID.c_str())) {
    Serial.println("Sucesso");
    return true;
  }

  // Informa o erro caso não consiga se conectar
  Serial.printf(" Falha (%d)\n", mqtt.state());

  // Verifica falha de KeepAlive ou Timeout
  if(!mqtt.loop()) {
    Serial.println("[MQTT] Dead Socket.");
    mqtt.disconnect();
    return false;
  }
  return true;
}

/*********************************************************************************************************************/
bool publish()
{
  if(!mqtt.connected()) {
    Serial.println("\n[APP] Publicação abortada (MQTT Offline)");
    return false;
  }
  Serial.println("\n[APP] Enviando: Hello World");
  String msg = crypto.encryptString("Hello World");

  if(!mqtt.publish(MQTT_TOPIC, msg.c_str()) ) {
    Serial.println("[APP] Falha no envio\n");
    mqttFailCount++;
    stackFailCount++;
    return false;
  }
  Serial.println("[APP] Dados publicados.\n");
  mqttFailCount = 0;
  return true;
}

/*********************************************************************************************************************/
void connManagement()
{
  // Gerenciamento de conexão (a cada 30 segundos ou se perder MQTT)
  // Realiza validação full stack apenas se MQTT estiver offline
  if(!mqtt.connected())
  {
    if( checkConnTimer.repeat()) 
    {
      // 0: Diagnóstico de Hardware.
      // Caso UART não responda, aciona Pwrpulse e sai do bloco de recovery
      if(!checkUART()) return;

      mqttFailCount++;
      Serial.printf("[LOG] Falha de conexão detectada (%d)\n", mqttFailCount);

      // 1. Reconexão de software MQTT
      if(mqttFailCount <= 2) {
        mqttConnect();
      }
      // 2. Verificação de IP e GPRS
      else if (mqttFailCount <= 4) {
        if(!checkIP()) {
          modem.gprsDisconnect();
          intDelay(2000);
          if(modemConnect()) {    // Realiza conexão com NET e LTE
            mqttConnect();
          }
        } else {
          mqttConnect();    // IP correto -> força o socket
        }
      }
      // 3. Reset de Software do Rádio (CFUN)
      else if (mqttFailCount <= 6) {
        Serial.println("[LOG] Reset suave de rádio (CFUN)");
        sendATAndCheck("+CFUN=0", "OK", 2000);
        intDelay(1000);
        sendATAndCheck("+CFUN=1", "OK", 5000);
        intDelay(2000);
        mqttConnect();
      } 
      // 4. Hardware Reset
      else {
        // Nível 4: Hardware Travado
        Serial.println("[LOG] Falha persistente. Reiniciando Hardware");
        pwrKeyPulse();
        mqttFailCount = 0;
      }
    }
  } else {
    // Sistema saudável
    mqttFailCount = 0;
    mqtt.loop();
  }
}

/*********************************************************************************************************************/
void setup()
{
  // Inicializa Serial
  Serial.begin(BAUD_RATE);

  // Inicializa Serial AT
  SerialAT.begin(BAUD_RATE, SERIAL_8N1, MODEM_RX, MODEM_TX);

  // Configurações de pinagem de controle
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  pinMode(MODEM_SLEEP, OUTPUT);
  digitalWrite(MODEM_SLEEP, DTR_SET_WAKE);

  // 1. LIGANDO MODEM GSM

  Serial.println("\n [SETUP GSM] **********************************");

  // Inicialização do modem GSM
  if(initModem()) 
  {
    Serial.println("[GSM] Waiting 5 seg..");
    intDelay(5000);
    // Conexão Network e LTE
    if (modemConnect()) {
      mqttConfig();           // Configurações MQTT
      mqttConnect();          // Conexão broker MQTT
    }
  }

  Serial.println("[GSM] Desconectando MQTT e colocando Modem em sleep");
  mqtt.disconnect();
  sleepModem();

  // 4. SETANDO TIMERS.
  publishTimer.start();
  checkConnTimer.start();
}

/*********************************************************************************************************************/
void loop()
{
  // Gerenciamento da conexão LTE e MQTT
  //connManagement();

  // Tenta publicação a cada 15 seg
  //if (publishTimer.repeat()) publish();

  if(publishTimer.repeat()) 
  {
    if(wakeModem()) 
    {
      modemDiag();
      intDelay(100);

      if(!modem.isNetworkConnected() || !modem.isGprsConnected()) {
        modemConnect();
      }
      intDelay(50);

      if(mqttConnect()) {
        publish();
        Serial.println("[APP] Desconectando MQTT antes de Modem dormir");
        mqtt.disconnect();
      }
    }
    sleepModem(); // volta a dormir
  }
}
