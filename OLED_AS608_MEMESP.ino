#include <Adafruit_SSD1306.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <fpm.h>
#include "FS.h"

#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_RESET);
HardwareSerial as608Serial(1);
FPM finger(&as608Serial);

#define BUTTON_SAVE 14
#define BUTTON_VERIFY 27

void setup() {
  Serial.begin(115200);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  as608Serial.begin(57600, SERIAL_8N1, 16, 17);
  displayMessage("Init sensor...");
  if (finger.begin()) {
    displayMessage("Sensor OK!");
  } else {
    displayMessage("Sensor fail");
    while (1);
  }

  finger.setSecurityLevel(FPMSecurityLevel::FRR_2);
  finger.setPacketLength(FPMPacketLength::PLEN_128);

  if (!SPIFFS.begin(true)) {
    displayMessage("SPIFFS failed!");
    delay(2000);
  }

  pinMode(BUTTON_SAVE, INPUT_PULLUP);
  pinMode(BUTTON_VERIFY, INPUT_PULLUP);

  Serial.println("Digite 'delete' no monitor para apagar tudo.");
}

void loop() {
  if (digitalRead(BUTTON_SAVE) == LOW) {
    enrollAndSaveNewTemplate();
    delay(500);
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
}

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
  if (finger.image2Tz(1) != FPMStatus::OK) {
    displayMessage("Error on image 1");
    return;
  }

  displayMessage("Remove finger");
  delay(1000);
  displayMessage("Finger (2/3)");
  while (finger.getImage() != FPMStatus::OK) delay(100);
  if (finger.image2Tz(2) != FPMStatus::OK) {
    displayMessage("Error on image 2");
    return;
  }

  displayMessage("Remove finger");
  delay(1000);
  displayMessage("Finger (3/3)");
  while (finger.getImage() != FPMStatus::OK) delay(100);

  if (finger.generateTemplate() != FPMStatus::OK) {
    displayMessage("Failed to generate");
    return;
  }

  int16_t nextID = 0;
  while (SPIFFS.exists("/template_" + String(nextID) + ".bin")) {
    nextID++;
  }

  displayMessage("Salvando template...");
  if (finger.downloadTemplate(0) != FPMStatus::OK) {
    displayMessage("Download failed");
    return;
  }

  const size_t maxTemplateSize = 1024;
  uint8_t buffer[maxTemplateSize];
  size_t offset = 0;
  bool complete = false;
  uint16_t packetLen = FPM::packetLengths[static_cast<uint8_t>(finger.getPacketLength())];
  bool downloadSuccess = true;

  while (!complete && offset < maxTemplateSize) {
    uint16_t len = packetLen;
    uint8_t temp[packetLen];

    if (!finger.readDataPacket(temp, nullptr, &len, &complete)) {
      Serial.println("Erro ao ler pacote. Template corrompido.");
      downloadSuccess = false;
      break;
    }

    memcpy(buffer + offset, temp, len);
    offset += len;
    Serial.printf("Pacote recebido: %d bytes (total: %d)\n", len, offset);
  }

  if (!downloadSuccess) {
    displayMessage("Erro no template");
    return;
  }

  // Debug dos primeiros bytes
  Serial.print("Início do template salvo: ");
  for (int i = 0; i < 16; i++) Serial.printf("%02X ", buffer[i]);
  Serial.println();

  String path = "/template_" + String(nextID) + ".bin";
  File file = SPIFFS.open(path, "w");
  if (!file) {
    displayMessage("Erro SPIFFS ao gravar");
    return;
  }

  file.write(buffer, offset);
  file.close();

  Serial.printf("Template salvo com sucesso: %s (%d bytes)\n", path.c_str(), offset);
  displayMessage("Saved ID: " + String(nextID));
  delay(2000);
}

void verifyAgainstAllTemplates() {
  displayMessage("Place finger...");
  while (finger.getImage() != FPMStatus::OK) delay(100);

  if (finger.image2Tz(2) != FPMStatus::OK) {
    displayMessage("Failed to process");
    Serial.println("Erro ao converter imagem capturada.");
    return;
  }

  Serial.println("Digital capturada no slot 2.");

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool found = false;

  FPMPacketLength plen = finger.getPacketLength();
  uint16_t packetLen = FPM::packetLengths[static_cast<uint8_t>(plen)];

  while (file) {
    String name = file.name();
    if (name.endsWith(".bin")) {
      uint8_t id = name.substring(10).toInt();
      File f = SPIFFS.open("/" + name, "r");
      if (!f || f.size() == 0) {
        Serial.println("Arquivo inválido: " + name);
        file = root.openNextFile();
        continue;
      }

      Serial.println("Verificando arquivo: " + name);
      Serial.printf("Tamanho: %d bytes\n", f.size());

      uint8_t debugBuf[16];
      f.read(debugBuf, 16);
      Serial.print("Início do template lido: ");
      for (int i = 0; i < 16; i++) Serial.printf("%02X ", debugBuf[i]);
      Serial.println();
      f.seek(0);

      if (finger.uploadTemplate(1) != FPMStatus::OK) {
        Serial.println("Falha em uploadTemplate(1)");
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
          Serial.println("Erro ao ler do arquivo.");
          erro = true;
          break;
        }

        bool isLast = (offset + len >= totalSize);
        uint16_t sendLen = len;

        if (!finger.writeDataPacket(tempBuf, nullptr, &sendLen, isLast)) {
          Serial.println("Erro ao enviar pacote.");
          erro = true;
          break;
        }

        offset += len;
        Serial.printf("Pacote enviado: %d bytes, total enviado: %d\n", sendLen, offset);
      }

      f.close();

      if (erro) {
        Serial.println("Erro no envio do template. Pulando...");
        file = root.openNextFile();
        continue;
      }

      Serial.println("Template enviado ao slot 1.");

      uint16_t score;
      if (finger.matchTemplatePair(&score) == FPMStatus::OK) {
      displayMessage("Match! Score: " + String(score));
        Serial.printf("✅ MATCH ENCONTRADO!| Score: %d\n",score);
        found = true;
        break;
      } else {
        Serial.println("❌ Sem correspondência para ID: " + String(id));
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
  Serial.println("⚠️ Formatando SPIFFS...");
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
