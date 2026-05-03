# 🤖 Skyro — ESP32 AI Voice Assistant Robot

**Final Year Project | ESP32 + Groq AI + Voice Control**

---

## Project kya hai?

Skyro ek AI-powered robot hai jo voice commands sunta hai, samajhta hai, aur bol ke jawab deta hai.
ESP32 microcontroller ke saath INMP441 mic se awaaz record hoti hai, Python server pe STT hota hai,
Groq AI se smart jawab aata hai, aur MAX98357 speaker pe bolta hai.

---

## Project Structure (Folder layout)

```
skyro-robot/
├── server.py              ← Python Flask server (PC pe chalta hai)
├── requirements.txt       ← Python packages list
├── skyro_esp32/
│   └── skyro_esp32.ino   ← Arduino code (ESP32 pe flash karo)
└── README_HINGLISH.md    ← Yahi file hai
```

---

## Hardware List (Kya kya chahiye)

| Component         | Kaam                              |
|-------------------|-----------------------------------|
| ESP32 DevKit v1   | Main microcontroller              |
| INMP441 Mic       | Voice record karna (I2S)         |
| MAX98357 Amp      | Speaker chalana (I2S)             |
| L298N Motor Driver| DC motors control karna           |
| 2x DC Motor       | Robot chalana                     |
| 2x Servo Motor    | Head/arm movement                 |
| LED x2            | Status indicator                  |

---

## Wiring / Pin Connection

### INMP441 Microphone → ESP32
| Mic Pin | ESP32 Pin |
|---------|-----------|
| SCK     | GPIO 32   |
| WS      | GPIO 25   |
| SD      | GPIO 33   |
| L/R     | GND       |
| VDD     | 3.3V      |
| GND     | GND       |

### MAX98357 Speaker Amplifier → ESP32
| Amp Pin | ESP32 Pin |
|---------|-----------|
| BCLK    | GPIO 26   |
| LRC     | GPIO 12   |  ← 14 nahi, 12 use karo!
| DIN     | GPIO 27   |
| VIN     | 5V        |
| GND     | GND       |

### L298N Motor Driver → ESP32
| L298N Pin | ESP32 Pin |
|-----------|-----------|
| IN1       | GPIO 4    |
| IN2       | GPIO 16   |
| IN3       | GPIO 17   |
| IN4       | GPIO 5    |
| ENA       | GPIO 21   |
| ENB       | GPIO 22   |

### Servo Motors
| Servo   | ESP32 Pin |
|---------|-----------|
| Servo 1 | GPIO 18   |
| Servo 2 | GPIO 19   |

---

## Setup Kaise Kare — Step by Step

### Step 1: Groq API Key lo (Free hai!)

1. Browser mein jao: **https://console.groq.com**
2. Sign up karo (Google account se bhi ho sakta hai)
3. "API Keys" section mein jao
4. "Create API Key" pe click karo
5. Key copy karo — ek baar hi dikhta hai!

---

### Step 2: Python Server Setup (Tumhara PC)

**Python install hai? Check karo:**
```
python --version
```
Python 3.9 ya upar chahiye.

**Dependencies install karo:**
```bash
pip install -r requirements.txt
```

**`server.py` mein apni Groq API Key daalo:**
```python
# Is line ko dhundo:
GROQ_API_KEY = os.getenv("GROQ_API_KEY", "YOUR_GROQ_API_KEY_HERE")
# Aur apni real key daalo:
GROQ_API_KEY = os.getenv("GROQ_API_KEY", "gsk_xxxxxxxxxxxxxxxx")
```

**PC ka IP address pata karo:**
- Windows: `ipconfig` command chalao → "IPv4 Address" dekho
- Mac/Linux: `ifconfig` → inet address dekho
- Example IP: `192.168.1.105`

**Server chalao:**
```bash
python server.py
```

Yeh dikhna chahiye:
```
skyro-server-groq-v3-2026-04-29
Running on http://0.0.0.0:5000
```

**Test karo browser mein:**
```
http://localhost:5000/ask?q=hello
```

---

### Step 3: Arduino Code Setup (ESP32)

**Arduino IDE install karo:** https://www.arduino.cc/en/software

**ESP32 board support add karo:**
1. Arduino IDE kholo
2. File → Preferences jaao
3. "Additional Board Manager URLs" mein ye daalo:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Tools → Board → Board Manager → "esp32" search karo → Install

**Required Libraries install karo (Tools → Manage Libraries):**
- `ESP32Servo` by Kevin Harrington

**`skyro_esp32.ino` mein ye 3 cheezein badlo:**
```cpp
// 1. WiFi name
const char *WIFI_SSID = "TumhareGharKaWiFi";

// 2. WiFi password  
const char *WIFI_PASS = "TumharaPassword";

// 3. PC ka IP address (wahi jo Step 2 mein mila)
const char *STT_SERVER_URL = "http://192.168.1.105:5000/transcribe";
```

**ESP32 pe upload karo:**
1. ESP32 ko USB se connect karo
2. Tools → Board → "ESP32 Dev Module" select karo
3. Tools → Port → Tumhara COM port select karo
4. Upload button (→) dabao
5. Serial Monitor mein dekho — "IP: 192.168.x.x" dikhega

---

## Use Kaise Kare

1. Python server chalao: `python server.py`
2. ESP32 boot hoga aur bolega: *"Hi, I am Skyro. How can I help you?"*
3. Browser mein ESP32 ka IP kholo (Serial Monitor mein milega)
4. **"Record Voice 3 sec"** button dabao aur bolo
5. Robot samjhega, bolega, aur move karega!

**Voice Commands:**
- "Forward / Aage jao" → Robot aage chalega
- "Backward / Peeche" → Robot peeche chalega
- "Left / Baye" → Left turn
- "Right / Daye" → Right turn
- "Stop / Ruk" → Ruk jayega

**Kuch aur bhi puch sakte ho:**
- "2 plus 2 kya hai?" → Math solve karega
- "What is the capital of India?" → AI jawab dega
- "How are you?" → "Main theek hu!"

---

## Common Problems aur Solutions

### ❌ "Could not understand audio"
- Mic ke paas se bolo
- INMP441 ka L/R pin GND se connect hai? Check karo
- `MIC_GAIN_SHIFT` value kam karo (18 se 15 karo)

### ❌ "Could not connect to STT server"
- Python server chal raha hai? `python server.py` check karo
- ESP32 aur PC same WiFi pe hain?
- PC ka IP sahi dala hai `.ino` mein?
- Windows Firewall → Port 5000 allow karo

### ❌ Speaker se awaaz nahi aa rahi
- MAX98357 ka LRC pin GPIO 12 pe hai? (14 galat hai)
- `i2s_driver_install failed` error aa raha hai Serial Monitor mein?

### ❌ Groq API Error
- API key sahi hai? Spaces toh nahi?
- https://console.groq.com pe check karo ki key active hai

---

## Tech Stack

- **Microcontroller:** ESP32 (Dual core, WiFi built-in)
- **AI Model:** Llama 3.3 70B via Groq API
- **STT:** Google Speech Recognition (SpeechRecognition library)
- **TTS:** gTTS (Google Text-to-Speech) + miniaudio
- **Backend:** Python Flask
- **Audio:** I2S protocol (INMP441 mic + MAX98357 amp)
- **Motor Control:** L298N H-Bridge

---

## Future Plans

- [ ] Offline STT (Whisper local model)
- [ ] Face detection (ESP32-CAM)
- [ ] Mobile app control
- [ ] Memory — pichli baatein yaad rakhna
- [ ] Hinglish voice TTS improve karna

---

## Made By

**Final Year Engineering Project — 2026**
Skyro — *"Ek robot jo sunta hai, samajhta hai, aur bolta hai"*

---

*Agar koi problem ho toh GitHub Issues mein batao!*
