#include <SPI.h>
#include <Ethernet.h>
#include <esp_now.h>
#include <WiFi.h>
#include <map>
#include <vector>
#include <algorithm>

// ==========================================
// === КОНФИГУРАЦИЯ (ВСЕ НАСТРОЙКИ ЗДЕСЬ) ===
// ==========================================

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(2, 0, 0, 1);
IPAddress subnet(255, 0, 0, 0);
IPAddress gateway(2, 0, 0, 1);
IPAddress dns(2, 0, 0, 1);

#define ARTNET_PORT 6454
#define CS_PIN  5
#define RST_PIN 26

#define LED_R_PIN  14
#define LED_G_PIN  27
#define LED_B_PIN  13

// ==========================================
// === PROTOCOL                           ===
// ==========================================
#ifndef ESPNOW_BROADCAST_CHANNEL
#define ESPNOW_BROADCAST_CHANNEL 1
#endif

enum PacketType : uint8_t {
  PKT_REGISTER   = 0x01,
  PKT_ACK        = 0x02,
  PKT_DMX_CHUNK  = 0x03,
  PKT_PING       = 0x04,
  PKT_PONG       = 0x05,
};

struct RegisterPacket {
  PacketType type = PKT_REGISTER;
  uint16_t   universe;
  char       name[16];
};

struct AckPacket {
  PacketType type = PKT_ACK;
  uint8_t    masterMac[6];
};

struct DmxChunk {
  PacketType type = PKT_DMX_CHUNK;
  uint16_t universe;
  uint16_t offset;
  uint16_t length;
  uint32_t sequence;
  uint8_t  dmx[200];
};

struct PingPacket {
  PacketType type = PKT_PING;
  uint32_t   timestamp;
};

struct PongPacket {
  PacketType type = PKT_PONG;
  uint32_t   timestamp;
  uint16_t   universe;
};

// ==========================================
// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ              ===
// ==========================================

uint8_t prevDmxBuffer[512] = {0};
bool dmxChanged = false;

uint32_t dmxSequence = 0;

struct SlaveInfo {
  uint8_t  mac[6];
  uint16_t universe;
  char     name[16];
  uint32_t lastSeen;
};

std::map<String, SlaveInfo> slaveRegistry;
std::map<uint16_t, std::vector<SlaveInfo>> universeMap;

uint8_t broadcastMac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

EthernetUDP Udp;
uint8_t packetBuffer[530];

uint32_t lastDmxSentAt = 0;

// ==========================================
// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ            ===
// ==========================================

void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R_PIN, r);
  digitalWrite(LED_G_PIN, g);
  digitalWrite(LED_B_PIN, b);
}

String macToStr(const uint8_t* m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

void addOrUpdatePeer(const uint8_t* peerMac) {
  if (!esp_now_is_peer_exist(peerMac)) {
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, peerMac, 6);
    info.channel = ESPNOW_BROADCAST_CHANNEL;
    info.encrypt = false;
    esp_now_add_peer(&info);
  }
}

void removePeer(const uint8_t* peerMac) {
  if (esp_now_is_peer_exist(peerMac)) {
    esp_now_del_peer(peerMac);
  }
}

void sendArtPollReply(IPAddress toIP) {
  uint8_t reply[239] = {0};
  
  // ID
  memcpy(reply, "Art-Net\0", 8);
  
  // OpCode ArtPollReply = 0x2100 (little-endian)
  reply[8] = 0x00;
  reply[9] = 0x21;
  
  // IP адрес ноды
  IPAddress local = Ethernet.localIP();
  reply[10] = local[0];
  reply[11] = local[1];
  reply[12] = local[2];
  reply[13] = local[3];
  
  // Port = 0x1936 (6454) little-endian
  reply[14] = 0x36;
  reply[15] = 0x19;
  
  // VersInfo
  reply[16] = 0x00;
  reply[17] = 0x0E;
  
  // Net, SubNet
  reply[18] = 0x00;  // NetSwitch
  reply[19] = 0x00;  // SubSwitch
  
  // OEM
  reply[20] = 0x00;
  reply[21] = 0x58;
  
  // UbeaVersion, Status1
  reply[22] = 0x00;
  reply[23] = 0x00;  // ← было 0xD0, попробуем 0x00
  
  // EstaMan
  reply[24] = 0x00;
  reply[25] = 0x00;
  
  // ShortName (18 bytes)
  strncpy((char*)&reply[26], "ESP32-ArtNet", 17);
  
  // LongName (64 bytes)
  strncpy((char*)&reply[44], "ESP32 ArtNet Node", 63);
  
  // NodeReport (64 bytes)
  strncpy((char*)&reply[108], "#0001 [0000] OK", 63);
  
  // NumPorts
  reply[172] = 0x00;
  reply[173] = 0x01;  // 1 порт
  
  // PortTypes — output DMX
  reply[174] = 0x80;
  
  // GoodInput
  reply[178] = 0x00;
  
  // GoodOutput
  reply[182] = 0x80;
  
  // SwIn (universe)
  reply[186] = 0x00;
  
  // SwOut (universe)
  reply[190] = 0x00;
  
  // Style = StNode
  reply[200] = 0x00;
  
  // MAC адрес
  uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
  memcpy(&reply[201], mac, 6);
  
  // BindIp
  reply[207] = local[0];
  reply[208] = local[1];
  reply[209] = local[2];
  reply[210] = local[3];

  // BindIndex
  reply[211] = 0x01;
  
  // Status2
  reply[212] = 0x08;

  // Шлём broadcast
  Udp.beginPacket(IPAddress(255, 255, 255, 255), ARTNET_PORT);
  Udp.write(reply, sizeof(reply));
  Udp.endPacket();
  
  // И unicast тоже
  Udp.beginPacket(toIP, ARTNET_PORT);
  Udp.write(reply, sizeof(reply));
  Udp.endPacket();
}

void sendDmxToUniverse(uint16_t universe, uint8_t* data, uint16_t total) {
  if (!universeMap.count(universe)) return;
  auto& slaves = universeMap[universe];
  if (slaves.empty()) return;
  
  // Сравниваем с предыдущим кадром
  if (memcmp(prevDmxBuffer, data, total) == 0) return;  // ← данные не изменились
  memcpy(prevDmxBuffer, data, total);                    // ← сохраняем новый снимок

  dmxSequence++;

  // Выбираем адрес: broadcast если слейвов > 1, иначе unicast
  uint8_t* targetMac = (slaves.size() > 1) ? broadcastMac : slaves[0].mac;

  for (uint16_t offset = 0; offset < total; offset += 200) {
    DmxChunk chunk;
    chunk.type     = PKT_DMX_CHUNK;
    chunk.universe = universe;
    chunk.offset   = offset;
    chunk.sequence = dmxSequence;
    chunk.length   = min((uint16_t)200, (uint16_t)(total - offset));
    memcpy(chunk.dmx, data + offset, chunk.length);

    esp_now_send(targetMac, (uint8_t*)&chunk, sizeof(chunk));

    lastDmxSentAt = millis();
  }
}

void registerSlave(const uint8_t* senderMac, const RegisterPacket* pkt) {
  String key = macToStr(senderMac);
  if (slaveRegistry.count(key)) {
    uint16_t oldUniverse = slaveRegistry[key].universe;
    if (oldUniverse != pkt->universe) {
      auto& vec = universeMap[oldUniverse];
      vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const SlaveInfo& s) {
        return macToStr(s.mac) == key;
      }), vec.end());
    }
  }
  SlaveInfo info;
  memcpy(info.mac, senderMac, 6);
  info.universe = pkt->universe;
  strncpy(info.name, pkt->name, 16);
  info.lastSeen = millis();
  slaveRegistry[key] = info;
  auto& vec = universeMap[pkt->universe];
  bool found = false;
  for (auto& s : vec) {
    if (macToStr(s.mac) == key) { s = info; found = true; break; }
  }
  if (!found) vec.push_back(info);
  addOrUpdatePeer(senderMac);
  Serial.printf("[+] %s '%s' U:%d\n", key.c_str(), pkt->name, pkt->universe);
  AckPacket ack;
  ack.type = PKT_ACK;
  uint8_t myMac[6];
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  memcpy(ack.masterMac, myMac, 6);
  esp_now_send(senderMac, (uint8_t*)&ack, sizeof(ack));
}

void cleanDeadSlaves(uint32_t timeoutMs = 8000) {
  uint32_t now = millis();
  std::vector<String> toRemove;
  for (auto it = slaveRegistry.begin(); it != slaveRegistry.end(); ++it) {
    if (now - it->second.lastSeen > timeoutMs) toRemove.push_back(it->first);
  }
  for (auto& key : toRemove) {
    auto& slave = slaveRegistry[key];
    auto& vec = universeMap[slave.universe];
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const SlaveInfo& s) {
      return macToStr(s.mac) == key;
    }), vec.end());
    removePeer(slave.mac);
    slaveRegistry.erase(key);
    Serial.printf("[-] Lost: %s\n", key.c_str());
  }
}

void onEspNowReceive(const uint8_t* senderMac, const uint8_t* data, int len) {
  if (len < 1) return;
  PacketType type = (PacketType)data[0];
  switch (type) {
    case PKT_REGISTER:
      if (len >= (int)sizeof(RegisterPacket)) {
        RegisterPacket pkt; memcpy(&pkt, data, sizeof(pkt));
        registerSlave(senderMac, &pkt);
      }
      break;
    case PKT_PONG: {
      String key = macToStr(senderMac);
      if (slaveRegistry.count(key)) slaveRegistry[key].lastSeen = millis();
      break;
    }
    default: break;
  }
}

// ==========================================
// === SETUP                              ===
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(RST_PIN, OUTPUT);
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  setLed(1, 0, 0);
  digitalWrite(RST_PIN, LOW); delay(100);
  digitalWrite(RST_PIN, HIGH); delay(200);
  SPI.begin();
  Ethernet.init(CS_PIN);
  Ethernet.begin(mac, ip, dns, gateway, subnet);
  delay(500);
  Serial.print("Art-Net IP: "); Serial.println(Ethernet.localIP());
  Udp.begin(ARTNET_PORT);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW Fail"); return; }
  esp_now_register_recv_cb(onEspNowReceive);
  addOrUpdatePeer(broadcastMac);
  Serial.println("--- MASTER READY ---");
}

uint32_t lastPing = 0;

// ==========================================
// === LOOP                               ===
// ==========================================
void loop() {
  int packetSize = Udp.parsePacket();
  if (packetSize > 0) {
    Udp.read(packetBuffer, sizeof(packetBuffer));
    
    if (packetSize >= 12 && strncmp((char*)packetBuffer, "Art-Net", 7) == 0) {
      uint16_t opcode = packetBuffer[8] | (packetBuffer[9] << 8);
      
      // Получаем IP отправителя для ответа в Unicast
      IPAddress remoteIP = Udp.remoteIP();
      
      if (opcode == 0x2000) {
      Serial.printf("ArtPoll from %s\n", remoteIP.toString().c_str());
      sendArtPollReply(remoteIP);
    } else if (opcode == 0x5000 && packetSize >= 18) {
      uint16_t incomingUniverse = packetBuffer[14] | (packetBuffer[15] << 8);
      uint16_t dmxLen = (packetBuffer[16] << 8) | packetBuffer[17];
      sendDmxToUniverse(incomingUniverse, packetBuffer + 18, dmxLen);
    } else {
      Serial.printf("Unknown opcode: 0x%04X from %s\n", opcode, remoteIP.toString().c_str());
    }
    }
  }

  uint32_t now = millis();
  if (now - lastPing > 2000) {
    PingPacket ping;
    ping.type = PKT_PING;
    ping.timestamp = now;
    for (auto it = slaveRegistry.begin(); it != slaveRegistry.end(); ++it) {
      esp_now_send(it->second.mac, (uint8_t*)&ping, sizeof(ping));
    }
    cleanDeadSlaves();
    lastPing = now;
  }

  // LED статус мастера
  static uint32_t lastLedUpdate = 0;
  uint32_t nowLed = millis();
  if (nowLed - lastLedUpdate >= 50) {
      lastLedUpdate = nowLed;

      bool hasSlaves = !slaveRegistry.empty();
      bool dataSent  = (nowLed - lastDmxSentAt < 500);
      bool blink     = (nowLed / 250) % 2;

      if (!hasSlaves) {
          setLed(1, 0, 0);              // красный — нет слейвов
      } else if (!dataSent) {
          setLed(0, 0, 1);              // синий — слейвы есть, данных нет
      } else {
          setLed(1, 1, blink);          // желтый мигает — идёт передача
      }
  }
}