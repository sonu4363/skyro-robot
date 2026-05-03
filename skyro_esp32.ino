#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include "driver/i2s.h"

// ---------- WiFi ----------
const char *WIFI_SSID = "YOUR_WIFI_NAME";        // <-- Apna WiFi naam likho
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";    // <-- Apna WiFi password likho
const char *FIRMWARE_VERSION = "skyro-tts-all-output-v1-2026-04-29";

// Python server endpoint — apne PC ka IP daalo
const char *STT_SERVER_URL = "http://YOUR_PC_IP:5000/transcribe";

// ---------- Pins ----------
// INMP441 microphone  ->  I2S_NUM_0
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_SCK_PIN     32      // BCLK
#define MIC_WS_PIN      25      // WS / LR
#define MIC_SD_PIN      33      // DATA IN
#define MIC_CHANNEL_FORMAT I2S_CHANNEL_FMT_ONLY_LEFT

// MAX98357 speaker  ->  I2S_NUM_1
#define SPK_I2S_PORT    I2S_NUM_1
#define SPK_BCLK_PIN    26      // BCLK
#define SPK_LRC_PIN     12      // WS/LRC  (GPIO 12, NOT 14)
#define SPK_DIN_PIN     27      // DATA OUT

// L298N motor driver
#define IN1_PIN 4
#define IN2_PIN 16
#define IN3_PIN 17
#define IN4_PIN 5
#define ENA_PIN 21
#define ENB_PIN 22

// Servos
#define SERVO1_PIN 18
#define SERVO2_PIN 19

// Optional LEDs
#define LED1_PIN 15
#define LED2_PIN 2

// Audio settings
#define SAMPLE_RATE      16000
#define RECORD_SECONDS   3
#define BITS_PER_SAMPLE  16
#define VOICE_MOVE_MS    1200
#define MIC_GAIN_SHIFT   18

WebServer server(80);
Servo servo1;
Servo servo2;

int servo1Angle = 90;
int servo2Angle = 90;
unsigned long voiceMoveStopAt = 0;
int currentSpeakerRate = 0;
bool speakerInstalled = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 Assistant Robot</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 18px; background:#f7f7f7; color:#111; }
    h2 { margin: 0 0 14px; }
    .pad { display:grid; grid-template-columns:80px 80px 80px; gap:10px; width:260px; }
    button { height:54px; border:0; border-radius:8px; background:#1f6feb; color:white; font-size:15px; font-weight:700; }
    button.stop { background:#d1242f; }
    button.record { width:260px; background:#238636; margin-top:16px; }
    .row { margin:16px 0; }
    input { width:210px; }
    pre { min-height:70px; background:white; padding:12px; border-radius:8px; white-space:pre-wrap; }
  </style>
</head>
<body>
  <h2>ESP32 Assistant Robot</h2>
  <div class="pad">
    <div></div>
    <button onpointerdown="cmd('forward')" onpointerup="cmd('stop')" onpointerleave="cmd('stop')">Forward</button>
    <div></div>
    <button onpointerdown="cmd('left')" onpointerup="cmd('stop')" onpointerleave="cmd('stop')">Left</button>
    <button class="stop" onclick="cmd('stop')">Stop</button>
    <button onpointerdown="cmd('right')" onpointerup="cmd('stop')" onpointerleave="cmd('stop')">Right</button>
    <div></div>
    <button onpointerdown="cmd('backward')" onpointerup="cmd('stop')" onpointerleave="cmd('stop')">Backward</button>
    <div></div>
  </div>
  <div class="row">
    Servo 1: <input type="range" min="0" max="180" value="90" oninput="servo(1,this.value)">
  </div>
  <div class="row">
    Servo 2: <input type="range" min="0" max="180" value="90" oninput="servo(2,this.value)">
  </div>
  <button class="record" onclick="recordVoice()">Record Voice 3 sec</button>
  <pre id="out">Ready</pre>
  <script>
    async function cmd(m) {
      document.getElementById('out').textContent = 'Command: ' + m;
      await fetch('/cmd?m=' + encodeURIComponent(m));
    }
    async function servo(n, angle) {
      await fetch('/servo?n=' + n + '&angle=' + angle);
    }
    async function recordVoice() {
      const out = document.getElementById('out');
      out.textContent = 'Recording 3 seconds... speak now';
      try {
        const res = await fetch('/record');
        const text = await res.text();
        out.textContent = text;
      } catch (e) {
        out.textContent = 'Record failed: ' + e;
      }
    }
  </script>
</body>
</html>
)rawliteral";

void setMotorPins(bool in1, bool in2, bool in3, bool in4) {
  digitalWrite(IN1_PIN, in1); digitalWrite(IN2_PIN, in2);
  digitalWrite(IN3_PIN, in3); digitalWrite(IN4_PIN, in4);
}
void motorStop()     { setMotorPins(LOW,  LOW,  LOW,  LOW);  }
void motorForward()  { setMotorPins(HIGH, LOW,  HIGH, LOW);  }
void motorBackward() { setMotorPins(LOW,  HIGH, LOW,  HIGH); }
void motorLeft()     { setMotorPins(LOW,  HIGH, HIGH, LOW);  }
void motorRight()    { setMotorPins(HIGH, LOW,  LOW,  HIGH); }

void handleCommand() {
  String m = server.arg("m");
  Serial.print("Remote command: "); Serial.println(m);
  voiceMoveStopAt = 0;
  if      (m == "forward")  motorForward();
  else if (m == "backward") motorBackward();
  else if (m == "left")     motorLeft();
  else if (m == "right")    motorRight();
  else                      motorStop();
  server.send(200, "text/plain", String("OK: ") + m);
}

void handleServo() {
  int n     = server.arg("n").toInt();
  int angle = constrain(server.arg("angle").toInt(), 0, 180);
  if (n == 1) { servo1Angle = angle; servo1.write(servo1Angle); }
  else if (n == 2) { servo2Angle = angle; servo2.write(servo2Angle); }
  Serial.printf("Servo %d angle: %d\n", n, angle);
  server.send(200, "text/plain", "OK");
}

String readResponseField(const String &response, const char *fieldName) {
  int start = response.indexOf(fieldName);
  if (start < 0) return "";
  start += strlen(fieldName);
  while (start < (int)response.length() &&
         (response[start] == ' ' || response[start] == '\t')) start++;
  int end = response.indexOf('\n', start);
  if (end < 0) end = response.length();
  String value = response.substring(start, end);
  value.replace("\r", ""); value.trim();
  return value;
}

void executeVoiceCommand(const String &response) {
  String command = readResponseField(response, "COMMAND:");
  command.toLowerCase();
  if (command.length() == 0) return;
  Serial.print("Voice command detected: "); Serial.println(command);
  if      (command == "forward")  { motorForward();  voiceMoveStopAt = millis() + VOICE_MOVE_MS; }
  else if (command == "backward") { motorBackward(); voiceMoveStopAt = millis() + VOICE_MOVE_MS; }
  else if (command == "left")     { motorLeft();     voiceMoveStopAt = millis() + VOICE_MOVE_MS; }
  else if (command == "right")    { motorRight();    voiceMoveStopAt = millis() + VOICE_MOVE_MS; }
  else if (command == "stop")     { motorStop();     voiceMoveStopAt = 0; }
}

void stopVoiceMoveIfDue() {
  if (voiceMoveStopAt != 0 && (long)(millis() - voiceMoveStopAt) >= 0) {
    motorStop(); voiceMoveStopAt = 0;
    Serial.println("Voice move complete: stop");
  }
}

String serverBaseUrl() {
  String url = STT_SERVER_URL;
  int s = url.indexOf('/', 7);
  if (s >= 0) url = url.substring(0, s);
  return url;
}

String urlEncode(const String &value) {
  const char *hex = "0123456789ABCDEF";
  String encoded = "";
  for (int i = 0; i < (int)value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') {
      encoded += (char)c;
    } else if (c == ' ') { encoded += "%20";
    } else { encoded += '%'; encoded += hex[(c>>4)&0x0F]; encoded += hex[c&0x0F]; }
  }
  return encoded;
}

String buildSpeakText(const String &response) {
  String say = readResponseField(response, "SAY:");
  if (say.length() > 0) return say;
  String text = response;
  text.replace("\r", " "); text.replace("\n", " "); text.trim();
  while (text.indexOf("  ") >= 0) text.replace("  ", " ");
  if (text.length() > 260) text = text.substring(0, 260);
  return text;
}

uint16_t readLE16(const uint8_t *d) { return (uint16_t)d[0]|((uint16_t)d[1]<<8); }
uint32_t readLE32(const uint8_t *d) {
  return (uint32_t)d[0]|((uint32_t)d[1]<<8)|((uint32_t)d[2]<<16)|((uint32_t)d[3]<<24);
}

bool readExact(WiFiClient &client, uint8_t *buf, size_t len, uint32_t timeoutMs=15000) {
  size_t got = 0; unsigned long started = millis();
  while (got < len) {
    int avail = client.available();
    if (avail > 0) { int n = client.read(buf+got, len-got); if (n>0){got+=n;started=millis();} }
    else { if (!client.connected()&&!client.available()) return false;
           if (millis()-started>timeoutMs) return false; delay(1); }
    stopVoiceMoveIfDue();
  }
  return true;
}

bool skipBytes(WiFiClient &client, uint32_t len) {
  uint8_t temp[64];
  while (len > 0) { size_t n=len>sizeof(temp)?sizeof(temp):len; if(!readExact(client,temp,n))return false; len-=n; }
  return true;
}

bool setupSpeakerI2S(int sampleRate) {
  if (speakerInstalled && currentSpeakerRate == sampleRate) return true;
  if (speakerInstalled) { i2s_driver_uninstall(SPK_I2S_PORT); speakerInstalled=false; delay(20); }

  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX),
    .sample_rate          = (uint32_t)sampleRate,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8, .dma_buf_len = 512,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pin_config = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = SPK_BCLK_PIN,
    .ws_io_num    = SPK_LRC_PIN,
    .data_out_num = SPK_DIN_PIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  esp_err_t err = i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) { Serial.printf("speaker i2s_driver_install failed: %d\n", err); return false; }
  err = i2s_set_pin(SPK_I2S_PORT, &pin_config);
  if (err != ESP_OK) { Serial.printf("speaker i2s_set_pin failed: %d\n", err); i2s_driver_uninstall(SPK_I2S_PORT); return false; }

  currentSpeakerRate = sampleRate; speakerInstalled = true;
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  Serial.printf("Speaker I2S ready: %d Hz on BCLK=%d LRC=%d DIN=%d\n", sampleRate, SPK_BCLK_PIN, SPK_LRC_PIN, SPK_DIN_PIN);
  return true;
}

bool parseWavHeader(WiFiClient &client, uint16_t &channels, uint32_t &sampleRate, uint16_t &bitsPerSample, uint32_t &dataSize) {
  uint8_t riff[12];
  if (!readExact(client, riff, sizeof(riff))) return false;
  if (memcmp(riff,"RIFF",4)!=0||memcmp(riff+8,"WAVE",4)!=0) return false;
  bool foundFmt=false; channels=sampleRate=bitsPerSample=dataSize=0;
  while (client.connected()||client.available()) {
    uint8_t chunkHeader[8]; if (!readExact(client,chunkHeader,sizeof(chunkHeader))) return false;
    uint32_t chunkSize=readLE32(chunkHeader+4);
    if (memcmp(chunkHeader,"fmt ",4)==0) {
      uint8_t fmt[32]={0}; uint32_t toRead=chunkSize<sizeof(fmt)?chunkSize:sizeof(fmt);
      if (!readExact(client,fmt,toRead)) return false;
      if (chunkSize>toRead&&!skipBytes(client,chunkSize-toRead)) return false;
      uint16_t audioFormat=readLE16(fmt); channels=readLE16(fmt+2);
      sampleRate=readLE32(fmt+4); bitsPerSample=readLE16(fmt+14);
      foundFmt=(audioFormat==1);
    } else if (memcmp(chunkHeader,"data",4)==0) { dataSize=chunkSize; return foundFmt; }
    else { if (!skipBytes(client,chunkSize)) return false; }
    if (chunkSize&1){if (!skipBytes(client,1)) return false;}
  }
  return false;
}

bool playTtsText(const String &text) {
  String speak=text; speak.trim(); if (speak.length()==0) return false;
  String base=serverBaseUrl(); if (!base.startsWith("http://")) { Serial.println("TTS only supports http://"); return false; }
  String url=base; url.remove(0,7);
  int colonIndex=url.indexOf(':'); String host=url; int port=80;
  if (colonIndex>=0){host=url.substring(0,colonIndex);port=url.substring(colonIndex+1).toInt();}
  String path=String("/tts?text=")+urlEncode(speak);
  WiFiClient client; client.setTimeout(30000);
  Serial.print("TTS speak: "); Serial.println(speak);
  if (!client.connect(host.c_str(),port)){Serial.println("TTS server connect failed");return false;}
  client.print(String("GET ")+path+" HTTP/1.1\r\n");
  client.print(String("Host: ")+host+"\r\n"); client.print("Connection: close\r\n\r\n");
  String statusLine=client.readStringUntil('\n'); statusLine.trim();
  if (!statusLine.startsWith("HTTP/1.1 200")&&!statusLine.startsWith("HTTP/1.0 200")){
    Serial.print("TTS HTTP failed: ");Serial.println(statusLine);client.stop();return false;}
  while (client.connected()||client.available()){String line=client.readStringUntil('\n');if(line=="\r"||line.length()==0)break;}
  uint16_t channels; uint32_t sampleRate; uint16_t bitsPerSample; uint32_t dataSize;
  if (!parseWavHeader(client,channels,sampleRate,bitsPerSample,dataSize)){Serial.println("Could not parse TTS WAV header");client.stop();return false;}
  if (bitsPerSample!=16||(channels!=1&&channels!=2)){Serial.printf("Unsupported WAV: ch=%u bits=%u\n",channels,bitsPerSample);client.stop();return false;}
  if (!setupSpeakerI2S(sampleRate)){client.stop();return false;}
  uint8_t inBuf[512]; int16_t stereoBuf[512]; uint32_t remaining=dataSize;
  while (remaining>0) {
    size_t toRead=remaining>sizeof(inBuf)?sizeof(inBuf):remaining;
    if (!readExact(client,inBuf,toRead)){Serial.println("TTS stream ended early");client.stop();return false;}
    remaining-=toRead; size_t bytesToWrite=toRead; uint8_t *writePtr=inBuf;
    if (channels==1){int samples=toRead/2;for(int i=0;i<samples;i++){int16_t s=(int16_t)((uint16_t)inBuf[i*2]|((uint16_t)inBuf[i*2+1]<<8));stereoBuf[i*2]=s;stereoBuf[i*2+1]=s;}writePtr=(uint8_t*)stereoBuf;bytesToWrite=samples*4;}
    size_t written=0; i2s_write(SPK_I2S_PORT,writePtr,bytesToWrite,&written,portMAX_DELAY);
    stopVoiceMoveIfDue();
  }
  client.stop(); i2s_zero_dma_buffer(SPK_I2S_PORT); return true;
}

bool setupMicI2S() {
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = MIC_CHANNEL_FORMAT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8, .dma_buf_len = 512,
    .use_apll             = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = MIC_SCK_PIN,
    .ws_io_num    = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD_PIN
  };
  esp_err_t err=i2s_driver_install(MIC_I2S_PORT,&i2s_config,0,NULL);
  if (err!=ESP_OK){Serial.printf("mic i2s_driver_install failed: %d\n",err);return false;}
  err=i2s_set_pin(MIC_I2S_PORT,&pin_config);
  if (err!=ESP_OK){Serial.printf("mic i2s_set_pin failed: %d\n",err);return false;}
  i2s_zero_dma_buffer(MIC_I2S_PORT);
  Serial.printf("Mic I2S ready: SCK=%d WS=%d SD=%d\n",MIC_SCK_PIN,MIC_WS_PIN,MIC_SD_PIN);
  return true;
}

void writeWavHeader(uint8_t *header, uint32_t dataSize) {
  uint32_t fileSize=dataSize+36; uint32_t byteRate=SAMPLE_RATE*1*BITS_PER_SAMPLE/8; uint16_t blockAlign=1*BITS_PER_SAMPLE/8;
  memcpy(header,"RIFF",4); header[4]=fileSize&0xff;header[5]=(fileSize>>8)&0xff;header[6]=(fileSize>>16)&0xff;header[7]=(fileSize>>24)&0xff;
  memcpy(header+8,"WAVE",4);memcpy(header+12,"fmt ",4);header[16]=16;header[17]=0;header[18]=0;header[19]=0;
  header[20]=1;header[21]=0;header[22]=1;header[23]=0;
  header[24]=SAMPLE_RATE&0xff;header[25]=(SAMPLE_RATE>>8)&0xff;header[26]=(SAMPLE_RATE>>16)&0xff;header[27]=(SAMPLE_RATE>>24)&0xff;
  header[28]=byteRate&0xff;header[29]=(byteRate>>8)&0xff;header[30]=(byteRate>>16)&0xff;header[31]=(byteRate>>24)&0xff;
  header[32]=blockAlign&0xff;header[33]=(blockAlign>>8)&0xff;header[34]=BITS_PER_SAMPLE;header[35]=0;
  memcpy(header+36,"data",4);header[40]=dataSize&0xff;header[41]=(dataSize>>8)&0xff;header[42]=(dataSize>>16)&0xff;header[43]=(dataSize>>24)&0xff;
}

bool writeAll(WiFiClient &client, const uint8_t *data, size_t len, uint32_t timeoutMs=10000) {
  size_t sent=0; unsigned long lastWrite=millis();
  while (sent<len){size_t written=client.write(data+sent,len-sent);if(written>0){sent+=written;lastWrite=millis();continue;}if(!client.connected())return false;if(millis()-lastWrite>timeoutMs)return false;delay(1);}
  return true;
}

String recordAndSendToServer() {
  const int totalSamples=SAMPLE_RATE*RECORD_SECONDS; const uint32_t audioDataSize=totalSamples*2; const uint32_t contentLength=44+audioDataSize;
  String url=STT_SERVER_URL; if (!url.startsWith("http://")) return "Only http:// STT_SERVER_URL supported";
  url.remove(0,7); int slashIndex=url.indexOf('/'); String hostPort=slashIndex>=0?url.substring(0,slashIndex):url;
  String path=slashIndex>=0?url.substring(slashIndex):"/"; int port=80; int colonIndex=hostPort.indexOf(':');
  String host=hostPort; if (colonIndex>=0){host=hostPort.substring(0,colonIndex);port=hostPort.substring(colonIndex+1).toInt();}
  WiFiClient client; Serial.printf("Connecting to STT %s:%d\n",host.c_str(),port);
  if (!client.connect(host.c_str(),port)) return "Could not connect to STT server";
  client.setTimeout(20000);
  client.print(String("POST ")+path+" HTTP/1.1\r\n"); client.print(String("Host: ")+host+"\r\n");
  client.print("Content-Type: audio/wav\r\n"); client.print("Content-Length: "); client.print(contentLength); client.print("\r\n"); client.print("Connection: close\r\n\r\n");
  uint8_t wavHeader[44]; writeWavHeader(wavHeader,audioDataSize);
  if (!writeAll(client,wavHeader,sizeof(wavHeader))){client.stop();return "Header send failed";}
  Serial.println("Recording..."); digitalWrite(LED1_PIN,HIGH);
  int32_t rawSamples[256]; uint8_t pcmBytes[512]; int sampleIndex=0;
  int32_t rawMin=2147483647,rawMax=-2147483647; int16_t pcmPeak=0;
  while (sampleIndex<totalSamples) {
    size_t bytesRead=0; esp_err_t err=i2s_read(MIC_I2S_PORT,rawSamples,sizeof(rawSamples),&bytesRead,portMAX_DELAY);
    if (err!=ESP_OK||bytesRead==0) continue;
    int samplesRead=bytesRead/sizeof(int32_t); int pcmByteCount=0;
    for (int i=0;i<samplesRead&&sampleIndex<totalSamples;i++){
      if (rawSamples[i]<rawMin) rawMin=rawSamples[i]; if (rawSamples[i]>rawMax) rawMax=rawSamples[i];
      int32_t amplified=rawSamples[i]>>MIC_GAIN_SHIFT; if(amplified>32767)amplified=32767; if(amplified<-32768)amplified=-32768;
      int16_t pcm=(int16_t)amplified; int16_t absPcm=pcm<0?-pcm:pcm; if(absPcm>pcmPeak)pcmPeak=absPcm;
      pcmBytes[pcmByteCount++]=pcm&0xff; pcmBytes[pcmByteCount++]=(pcm>>8)&0xff; sampleIndex++;
    }
    if (pcmByteCount>0){if(!writeAll(client,pcmBytes,pcmByteCount)){digitalWrite(LED1_PIN,LOW);client.stop();return "PCM send failed";}}
  }
  digitalWrite(LED1_PIN,LOW);
  Serial.printf("Mic: rawMin=%ld rawMax=%ld pcmPeak=%d\n",(long)rawMin,(long)rawMax,pcmPeak);
  Serial.println("Recording done. Waiting for STT...");
  unsigned long timeout=millis();
  while (client.connected()&&!client.available()){if(millis()-timeout>60000){client.stop();return "STT timeout";}delay(10);}
  String response=client.readString(); client.stop();
  int bodyIndex=response.indexOf("\r\n\r\n"); if(bodyIndex>=0) response=response.substring(bodyIndex+4);
  response.trim(); Serial.print("STT response: "); Serial.println(response);
  return response;
}

void handleRecord() {
  motorStop();
  String text=recordAndSendToServer();
  executeVoiceCommand(text);
  String speak=buildSpeakText(text);
  if (speak.length()>0) playTtsText(speak);
  server.send(200,"text/plain",text);
}

void setup() {
  Serial.begin(115200); delay(1000);
  pinMode(IN1_PIN,OUTPUT);pinMode(IN2_PIN,OUTPUT);pinMode(IN3_PIN,OUTPUT);pinMode(IN4_PIN,OUTPUT);
  pinMode(ENA_PIN,OUTPUT);pinMode(ENB_PIN,OUTPUT);pinMode(LED1_PIN,OUTPUT);pinMode(LED2_PIN,OUTPUT);
  digitalWrite(ENA_PIN,HIGH);digitalWrite(ENB_PIN,HIGH); motorStop();
  servo1.attach(SERVO1_PIN);servo2.attach(SERVO2_PIN);servo1.write(servo1Angle);servo2.write(servo2Angle);
  Serial.println("\n=== ESP32 Robot Boot ===");
  Serial.print("Firmware: ");Serial.println(FIRMWARE_VERSION);
  Serial.printf("Speaker pins: BCLK=%d LRC=%d DIN=%d\n",SPK_BCLK_PIN,SPK_LRC_PIN,SPK_DIN_PIN);
  Serial.printf("Mic    pins: SCK=%d  WS=%d  SD=%d\n",MIC_SCK_PIN,MIC_WS_PIN,MIC_SD_PIN);
  if (!setupMicI2S()) Serial.println("WARNING: Mic I2S failed.");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println(); Serial.print("IP: ");Serial.println(WiFi.localIP());
  server.on("/",       [](){server.send_P(200,"text/html",INDEX_HTML);});
  server.on("/cmd",    handleCommand);
  server.on("/servo",  handleServo);
  server.on("/record", handleRecord);
  server.begin();
  Serial.println("Server started. Open IP in browser.");
  playTtsText("Hi, I am Skyro. How can I help you?");
}

void loop() {
  server.handleClient();
  stopVoiceMoveIfDue();
}
