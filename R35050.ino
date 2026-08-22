// ============================================================
// CPU LED 灯带控制固件  (UNO R3 + WS2812B 5050 灯带)
// ------------------------------------------------------------
// 功能：通过串口(USB)接收上位机发来的 "R,G,B\n" 颜色指令，
//       将整条灯带点亮为该颜色。
// 协议：ASCII 文本行，格式 "R,G,B\n"，R/G/B 取值 0~255。
//       握手：收到 "?\n" 或 "PING\n" 回复 "OK\n"，供上位机自动识别端口。
// 断连告警：超过 NO_DATA_TIMEOUT 未收到数据，灯带以 50% 亮度慢闪红色；
//           收到任何数据后恢复正常显示。
// 依赖库：FastLED（Arduino IDE → 工具 → 管理库 → 搜索 "FastLED" 安装）
// ============================================================

#include <FastLED.h>

// ---------- 用户配置 ----------
#define NUM_LEDS     30        // 灯带 LED 数量（按实际灯珠数修改）
#define DATA_PIN     6         // 数据线 DIN 所接 UNO 引脚
#define LED_TYPE     WS2812B
#define COLOR_ORDER  GRB       // WS2812B 像素顺序为 GRB
#define BRIGHTNESS   64        // 正常亮度 0~255（64 ≈ 25%）
// -----------------------------

// 断连告警参数
#define NO_DATA_TIMEOUT   3000   // 超过该毫秒数未收到数据则视为断开
#define ALERT_BRIGHTNESS  128    // 告警亮度（50% = 128/255）
#define BLINK_INTERVAL    500    // 红闪半周期(ms)，完整闪烁约 1s

CRGB leds[NUM_LEDS];
unsigned long lastDataMs  = 0;
unsigned long lastBlinkMs = 0;
bool disconnected = true;   // 上电默认视为未连接，进入慢闪红
bool blinkOn      = false;

void setup() {
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  Serial.begin(115200);

  // 上电先以告警红色点亮，再进入慢闪
  FastLED.setBrightness(ALERT_BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  blinkOn      = true;
  lastBlinkMs  = millis();
}

// 正常颜色显示（恢复常亮亮度）
void showColor(uint8_t r, uint8_t g, uint8_t b) {
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB(r, g, b));
  FastLED.show();
}

// 断连告警：50% 亮度慢闪红色
void updateAlert() {
  unsigned long now = millis();
  if (now - lastBlinkMs >= BLINK_INTERVAL) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    FastLED.setBrightness(blinkOn ? ALERT_BRIGHTNESS : 0);
    fill_solid(leds, NUM_LEDS, blinkOn ? CRGB::Red : CRGB::Black);
    FastLED.show();
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