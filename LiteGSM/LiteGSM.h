#ifndef LITEGSM_H
#define LITEGSM_H


#include <Arduino.h>
#define TINY_GSM_MODEM_A7670  // Definido antes do include
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include "neotimer.h"
#include "ESPEncrypt.h"

#define MODEM_TX      17
#define MODEM_RX      16
#define MODEM_PWRKEY  4
#define MODEM_SLEEP   18

class LiteGSM 
{
public:

    // Construtor
    LiteGSM(HardwareSerial& serial, const char* aesKey);

    /**
     * @brief
     */
    bool begin(const char* apn, const char* user, const char* pass);

    /**
     * @brief 
     */
    void loop();

    /**
     * @brief
     */
    bool publish(const char* topic, const char* payload);

    /**
     * @brief
     */
    bool wake();

    /**
     * @brief
     */
    bool sleep();

    /**
     * @brief
     */
    bool isConnected();

private:

    // Instâncias internas
    HardwareSerial& _serialAT;
    TinyGsm _modem;
    TinyGsmClient _gsmClient;
    PubSubClient _mqtt;
    ESPEncrypt _crypto; 

    // Configurações de rede
    const char* _apn;
    const char* _gprsUser;
    const char* _gprsPass;

    Neotimer _checkConnTimer;
    uint8_t _uartFailCount = 0;
    int _mqttFailCount = 0;

    void pwrKeyPulse();

    bool checkUART();

    bool modemConnect();

    bool mqttConnect();

    void connManagement();

    bool sendATAndCheck(const char* cmd, const char* expected, uint32_t timeout);

    void flushUART();

//
};

#endif