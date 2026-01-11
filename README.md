# ESP32 Fire Monitor + Pump + Laser + Camera + Dual-Servo Web Controller

A single-firmware ESP32 project that hosts a **web control panel** to monitor **10 flame sensors**, control a **DC pump (BTS7960)** with speed/direction and safety ramping, toggle a **laser**, display an external **camera preview**, and move a **dual-servo pan/tilt** system using a **joystick UI** (press-and-hold) or **direct angle inputs**.

---

## ✨ Features

### 🔥 Flame Monitoring (10 Sensors)
- Reads **10 digital flame/IR sensors**
- UI shows **10 circular indicators**
- Any triggered sensor **blinks red**
- Status + events appear in:
  - Web “Live Logs”
  - Serial Monitor logs

### 🚰 Pump Control (BTS7960 / 775 DC Motor)
- **Speed slider** (0–255) with **fade/ramp** to reduce stress
- **Pump ON / OFF buttons**
  - OFF: immediate stop
  - ON: resumes last speed
- **Direction** change is locked to safe behavior
  - Direction toggle available in **/settings**
  - Only allowed when speed is **0**

### 🔦 Laser
- Web button toggles laser output
- Recommended: drive laser via MOSFET/transistor (GPIO should not power laser directly)

### 📷 Camera Preview
- Web page embeds a camera stream/webpage via iframe
- **Rotation controls** (0/90/180/270)
- **Fullscreen**
- **Resizable panel**
- The camera iframe is designed to remain **stable** (not refresh on every poll)

### 🎛️ Dual Servo Control (Pan/Tilt)
- Supports:
  - **SG90 micro servo** (X axis)
  - **MG996R high torque servo** (Y axis)
- Web UI provides:
  - **Joystick (4 buttons)**: Up/Down/Left/Right
  - **Press-and-hold** continuous movement
  - **Exact angle inputs** for X and Y
  - **Zero** for each axis + optional “Zero both”

### 🧾 Live Logs
- Ring buffer log system shown on web UI
- Logs also printed to Serial Monitor (debugging + record)

---

## 🧩 Hardware Used

- **ESP32 DevKit V1 (ESP-WROOM-32)**
- **10x flame sensors (digital output)**
- **BTS7960 motor driver module**
- **775 brushed DC motor / pump**
- **SG90** micro servo (X axis)
- **MG996R** high torque servo (Y axis)
- Laser module (via MOSFET recommended)
- External camera feed (any IP cam / ESP32-CAM page / stream URL)

---

## 🔌 Wiring / Pin Map (ESP32 DevKit V1)

### Flame sensors (digital inputs)
34, 35, 36, 39, 32, 33, 25, 26, 27, 14

### BTS7960
RPWM=18, LPWM=19, REN=21, LEN=22

### Laser
GPIO 23

### Servos
SG90 (X) = GPIO16  
MG996R (Y) = GPIO17

---

## ⚠️ Power & Safety Notes
- Use **external 5V** for servos
- **Common GND required**
- Use MOSFET for laser
- Use proper PSU for motor current

---

## 🌐 Web Pages
- `/` Main Dashboard
- `/settings` Configuration

---

## ✅ License
MIT (recommended)
