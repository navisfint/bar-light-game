/*
 * ============================================================
 *  15 灯跑马灯喝酒游戏  ·  简易版固件
 * ============================================================
 *
 *  硬件：Arduino Nano + 15 颗 WS2812B + 1 个拍击按钮 + 16 个拨动开关
 *
 *  玩法：拍一下开始乱闪，再拍一下停下，停下时恰好亮 2 盏或 3 盏。
 *        拨动开关可以把任意一盏灯排除在游戏之外，该灯全程不亮。
 *
 *  原理：按下停止键的瞬间，程序先从「启用的灯」里抽好结果，
 *        再用 700ms 的减速动画把结果演出来。
 *        跑马灯本身无规律，所以观众无法从停止位置反推结果。
 *
 *  烧录：Arduino IDE
 *        - 开发板：Arduino Nano
 *        - 处理器：ATmega328P（老款国产板可能要选 Old Bootloader）
 *        - 库：菜单「工具 → 管理库」搜索 "Adafruit NeoPixel" 安装
 *
 *  引脚分配（Nano 刚好够用，不要随意改）
 *    D2  D3  D7  D8  D9  D10 D11 D12 D13 A0 A1 A2 A3 A4 A5  → 15 个禁用开关
 *    D4  → 拍击按钮（另一端接 GND）
 *    D5  → 2盏/3盏 切换开关（另一端接 GND，断开=2盏，闭合=3盏）
 *    D6  → 灯带数据线 DIN
 *    D0/D1 留给串口，A6 不用，A7 用作随机种子的噪声源
 * ============================================================
 */

#include <Adafruit_NeoPixel.h>

// ---------- 引脚 ----------
#define LED_PIN    6
#define BTN_PIN    4
#define MODE_PIN   5
#define NUM_LEDS   15

const uint8_t DIS_PINS[NUM_LEDS] = {2, 3, 7, 8, 9, 10, 11, 12, 13, A0, A1, A2, A3, A4, A5};

// ---------- 可调参数 ----------
#define LOCK_MS      700     // 减速收束时长（毫秒），500~900 之间效果最好
#define RESULT_MS    15000   // 结果保持时间（毫秒）
#define SPEED_MIN    25      // 乱闪最快间隔（毫秒）
#define SPEED_VAR    18      // 速度抖动幅度，越大越"无规律"
#define IDLE_LEVEL   6       // 待机微光亮度 0~255，设 0 则待机全灭
#define SW_CLOSED_IS_DISABLED 1  // 开关闭合到 GND = 该灯不参与。若装反了改成 0

// 灯光颜色（跑马灯与结果灯同色，改这里就行）
#define R_RUN 255
#define G_RUN 170
#define B_RUN 70

#define NO_LED 255

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

enum State { IDLE, RUNNING, LOCKING, RESULT };
State st = IDLE;

bool enabled[NUM_LEDS];
uint8_t target[3] = {0, 0, 0};
uint8_t targetN = 2;

unsigned long lastStep = 0;
unsigned long stepGap  = 30;
unsigned long t0       = 0;

// 按钮消抖状态
bool btnPrevRaw = HIGH;
bool btnHeld    = false;
unsigned long btnT = 0;


// ============================================================
//  开关与按钮
// ============================================================
void readSwitches() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    bool closed = (digitalRead(DIS_PINS[i]) == LOW);
#if SW_CLOSED_IS_DISABLED
    enabled[i] = !closed;
#else
    enabled[i] = closed;
#endif
  }
}

// 返回 true 表示检测到一次「按下」（下降沿，已消抖 30ms）
bool pollBtn() {
  bool raw = digitalRead(BTN_PIN);
  if (raw != btnPrevRaw) {      // 电平有变化，重新计时
    btnPrevRaw = raw;
    btnT = millis();
  }
  if (millis() - btnT > 30) {   // 电平稳定超过 30ms 才认
    if (btnPrevRaw == LOW && !btnHeld) {
      btnHeld = true;
      return true;
    }
    if (btnPrevRaw == HIGH) btnHeld = false;
  }
  return false;
}


// ============================================================
//  抽签：从启用的灯里随机取 N 盏（部分洗牌，保证不重复）
// ============================================================
void pickTargets() {
  uint8_t pool[NUM_LEDS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (enabled[i]) pool[n++] = i;
  }

  if (n == 0) { targetN = 0; return; }

  uint8_t k = (targetN > n) ? n : targetN;

  for (uint8_t i = 0; i < k; i++) {
    uint8_t j = i + random(n - i);
    uint8_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
  }
  for (uint8_t i = 0; i < k; i++) target[i] = pool[i];

  // 从小到大排序，亮起来的顺序更好看
  for (uint8_t i = 0; i < k - 1; i++) {
    for (uint8_t j = i + 1; j < k; j++) {
      if (target[j] < target[i]) {
        uint8_t tmp = target[i]; target[i] = target[j]; target[j] = tmp;
      }
    }
  }
  targetN = k;
}

uint8_t randomEnabled() {
  uint8_t pool[NUM_LEDS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (enabled[i]) pool[n++] = i;
  }
  if (n == 0) return NO_LED;
  return pool[random(n)];
}


// ============================================================
//  显示
// ============================================================
void clearAll() {
  strip.clear();
  strip.show();
}

void showOnly(uint8_t pos) {
  strip.clear();
  if (pos != NO_LED) strip.setPixelColor(pos, R_RUN, G_RUN, B_RUN);
  strip.show();
}

// 跑马灯那颗 + 目标灯逐渐变亮（用于减速阶段，让结果自然浮现）
void showRunnerPlusTargets(uint8_t runner, uint8_t fade) {
  strip.clear();
  for (uint8_t i = 0; i < targetN; i++) {
    strip.setPixelColor(target[i],
                        (uint16_t)R_RUN * fade / 255,
                        (uint16_t)G_RUN * fade / 255,
                        (uint16_t)B_RUN * fade / 255);
  }
  if (runner != NO_LED) strip.setPixelColor(runner, R_RUN, G_RUN, B_RUN);
  strip.show();
}

void showTargets() {
  strip.clear();
  for (uint8_t i = 0; i < targetN; i++) {
    strip.setPixelColor(target[i], R_RUN, G_RUN, B_RUN);
  }
  strip.show();
}

void idleGlow() {
  readSwitches();
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (enabled[i] && IDLE_LEVEL > 0) {
      strip.setPixelColor(i, IDLE_LEVEL, IDLE_LEVEL * 4 / 5, IDLE_LEVEL / 3);
    } else {
      strip.setPixelColor(i, 0);
    }
  }
  strip.show();
}


// ============================================================
//  状态切换
// ============================================================
void startRound() {
  // 每次开局重新播种，用 A7 悬空脚的电噪声 + 微秒计数做种子
  randomSeed((unsigned long)analogRead(A7) * 2654435761UL + micros());

  // 2 盏还是 3 盏，由 MODE_PIN 上的拨动开关决定
  targetN = (digitalRead(MODE_PIN) == LOW) ? 3 : 2;

  stepGap  = SPEED_MIN;
  lastStep = 0;
  st = RUNNING;
}

void lockResult() {
  pickTargets();
  if (targetN == 0) {      // 15 盏全被禁用，直接回待机，防止卡死
    st = IDLE;
    idleGlow();
    return;
  }
  t0 = millis();
  lastStep = 0;
  stepGap = SPEED_MIN;
  st = LOCKING;
}


// ============================================================
void setup() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  for (uint8_t i = 0; i < NUM_LEDS; i++) pinMode(DIS_PINS[i], INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(255);   // 整体亮度上限，嫌刺眼就调小
  strip.show();

  readSwitches();
  idleGlow();
  st = IDLE;
}


void loop() {
  readSwitches();
  bool pressed = pollBtn();

  switch (st) {

    case IDLE:
      if (pressed) startRound();
      break;

    case RUNNING:
      if (pressed) { lockResult(); break; }
      if (millis() - lastStep >= stepGap) {
        lastStep = millis();
        stepGap  = SPEED_MIN + random(SPEED_VAR);   // 每步速度都不一样，看起来无规律
        showOnly(randomEnabled());
      }
      break;

    case LOCKING: {
      unsigned long el = millis() - t0;
      if (el >= LOCK_MS) {                        // 收束结束，亮结果
        st = RESULT;
        t0 = millis();
        showTargets();
        break;
      }
      if (millis() - lastStep >= stepGap) {
        lastStep = millis();
        stepGap  = SPEED_MIN + (el * 190UL) / LOCK_MS;   // 25ms 逐渐拉到 215ms
        uint8_t fade = 0;
        unsigned long fadeFrom = LOCK_MS * 55 / 100;     // 后 45% 时间目标灯开始浮现
        if (el > fadeFrom) {
          fade = map(el, fadeFrom, LOCK_MS, 0, 255);
        }
        showRunnerPlusTargets(randomEnabled(), (uint8_t)fade);
      }
      break;
    }

    case RESULT:
      if (pressed || millis() - t0 >= RESULT_MS) {
        st = IDLE;
        idleGlow();
      }
      break;
  }
}
