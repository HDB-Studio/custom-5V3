// ============================================================
// CPU LED 灯带控制固件 —— ESP32-C3 版 (Arduino + WS2812B 5050)
// 使用 Adafruit NeoPixel 库（在 ESP32-C3 上比 FastLED 更稳定）
// ------------------------------------------------------------
// 功能与 UNO 版一致：
//   串口接收 "R,G,B\n" 颜色指令，整条灯带点亮为该颜色；
//   收到 "?\n"/"PING\n" 回复 "OK\n"，供上位机自动识别端口；
//   断连后 50% 亮度慢闪红色，收到数据即恢复。
// 协议：ASCII 文本行，格式 "R,G,B\n"，R/G/B 取值 0~255。
// 依赖库：Adafruit NeoPixel（工具 → 管理库 → 搜索 "Adafruit NeoPixel"）
// ============================================================

#include <Adafruit_NeoPixel.h>

// ---------- 用户配置 ----------
#define NUM_LEDS     30        // 灯带 LED 数量
#define DATA_PIN     5         // 数据线 DIN 所接 GPIO（避开 8/9 启动脚，可改 2/3/4/6/7/10/18/19/20/21）
#define BRIGHTNESS   64        // 正常亮度 0-255，64 = 25%
// -----------------------------

// 断连告警参数
#define NO_DATA_TIMEOUT   3000   // 超过该毫秒数未收到数据则视为断开
#define ALERT_BRIGHTNESS  128    // 告警亮度（50% = 128/255）
#define BLINK_INTERVAL    500    // 红闪半周期(ms)，完整闪烁约 1s

// ESP32-C3 的 GPIO 为 3.3V 逻辑电平。为可靠驱动 WS2812B，建议在 DIN 与 GPIO
// 之间加 3.3V→5V 电平转换（如 74AHCT125 / SN74HCT245N）；短灯带通常也能直连。
// 供电：WS2812B 的 VDD 接 5V，GND 必须与 ESP32-C3 共地。

Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

unsigned long lastDataMs  = 0;
unsigned long lastBlinkMs = 0;
bool disconnected = true;   // 上电默认视为未连接，进入慢闪红
bool blinkOn      = false;

void setup() {
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();

  Serial.begin(115200);

  // 上电先以告警红色点亮，再进入慢闪
  strip.setBrightness(ALERT_BRIGHTNESS);
  strip.fill(strip.Color(255, 0, 0), 0, NUM_LEDS);
  strip.show();
  blinkOn     = true;
  lastBlinkMs = millis();
}

// 正常颜色显示（恢复常亮亮度）
void showColor(uint8_t r, uint8_t g, uint8_t b) {
  strip.setBrightness(BRIGHTNESS);
  strip.fill(strip.Color(r, g, b), 0, NUM_LEDS);
  strip.show();
}

// 断连告警：50% 亮度慢闪红色
void updateAlert() {
  unsigned long now = millis();
  if (now - lastBlinkMs >= BLINK_INTERVAL) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    strip.setBrightness(blinkOn ? ALERT_BRIGHTNESS : 0);
    strip.fill(strip.Color(255, 0, 0), 0, NUM_LEDS);
    strip.show();
  }
}

void loop() {
  static char buf[32];
  static uint8_t idx = 0;

  // 逐字符读取一行，遇到换行符后解析
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      buf[idx] = '\0';
      idx = 0;
      handleLine(buf);
      lastDataMs = millis();
      disconnected = false;   // 收到有效数据即恢复连接
    } else if (c == '\r') {
      // 忽略回车
    } else if (idx < sizeof(buf) - 1) {
      buf[idx++] = c;
    }
  }

  // 断连检测：超过阈值无数据则进入慢闪红告警
  if (!disconnected && (millis() - lastDataMs > NO_DATA_TIMEOUT)) {
    disconnected = true;
    blinkOn = false;
    lastBlinkMs = millis();
  }

  if (disconnected) {
    updateAlert();
  }
}

void handleLine(char* line) {
  // 握手：回复 OK，供上位机识别
  if (strcmp(line, "?") == 0 || strcmp(line, "PING") == 0) {
    Serial.println("OK");
    return;
  }

  // 颜色指令：R,G,B
  int r = 0, g = 0, b = 0;
  if (sscanf(line, "%d,%d,%d", &r, &g, &b) == 3) {
    r = constrain(r, 0, 255);
    g = constrain(g, 0, 255);
    b = constrain(b, 0, 255);
    showColor(r, g, b);
  }
}