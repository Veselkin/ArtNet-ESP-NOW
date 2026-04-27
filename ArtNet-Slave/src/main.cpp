#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "driver/uart.h"

// ==========================================
// === PROTOCOL.H (Структуры данных)      ===
// ==========================================
#ifndef ESPNOW_BROADCAST_CHANNEL
#define ESPNOW_BROADCAST_CHANNEL 1
#endif

enum PacketType : uint8_t {
    PKT_REGISTER = 0x01,
    PKT_ACK = 0x02,
    PKT_DMX_CHUNK = 0x03,
    PKT_PING = 0x04,
    PKT_PONG = 0x05,
};

struct RegisterPacket {
    PacketType type = PKT_REGISTER;
    uint16_t universe;
    char name[16];
};

struct AckPacket {
    PacketType type = PKT_ACK;
    uint8_t masterMac[6];
};

struct DmxChunk {
    PacketType type = PKT_DMX_CHUNK;
    uint16_t universe;
    uint16_t offset;
    uint16_t length;
    uint32_t sequence;
    uint8_t dmx[200];
};

struct PingPacket {
    PacketType type = PKT_PING;
    uint32_t timestamp;
};

struct PongPacket {
    PacketType type = PKT_PONG;
    uint32_t timestamp;
    uint16_t universe;
};

// ==========================================
// === Настройки                          ===
// ==========================================
#define DMX_TX_PIN 17
#define DEFAULT_UNIVERSE 0

// === Пины RGB ===
#define LED_R_PIN 25
#define LED_G_PIN 26
#define LED_B_PIN 27

// ==========================================
// === Глобальные переменные              ===
// ==========================================
char myName[16] = "";
uint16_t myUniverse = DEFAULT_UNIVERSE;
uint8_t masterMac[6] = {0};
bool registered = false;
uint32_t lastPingReceived = 0;
uint32_t lastRegRetry = 0;

uint8_t dmxBuffer[513] = {0};  // байт 0 = start code (0x00), байты 1-512 = каналы
uint32_t lastDataAt = 0;
uint32_t lastSequence = 0;

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Preferences prefs;

#define MASTER_TIMEOUT 10000

// ==========================================
// === Пины RGB                           ===
// Инвертированная логика для общего анода
// ==========================================
void setLed(bool r, bool g, bool b) {
    digitalWrite(LED_R_PIN, r);
    digitalWrite(LED_G_PIN, g);
    digitalWrite(LED_B_PIN, b);
}

// ==========================================
// === DMX через сырой UART               ===
// ==========================================
void dmxUartInit() {
    Serial1.begin(250000, SERIAL_8N2, -1, DMX_TX_PIN);
    Serial.println("DMX UART ready (raw mode)");
}

void sendDmxRaw() {
    // 1. DMX Break - держим линию LOW 92мкс
    uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);
    delayMicroseconds(92);

    // 2. MAB (Mark After Break) - линия HIGH 12мкс
    uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);
    delayMicroseconds(12);

    // 3. Start code (0x00) + 512 каналов
    Serial1.write(dmxBuffer, 513);
    Serial1.flush();
}

// ==========================================
// === NVS                                ===
// ==========================================
void saveUniverse(uint16_t u) {
    prefs.begin("dmx", false);
    prefs.putUShort("universe", u);
    prefs.end();
}

uint16_t loadUniverse() {
    prefs.begin("dmx", true);
    uint16_t u = prefs.getUShort("universe", DEFAULT_UNIVERSE);
    prefs.end();
    return u;
}

// ==========================================
// === Регистрация                        ===
// ==========================================
void sendRegister() {
    RegisterPacket pkt;
    pkt.type = PKT_REGISTER;
    pkt.universe = myUniverse;
    strncpy(pkt.name, myName, 16);

    uint8_t* target = registered ? masterMac : broadcastMac;
    esp_err_t result = esp_now_send(target, (uint8_t*)&pkt, sizeof(pkt));

    if (result == ESP_OK) {
        Serial.printf(">> Sent Register req (Uni: %d)\n", myUniverse);
    } else {
        Serial.println(">> Send Reg failed");
    }
}

// ==========================================
// === Применить DMX                      ===
// ==========================================
void applyDmx() {
    sendDmxRaw();
    // Serial.printf("DMX sent: ch1=%d ch2=%d ch3=%d\n",
    //   dmxBuffer[1], dmxBuffer[2], dmxBuffer[3]);
}

// ==========================================
// === ESP-NOW коллбек                    ===
// ==========================================
void onReceive(const uint8_t* senderMac, const uint8_t* data, int len) {
    if (len < 1) return;
    PacketType type = (PacketType)data[0];

    switch (type) {
        case PKT_ACK: {
            if (len < (int)sizeof(AckPacket)) return;
            AckPacket ack;
            memcpy(&ack, data, sizeof(ack));
            memcpy(masterMac, ack.masterMac, 6);

            registered = true;
            lastPingReceived = millis();
            lastSequence = 0;
            lastDataAt = 0;

            if (!esp_now_is_peer_exist(masterMac)) {
                esp_now_peer_info_t pi = {};
                memcpy(pi.peer_addr, masterMac, 6);
                pi.channel = ESPNOW_BROADCAST_CHANNEL;
                pi.encrypt = false;
                esp_now_add_peer(&pi);
            }
            Serial.println("!!! Registered with MASTER !!!");
            break;
        }

        case PKT_DMX_CHUNK: {
            if (len < (int)sizeof(DmxChunk)) return;
            DmxChunk chunk;
            memcpy(&chunk, data, sizeof(chunk));
            lastDataAt = millis();

            if (chunk.universe != myUniverse) return;
            if (chunk.offset + chunk.length > 512) return;
            if (lastSequence > 0 && chunk.sequence < lastSequence && (lastSequence - chunk.sequence) < 10000) return;

            lastSequence = chunk.sequence;
            memcpy(dmxBuffer + 1 + chunk.offset, chunk.dmx, chunk.length);
            lastDataAt = millis();
            break;
        }

        case PKT_PING: {
            lastPingReceived = millis();
            if (!registered) return;

            PingPacket ping;
            memcpy(&ping, data, sizeof(ping));

            PongPacket pong;
            pong.type = PKT_PONG;
            pong.timestamp = ping.timestamp;
            pong.universe = myUniverse;

            esp_now_send(masterMac, (uint8_t*)&pong, sizeof(pong));
            break;
        }

        default: break;
    }
}

// ==========================================
// === Смена universe через Serial        ===
// ==========================================
void changeUniverse(uint16_t newUniverse) {
    myUniverse = newUniverse;
    saveUniverse(myUniverse);
    registered = false;
    memset(masterMac, 0, 6);
    lastRegRetry = 0;
    Serial.printf("\n*** Universe changed to %d ***\n", myUniverse);
}

// ==========================================
// === SETUP                              ===
// ==========================================
void setup() {
    Serial.begin(115200);

    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
    setLed(1, 0, 0);

    myUniverse = loadUniverse();
    Serial.printf("Loaded universe: %d\n", myUniverse);

    dmxUartInit();

    // Отправляем нулевой пакет сразу
    memset(dmxBuffer, 0, sizeof(dmxBuffer));
    sendDmxRaw();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_BROADCAST_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(myName, sizeof(myName), "%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    Serial.print("My MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.print("My Name: ");
    Serial.println(myName);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed!");
        ESP.restart();
    }
    esp_now_register_recv_cb(onReceive);

    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, broadcastMac, 6);
    pi.channel = ESPNOW_BROADCAST_CHANNEL;
    pi.encrypt = false;
    esp_now_add_peer(&pi);

    lastPingReceived = millis();
    Serial.println("Setup done. Waiting for master...");
}

// ==========================================
// === LOOP                               ===
// ==========================================
void loop() {
    uint32_t now = millis();

    // Watchdog: мастер пропал
    if (registered && now - lastPingReceived > MASTER_TIMEOUT) {
        Serial.println("Master lost (timeout). Re-registering...");
        registered = false;
        memset(masterMac, 0, 6);
    }

    // Регистрация
    if (!registered && now - lastRegRetry > 3000) {
        sendRegister();
        lastRegRetry = now;
    }

    // DMX шлём непрерывно ~44 раза в секунду
    static uint32_t lastDmx = 0;

    if (now - lastDmx >= 23) {
        applyDmx();
        lastDmx = now;
    }

    // LED обновление
    static uint32_t lastLedUpdate = 0;
    if (now - lastLedUpdate >= 50) {
        lastLedUpdate = now;

        if (!registered) {
            setLed(1, 0, 0);
        } else {
            bool dataRecent = (now - lastDataAt < 500);
            bool blink = (now / 250) % 2;
            setLed(0, 1, dataRecent ? blink : 0);
        }
    }

    // Serial команды (смена universe)
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            changeUniverse((uint16_t)input.toInt());
        }
    }
}
