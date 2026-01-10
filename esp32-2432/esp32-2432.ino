// ===== FILE: tft_client_2432s028.ino =====
#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// ✅ ตามรูป: GPIO1 = TX, GPIO3 = RX
static const int UART_RX_PIN = 3;   // RX ของบอร์ดจอ
static const int UART_TX_PIN = 1;   // TX ของบอร์ดจอ
static const uint32_t UART_BAUD = 115200;

static String bankName = "bank";
static String swName[8] = {"sw1","sw2","sw3","sw4","sw5","sw6","sw7","sw8"};
static int bankIndex = 0;

static String lineBuf;

// -------------------- NVS (eeprom-like) --------------------
static void saveToNVS()
{
  prefs.begin("disp", false);
  prefs.putInt("bank_i", bankIndex);
  prefs.putString("bank_n", bankName);
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    prefs.putString(key.c_str(), swName[i]);
  }
  prefs.end();
}

static void loadFromNVS()
{
  prefs.begin("disp", true);
  bankIndex = prefs.getInt("bank_i", 0);
  bankName  = prefs.getString("bank_n", "bank");
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    swName[i] = prefs.getString(key.c_str(), "sw" + String(i + 1));
  }
  prefs.end();
}

// -------------------- UI --------------------
static void drawUI()
{
  tft.fillScreen(TFT_BLACK);

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setCursor(10, 10);
  tft.printf("bank %d:", bankIndex + 1);

  tft.setCursor(10, 30);
  tft.setTextFont(4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.print(bankName);

  tft.setTextFont(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  int startY = 70;
  int rowH   = 38;
  int colW   = 155;

  for (int i = 0; i < 8; i++) {
    int r = i / 2;
    int c = i % 2;

    int x = 10 + c * colW;
    int y = startY + r * rowH;

    tft.drawRect(x, y, colW - 15, rowH - 6, TFT_DARKGREY);

    tft.setCursor(x + 6, y + 6);
    tft.printf("%d:", i + 1);

    tft.setCursor(x + 30, y + 6);
    tft.print(swName[i]);
  }
}

// -------------------- protocol --------------------
// @U,<bank>,<bankname>,<sw1>..,<sw8>
static bool parseUpdateLine(const String &line)
{
  if (!line.startsWith("@U,")) return false;

  String parts[11];
  int count = 0;

  int start = 0;
  while (count < 11) {
    int idx = line.indexOf(',', start);
    if (idx < 0) {
      parts[count++] = line.substring(start);
      break;
    }
    parts[count++] = line.substring(start, idx);
    start = idx + 1;
  }
  if (count < 11) return false;

  bankIndex = parts[1].toInt();
  bankName  = parts[2];
  bankName.trim();
  if (bankName.length() == 0) bankName = "bank";

  for (int i = 0; i < 8; i++) {
    String s = parts[3 + i];
    s.trim();
    if (s.length() == 0) s = "sw" + String(i + 1);
    swName[i] = s;
  }

  return true;
}

void setup()
{
  // debug ทาง usb ได้ (ถ้าบอร์ดคุณใช้ usb-serial)
  Serial.begin(115200);

  // uart คุยกับ S3 (ตามรูป)
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  tft.init();
  tft.setRotation(1);

  loadFromNVS();
  drawUI();

  Serial.println("tft client ready ✅");
  Serial.printf("uart1 rx=gpio%d tx=gpio%d baud=%lu\n", UART_RX_PIN, UART_TX_PIN, (unsigned long)UART_BAUD);
}

void loop()
{
  while (Serial1.available()) {
    char ch = (char)Serial1.read();
    if (ch == '\r') continue;

    if (ch == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      if (line.length() == 0) continue;

      if (parseUpdateLine(line)) {
        // ✅ ส่ง ACK ให้ไวที่สุดก่อน กันฝั่ง S3 timeout/หน่วง
        Serial1.print("@A,SAVED\n");
        Serial1.flush();

        // ค่อยทำงานหนักทีหลัง
        saveToNVS();
        drawUI();

        Serial.println("saved -> ACK sent ✅");
      }
    } else {
      if (lineBuf.length() < 300) lineBuf += ch;
    }
  }
}
