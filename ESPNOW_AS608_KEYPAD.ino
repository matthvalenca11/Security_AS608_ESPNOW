#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_SSD1306.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <fpm.h>
#include "FS.h"
#include <Keypad.h>

#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_RESET);
HardwareSerial as608Serial(1);
FPM finger(&as608Serial);

#define BUTTON_VERIFY 27
#define COMM_LED 26

unsigned long lastCommTime = 0;
const unsigned long COMM_TIMEOUT = 5000;

uint8_t peerMac[] = { 0xA4, 0xE5, 0x7C, 0x47, 0x39, 0x80 };

typedef struct {
  uint16_t size;
  uint8_t data[768];
  uint8_t id;
  uint8_t type; // 0 = template, 1 = ack
} FingerTemplatePacket;

int remoteTemplateCounter = 0;

// === CONFIGURAÇÃO DO TECLADO 4x4 ===
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {32, 33, 25, 12};
byte colPins[COLS] = {13, 15, 5, 4};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const int SOMA_TOKEN = 51;
bool tokenLiberado = false;

void setup() {
  Serial.begin(115200);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  as608Serial.begin(57600, SERIAL_8N1, 16, 17);
  displayMessage("Init sensor...");
  if (finger.begin()) displayMessage("Sensor OK!");
  else {
    displayMessage("Sensor fail");
    while (1);
  }

  finger.setSecurityLevel(FPMSecurityLevel::FRR_2);
  finger.setPacketLength(FPMPacketLength::PLEN_128);

  if (!SPIFFS.begin(true)) {
    displayMessage("SPIFFS failed!");
    delay(2000);
  }

  pinMode(BUTTON_VERIFY, INPUT_PULLUP);
  pinMode(COMM_LED, OUTPUT);
  digitalWrite(COMM_LED, LOW);
  lastCommTime = millis();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.print("ESP32 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ao iniciar ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(peerMac)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Erro ao adicionar peer ESP-NOW");
    }
  }

  Serial.println("Digite 'delete' no monitor para apagar tudo.");
}

bool verificaSomaToken(const String& token) {
  int soma = 0;
  for (char c : token) {
    if (c >= '0' && c <= '9') soma += (c - '0');
    else return false;
  }
  return soma == SOMA_TOKEN;
}

void loop() {
  static String tokenDigitado = "";
  char key = keypad.getKey();

  if (key) {
    Serial.print("Tecla: ");
    Serial.println(key);

    if (key == '#') {
      if (verificaSomaToken(tokenDigitado)) {
        Serial.println("✅ Token válido (soma correta)! Liberado para cadastro.");
        displayMessage("Token OK!");
        tokenLiberado = true;
      } else {
        Serial.println("❌ Token incorreto (soma inválida)!");
        displayMessage("Token incorreto");
      }
      tokenDigitado = "";
      delay(1000);
    } else if (key == '*') {
      tokenDigitado = "";
      displayMessage("Token limpo");
    } else if (key >= '0' && key <= '9') {
      tokenDigitado += key;
      displayMessage("Token: " + tokenDigitado);
    }
  }

  if (tokenLiberado) {
    enrollAndSaveNewTemplate();
    tokenLiberado = false;
  }

  if (digitalRead(BUTTON_VERIFY) == LOW) {
    verifyAgainstAllTemplates();
    delay(500);
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "delete") {
      deleteAllTemplates();
    } else if (cmd == "list") {
      listAllTemplates();
    }
  }

  if (millis() - lastCommTime > COMM_TIMEOUT) {
    digitalWrite(COMM_LED, LOW);
  }
}


// Funções auxiliares (sem alterações a partir daqui):

void displayMessage(const String& message) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(message);
  display.display();
  Serial.println("[DISPLAY] " + message);
}

void enrollAndSaveNewTemplate() {
  displayMessage("Capturing...");

  displayMessage("Finger (1/3)");
  while (finger.getImage() != FPMStatus::OK) delay(100);
  if (finger.image2Tz(1) != FPMStatus::OK) return;

  displayMessage("Remove finger");
  delay(1000);
  displayMessage("Finger (2/3)");
  while (finger.getImage() != FPMStatus::OK) delay(100);
  if (finger.image2Tz(2) != FPMStatus::OK) return;

  displayMessage("Remove finger");
  delay(1000);
  displayMessage("Finger (3/3)");
  while (finger.getImage() != FPMStatus::OK) delay(100);

  if (finger.generateTemplate() != FPMStatus::OK) return;

  int16_t nextID = 0;
  while (SPIFFS.exists("/template_" + String(nextID) + ".bin")) nextID++;

  displayMessage("Salvando template...");
  if (finger.downloadTemplate(0) != FPMStatus::OK) return;

  const size_t maxTemplateSize = 768;
  uint8_t buffer[maxTemplateSize];
  size_t offset = 0;
  bool complete = false;
  uint16_t packetLen = FPM::packetLengths[static_cast<uint8_t>(finger.getPacketLength())];

  while (!complete && offset < maxTemplateSize) {
    uint16_t len = packetLen;
    uint8_t temp[packetLen];
    if (!finger.readDataPacket(temp, nullptr, &len, &complete)) break;
    memcpy(buffer + offset, temp, len);
    offset += len;
  }

  String path = "/template_" + String(nextID) + ".bin";
  File file = SPIFFS.open(path, "w");
  if (!file) return;
  file.write(buffer, offset);
  file.close();

  Serial.printf("Template salvo: %s (%d bytes)\n", path.c_str(), offset);
  displayMessage("Saved template");

  FingerTemplatePacket packet;
  packet.size = offset;
  packet.id = nextID;
  packet.type = 0;
  memcpy(packet.data, buffer, offset);

  esp_err_t result = esp_now_send(peerMac, (uint8_t *)&packet, sizeof(FingerTemplatePacket));
  if (result == ESP_OK) {
    Serial.println("Template enviado via ESP-NOW!");
    digitalWrite(COMM_LED, HIGH);
    lastCommTime = millis();
  } else {
    Serial.println("Erro ao enviar template via ESP-NOW.");
  }

  delay(2000);
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len < sizeof(FingerTemplatePacket)) return;

  FingerTemplatePacket packet;
  memcpy(&packet, incomingData, sizeof(FingerTemplatePacket));

  digitalWrite(COMM_LED, HIGH);
  lastCommTime = millis();

  if (packet.type == 1) {
    Serial.println("ACK recebido! Comunicação OK.");
    return;
  }

  String path = "/remote_" + String(remoteTemplateCounter++) + ".bin";
  File file = SPIFFS.open(path, "w");
  if (!file) {
    Serial.println("Erro ao salvar template remoto");
    return;
  }
  file.write(packet.data, packet.size);
  file.close();

  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          info->src_addr[0], info->src_addr[1], info->src_addr[2],
          info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  Serial.printf("Template recebido de %s e salvo como %s (%d bytes)\n", macStr, path.c_str(), packet.size);

  FingerTemplatePacket ack;
  ack.size = 0;
  ack.id = 0;
  ack.type = 1;
  esp_now_send(info->src_addr, (uint8_t *)&ack, sizeof(FingerTemplatePacket));
}

void verifyAgainstAllTemplates() {
  displayMessage("Place finger...");
  while (finger.getImage() != FPMStatus::OK) delay(100);

  if (finger.image2Tz(2) != FPMStatus::OK) return;
  Serial.println("Digital capturada no slot 2.");

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool found = false;

  FPMPacketLength plen = finger.getPacketLength();
  uint16_t packetLen = FPM::packetLengths[static_cast<uint8_t>(plen)];

  while (file) {
    String name = file.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.endsWith(".bin")) {
      File f = SPIFFS.open(name, "r");
      if (!f || f.size() == 0) {
        file = root.openNextFile();
        continue;
      }

      if (finger.uploadTemplate(1) != FPMStatus::OK) {
        f.close();
        file = root.openNextFile();
        continue;
      }

      size_t totalSize = f.size();
      uint8_t tempBuf[packetLen];
      size_t offset = 0;
      bool erro = false;

      while (offset < totalSize) {
        size_t len = min((size_t)packetLen, totalSize - offset);
        size_t readBytes = f.read(tempBuf, len);
        if (readBytes != len) {
          erro = true;
          break;
        }

        bool isLast = (offset + len >= totalSize);
        uint16_t sendLen = len;

        if (!finger.writeDataPacket(tempBuf, nullptr, &sendLen, isLast)) {
          erro = true;
          break;
        }

        offset += len;
      }

      f.close();
      if (erro) {
        file = root.openNextFile();
        continue;
      }

      uint16_t score;
      if (finger.matchTemplatePair(&score) == FPMStatus::OK) {
        displayMessage("Match! Score: " + String(score));
        Serial.printf("\u2705 MATCH: Score = %d\n", score);
        found = true;
        break;
      }
    }

    file = root.openNextFile();
  }

  if (!found) {
    displayMessage("No match found!");
    Serial.println("Nenhuma correspondência encontrada.");
  }

  delay(2000);
}

void deleteAllTemplates() {
  displayMessage("Formatando SPIFFS...");
  Serial.println("\u26a0\ufe0f Formatando SPIFFS...");
  if (SPIFFS.format()) {
    displayMessage("SPIFFS formatado!");
  } else {
    displayMessage("Erro ao formatar");
  }
  delay(2000);
}

void listAllTemplates() {
  Serial.println("Listando arquivos:");
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("Arquivo: %s (%d bytes)\n", file.name(), file.size());
    file = root.openNextFile();
  }
}
