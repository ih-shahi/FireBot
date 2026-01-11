/*******************************************************************************************
  ESP32 DevKit V1: Fire Sensors + Pump + Laser + Camera Preview + Dual Servo Pan/Tilt UI

  WHAT YOU GET
  ============
  MAIN PAGE "/"
   - Flame indicator grid (10 sensors)
   - Camera preview panel
       * Resizable (drag bottom-right corner)
       * Rotate 0/90/180/270 degrees
       * Fullscreen button
   - Pump control
       * Speed slider (0..255) with ramp safety
       * Pump ON and OFF buttons (fast stop / resume last speed)
   - Servo control
       * X axis (SG90) : X+ / X- / ZERO
       * Y axis (MG996R): Y+ / Y- / ZERO
       * Smooth motion and limits
   - Laser toggle
   - Live logs (web) + all logs also printed on Serial Monitor

  SETTINGS PAGE "/settings"
   - Set camera URL (saved in NVS)
   - Toggle pump direction (only when speed is 0; saved)
   - Servo configuration preview (current angles)

  IMPORTANT HARDWARE NOTES
  ========================
  1) COMMON GROUND:
     If servos have separate 5V supply, connect servo GND to ESP32 GND.

  2) SERVO POWER:
     SG90 and MG996R must NOT be powered from ESP32 3.3V.
     Use a dedicated 5V supply (MG996R can draw high current).

  3) SIGNAL LEVEL:
     ESP32 outputs 3.3V PWM. Most servos accept it fine.
     If MG996R is unstable, use a transistor buffer/level shifter.

  4) FLAME SENSOR LOGIC:
     You said sensors give 3.3V HIGH on fire -> ACTIVE HIGH.
     If ever inverted, set FLAME_ACTIVE_LOW to true.

*******************************************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ESP32 LEDC PWM is used both for BTS7960 motor PWM and for servos.
// We use separate LEDC channels for everything.

// =========================================================================================
//                                USER CONFIG
// =========================================================================================

// ---------------- Wi-Fi credentials ----------------
const char* WIFI_SSID = "Fire";
const char* WIFI_PASS = "12345678";

// ---------------- Flame Sensors (10 digital inputs) ----------------
static const int FLAME_COUNT = 10;
const bool FLAME_ACTIVE_LOW = false;    // ACTIVE HIGH (3.3V = fire)

int flamePins[FLAME_COUNT] = {
  34, 35, 36, 39, 32, 33, 25, 26, 27, 14
};

// NOTE: GPIO 34/35/36/39 have no internal pull resistors.
// If you ever get noise again, use external pulldown resistors (10k to GND).
const bool FLAME_USE_PULLUP = false;

// ---------------- Laser ----------------
const int LASER_PIN = 23;  // Use MOSFET/transistor

// ---------------- BTS7960 Motor Driver ----------------
const int RPWM_PIN = 18;
const int LPWM_PIN = 19;
const int REN_PIN  = 21;
const int LEN_PIN  = 22;

// Motor PWM settings
const int MOTOR_PWM_FREQ = 20000;  // 20 kHz
const int MOTOR_PWM_RES  = 8;      // 0..255
const int MOTOR_RPWM_CH  = 0;
const int MOTOR_LPWM_CH  = 1;

// Motor ramp safety
const int RAMP_STEP = 5;
const int RAMP_DELAY_MS = 10;
const int REVERSE_LOCKOUT_MS = 200;

// ---------------- Servo Pins ----------------
// Servo X (SG90) and Servo Y (MG996R)
const int SERVO_X_PIN = 16;  // safe pin
const int SERVO_Y_PIN = 17;  // safe pin

// LEDC channels for servo PWM
const int SERVO_X_CH = 2;
const int SERVO_Y_CH = 3;

// Servo PWM parameters (50 Hz typical)
const int SERVO_PWM_FREQ = 50;      // 50 Hz
const int SERVO_PWM_RES  = 16;      // 16-bit resolution for better pulse precision

// Servo pulse width range in microseconds.
// SG90 commonly ~500-2400 us, MG996R ~500-2500 us.
// You can tune these if endpoints are off.
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2500;

// Servo travel limits in degrees (protect mechanics)
const int SERVO_MIN_DEG = 0;
const int SERVO_MAX_DEG = 180;

// Servo motion step (degrees per click and degrees per smooth step)
const int SERVO_CLICK_STEP_DEG = 5;     // button press moves this many degrees
const int SERVO_SMOOTH_STEP_DEG = 1;    // smooth movement increment
const int SERVO_SMOOTH_DELAY_MS = 10;   // delay between smooth steps

// Default "zero" position for each servo
// Choose what "zero" means in your mechanical setup.
const int SERVO_X_ZERO_DEG = 90;
const int SERVO_Y_ZERO_DEG = 90;

// ---------------- Logs ----------------
const int LOG_CAPACITY = 250;
const unsigned long FLAME_EVENT_DEBOUNCE_MS = 600;

// =========================================================================================
//                                GLOBAL STATE
// =========================================================================================

WebServer server(80);
Preferences prefs;

// Persistent settings (NVS)
String g_cameraUrl = "";
int    g_direction = 1;  // 1 forward, -1 reverse

// Runtime states
bool g_laserOn = false;

// Motor state
int  g_currentSpeedSigned = 0;     // applied speed, signed (-255..255)
int  g_lastSpeedAbs = 0;           // last requested abs speed (0..255)
bool g_pumpEnabled = true;         // ON/OFF button state

// Flame states
bool g_flameState[FLAME_COUNT] = {false};
bool g_flamePrev[FLAME_COUNT]  = {false};
unsigned long g_lastFlameEventMs[FLAME_COUNT] = {0};

// Servo states
int g_servoX_deg = SERVO_X_ZERO_DEG;
int g_servoY_deg = SERVO_Y_ZERO_DEG;

// Logs
String g_logs[LOG_CAPACITY];
unsigned long g_logCountTotal = 0;

// =========================================================================================
//                                LOGGING
// =========================================================================================

void logEvent(const String& msg) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms] ");
  Serial.println(msg);

  g_logs[g_logCountTotal % LOG_CAPACITY] = msg;
  g_logCountTotal++;
}

bool getLogAt(unsigned long absIndex, String &out) {
  if (g_logCountTotal == 0) return false;
  unsigned long oldest = (g_logCountTotal > LOG_CAPACITY) ? (g_logCountTotal - LOG_CAPACITY) : 0;
  if (absIndex < oldest) return false;
  if (absIndex >= g_logCountTotal) return false;
  out = g_logs[absIndex % LOG_CAPACITY];
  return true;
}

// =========================================================================================
//                                FLAME INPUT
// =========================================================================================

bool readFlameDetected(int idx) {
  int v = digitalRead(flamePins[idx]);
  if (FLAME_ACTIVE_LOW) return (v == LOW);
  return (v == HIGH);
}

void updateFlameStates() {
  for (int i = 0; i < FLAME_COUNT; i++) {
    bool detected = readFlameDetected(i);
    g_flameState[i] = detected;

    if (detected != g_flamePrev[i]) {
      unsigned long now = millis();
      if (now - g_lastFlameEventMs[i] > FLAME_EVENT_DEBOUNCE_MS) {
        g_lastFlameEventMs[i] = now;
        g_flamePrev[i] = detected;
        if (detected) logEvent("🔥 Flame detected on sensor #" + String(i + 1));
        else          logEvent("✅ Flame cleared on sensor #" + String(i + 1));
      }
    }
  }
}

bool anyFlame() {
  for (int i = 0; i < FLAME_COUNT; i++) {
    if (g_flameState[i]) return true;
  }
  return false;
}

// =========================================================================================
//                                MOTOR (BTS7960)
// =========================================================================================

void motorApplySigned(int spdSigned) {
  spdSigned = constrain(spdSigned, -255, 255);

  digitalWrite(REN_PIN, HIGH);
  digitalWrite(LEN_PIN, HIGH);

  if (!g_pumpEnabled) {
    // If pump is OFF, force PWM = 0
    ledcWrite(MOTOR_RPWM_CH, 0);
    ledcWrite(MOTOR_LPWM_CH, 0);
    return;
  }

  if (spdSigned > 0) {
    ledcWrite(MOTOR_RPWM_CH, spdSigned);
    ledcWrite(MOTOR_LPWM_CH, 0);
  } else if (spdSigned < 0) {
    ledcWrite(MOTOR_RPWM_CH, 0);
    ledcWrite(MOTOR_LPWM_CH, -spdSigned);
  } else {
    ledcWrite(MOTOR_RPWM_CH, 0);
    ledcWrite(MOTOR_LPWM_CH, 0);
  }
}

void motorSetFadeSigned(int targetSigned) {
  targetSigned = constrain(targetSigned, -255, 255);

  bool dirChange = ((g_currentSpeedSigned > 0 && targetSigned < 0) ||
                    (g_currentSpeedSigned < 0 && targetSigned > 0));

  if (dirChange) {
    logEvent("↔️ Motor direction change: ramping to 0 first");
    while (g_currentSpeedSigned != 0) {
      if (g_currentSpeedSigned > 0) g_currentSpeedSigned -= RAMP_STEP;
      else                          g_currentSpeedSigned += RAMP_STEP;

      if (abs(g_currentSpeedSigned) < RAMP_STEP) g_currentSpeedSigned = 0;
      motorApplySigned(g_currentSpeedSigned);
      delay(RAMP_DELAY_MS);
    }
    delay(REVERSE_LOCKOUT_MS);
  }

  while (g_currentSpeedSigned != targetSigned) {
    if (g_currentSpeedSigned < targetSigned) g_currentSpeedSigned += RAMP_STEP;
    else                                     g_currentSpeedSigned -= RAMP_STEP;

    if (abs(targetSigned - g_currentSpeedSigned) < RAMP_STEP)
      g_currentSpeedSigned = targetSigned;

    motorApplySigned(g_currentSpeedSigned);
    delay(RAMP_DELAY_MS);
  }
}

void setSpeedFromUi(int speedAbs) {
  speedAbs = constrain(speedAbs, 0, 255);
  g_lastSpeedAbs = speedAbs;

  int targetSigned = speedAbs * g_direction;

  logEvent("🧯 Pump speed command: " + String(speedAbs) +
           " (dir=" + String(g_direction == 1 ? "FWD" : "REV") + ") " +
           (g_pumpEnabled ? "[ENABLED]" : "[DISABLED]"));

  motorSetFadeSigned(targetSigned);
}

void pumpOffImmediate() {
  logEvent("🛑 Pump OFF (immediate)");
  g_pumpEnabled = false;
  motorApplySigned(0); // force stop
}

void pumpOnResume() {
  logEvent("▶️ Pump ON (resume last speed=" + String(g_lastSpeedAbs) + ")");
  g_pumpEnabled = true;
  setSpeedFromUi(g_lastSpeedAbs);
}

// =========================================================================================
//                                LASER
// =========================================================================================

void setLaser(bool on) {
  g_laserOn = on;
  digitalWrite(LASER_PIN, on ? HIGH : LOW);
  logEvent(String("🔦 Laser ") + (on ? "ON" : "OFF"));
}

void toggleLaser() {
  setLaser(!g_laserOn);
}

// =========================================================================================
//                                SERVO CONTROL
// =========================================================================================

// Convert microseconds to LEDC duty ticks for 16-bit at 50Hz
// Duty ticks = (pulse_us / period_us) * (2^resolution - 1)
uint32_t usToDutyTicks(int pulse_us) {
  const uint32_t maxDuty = (1UL << SERVO_PWM_RES) - 1; // 65535 for 16-bit
  const float period_us = 1000000.0f / SERVO_PWM_FREQ; // 20000 us at 50 Hz
  float duty = (pulse_us / period_us) * maxDuty;
  if (duty < 0) duty = 0;
  if (duty > maxDuty) duty = maxDuty;
  return (uint32_t)duty;
}

int degToPulseUs(int deg) {
  deg = constrain(deg, SERVO_MIN_DEG, SERVO_MAX_DEG);
  // linear map
  long us = map(deg, SERVO_MIN_DEG, SERVO_MAX_DEG, SERVO_MIN_US, SERVO_MAX_US);
  return (int)us;
}

void servoWriteDeg(int channel, int pin, int deg) {
  int pulse = degToPulseUs(deg);
  uint32_t duty = usToDutyTicks(pulse);
  ledcWrite(channel, duty);
}

void servoInit() {
  // Setup LEDC for servos
  ledcSetup(SERVO_X_CH, SERVO_PWM_FREQ, SERVO_PWM_RES);
  ledcSetup(SERVO_Y_CH, SERVO_PWM_FREQ, SERVO_PWM_RES);
  ledcAttachPin(SERVO_X_PIN, SERVO_X_CH);
  ledcAttachPin(SERVO_Y_PIN, SERVO_Y_CH);

  // Go to zero on startup
  g_servoX_deg = SERVO_X_ZERO_DEG;
  g_servoY_deg = SERVO_Y_ZERO_DEG;

  servoWriteDeg(SERVO_X_CH, SERVO_X_PIN, g_servoX_deg);
  servoWriteDeg(SERVO_Y_CH, SERVO_Y_PIN, g_servoY_deg);

  logEvent("🎛️ Servos initialized: X(GPIO16)=" + String(g_servoX_deg) +
           "°, Y(GPIO17)=" + String(g_servoY_deg) + "°");
}

// Smooth move helper
void servoMoveSmooth(int &currentDeg, int targetDeg, int channel, int pin, const String& name) {
  targetDeg = constrain(targetDeg, SERVO_MIN_DEG, SERVO_MAX_DEG);
  if (targetDeg == currentDeg) return;

  logEvent("🎯 Servo " + name + " move: " + String(currentDeg) + "° -> " + String(targetDeg) + "°");

  while (currentDeg != targetDeg) {
    if (currentDeg < targetDeg) currentDeg += SERVO_SMOOTH_STEP_DEG;
    else                        currentDeg -= SERVO_SMOOTH_STEP_DEG;

    // snap when close
    if (abs(targetDeg - currentDeg) < SERVO_SMOOTH_STEP_DEG) currentDeg = targetDeg;

    servoWriteDeg(channel, pin, currentDeg);
    delay(SERVO_SMOOTH_DELAY_MS);
  }
}

void servoStepX(int delta) {
  int target = g_servoX_deg + delta;
  servoMoveSmooth(g_servoX_deg, target, SERVO_X_CH, SERVO_X_PIN, "X");
}

void servoStepY(int delta) {
  int target = g_servoY_deg + delta;
  servoMoveSmooth(g_servoY_deg, target, SERVO_Y_CH, SERVO_Y_PIN, "Y");
}

void servoZeroX() {
  servoMoveSmooth(g_servoX_deg, SERVO_X_ZERO_DEG, SERVO_X_CH, SERVO_X_PIN, "X");
}

void servoZeroY() {
  servoMoveSmooth(g_servoY_deg, SERVO_Y_ZERO_DEG, SERVO_Y_CH, SERVO_Y_PIN, "Y");
}

// =========================================================================================
//                                NVS
// =========================================================================================

void loadPreferences() {
  prefs.begin("app", false);
  g_cameraUrl = prefs.getString("camUrl", "");
  g_direction = prefs.getInt("dir", 1);
  if (g_direction != 1 && g_direction != -1) g_direction = 1;

  logEvent("⚙️ Preferences loaded: dir=" + String(g_direction) +
           ", camUrl=" + (g_cameraUrl.length() ? g_cameraUrl : "(empty)"));
}

void saveCameraUrl(const String& url) {
  g_cameraUrl = url;
  prefs.putString("camUrl", g_cameraUrl);
  logEvent("📷 Camera URL saved: " + g_cameraUrl);
}

void saveDirection(int dir) {
  g_direction = (dir >= 0) ? 1 : -1;
  prefs.putInt("dir", g_direction);
  logEvent(String("↔️ Direction saved: ") + (g_direction == 1 ? "Forward" : "Reverse"));
}

// =========================================================================================
//                                JSON HELPERS
// =========================================================================================

String jsonEscape(const String& s) {
  String out = s;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  return out;
}

String buildStatusJson() {
  String json = "{";
  json += "\"flames\":[";
  for (int i = 0; i < FLAME_COUNT; i++) {
    json += (g_flameState[i] ? "true" : "false");
    if (i < FLAME_COUNT - 1) json += ",";
  }
  json += "],";
  json += "\"anyFlame\":" + String(anyFlame() ? "true" : "false") + ",";
  json += "\"laser\":" + String(g_laserOn ? "true" : "false") + ",";
  json += "\"direction\":" + String(g_direction) + ",";
  json += "\"speedAbs\":" + String(abs(g_currentSpeedSigned)) + ",";
  json += "\"pumpEnabled\":" + String(g_pumpEnabled ? "true" : "false") + ",";
  json += "\"servoX\":" + String(g_servoX_deg) + ",";
  json += "\"servoY\":" + String(g_servoY_deg) + ",";
  json += "\"uptimeMs\":" + String(millis());
  json += "}";
  return json;
}

String buildLogsJson(unsigned long sinceIndex, int maxItems) {
  if (maxItems < 1) maxItems = 1;
  if (maxItems > 40) maxItems = 40;

  unsigned long nextIndex = sinceIndex;

  String json = "{";
  String items = "\"items\":[";
  bool first = true;

  int sent = 0;
  for (unsigned long i = sinceIndex; i < g_logCountTotal && sent < maxItems; i++) {
    String line;
    if (!getLogAt(i, line)) break;

    if (!first) items += ",";
    first = false;

    items += "{";
    items += "\"i\":" + String(i) + ",";
    items += "\"msg\":\"" + jsonEscape(line) + "\"";
    items += "}";

    nextIndex = i + 1;
    sent++;
  }
  items += "]";

  json += "\"next\":" + String(nextIndex) + ",";
  json += items;
  json += "}";

  return json;
}

// =========================================================================================
//                                WEB: MAIN PAGE "/"
// =========================================================================================

void handleMainPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  // Styles include camera rotation & resize handle
  server.sendContent(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>ESP32 Control Panel</title>"
    "<style>"
    "body{font-family:Arial;margin:14px;background:#fafafa;color:#111;}"
    ".topbar{display:flex;justify-content:space-between;align-items:center;gap:10px;}"
    ".linkbtn{padding:8px 12px;border-radius:10px;border:1px solid #999;background:#fff;text-decoration:none;color:#111;font-weight:700;}"
    ".wrap{max-width:1050px;margin:0 auto;}"
    ".card{background:#fff;border:1px solid #ddd;border-radius:14px;padding:14px;margin-top:14px;box-shadow:0 1px 2px rgba(0,0,0,.05);}"
    ".title{font-size:18px;font-weight:800;margin-bottom:10px;}"
    ".row{display:flex;gap:14px;flex-wrap:wrap;align-items:center;}"
    ".dots{display:grid;grid-template-columns:repeat(10,1fr);gap:10px;}"
    ".dot{width:28px;height:28px;border-radius:999px;background:#9aa0a6;border:2px solid #666;}"
    ".ok{background:#16a34a;border-color:#0f7a35;}"
    ".alarm{background:#ef4444;border-color:#b91c1c;animation:blink .7s infinite;}"
    "@keyframes blink{0%{opacity:1;}50%{opacity:.2;}100%{opacity:1;}}"
    ".btn{padding:10px 14px;border-radius:12px;border:1px solid #444;background:#f2f2f2;cursor:pointer;font-weight:800;}"
    ".btnPrimary{background:#e8f0fe;border-color:#1a73e8;}"
    ".btnDanger{background:#fee2e2;border-color:#ef4444;}"
    ".small{color:#555;font-size:13px;}"
    ".status{font-weight:900;}"
    "input[type=range]{width:320px;}"
    ".logBox{height:220px;overflow:auto;background:#0b1020;color:#d6e1ff;border-radius:12px;padding:10px;font-family:monospace;font-size:12px;white-space:pre-wrap;}"
    ".pill{display:inline-block;padding:4px 10px;border-radius:999px;border:1px solid #ccc;background:#f8f8f8;font-size:12px;font-weight:800;}"
    ".overlayWrap{position:relative;width:100%;height:420px;border-radius:12px;overflow:hidden;border:1px solid #ddd;background:#111;resize:both;}"
    ".overlayWrap::after{content:'';position:absolute;right:6px;bottom:6px;width:14px;height:14px;border-right:3px solid rgba(255,255,255,.6);border-bottom:3px solid rgba(255,255,255,.6);pointer-events:none;}"
    "iframe{width:100%;height:100%;border:0;transform-origin:center center;}"
    ".overlayNote{position:absolute;top:10px;left:10px;background:rgba(0,0,0,.55);color:#fff;padding:6px 10px;border-radius:10px;font-size:13px;z-index:5;}"
    ".camTools{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;}"
    "</style></head><body><div class='wrap'>"
  );

  server.sendContent(
    "<div class='topbar'>"
    "<div><span class='pill'>ESP32 DevKit V1</span> <span class='pill' id='ipPill'>IP: ...</span> <span class='pill' id='upPill'>Uptime: ...</span></div>"
    "<div><a class='linkbtn' href='/settings'>Settings</a></div>"
    "</div>"
  );

  // Flame
  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Flame Sensors (Digital)</div>"
    "<div class='dots' id='dots'></div>"
    "<p class='small'>Blinking red = flame detected.</p>"
    "<p class='status'>Overall: <span id='overall'>---</span></p>"
    "</div>"
  );

  // Camera preview with rotate + fullscreen + resizable
  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Camera Preview</div>"
    "<div class='small'>Loads from saved URL. Rotate/fullscreen below. Resize by dragging bottom-right corner.</div>"
    "<div class='camTools'>"
      "<button class='btn btnPrimary' onclick='rot(0)'>0°</button>"
      "<button class='btn btnPrimary' onclick='rot(90)'>90°</button>"
      "<button class='btn btnPrimary' onclick='rot(180)'>180°</button>"
      "<button class='btn btnPrimary' onclick='rot(270)'>270°</button>"
      "<button class='btn' onclick='fullCam()'>Fullscreen</button>"
    "</div>"
    "<div style='margin-top:10px;' class='overlayWrap' id='camWrap'>"
      "<div class='overlayNote'>Camera Preview</div>"
      "<iframe id='camFrame' src=''></iframe>"
    "</div>"
    "</div>"
  );

  // Controls
  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Pump + Laser</div>"
    "<div class='row'>"

      "<div>"
        "<div><b>Pump Speed:</b> <span id='speedVal'>0</span> / 255</div>"
        "<input id='speed' type='range' min='0' max='255' value='0'/>"
        "<div class='small'>Ramped speed changes.</div>"
        "<div class='row' style='margin-top:8px;'>"
          "<button class='btn btnPrimary' onclick='pumpOn()'>Pump ON</button>"
          "<button class='btn btnDanger' onclick='pumpOff()'>Pump OFF</button>"
          "<span class='small'>State: <b id='pumpState'>---</b></span>"
        "</div>"
      "</div>"

      "<div>"
        "<div><b>Direction:</b> <span id='dirVal'>---</span></div>"
        "<div class='small'>Change direction in <a href='/settings'>Settings</a> (only at speed 0).</div>"
      "</div>"

      "<div>"
        "<div><b>Laser:</b> <span id='laserVal'>---</span></div>"
        "<button class='btn btnDanger' onclick='toggleLaser()'>Laser ON/OFF</button>"
      "</div>"

    "</div></div>"
  );

  // Servo controls
  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Servo Controls</div>"
    "<div class='small'>X servo (SG90) and Y servo (MG996R). Use separate 5V power and common GND.</div>"

    "<div class='row' style='margin-top:10px;'>"
      "<div>"
        "<div><b>X Axis (SG90):</b> <span id='sx'>---</span>°</div>"
        "<div class='row' style='margin-top:6px;'>"
          "<button class='btn btnPrimary' onclick='sxMinus()'>X −</button>"
          "<button class='btn btnPrimary' onclick='sxPlus()'>X +</button>"
          "<button class='btn' onclick='sxZero()'>X ZERO</button>"
        "</div>"
      "</div>"

      "<div>"
        "<div><b>Y Axis (MG996R):</b> <span id='sy'>---</span>°</div>"
        "<div class='row' style='margin-top:6px;'>"
          "<button class='btn btnPrimary' onclick='syMinus()'>Y −</button>"
          "<button class='btn btnPrimary' onclick='syPlus()'>Y +</button>"
          "<button class='btn' onclick='syZero()'>Y ZERO</button>"
        "</div>"
      "</div>"
    "</div>"
    "</div>"
  );

  // Logs
  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Live Logs</div>"
    "<div class='small'>Commands and flame events. Also printed to Serial Monitor.</div>"
    "<div class='logBox' id='logBox'></div>"
    "<div class='row' style='margin-top:10px;'>"
      "<button class='btn btnPrimary' onclick='clearLogView()'>Clear View</button>"
      "<span class='small'>Ring buffer: " + String(LOG_CAPACITY) + " lines</span>"
    "</div>"
    "</div>"
  );

  // JS
  server.sendContent(
    "<script>"
    "const dotsEl=document.getElementById('dots');"
    "const dotEls=[];"
    "for(let i=0;i<10;i++){const d=document.createElement('div');d.className='dot';d.title='Sensor '+(i+1);dotsEl.appendChild(d);dotEls.push(d);}"
    "const camFrame=document.getElementById('camFrame');"
    "const camWrap=document.getElementById('camWrap');"
    "const speedEl=document.getElementById('speed');"
    "const speedValEl=document.getElementById('speedVal');"
    "const overallEl=document.getElementById('overall');"
    "const laserValEl=document.getElementById('laserVal');"
    "const dirValEl=document.getElementById('dirVal');"
    "const pumpState=document.getElementById('pumpState');"
    "const logBox=document.getElementById('logBox');"
    "const ipPill=document.getElementById('ipPill');"
    "const upPill=document.getElementById('upPill');"
    "const sx=document.getElementById('sx');"
    "const sy=document.getElementById('sy');"
    "let logNext=0;"
    "let camRot=0;"

    "function loadCameraOnce(){fetch('/camUrl').then(r=>r.text()).then(url=>{url=(url||'').trim();if(url){camFrame.src=url;}}).catch(()=>{});}"

    "function rot(deg){camRot=deg;camFrame.style.transform='rotate('+deg+'deg)';}"
    "function fullCam(){if(camWrap.requestFullscreen) camWrap.requestFullscreen();}"

    "speedEl.addEventListener('input',()=>{speedValEl.textContent=speedEl.value;});"
    "speedEl.addEventListener('change',()=>{const v=parseInt(speedEl.value,10);fetch('/setSpeed?value='+encodeURIComponent(v),{method:'POST'}).catch(()=>{});});"

    "function pumpOn(){fetch('/pumpOn',{method:'POST'}).catch(()=>{});}"
    "function pumpOff(){fetch('/pumpOff',{method:'POST'}).catch(()=>{});}"
    "function toggleLaser(){fetch('/toggleLaser',{method:'POST'}).catch(()=>{});}"

    "function sxPlus(){fetch('/servoX?delta=+'+encodeURIComponent(" + String(SERVO_CLICK_STEP_DEG) + "),{method:'POST'}).catch(()=>{});}"
    "function sxMinus(){fetch('/servoX?delta=-'+encodeURIComponent(" + String(SERVO_CLICK_STEP_DEG) + "),{method:'POST'}).catch(()=>{});}"
    "function sxZero(){fetch('/servoXzero',{method:'POST'}).catch(()=>{});}"

    "function syPlus(){fetch('/servoY?delta=+'+encodeURIComponent(" + String(SERVO_CLICK_STEP_DEG) + "),{method:'POST'}).catch(()=>{});}"
    "function syMinus(){fetch('/servoY?delta=-'+encodeURIComponent(" + String(SERVO_CLICK_STEP_DEG) + "),{method:'POST'}).catch(()=>{});}"
    "function syZero(){fetch('/servoYzero',{method:'POST'}).catch(()=>{});}"

    "function clearLogView(){logBox.textContent='';}"
    "function appendLog(line){logBox.textContent += line + '\\n';logBox.scrollTop = logBox.scrollHeight;}"

    "function pollStatus(){"
      "fetch('/status').then(r=>r.json()).then(st=>{"
        "let any=false;"
        "for(let i=0;i<10;i++){const on=!!st.flames[i]; if(on) any=true; dotEls[i].className='dot '+(on?'alarm':'ok');}"
        "overallEl.textContent = any ? 'FLAME DETECTED!' : 'OK';"
        "overallEl.style.color = any ? '#b91c1c' : '#0f7a35';"
        "laserValEl.textContent = st.laser ? 'ON' : 'OFF';"
        "laserValEl.style.color = st.laser ? '#b91c1c' : '#111';"
        "dirValEl.textContent = (st.direction===1) ? 'Forward' : 'Reverse';"
        "speedEl.value = st.speedAbs; speedValEl.textContent = st.speedAbs;"
        "pumpState.textContent = st.pumpEnabled ? 'ON' : 'OFF';"
        "sx.textContent = st.servoX; sy.textContent = st.servoY;"
        "upPill.textContent = 'Uptime: ' + Math.floor(st.uptimeMs/1000) + 's';"
      "}).catch(()=>{}).finally(()=>setTimeout(pollStatus,300));"
    "}"

    "function pollLogs(){"
      "fetch('/logs?since='+encodeURIComponent(logNext)).then(r=>r.json()).then(d=>{"
        "if(typeof d.next === 'number') logNext=d.next;"
        "if(d.items && d.items.length){for(const it of d.items){appendLog(it.msg);}}"
      "}).catch(()=>{}).finally(()=>setTimeout(pollLogs,450));"
    "}"

    "ipPill.textContent='IP: '+window.location.host;"
    "loadCameraOnce();"
    "pollStatus();"
    "pollLogs();"
    "</script>"
  );

  server.sendContent("</div></body></html>");
  server.sendContent("");
}

// =========================================================================================
//                                WEB: SETTINGS "/settings"
// =========================================================================================

void handleSettingsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>Settings</title>"
    "<style>"
    "body{font-family:Arial;margin:14px;background:#fafafa;color:#111;}"
    ".wrap{max-width:1050px;margin:0 auto;}"
    ".card{background:#fff;border:1px solid #ddd;border-radius:14px;padding:14px;margin-top:14px;box-shadow:0 1px 2px rgba(0,0,0,.05);}"
    ".title{font-size:18px;font-weight:800;margin-bottom:10px;}"
    ".row{display:flex;gap:14px;flex-wrap:wrap;align-items:center;}"
    ".btn{padding:10px 14px;border-radius:12px;border:1px solid #444;background:#f2f2f2;cursor:pointer;font-weight:800;}"
    ".btnPrimary{background:#e8f0fe;border-color:#1a73e8;}"
    ".small{color:#555;font-size:13px;}"
    "input[type=text]{width:100%;padding:10px;border-radius:12px;border:1px solid #ccc;}"
    ".overlayWrap{position:relative;width:100%;height:420px;border-radius:12px;overflow:hidden;border:1px solid #ddd;background:#111;}"
    "iframe{width:100%;height:100%;border:0;}"
    ".overlayNote{position:absolute;top:10px;left:10px;background:rgba(0,0,0,.55);color:#fff;padding:6px 10px;border-radius:10px;font-size:13px;}"
    ".linkbtn{padding:8px 12px;border-radius:10px;border:1px solid #999;background:#fff;text-decoration:none;color:#111;font-weight:700;}"
    "</style></head><body><div class='wrap'>"
    "<div class='row' style='justify-content:space-between;'>"
      "<div><a class='linkbtn' href='/'>← Back</a></div>"
      "<div class='small'>Saved in NVS</div>"
    "</div>"
  );

  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Direction (Startup Setting)</div>"
    "<div class='small'>Direction change allowed only when speed is 0.</div>"
    "<div class='row' style='margin-top:10px;'>"
      "<div><b>Current:</b> <span id='dirText'>...</span></div>"
      "<button class='btn btnPrimary' onclick='toggleDir()'>Toggle Direction</button>"
      "<span class='small' id='dirMsg'></span>"
    "</div>"
    "</div>"
  );

  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Camera URL</div>"
    "<div class='small'>Paste URL and Save. Preview loads only when saved.</div>"
    "<div class='row' style='width:100%; margin-top:10px;'>"
      "<div style='flex:1;'><input id='camUrl' type='text' placeholder='http://...'/></div>"
      "<div><button class='btn btnPrimary' onclick='saveCamUrl()'>Save</button></div>"
      "<span class='small' id='camMsg'></span>"
    "</div>"
    "<div style='margin-top:10px;' class='overlayWrap'>"
      "<div class='overlayNote'>Camera Preview</div>"
      "<iframe id='camFrame' src=''></iframe>"
    "</div>"
    "</div>"
  );

  server.sendContent(
    "<div class='card'>"
    "<div class='title'>Servo Status</div>"
    "<div class='small'>Current angles from device status.</div>"
    "<div class='row'>"
      "<div><b>X:</b> <span id='sx'>---</span>°</div>"
      "<div><b>Y:</b> <span id='sy'>---</span>°</div>"
    "</div>"
    "</div>"
  );

  server.sendContent(
    "<script>"
    "const dirText=document.getElementById('dirText');"
    "const dirMsg=document.getElementById('dirMsg');"
    "const camUrlEl=document.getElementById('camUrl');"
    "const camFrame=document.getElementById('camFrame');"
    "const camMsg=document.getElementById('camMsg');"
    "const sx=document.getElementById('sx');"
    "const sy=document.getElementById('sy');"

    "function refreshAll(){"
      "fetch('/status').then(r=>r.json()).then(st=>{"
        "dirText.textContent=(st.direction===1)?'Forward':'Reverse';"
        "sx.textContent=st.servoX; sy.textContent=st.servoY;"
      "}).catch(()=>{});"
    "}"

    "function toggleDir(){"
      "dirMsg.textContent='...';"
      "fetch('/toggleDir',{method:'POST'})"
        ".then(r=>r.text())"
        ".then(t=>{dirMsg.textContent=t||'OK'; refreshAll();})"
        ".catch(()=>{dirMsg.textContent='Error';});"
    "}"

    "function loadCamUrlOnce(){"
      "fetch('/camUrl').then(r=>r.text()).then(url=>{"
        "url=(url||'').trim();"
        "camUrlEl.value=url;"
        "camFrame.src=url;"
      "}).catch(()=>{});"
    "}"

    "function saveCamUrl(){"
      "const url=camUrlEl.value.trim();"
      "camMsg.textContent='Saving...';"
      "fetch('/setCamUrl',{method:'POST',headers:{'Content-Type':'text/plain'},body:url})"
        ".then(r=>r.text())"
        ".then(t=>{camMsg.textContent=t||'Saved'; camFrame.src=url;})"
        ".catch(()=>{camMsg.textContent='Error';});"
    "}"

    "refreshAll();"
    "loadCamUrlOnce();"
    "</script>"
  );

  server.sendContent("</div></body></html>");
  server.sendContent("");
}

// =========================================================================================
//                                API ROUTES
// =========================================================================================

void handleStatus() {
  updateFlameStates();
  server.send(200, "application/json", buildStatusJson());
}

// Pump speed
void handleSetSpeed() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing value");
    return;
  }
  int v = server.arg("value").toInt();
  v = constrain(v, 0, 255);
  setSpeedFromUi(v);
  server.send(200, "text/plain", "OK");
}

// Pump ON/OFF routes
void handlePumpOff() {
  pumpOffImmediate();
  server.send(200, "text/plain", "Pump OFF");
}
void handlePumpOn() {
  pumpOnResume();
  server.send(200, "text/plain", "Pump ON");
}

// Laser
void handleToggleLaser() {
  toggleLaser();
  server.send(200, "text/plain", "OK");
}

// Direction (only when motor speed is 0)
void handleToggleDir() {
  if (abs(g_currentSpeedSigned) > 0) {
    logEvent("❌ Direction change rejected (pump not stopped)");
    server.send(200, "text/plain", "Stop pump (speed 0) before changing direction.");
    return;
  }
  saveDirection(-g_direction);
  server.send(200, "text/plain", String("Direction now ") + (g_direction == 1 ? "Forward" : "Reverse"));
}

// Camera URL
void handleSetCamUrl() {
  String body = server.arg("plain");
  body.trim();
  saveCameraUrl(body);
  server.send(200, "text/plain", "Saved");
}

void handleGetCamUrl() {
  server.send(200, "text/plain", g_cameraUrl);
}

// Logs
void handleLogs() {
  unsigned long since = 0;
  int maxItems = 20;

  if (server.hasArg("since")) since = (unsigned long) server.arg("since").toInt();
  if (server.hasArg("max"))   maxItems = server.arg("max").toInt();

  unsigned long oldest = (g_logCountTotal > LOG_CAPACITY) ? (g_logCountTotal - LOG_CAPACITY) : 0;
  if (since < oldest) since = oldest;

  server.send(200, "application/json", buildLogsJson(since, maxItems));
}

// Servo API
// POST /servoX?delta=+5 or -5
void handleServoXDelta() {
  int delta = 0;
  if (server.hasArg("delta")) delta = server.arg("delta").toInt();
  if (delta == 0) { server.send(400, "text/plain", "delta required"); return; }

  servoStepX(delta);
  server.send(200, "text/plain", "OK");
}

// POST /servoY?delta=+5 or -5
void handleServoYDelta() {
  int delta = 0;
  if (server.hasArg("delta")) delta = server.arg("delta").toInt();
  if (delta == 0) { server.send(400, "text/plain", "delta required"); return; }

  servoStepY(delta);
  server.send(200, "text/plain", "OK");
}

// POST /servoXzero
void handleServoXZero() {
  servoZeroX();
  server.send(200, "text/plain", "OK");
}

// POST /servoYzero
void handleServoYZero() {
  servoZeroY();
  server.send(200, "text/plain", "OK");
}

// =========================================================================================
//                                WIFI + SETUP
// =========================================================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println();
      Serial.println("WiFi connect timeout. Restarting...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  logEvent("📡 WiFi connected: " + WiFi.localIP().toString());
}

void setupPins() {
  // Flame inputs
  for (int i = 0; i < FLAME_COUNT; i++) {
    if (FLAME_USE_PULLUP) pinMode(flamePins[i], INPUT_PULLUP);
    else                  pinMode(flamePins[i], INPUT);

    g_flameState[i] = false;
    g_flamePrev[i]  = false;
    g_lastFlameEventMs[i] = 0;
  }

  // Laser
  pinMode(LASER_PIN, OUTPUT);
  digitalWrite(LASER_PIN, LOW);
  g_laserOn = false;

  // Motor enable pins
  pinMode(REN_PIN, OUTPUT);
  pinMode(LEN_PIN, OUTPUT);
  digitalWrite(REN_PIN, HIGH);
  digitalWrite(LEN_PIN, HIGH);

  // Motor PWM (LEDC)
  ledcSetup(MOTOR_RPWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcSetup(MOTOR_LPWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcAttachPin(RPWM_PIN, MOTOR_RPWM_CH);
  ledcAttachPin(LPWM_PIN, MOTOR_LPWM_CH);

  g_currentSpeedSigned = 0;
  g_lastSpeedAbs = 0;
  g_pumpEnabled = true;
  motorApplySigned(0);

  logEvent("🔧 Pins initialized.");
  logEvent("Flame pins: 34,35,36,39,32,33,25,26,27,14 (ACTIVE HIGH)");
  logEvent("Motor pins: RPWM=18 LPWM=19 REN=21 LEN=22");
  logEvent("Laser pin: GPIO23");
  logEvent("Servo pins: X=GPIO16 (SG90), Y=GPIO17 (MG996R)");
}

void setupRoutes() {
  // Pages
  server.on("/", HTTP_GET, handleMainPage);
  server.on("/settings", HTTP_GET, handleSettingsPage);

  // Status/logs
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/logs", HTTP_GET, handleLogs);

  // Motor
  server.on("/setSpeed", HTTP_POST, handleSetSpeed);
  server.on("/pumpOff", HTTP_POST, handlePumpOff);
  server.on("/pumpOn", HTTP_POST, handlePumpOn);
  server.on("/toggleDir", HTTP_POST, handleToggleDir);

  // Laser
  server.on("/toggleLaser", HTTP_POST, handleToggleLaser);

  // Camera URL
  server.on("/setCamUrl", HTTP_POST, handleSetCamUrl);
  server.on("/camUrl", HTTP_GET, handleGetCamUrl);

  // Servo
  server.on("/servoX", HTTP_POST, handleServoXDelta);
  server.on("/servoY", HTTP_POST, handleServoYDelta);
  server.on("/servoXzero", HTTP_POST, handleServoXZero);
  server.on("/servoYzero", HTTP_POST, handleServoYZero);

  // 404
  server.onNotFound([]() {
    String msg = "404 Not Found\n\nTry:\n";
    msg += "  /         (main)\n";
    msg += "  /settings (settings)\n";
    msg += "  /status   (json)\n";
    msg += "  /logs     (json)\n";
    server.send(404, "text/plain", msg);
  });

  logEvent("🌐 HTTP routes configured.");
}

// =========================================================================================
//                                SETUP / LOOP
// =========================================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  logEvent("Boot: ESP32 Master Controller starting...");

  loadPreferences();
  setupPins();

  // init servos after pins
  servoInit();

  connectWiFi();

  setupRoutes();
  server.begin();

  logEvent("✅ Web server started: http://" + WiFi.localIP().toString() + "/");
  logEvent("✅ Settings:          http://" + WiFi.localIP().toString() + "/settings");
}

void loop() {
  server.handleClient();

  // Keep flame sensing active even if UI not open
  static unsigned long lastFlamePoll = 0;
  unsigned long now = millis();
  if (now - lastFlamePoll > 120) {
    lastFlamePoll = now;
    updateFlameStates();
  }
}
