#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <DHT.h>

// =========================================================================
// 1. CẤU HÌNH CHÂN, NGƯỠNG & CẤU TRÚC DỮ LIỆU
// =========================================================================
// true: mô phỏng độ ồn bằng biến trở trên Wokwi | false: cắm cảm biến KY-037 thật
#define SIMULATE_WOKWI true

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define PIN_TRIG 5
#define PIN_ECHO 35
#define PIN_LDR 34
#define PIN_POT_DIMMER 32
#define PIN_SOUND 36    // Chân VP trên ESP32 DevKit V1
#define PIN_GAS_FIRE 39 // Chân VN trên ESP32 DevKit V1 (MQ-2 / Báo cháy)
#define PIN_DHT 23      // Cảm biến DHT22
#define PIN_LED 2
#define PIN_BUZZER 4

#define DHT_TYPE DHT22

#define DIST_PRESENT_CM 50        // < 50cm: có người ngồi tại bàn
#define LDR_DARK_THRESHOLD 2000   // > 2000: trời tối (module LDR chuẩn / Wokwi)
#define NOISE_THRESHOLD 2500      // > 2500: môi trường ồn ào
#define FIRE_ALARM_THRESHOLD 2200 // >= 2200: kích hoạt báo cháy (MQ-2 Wokwi)
#define FIRE_CLEAR_THRESHOLD 1800 // < 1800: tắt báo cháy (tránh nhấp nháy ngưỡng)
#define MAX_BREAK_MINUTES 10      // Giới hạn thời gian nghỉ tối đa (phút)

#define SENSOR_INTERVAL_MS 300  // Chu kỳ quét cảm biến
#define LEAVE_DEBOUNCE_MS 3000  // Rời bàn liên tục 3s mới tính 1 lần
#define NOISE_DEBOUNCE_MS 5000  // Ồn liên tục 5s mới tính 1 lần vi phạm
#define SUBVIEW_TIMEOUT_MS 4000 // Tự thoát màn hình phụ (phím C / D) sau 4s

enum SystemState
{
  STATE_IDLE,
  STATE_WELCOME,
  STATE_INPUT_TOTAL,
  STATE_INPUT_STUDY,
  STATE_INPUT_BREAK,
  STATE_RUNNING,
  STATE_PAUSED
};

enum SubViewMode
{
  VIEW_MAIN,
  VIEW_NOISE,
  VIEW_ENV
};

struct StudySession
{
  int totalStudyMin = 0;
  int cycleStudyMin = 0;
  int cycleBreakMin = 0;

  bool isBreakPhase = false;
  unsigned long cycleElapsedSec = 0;
  unsigned long totalStudyElapsedSec = 0;

  int leaveDeskCount = 0;
  int noiseViolationCount = 0;

  void resetCounters()
  {
    isBreakPhase = false;
    cycleElapsedSec = 0;
    totalStudyElapsedSec = 0;
    leaveDeskCount = 0;
    noiseViolationCount = 0;
  }
};

// =========================================================================
// 2. MODULE WEB / MQTT (PLACEHOLDER)
// =========================================================================
namespace WebComm
{
  void sendTelemetry(SystemState state, float distCm, int ldrVal, int noiseVal,
                     int gasVal, float tempC, float humPct, int lampPwm, const StudySession &s)
  {
    Serial.printf(
        "[MQTT_TELEMETRY] {\"state\":%d,\"dist\":%.1f,\"ldr\":%d,\"noise\":%d,\"gas\":%d,\"temp\":%.1f,\"hum\":%.1f,\"lamp\":%d,\"leaves\":%d,\"noise_alerts\":%d}\n",
        state, distCm, ldrVal, noiseVal, gasVal, tempC, humPct, lampPwm, s.leaveDeskCount, s.noiseViolationCount);
  }

  void logLeaveDesk(int count)
  {
    Serial.printf("[MQTT_EVENT] {\"event\":\"leave_desk\",\"count\":%d}\n", count);
  }

  void logNoiseAlert(int count, int noiseVal)
  {
    Serial.printf("[MQTT_EVENT] {\"event\":\"noise_alert\",\"count\":%d,\"level\":%d}\n", count, noiseVal);
  }

  void sendFireAlarm(int gasVal, float tempC)
  {
    Serial.printf(
        "[MQTT_FIRE_ALARM] {\"event\":\"fire_detected\",\"gas\":%d,\"temp\":%.1f,\"msg\":\"KHAN CAP! Phat hien khoi/chay tai ban hoc!\"}\n",
        gasVal, tempC);
  }

  void sendPushNotification(const char *message)
  {
    Serial.printf("[MQTT_PUSH] {\"notify\":\"%s\"}\n", message);
  }

  void sendCongratEmail(const StudySession &s)
  {
    Serial.printf(
        "[MQTT_EMAIL] {\"event\":\"session_complete\",\"study_min\":%d,\"leaves\":%d,\"noise_alerts\":%d}\n",
        s.totalStudyMin, s.leaveDeskCount, s.noiseViolationCount);
  }
}

// =========================================================================
// 3. MODULE PHẦN CỨNG (CẢM BIẾN, ĐÈN PWM, CÒI)
// =========================================================================
namespace Hardware
{
  DHT dht(PIN_DHT, DHT_TYPE);

  void init()
  {
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_LDR, INPUT);
    pinMode(PIN_POT_DIMMER, INPUT);
    pinMode(PIN_SOUND, INPUT);
    pinMode(PIN_GAS_FIRE, INPUT);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    analogWrite(PIN_LED, 0);
    dht.begin();
  }

  void beep(int count, int durationMs, int pauseMs = 150)
  {
    for (int i = 0; i < count; i++)
    {
      tone(PIN_BUZZER, 2000);
      delay(durationMs);
      noTone(PIN_BUZZER);
      if (i < count - 1)
        delay(pauseMs);
    }
  }

  void setAlarmSiren(bool active)
  {
    if (active)
    {
      tone(PIN_BUZZER, 3000);
    }
    else
    {
      noTone(PIN_BUZZER);
    }
  }

  float readDistanceCm()
  {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long duration = pulseIn(PIN_ECHO, HIGH, 30000);
    return (duration == 0) ? 999.0f : (duration * 0.034f / 2.0f);
  }

  int readLightRaw()
  {
    return analogRead(PIN_LDR);
  }

  int readGasRaw()
  {
    return analogRead(PIN_GAS_FIRE);
  }

  bool readDHT(float &tempOut, float &humOut)
  {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h))
      return false;
    tempOut = t;
    humOut = h;
    return true;
  }

  int readNoiseRaw()
  {
#if SIMULATE_WOKWI
    return analogRead(PIN_SOUND);
#else
    unsigned long startMs = millis();
    int signalMax = 0, signalMin = 4095;
    while (millis() - startMs < 50)
    {
      int sample = analogRead(PIN_SOUND);
      if (sample > signalMax)
        signalMax = sample;
      if (sample < signalMin)
        signalMin = sample;
    }
    return signalMax - signalMin;
#endif
  }

  int noiseToPercent(int rawVal)
  {
    return constrain(map(rawVal, 0, 4095, 0, 100), 0, 100);
  }

  int updateSmartLamp(bool enable)
  {
    if (!enable)
    {
      analogWrite(PIN_LED, 0);
      return 0;
    }
    if (readLightRaw() > LDR_DARK_THRESHOLD)
    {
      int pwmVal = map(analogRead(PIN_POT_DIMMER), 0, 4095, 25, 255);
      analogWrite(PIN_LED, pwmVal);
      return pwmVal;
    }
    analogWrite(PIN_LED, 0);
    return 0;
  }

  void turnOffLamp()
  {
    analogWrite(PIN_LED, 0);
  }
}

// =========================================================================
// 4. MODULE HIỂN THỊ LCD 16x2
// =========================================================================
namespace Display
{
  LiquidCrystal_I2C lcd(0x27, 16, 2);

  void init()
  {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    lcd.init();
    lcd.noBacklight();
    lcd.clear();
  }

  void sleep()
  {
    lcd.clear();
    lcd.noBacklight();
  }

  void showWelcome()
  {
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("CHAO MUNG DA DEN");
    lcd.setCursor(0, 1);
    lcd.print("BAM # DE BAT DAU");
  }

  void showInputIntro()
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NHAP TONG T.GIAN");
    lcd.setCursor(0, 1);
    lcd.print("CHU KY HOC&NGHI ");
  }

  void showInput(const char *title, const String &value)
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(title);
    lcd.setCursor(0, 1);
    lcd.print("> ");
    lcd.print(value);
    lcd.print(" phut");
  }

  void showBreakLimitError()
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("BAO LOI NHAP:   ");
    lcd.setCursor(0, 1);
    lcd.print("NGHI TOI DA 10P ");
  }

  void showRunning(SystemState state, const StudySession &s)
  {
    lcd.setCursor(0, 0);
    if (state == STATE_PAUSED)
    {
      lcd.print("CHU KY: TAM DUNG");
    }
    else if (s.isBreakPhase)
    {
      lcd.print("CHU KY: NGHI    ");
    }
    else
    {
      lcd.print("CHU KY: DANG HOC");
    }

    unsigned long targetMin = s.isBreakPhase ? s.cycleBreakMin : s.cycleStudyMin;
    unsigned long curMin = s.cycleElapsedSec / 60;
    unsigned long curSec = s.cycleElapsedSec % 60;

    char line2[17];
    snprintf(line2, sizeof(line2), "  %02lu:%02lu/%02lu:00   ", curMin, curSec, targetMin);
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  void showNoiseMonitor(int rawVal, int percent, bool isNoisy)
  {
    char line1[17], line2[17];
    snprintf(line1, sizeof(line1), "DO ON:%3d%% %-5s", percent, isNoisy ? "(ON!)" : "(OK)");
    snprintf(line2, sizeof(line2), "ADC:%4d | C:TAT", rawVal);
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  void showEnvMonitor(float tempC, float humPct)
  {
    char line1[17], line2[17];
    snprintf(line1, sizeof(line1), "NHIET DO:%5.1f C", tempC);
    snprintf(line2, sizeof(line2), "DO AM:%5.1f%% |D ", humPct);
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  void showFireAlarm(int gasVal)
  {
    lcd.backlight();
    char line2[17];
    snprintf(line2, sizeof(line2), "CHAY! GAS:%4d  ", gasVal);
    lcd.setCursor(0, 0);
    lcd.print("!!! CANH BAO !!!");
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  void showFinished(bool earlyExit)
  {
    lcd.clear();
    if (earlyExit)
    {
      lcd.setCursor(0, 0);
      lcd.print("KET THUC SOM!   ");
      lcd.setCursor(0, 1);
      lcd.print("HEN GAP LAI BAN ");
    }
    else
    {
      lcd.setCursor(0, 0);
      lcd.print("DA HOAN TAT HOC!");
      lcd.setCursor(0, 1);
      lcd.print("CHUC MUNG BAN!  ");
    }
  }
}

// =========================================================================
// 5. KHỞI TẠO KEYPAD & BIẾN ĐIỀU PHỐI CHÍNH
// =========================================================================
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};
byte rowPins[ROWS] = {13, 18, 14, 27};
byte colPins[COLS] = {26, 25, 33, 19};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

SystemState currentState = STATE_IDLE;
SubViewMode currentSubView = VIEW_MAIN;
StudySession session;
String inputBuffer = "";

unsigned long lastSecondTick = 0;
unsigned long lastSensorTick = 0;
unsigned long subViewStartMs = 0;

// Giá trị cảm biến hiện tại
int currentNoiseRaw = 0;
int currentGasRaw = 0;
float currentTempC = 0.0f;
float currentHumPct = 0.0f;

// Cờ cảnh báo cháy
bool isFireAlarm = false;
bool sirenToggle = false;

// Cờ theo dõi rời bàn & tiếng ồn
bool wasPresent = true, leaveCounted = false;
unsigned long absentStartMs = 0;

bool wasNoisy = false, noiseCounted = false;
unsigned long noisyStartMs = 0;

// =========================================================================
// 6. LOGIC NGHIỆP VỤ & XỬ LÝ PHÍM
// =========================================================================
void restoreCurrentScreen()
{
  switch (currentState)
  {
  case STATE_IDLE:
    Display::sleep();
    break;
  case STATE_WELCOME:
    Display::showWelcome();
    break;
  case STATE_INPUT_TOTAL:
    Display::showInput("1.TONG TG HOC:", inputBuffer);
    break;
  case STATE_INPUT_STUDY:
    Display::showInput("2.CHU KY HOC:", inputBuffer);
    break;
  case STATE_INPUT_BREAK:
    Display::showInput("3.THOI GIAN NGHI", inputBuffer);
    break;
  case STATE_RUNNING:
  case STATE_PAUSED:
    Display::showRunning(currentState, session);
    break;
  }
}

void finishSession(bool earlyExit)
{
  currentSubView = VIEW_MAIN;
  Hardware::turnOffLamp();
  Display::showFinished(earlyExit);

  if (earlyExit)
  {
    Hardware::beep(1, 400);
  }
  else
  {
    Hardware::beep(3, 600, 250);
    WebComm::sendCongratEmail(session);
  }

  delay(3000);
  Display::sleep();
  currentState = STATE_IDLE;
}

void startSession()
{
  Hardware::beep(1, 300);
  session.resetCounters();
  wasPresent = true;
  leaveCounted = false;
  wasNoisy = false;
  noiseCounted = false;
  currentSubView = VIEW_MAIN;

  lastSecondTick = millis();
  currentState = STATE_RUNNING;
  Display::showRunning(currentState, session);
}

void handleSetupKeys(char key)
{
  if (currentState == STATE_WELCOME)
  {
    if (key == '#')
    {
      Display::showInputIntro();
      delay(1600);
      inputBuffer = "";
      currentState = STATE_INPUT_TOTAL;
      Display::showInput("1.TONG TG HOC:", inputBuffer);
    }
    return;
  }

  if (key >= '0' && key <= '9')
  {
    int maxLen = (currentState == STATE_INPUT_BREAK) ? 2 : 3;
    if ((int)inputBuffer.length() < maxLen)
    {
      inputBuffer += key;
      const char *title = (currentState == STATE_INPUT_TOTAL) ? "1.TONG TG HOC:" : (currentState == STATE_INPUT_STUDY) ? "2.CHU KY HOC:"
                                                                                                                       : "3.THOI GIAN NGHI";
      Display::showInput(title, inputBuffer);
    }
    return;
  }

  if (key == '*')
  {
    if (inputBuffer.length() > 0)
    {
      inputBuffer.remove(inputBuffer.length() - 1);
      const char *title = (currentState == STATE_INPUT_TOTAL) ? "1.TONG TG HOC:" : (currentState == STATE_INPUT_STUDY) ? "2.CHU KY HOC:"
                                                                                                                       : "3.THOI GIAN NGHI";
      Display::showInput(title, inputBuffer);
    }
    else if (currentState == STATE_INPUT_TOTAL)
    {
      currentState = STATE_WELCOME;
      Display::showWelcome();
    }
    else if (currentState == STATE_INPUT_STUDY)
    {
      inputBuffer = String(session.totalStudyMin);
      currentState = STATE_INPUT_TOTAL;
      Display::showInput("1.TONG TG HOC:", inputBuffer);
    }
    else if (currentState == STATE_INPUT_BREAK)
    {
      inputBuffer = String(session.cycleStudyMin);
      currentState = STATE_INPUT_STUDY;
      Display::showInput("2.CHU KY HOC:", inputBuffer);
    }
    return;
  }

  if (key == '#' && inputBuffer.length() > 0)
  {
    int val = inputBuffer.toInt();
    if (val <= 0)
      return;

    if (currentState == STATE_INPUT_TOTAL)
    {
      session.totalStudyMin = val;
      inputBuffer = "";
      currentState = STATE_INPUT_STUDY;
      Display::showInput("2.CHU KY HOC:", inputBuffer);
    }
    else if (currentState == STATE_INPUT_STUDY)
    {
      session.cycleStudyMin = val;
      inputBuffer = "";
      currentState = STATE_INPUT_BREAK;
      Display::showInput("3.THOI GIAN NGHI", inputBuffer);
    }
    else if (currentState == STATE_INPUT_BREAK)
    {
      if (val > MAX_BREAK_MINUTES)
      {
        Display::showBreakLimitError();
        Hardware::beep(2, 150, 100);
        delay(2000);
        inputBuffer = "";
        Display::showInput("3.THOI GIAN NGHI", inputBuffer);
      }
      else
      {
        session.cycleBreakMin = val;
        startSession();
      }
    }
  }
}

void handleSessionKeys(char key)
{
  if (key == 'A')
  {
    currentSubView = VIEW_MAIN;
    if (currentState == STATE_RUNNING)
    {
      currentState = STATE_PAUSED;
    }
    else
    {
      lastSecondTick = millis();
      currentState = STATE_RUNNING;
    }
    Display::showRunning(currentState, session);
  }
  else if (key == 'B')
  {
    finishSession(true);
  }
  else if (key == 'C')
  {
    if (currentSubView == VIEW_NOISE)
    {
      currentSubView = VIEW_MAIN;
      Display::showRunning(currentState, session);
    }
    else
    {
      currentSubView = VIEW_NOISE;
      subViewStartMs = millis();
      currentNoiseRaw = Hardware::readNoiseRaw();
      Display::showNoiseMonitor(
          currentNoiseRaw,
          Hardware::noiseToPercent(currentNoiseRaw),
          currentNoiseRaw > NOISE_THRESHOLD);
    }
  }
  else if (key == 'D')
  {
    if (currentSubView == VIEW_ENV)
    {
      currentSubView = VIEW_MAIN;
      Display::showRunning(currentState, session);
    }
    else
    {
      currentSubView = VIEW_ENV;
      subViewStartMs = millis();
      Hardware::readDHT(currentTempC, currentHumPct);
      Display::showEnvMonitor(currentTempC, currentHumPct);
    }
  }
}

void processSensors(unsigned long now)
{
  float dist = Hardware::readDistanceCm();
  bool userPresent = (dist < DIST_PRESENT_CM);
  currentNoiseRaw = Hardware::readNoiseRaw();
  currentGasRaw = Hardware::readGasRaw();

  // Đọc DHT22 định kỳ mỗi ~2.1 giây (7 * 300ms) đúng chuẩn thời gian lấy mẫu của DHT22
  static int dhtTick = 7;
  if (++dhtTick >= 7)
  {
    dhtTick = 0;
    Hardware::readDHT(currentTempC, currentHumPct);
  }

  // 1. ƯU TIÊN CAO NHẤT: Kiểm tra báo cháy 24/7
  if (!isFireAlarm && currentGasRaw >= FIRE_ALARM_THRESHOLD)
  {
    isFireAlarm = true;
    sirenToggle = true;
    Hardware::setAlarmSiren(true);
    Display::showFireAlarm(currentGasRaw);
    WebComm::sendFireAlarm(currentGasRaw, currentTempC);
  }
  else if (isFireAlarm)
  {
    if (currentGasRaw < FIRE_CLEAR_THRESHOLD)
    {
      isFireAlarm = false;
      Hardware::setAlarmSiren(false);
      restoreCurrentScreen();
    }
    else
    {
      // Nhịp còi báo động kêu ngắt quãng mỗi 300ms không chặn hệ thống
      sirenToggle = !sirenToggle;
      Hardware::setAlarmSiren(sirenToggle);
      Display::showFireAlarm(currentGasRaw);
    }
  }

  bool inSession = (currentState == STATE_RUNNING || currentState == STATE_PAUSED);
  int lampPwm = Hardware::updateSmartLamp(inSession);

  // 2. Tự mở màn hình chào khi có người lại gần (nếu không đang báo cháy)
  if (!isFireAlarm && currentState == STATE_IDLE && userPresent)
  {
    currentState = STATE_WELCOME;
    Display::showWelcome();
  }

  // 3. Giám sát rời bàn & tiếng ồn trong chu kỳ ĐANG HỌC
  if (currentState == STATE_RUNNING && !session.isBreakPhase)
  {
    if (!userPresent)
    {
      if (wasPresent)
      {
        wasPresent = false;
        absentStartMs = now;
      }
      else if (!leaveCounted && (now - absentStartMs >= LEAVE_DEBOUNCE_MS))
      {
        session.leaveDeskCount++;
        leaveCounted = true;
        WebComm::logLeaveDesk(session.leaveDeskCount);
      }
    }
    else
    {
      wasPresent = true;
      leaveCounted = false;
    }

    if (currentNoiseRaw > NOISE_THRESHOLD)
    {
      if (!wasNoisy)
      {
        wasNoisy = true;
        noisyStartMs = now;
      }
      else if (!noiseCounted && (now - noisyStartMs >= NOISE_DEBOUNCE_MS))
      {
        session.noiseViolationCount++;
        noiseCounted = true;
        WebComm::logNoiseAlert(session.noiseViolationCount, currentNoiseRaw);
      }
    }
    else
    {
      wasNoisy = false;
      noiseCounted = false;
    }
  }

  // 4. Cập nhật màn hình phụ (Phím C: Độ ồn | Phím D: Nhiệt độ & Độ ẩm)
  if (!isFireAlarm && currentSubView != VIEW_MAIN && inSession)
  {
    if (now >= subViewStartMs && (now - subViewStartMs >= SUBVIEW_TIMEOUT_MS))
    {
      currentSubView = VIEW_MAIN;
      Display::showRunning(currentState, session);
    }
    else if (currentSubView == VIEW_NOISE)
    {
      Display::showNoiseMonitor(
          currentNoiseRaw,
          Hardware::noiseToPercent(currentNoiseRaw),
          currentNoiseRaw > NOISE_THRESHOLD);
    }
    else if (currentSubView == VIEW_ENV)
    {
      Display::showEnvMonitor(currentTempC, currentHumPct);
    }
  }

  // 5. Gửi Telemetry định kỳ mỗi 3 giây (10 * 300ms)
  static int telemetryTick = 0;
  if (++telemetryTick >= 10)
  {
    telemetryTick = 0;
    WebComm::sendTelemetry(
        currentState, dist, Hardware::readLightRaw(), currentNoiseRaw,
        currentGasRaw, currentTempC, currentHumPct, lampPwm, session);
  }
}

void processTimerTick()
{
  session.cycleElapsedSec++;

  if (!session.isBreakPhase)
  {
    session.totalStudyElapsedSec++;

    if (session.totalStudyElapsedSec >= (unsigned long)session.totalStudyMin * 60UL)
    {
      finishSession(false);
      return;
    }

    if (session.cycleElapsedSec >= (unsigned long)session.cycleStudyMin * 60UL)
    {
      session.isBreakPhase = true;
      session.cycleElapsedSec = 0;
      if (!isFireAlarm)
        Hardware::beep(1, 400);
      WebComm::sendPushNotification("Het chu ky hoc! Hay nghi ngoi thu gian.");
    }
  }
  else
  {
    if (session.cycleElapsedSec >= (unsigned long)session.cycleBreakMin * 60UL)
    {
      session.isBreakPhase = false;
      session.cycleElapsedSec = 0;
      if (!isFireAlarm)
        Hardware::beep(1, 400);
      WebComm::sendPushNotification("Het gio nghi! Quay lai ban hoc tiep nhe.");
    }
  }

  if (!isFireAlarm && currentSubView == VIEW_MAIN)
  {
    Display::showRunning(currentState, session);
  }
}

// =========================================================================
// 7. SETUP & LOOP CHÍNH
// =========================================================================
void setup()
{
  Serial.begin(115200);
  Hardware::init();
  Display::init();
  Serial.println("=== SMART STUDY DESK MONITOR READY ===");
}

void loop()
{
  // 1. Quét phím bấm (tạm khóa thao tác phím khi đang báo cháy khẩn cấp)
  char key = keypad.getKey();
  if (key && !isFireAlarm)
  {
    Serial.printf("[KEYPAD] Phim nhan: %c\n", key);
    if (currentState == STATE_RUNNING || currentState == STATE_PAUSED)
    {
      handleSessionKeys(key);
    }
    else
    {
      handleSetupKeys(key);
    }
    return;
  }

  // 2. Xử lý cảm biến định kỳ 300ms
  unsigned long now = millis();
  if (now >= lastSensorTick && (now - lastSensorTick >= SENSOR_INTERVAL_MS))
  {
    lastSensorTick = now;
    processSensors(now);
  }

  // 3. Đếm giờ mỗi 1000ms khi đang chạy
  now = millis();
  if (currentState == STATE_RUNNING && now >= lastSecondTick && (now - lastSecondTick >= 1000))
  {
    lastSecondTick = now;
    processTimerTick();
  }
}