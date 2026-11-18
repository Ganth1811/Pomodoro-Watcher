#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ================= 1. ĐỊNH NGHĨA CHÂN PHẦN CỨNG =================
#define PIN_TRIG 5
#define PIN_ECHO 35
#define PIN_LDR 34
#define PIN_POT 32
#define PIN_LED 2
#define PIN_BUZZER 4

// Cấu hình PWM cho đèn trợ sáng (ESP32 Arduino Core 2.x)
#define PWM_CHANNEL 0
#define PWM_FREQ 5000
#define PWM_RESOLUTION 8

// Ngưỡng cảm biến
#define DIST_PRESENT_CM 50      // Khoảng cách nhận diện có người ngồi (< 50cm)
#define LDR_DARK_THRESHOLD 2000 // Giá trị ADC khi trời tối (trên Wokwi kéo thanh Lux thấp xuống)
#define LEAVE_DEBOUNCE_MS 3000  // Rời bàn quá 3 giây mới tính là 1 lần rời bàn

// ================= 2. KHỞI TẠO LCD & KEYPAD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 15};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ================= 3. MÁY TRẠNG THÁI & BIẾN HỆ THỐNG =================
enum SystemState
{
  STATE_IDLE,        // Chưa có người lại gần, tắt màn hình
  STATE_WELCOME,     // Có người lại gần, hiện lời chào chờ bấm #
  STATE_INPUT_INTRO, // Hiện hướng dẫn nhập thông số
  STATE_INPUT_TOTAL, // Nhập tổng thời gian học (phút)
  STATE_INPUT_STUDY, // Nhập độ dài 1 chu kỳ học (phút)
  STATE_INPUT_BREAK, // Nhập độ dài chu kỳ nghỉ (tối đa 10 phút)
  STATE_RUNNING,     // Đang trong quá trình học / nghỉ
  STATE_PAUSED       // Tạm dừng bằng phím A
};

SystemState currentState = STATE_IDLE;

// Thông số thời gian do người dùng nhập (đơn vị: phút)
int totalStudyMinutes = 0;
int cycleStudyMinutes = 0;
int cycleBreakMinutes = 0;
String inputBuffer = "";

// Biến đếm thời gian thực thi (đơn vị: giây)
bool isBreakPhase = false;
unsigned long currentCycleElapsedSec = 0; // Số giây đã trôi qua trong chu kỳ hiện tại
unsigned long totalStudyElapsedSec = 0;   // Tổng số giây ĐÃ HỌC thực tế
unsigned long lastSecondTick = 0;

// Biến theo dõi người dùng & môi trường
unsigned long lastSensorCheck = 0;
bool userPresent = false;
bool wasPresentInStudy = true;
unsigned long absentStartTime = 0;
bool leaveCountedForCurrentAbsence = false;
int leaveDeskCount = 0;

// ================= 4. CÁC HÀM PLACEHOLDER CHO WEB / NODE-RED =================
void webPlaceholder_SendTelemetry(float distCm, int ldrVal, int pwmVal)
{
  Serial.printf("[WEB_PLACEHOLDER - TELEMETRY] {\"state\":%d, \"dist_cm\":%.1f, \"ldr\":%d, \"lamp_pwm\":%d, \"leave_count\":%d}\n",
                currentState, distCm, ldrVal, pwmVal, leaveDeskCount);
}

void webPlaceholder_LogLeaveDesk(int count)
{
  Serial.printf("[WEB_PLACEHOLDER - LEAVE_DESK] Ghi nhan nguoi hoc roi ban! Tong so lan roi ban: %d\n", count);
}

void webPlaceholder_SendPushNotification(const char *message)
{
  Serial.printf("[WEB_PLACEHOLDER - PUSH_NOTIFY] Gui thong bao dien thoai: \"%s\"\n", message);
}

void webPlaceholder_SendCongratEmail(int totalMin, int leaveCount)
{
  Serial.printf("[WEB_PLACEHOLDER - EMAIL] Gui Email chuc mung hoan tat! Tong phut hoc: %d, So lan roi ban: %d\n",
                totalMin, leaveCount);
}

// ================= 5. HÀM TIỆN ÍCH CÒI, ĐÈN & CẢM BIẾN =================
void beepBuzzer(int count, int durationMs, int pauseMs = 150)
{
  for (int i = 0; i < count; i++)
  {
    tone(PIN_BUZZER, 2000); // Tần số 2kHz nghe rõ trên Wokwi và mạch thật
    delay(durationMs);
    noTone(PIN_BUZZER);
    if (i < count - 1)
      delay(pauseMs);
  }
}

float readDistanceCm()
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duration = pulseIn(PIN_ECHO, HIGH, 30000); // Timeout 30ms
  if (duration == 0)
    return 999.0;
  return duration * 0.034 / 2.0;
}

// Xử lý đèn trợ sáng tự động + núm vặn Dimmer biến trở
int updateSmartLamp(bool activeSession)
{
  if (!activeSession)
  {
    analogWrite(PIN_LED, 0);
    return 0;
  }
  int ldrValue = analogRead(PIN_LDR);
  int potValue = analogRead(PIN_POT);

  bool isDark = (ldrValue > LDR_DARK_THRESHOLD);

  if (isDark)
  {
    int pwmVal = map(potValue, 0, 4095, 25, 255);
    analogWrite(PIN_LED, pwmVal);
    return pwmVal;
  }
  else
  {
    analogWrite(PIN_LED, 0);
    return 0;
  }
}

// ================= 6. CÁC HÀM HIỂN THỊ LCD =================
void showWelcomeScreen()
{
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CHAO MUNG DA DEN"); // Tối ưu vừa khít 16 ký tự LCD
  lcd.setCursor(0, 1);
  lcd.print("BAM # DE BAT DAU");
}

void showInputPrompt(const char *title)
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(title);
  lcd.setCursor(0, 1);
  lcd.print("> ");
  lcd.print(inputBuffer);
  lcd.print(" phut");
}

void updateRunningLCD()
{
  lcd.setCursor(0, 0);
  if (currentState == STATE_PAUSED)
  {
    lcd.print("CHU KY: TAM DUNG");
  }
  else if (isBreakPhase)
  {
    lcd.print("CHU KY: NGHI    ");
  }
  else
  {
    lcd.print("CHU KY: DANG HOC");
  }

  unsigned long targetMin = isBreakPhase ? cycleBreakMinutes : cycleStudyMinutes;
  unsigned long curMin = currentCycleElapsedSec / 60;
  unsigned long curSec = currentCycleElapsedSec % 60;

  char line2[17];
  snprintf(line2, sizeof(line2), "  %02lu:%02lu/%02lu:00   ", curMin, curSec, targetMin);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void finishStudySession(bool earlyExit)
{
  analogWrite(PIN_LED, 0); // Tắt đèn trợ sáng
  lcd.clear();
  if (earlyExit)
  {
    lcd.setCursor(0, 0);
    lcd.print("KET THUC SOM!");
    lcd.setCursor(0, 1);
    lcd.print("HEN GAP LAI BAN");
    beepBuzzer(1, 400);
  }
  else
  {
    lcd.setCursor(0, 0);
    lcd.print("HOAN TAT BUOI HOC");
    lcd.setCursor(0, 1);
    lcd.print("CHUC MUNG BAN!");
    // Báo hiệu hoàn tất: kêu 3 tiếng bíp dài + gửi Email chúc mừng
    beepBuzzer(3, 600, 250);
    webPlaceholder_SendCongratEmail(totalStudyMinutes, leaveDeskCount);
  }
  delay(3000);
  lcd.clear();
  lcd.noBacklight();
  currentState = STATE_IDLE;
}

// ================= 7. XỬ LÝ BÀN PHÍM KEYPAD =================
void handleKeypadInput(char key)
{
  if (!key)
    return;

  switch (currentState)
  {
  case STATE_WELCOME:
    if (key == '#')
    {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("NHAP TONG T.GIAN");
      lcd.setCursor(0, 1);
      lcd.print("CHU KY HOC&NGHI");
      delay(1800);
      inputBuffer = "";
      currentState = STATE_INPUT_TOTAL;
      showInputPrompt("1.TONG TG HOC:");
    }
    break;

  case STATE_INPUT_TOTAL:
    if (key >= '0' && key <= '9' && inputBuffer.length() < 3)
    {
      inputBuffer += key;
      showInputPrompt("1.TONG TG HOC:");
    }
    else if (key == '*')
    {
      if (inputBuffer.length() > 0)
      {
        inputBuffer.remove(inputBuffer.length() - 1);
        showInputPrompt("1.TONG TG HOC:");
      }
      else
      {
        currentState = STATE_WELCOME;
        showWelcomeScreen();
      }
    }
    else if (key == '#' && inputBuffer.toInt() > 0)
    {
      totalStudyMinutes = inputBuffer.toInt();
      inputBuffer = "";
      currentState = STATE_INPUT_STUDY;
      showInputPrompt("2.CHU KY HOC:");
    }
    break;

  case STATE_INPUT_STUDY:
    if (key >= '0' && key <= '9' && inputBuffer.length() < 3)
    {
      inputBuffer += key;
      showInputPrompt("2.CHU KY HOC:");
    }
    else if (key == '*')
    {
      if (inputBuffer.length() > 0)
      {
        inputBuffer.remove(inputBuffer.length() - 1);
        showInputPrompt("2.CHU KY HOC:");
      }
      else
      {
        inputBuffer = String(totalStudyMinutes);
        currentState = STATE_INPUT_TOTAL;
        showInputPrompt("1.TONG TG HOC:");
      }
    }
    else if (key == '#' && inputBuffer.toInt() > 0)
    {
      cycleStudyMinutes = inputBuffer.toInt();
      inputBuffer = "";
      currentState = STATE_INPUT_BREAK;
      showInputPrompt("3.THOI GIAN NGHI");
    }
    break;

  case STATE_INPUT_BREAK:
    if (key >= '0' && key <= '9' && inputBuffer.length() < 2)
    {
      inputBuffer += key;
      showInputPrompt("3.THOI GIAN NGHI");
    }
    else if (key == '*')
    {
      if (inputBuffer.length() > 0)
      {
        inputBuffer.remove(inputBuffer.length() - 1);
        showInputPrompt("3.THOI GIAN NGHI");
      }
      else
      {
        inputBuffer = String(cycleStudyMinutes);
        currentState = STATE_INPUT_STUDY;
        showInputPrompt("2.CHU KY HOC:");
      }
    }
    else if (key == '#' && inputBuffer.length() > 0)
    {
      int val = inputBuffer.toInt();
      if (val > 10)
      {
        // Báo lỗi giới hạn nghỉ tối đa 10 phút
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("BAO LOI NHAP:");
        lcd.setCursor(0, 1);
        lcd.print("NGHI TOI DA 10P");
        beepBuzzer(2, 150, 100);
        delay(2000);
        inputBuffer = "";
        showInputPrompt("3.THOI GIAN NGHI");
      }
      else if (val > 0)
      {
        cycleBreakMinutes = val;
        // Kích hoạt hệ thống: bíp 1 tiếng
        beepBuzzer(1, 300);
        isBreakPhase = false;
        currentCycleElapsedSec = 0;
        totalStudyElapsedSec = 0;
        leaveDeskCount = 0;
        wasPresentInStudy = true;
        leaveCountedForCurrentAbsence = false;
        lastSecondTick = millis();
        currentState = STATE_RUNNING;
        lcd.clear();
        updateRunningLCD();
      }
    }
    break;

  case STATE_RUNNING:
    if (key == 'A')
    {
      currentState = STATE_PAUSED;
      updateRunningLCD();
    }
    else if (key == 'B')
    {
      finishStudySession(true);
    }
    break;

  case STATE_PAUSED:
    if (key == 'A')
    {
      lastSecondTick = millis();
      currentState = STATE_RUNNING;
      updateRunningLCD();
    }
    else if (key == 'B')
    {
      finishStudySession(true);
    }
    break;

  default:
    break;
  }
}

// ================= 8. SETUP & LOOP CHÍNH =================
void setup()
{
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_POT, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Cấu hình kênh PWM cho đèn LED trợ sáng
  pinMode(PIN_LED, OUTPUT);
  analogWrite(PIN_LED, 0);
  ledcWrite(PWM_CHANNEL, 0);

  // Khởi tạo LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.noBacklight();
  lcd.clear();

  Serial.println("=== HE THONG TRAM GIAM SAT HOC TAP THONG MINH KHOI DONG ===");
}

void loop()
{
  unsigned long now = millis();

  // 1. Luôn quét bàn phím Keypad
  char key = keypad.getKey();
  handleKeypadInput(key);

  // 2. Đọc cảm biến & cập nhật đèn trợ sáng mỗi 300ms
  if (now - lastSensorCheck >= 300)
  {
    lastSensorCheck = now;
    float dist = readDistanceCm();
    userPresent = (dist < DIST_PRESENT_CM);

    bool isStudyingOrPaused = (currentState == STATE_RUNNING || currentState == STATE_PAUSED);
    int currentPwm = updateSmartLamp(isStudyingOrPaused);

    // Xử lý nhận diện người lại gần khi hệ thống đang nghỉ (IDLE)
    if (currentState == STATE_IDLE && userPresent)
    {
      currentState = STATE_WELCOME;
      showWelcomeScreen();
    }

    // Xử lý phát hiện và đếm số lần rời bàn khi đang trong chu kỳ HỌC
    if (currentState == STATE_RUNNING && !isBreakPhase)
    {
      if (!userPresent)
      {
        if (wasPresentInStudy)
        {
          wasPresentInStudy = false;
          absentStartTime = now;
        }
        else if (!leaveCountedForCurrentAbsence && (now - absentStartTime >= LEAVE_DEBOUNCE_MS))
        {
          leaveDeskCount++;
          leaveCountedForCurrentAbsence = true;
          webPlaceholder_LogLeaveDesk(leaveDeskCount);
        }
      }
      else
      {
        wasPresentInStudy = true;
        leaveCountedForCurrentAbsence = false;
      }
    }

    // Định kỳ mỗi 3 giây in log Telemetry ra Serial (Placeholder cho Node-RED)
    static int telemetryCounter = 0;
    if (++telemetryCounter >= 10)
    {
      telemetryCounter = 0;
      webPlaceholder_SendTelemetry(dist, analogRead(PIN_LDR), currentPwm);
    }
  }

  // 3. Bộ đếm thời gian mỗi 1 giây (1000ms) khi đang chạy
  if (currentState == STATE_RUNNING && (now - lastSecondTick >= 1000))
  {
    lastSecondTick += 1000;
    currentCycleElapsedSec++;

    if (!isBreakPhase)
    {
      totalStudyElapsedSec++;

      // Kiểm tra xem đã hoàn thành TỔNG THỜI GIAN HỌC chưa
      if (totalStudyElapsedSec >= (unsigned long)totalStudyMinutes * 60UL)
      {
        finishStudySession(false);
        return;
      }

      // Kiểm tra xem đã hết 1 CHU KỲ HỌC để chuyển sang CHU KỲ NGHỈ chưa
      if (currentCycleElapsedSec >= (unsigned long)cycleStudyMinutes * 60UL)
      {
        isBreakPhase = true;
        currentCycleElapsedSec = 0;
        beepBuzzer(1, 400);
        webPlaceholder_SendPushNotification("Het chu ky hoc! Hay nghi ngoi thu gian.");
      }
    }
    else
    {
      // Đang trong CHU KỲ NGHỈ -> Kiểm tra hết giờ nghỉ để quay lại CHU KỲ HỌC
      if (currentCycleElapsedSec >= (unsigned long)cycleBreakMinutes * 60UL)
      {
        isBreakPhase = false;
        currentCycleElapsedSec = 0;
        beepBuzzer(1, 400);
        webPlaceholder_SendPushNotification("Het gio nghi! Quay lai ban hoc tiep nhe.");
      }
    }

    updateRunningLCD();
  }
}