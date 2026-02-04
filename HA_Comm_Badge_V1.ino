/*
 * M5StickC PLUS2 - Home Assistant Voice Control v1.0
 * 
 * ⚡ STARFLEET COMM BADGE
 * https://sthi.space
 * Chief Engineer: Spanner
 * Based on Shay Moradi's awesome M5Stick with OpenAI Access project: https://github.com/organised/
 * COMMANDS:
 *   • Hold front button 5 sec: Enter config mode (anytime)
 *   • Hold front button during reset: Enter config mode
 *   • Enter your WiFi credentials, Home Assistant details and AI provider's API key in the config
 *   • Tap device: Record voice command
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>

// ========== HARDWARE PIN DEFINITIONS ==========

const uint8_t BUTTON_A_PIN = 37;  // M5StickC PLUS2 front button (active LOW)

// ========== CONFIGURATION STORAGE ==========

struct DeviceConfig {
  char wifiSsid[64];
  char wifiPass[64];
  char llmApiKey[128];     // Unified API key field (replaces groqApiKey)
  char llmUrl[128];        // Full API URL (editable)
  char llmProvider[16];    // "groq", "openai", "custom"
  char haUrl[128];
  char haToken[256];
  float tapThreshold;      // 2.0-4.5
  uint8_t recordTime;      // MAX recording duration (safety ceiling)
  uint8_t sleepTimeout;    // Deep sleep timeout (seconds)
  uint8_t vadSensitivity;  // 1-10 (1=very sensitive to silence, 10=less sensitive)
  bool configured;
} config;

Preferences prefs;

// ========== CONSTANTS ==========

const float DEFAULT_TAP_THRESHOLD = 3.4;
const uint8_t DEFAULT_RECORD_TIME = 4;    // Max recording duration (seconds)
const uint8_t DEFAULT_SLEEP_TIMEOUT = 60; // Deep sleep after 60s idle
const uint8_t DEFAULT_VAD_SENSITIVITY = 5; // 1-10

// MPU6886 registers
#define MPU6886_ADDRESS         0x68
#define MPU6886_PWR_MGMT_1      0x6B
#define MPU6886_PWR_MGMT_2      0x6C
#define MPU6886_ACCEL_CONFIG    0x1C
#define MPU6886_ACCEL_CONFIG2   0x1D
#define MPU6886_INT_ENABLE      0x38
#define MPU6886_INT_STATUS      0x3A
#define MPU6886_ACCEL_INTEL_CTRL 0x69
#define MPU6886_SMPLRT_DIV      0x19
#define MPU6886_WOM_THR_X       0x20
#define MPU6886_WOM_THR_Y       0x21
#define MPU6886_WOM_THR_Z       0x22
#define WOM_INT_MASK            0xE0

// Tap detection
const float SETTLE_THRESHOLD = 1.5;
const unsigned long SETTLE_TIME = 150;
const unsigned long DEBOUNCE_MS = 1000;
const uint8_t WOM_THRESHOLD = 200;

// Audio/VAD
const int SAMPLE_RATE = 16000;
const int VAD_CHUNK_MS = 100;                    // Process audio in 100ms chunks
const int VAD_SAMPLE_CHUNK = (SAMPLE_RATE * VAD_CHUNK_MS) / 1000;
const float VAD_ENERGY_THRESHOLD = 250.0;        // Lowered for better sensitivity
const int VAD_MIN_RECORD_MS = 800;               // Minimum 0.8s recording
const int VAD_TRAILING_SILENCE_FRAMES = 3;       // Keep 300ms trailing silence

// ========== GLOBALS ==========

int16_t* audioBuffer = nullptr;
int recordedSamples = 0;  // Actual samples recorded (not max)
unsigned long lastTapTime = 0;
unsigned long lastActivityTime = 0;
RTC_DATA_ATTR int bootCount = 0;
WiFiClientSecure* llmClient = nullptr;  // For HTTP keep-alive (unified for all providers)
unsigned long buttonHoldStart = 0;
bool buttonHeld = false;

// Forward declarations
void loadConfig();
void saveConfig();
void migrateConfig();
void startConfigMode();
String generateConfigPage();
void handleConfigRoot();
void handleConfigSave();
void playConfigBeep();
void playActivationBeep();
void playSuccessBeep();
void playErrorBeep();
void playLowBatteryWarning();
bool connectWiFi();
void goToSleep();
void handleVoiceCommand();
bool recordAudioAdaptive();
bool recordAudioFixed(int seconds);
String transcribeAudio();
bool sendToHomeAssistant(const String &text);
void mpuWrite(uint8_t reg, uint8_t val);
uint8_t mpuRead(uint8_t reg);
void setupWakeOnMotion();
bool checkWoM();
void clearWoM();
bool detectTap();
bool verifyTap();

// ========== CONFIG PERSISTENCE ==========

void loadConfig() {
  prefs.begin("stickc_ha", true);
  config.configured = prefs.getBool("configured", false);
  
  if (config.configured) {
    strlcpy(config.wifiSsid, prefs.getString("wifiSsid", "").c_str(), sizeof(config.wifiSsid));
    strlcpy(config.wifiPass, prefs.getString("wifiPass", "").c_str(), sizeof(config.wifiPass));
    strlcpy(config.llmApiKey, prefs.getString("llmKey", "").c_str(), sizeof(config.llmApiKey));
    strlcpy(config.llmUrl, prefs.getString("llmUrl", "").c_str(), sizeof(config.llmUrl));
    strlcpy(config.llmProvider, prefs.getString("llmProvider", "groq").c_str(), sizeof(config.llmProvider));
    strlcpy(config.haUrl, prefs.getString("haUrl", "").c_str(), sizeof(config.haUrl));
    strlcpy(config.haToken, prefs.getString("haToken", "").c_str(), sizeof(config.haToken));
    config.tapThreshold = prefs.getFloat("tapThresh", DEFAULT_TAP_THRESHOLD);
    config.recordTime = prefs.getUChar("recTime", DEFAULT_RECORD_TIME);
    config.sleepTimeout = prefs.getUChar("sleepTime", DEFAULT_SLEEP_TIMEOUT);
    config.vadSensitivity = prefs.getUChar("vadSens", DEFAULT_VAD_SENSITIVITY);
  } else {
    config.tapThreshold = DEFAULT_TAP_THRESHOLD;
    config.recordTime = DEFAULT_RECORD_TIME;
    config.sleepTimeout = DEFAULT_SLEEP_TIMEOUT;
    config.vadSensitivity = DEFAULT_VAD_SENSITIVITY;
    strcpy(config.llmProvider, "groq");
    strcpy(config.llmUrl, "https://api.groq.com/openai/v1/audio/transcriptions");
  }
  prefs.end();
  
  // Migrate old Groq-only config to unified LLM config
  migrateConfig();
}

void migrateConfig() {
  prefs.begin("stickc_ha", true);
  if (prefs.isKey("groqKey") && !prefs.isKey("llmKey")) {
    Serial.println("Migrating old Groq config to unified LLM config...");
    String oldKey = prefs.getString("groqKey", "");
    prefs.end();
    
    prefs.begin("stickc_ha", false);
    prefs.putString("llmKey", oldKey);
    prefs.putString("llmProvider", "groq");
    prefs.putString("llmUrl", "https://api.groq.com/openai/v1/audio/transcriptions");
    prefs.end();
    
    // Reload to pick up migrated values
    loadConfig();
  }
  prefs.end();
}

void saveConfig() {
  prefs.begin("stickc_ha", false);
  prefs.putBool("configured", true);
  prefs.putString("wifiSsid", config.wifiSsid);
  prefs.putString("wifiPass", config.wifiPass);
  prefs.putString("llmKey", config.llmApiKey);
  prefs.putString("llmUrl", config.llmUrl);
  prefs.putString("llmProvider", config.llmProvider);
  prefs.putString("haUrl", config.haUrl);
  prefs.putString("haToken", config.haToken);
  prefs.putFloat("tapThresh", config.tapThreshold);
  prefs.putUChar("recTime", config.recordTime);
  prefs.putUChar("sleepTime", config.sleepTimeout);
  prefs.putUChar("vadSens", config.vadSensitivity);
  prefs.end();
}

// ========== CONFIG MODE (AP + WEB SERVER) ==========

const char LCARS_CSS[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#ff9900;font-family:'Antonio',sans-serif;padding:10px}
@import url('https://fonts.googleapis.com/css2?family=Antonio:wght@400;700&display=swap');
.lcars-frame{border:3px solid #cc99ff;border-radius:30px;overflow:hidden;max-width:700px;margin:0 auto}
.lcars-header{background:#cc99ff;color:#000;padding:15px 25px;display:flex;justify-content:space-between;align-items:center}
.lcars-header h1{font-size:20px;letter-spacing:2px}
.lcars-header span{font-size:12px}
.lcars-body{display:flex;min-height:70vh}
.lcars-sidebar{background:#000;width:60px;display:flex;flex-direction:column;gap:2px;padding:2px}
.lcars-pill{border-radius:15px 0 0 15px;padding:12px 8px;text-align:center;font-size:10px;cursor:pointer}
.pill-orange{background:#ff9966;color:#000}
.pill-blue{background:#99ccff;color:#000}
.pill-purple{background:#cc99ff;color:#000}
.pill-tan{background:#ffcc99;color:#000}
.lcars-content{flex:1;padding:15px;overflow-y:auto}
.section{background:#111;border-left:4px solid #ff9966;padding:12px;margin-bottom:12px;border-radius:0 8px 8px 0}
.section h2{color:#ff9966;font-size:14px;margin-bottom:10px;letter-spacing:1px}
.section.blue{border-left-color:#99ccff}
.section.blue h2{color:#99ccff}
.section.purple{border-left-color:#cc99ff}
.section.purple h2{color:#cc99ff}
.section.tan{border-left-color:#ffcc99}
.section.tan h2{color:#ffcc99}
.field{margin-bottom:10px}
label{display:block;color:#cc99ff;font-size:11px;margin-bottom:3px;letter-spacing:1px}
input,select,textarea{width:100%;padding:8px;background:#0a0a0a;border:1px solid #333;border-radius:4px;color:#ff9900;font-family:'Antonio',sans-serif;font-size:14px}
input:focus,select:focus{outline:none;border-color:#ff9966}
.row{display:flex;gap:8px}
.row .field{flex:1}
.checkbox-field{display:flex;align-items:center;gap:8px}
.checkbox-field input{width:auto}
.checkbox-field label{margin:0;font-size:12px}
.status-bar{display:flex;gap:15px;padding:10px;background:#111;border-radius:8px;margin-bottom:12px;flex-wrap:wrap}
.status-item{display:flex;align-items:center;gap:5px}
.status-item .lbl{color:#666;font-size:10px}
.status-item .val{color:#99ccff;font-size:12px}
.btn{display:inline-block;padding:12px 25px;border:none;border-radius:20px;font-family:'Antonio',sans-serif;font-size:16px;cursor:pointer;margin-right:10px;margin-top:10px}
.btn-orange{background:#ff9966;color:#000}
.btn-purple{background:#cc99ff;color:#000}
.btn:hover{filter:brightness(1.2)}
.lcars-footer{background:#cc99ff;color:#000;padding:8px 20px;font-size:11px;display:flex;justify-content:space-between}
.hidden{display:none}
.provider-warning{color:#ffcc99;font-size:11px;margin-top:5px;padding:5px;background:#222;border-radius:4px}
@media(max-width:500px){.lcars-sidebar{width:40px}.lcars-pill{font-size:8px;padding:8px 4px}.row{flex-direction:column;gap:0}}
)rawliteral";

String generateConfigPage() {
  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>COMM BADGE CONFIG</title><style>");
  html += FPSTR(LCARS_CSS);
  html += F("</style></head><body>");
  
  html += F("<div class='lcars-frame'>");
  
  // Header
  html += F("<div class='lcars-header'><h1>COMM BADGE CONFIGURATION</h1><span>STARFLEET ISSUE</span></div>");
  
  // Body
  html += F("<div class='lcars-body'>");
  
  // Sidebar
  html += F("<div class='lcars-sidebar'>");
  html += F("<div class='lcars-pill pill-orange' style='flex:2'>LCARS</div>");
  html += F("<div class='lcars-pill pill-blue'>47</div>");
  html += F("<div class='lcars-pill pill-purple'>148</div>");
  html += F("<div class='lcars-pill pill-tan' style='flex:3'>CONFIG</div>");
  html += F("<div class='lcars-pill pill-blue'>22</div>");
  html += F("<div class='lcars-pill pill-orange' style='flex:2'>1701</div>");
  html += F("</div>");
  
  // Content
  html += F("<div class='lcars-content'><form action='/save' method='POST'>");
  
  // Status bar
  html += F("<div class='status-bar'>");
  html += F("<div class='status-item'><span class='lbl'>POWER:</span><span class='val'>");
  html += String(M5.Power.getBatteryLevel()) + F("%</span></div>");
  html += F("<div class='status-item'><span class='lbl'>SIGNAL:</span><span class='val'>");
  html += String(WiFi.RSSI()) + F(" dBm</span></div>");
  html += F("<div class='status-item'><span class='lbl'>VERSION:</span><span class='val'>3.8</span></div>");
  html += F("</div>");
  
  // Subspace Relay (WiFi)
  html += F("<div class='section blue'><h2>SUBSPACE RELAY (WIFI)</h2>");
  html += F("<div class='row'><div class='field'><label>NETWORK SSID</label><input name='ssid' value='");
  html += config.wifiSsid;
  html += F("'></div>");
  html += F("<div class='field'><label>ACCESS CODE</label><input type='password' name='pass' value='");
  html += config.wifiPass;
  html += F("'></div></div></div>");
  
  // Universal Translator (LLM)
  html += F("<div class='section purple'><h2>UNIVERSAL TRANSLATOR (SPEECH RECOGNITION)</h2>");
  html += F("<div class='field'><label>AI PROVIDER</label><select name='llmProvider' id='llmProvider'>");
  html += F("<option value='groq'"); if(strcmp(config.llmProvider,"groq")==0) html+=F(" selected"); html+=F(">GROQ (RECOMMENDED)</option>");
  html += F("<option value='openai'"); if(strcmp(config.llmProvider,"openai")==0) html+=F(" selected"); html+=F(">OPENAI</option>");
  html += F("<option value='custom'"); if(strcmp(config.llmProvider,"custom")==0) html+=F(" selected"); html+=F(">CUSTOM WHISPER ENDPOINT</option>");
  html += F("</select>");
  html += F("<div class='provider-warning' id='providerWarning'>");
  if (strcmp(config.llmProvider, "groq") == 0) html += F("✅ Official Groq Whisper endpoint (fastest)");
  else if (strcmp(config.llmProvider, "openai") == 0) html += F("✅ Official OpenAI Whisper endpoint");
  else html += F("🔧 Enter any Whisper-compatible API endpoint (e.g., proxy)");
  html += F("</div></div>");
  html += F("<div class='field'><label>API KEY</label><input type='password' name='llmKey' value='");
  html += config.llmApiKey;
  html += F("'></div>");
  html += F("<div class='field'><label>API URL</label><input name='llmUrl' id='llmUrl' value='");
  html += config.llmUrl;
  html += F("'></div>");
  html += F("<div class='field'><label>VOICE ACTIVITY DETECTION SENSITIVITY</label>");
  html += F("<input type='range' name='vadSens' min='1' max='10' value='");
  html += String(config.vadSensitivity);
  html += F("' style='width:100%;'>");
  html += F("<div style='display:flex;justify-content:space-between;font-size:12px'>");
  html += F("<span>Very Sensitive</span><span>Less Sensitive</span></div>");
  html += F("<div style='color:#666;font-size:11px;margin-top:5px'>1=stops quickly after speech | 10=waits longer</div></div></div>");
  
  // Ship's Computer (Home Assistant)
  html += F("<div class='section tan'><h2>SHIP'S COMPUTER (HOME ASSISTANT)</h2>");
  html += F("<div class='field'><label>SHIP'S COMPUTER URL</label><input name='haurl' value='");
  html += config.haUrl;
  html += F("'></div>");
  html += F("<div class='field'><label>LONG-LIVED (and prosper) ACCESS TOKEN</label><input type='password' name='hatoken' value='");
  html += config.haToken;
  html += F("'></div></div>");
  
  // Sensor Calibration
  html += F("<div class='section blue'><h2>SENSOR CALIBRATION</h2>");
  html += F("<div class='field'>");
  html += F("<label>TAP SENSITIVITY <span id='tapVal'>");
  html += String(config.tapThreshold, 1);
  html += F("</span></label>");
  html += F("<input type='range' name='tap' min='20' max='45' step='1' value='");
  html += String(config.tapThreshold * 10, 0);
  html += F("' style='width:100%;' oninput='document.getElementById(\"tapVal\").textContent=(this.value/10).toFixed(1)'>");
  html += F("<div style='color:#666;font-size:11px;margin-top:5px'>Gentle tap ← → Firm tap</div></div>");
  html += F("<div class='row'><div class='field'><label>MAX RECORD TIME (sec)</label><input type='number' min='1' max='6' name='rec' value='");
  html += String(config.recordTime);
  html += F("'></div>");
  html += F("<div class='field'><label>DEEP SLEEP TIMEOUT (sec)</label><input type='number' min='10' max='120' name='sleep' value='");
  html += String(config.sleepTimeout);
  html += F("'></div></div></div>");
  
  // Buttons
  html += F("<button type='submit' class='btn btn-purple'>ENGAGE</button>");
  html += F("<button type='button' class='btn btn-orange' onclick=\"if(confirm('Reset to factory defaults?'))location='/reset'\">RESET</button>");
  
  html += F("</form></div></div>");
  
  // Footer
  html += F("<div class='lcars-footer'><span>CHIEF ENGINEER: LIEUTENANT COMMANDER SPANNER  -  https://sthi.space</span><span>LCARS ACCESS 47988</span></div>");
  html += F("</div>");
  
  // JavaScript
  html += F("<script>");
  html += F("document.getElementById('llmProvider').onchange=function(){");
  html += F("const urlField=document.getElementById('llmUrl');");
  html += F("const warn=document.getElementById('providerWarning');");
  html += F("switch(this.value){");
  html += F("case'groq':urlField.value='https://api.groq.com/openai/v1/audio/transcriptions';");
  html += F("warn.textContent='✅ Official Groq Whisper endpoint (fastest)';");
  html += F("warn.style.color='#99ccff';break;");
  html += F("case'openai':urlField.value='https://api.openai.com/v1/audio/transcriptions';");
  html += F("warn.textContent='✅ Official OpenAI Whisper endpoint';");
  html += F("warn.style.color='#99ccff';break;");
  html += F("case'custom':urlField.value='';");
  html += F("warn.textContent='🔧 Enter any Whisper-compatible API endpoint (e.g., proxy)';");
  html += F("warn.style.color='#ff9966';break;");
  html += F("}};");
  html += F("document.getElementById('llmProvider').dispatchEvent(new Event('change'));");
  html += F("</script>");
  
  html += F("</body></html>");
  return html;
}

String generateSavedPage() {
  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<style>body{background:#000;color:#ff9900;font-family:sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;text-align:center}");
  html += F(".box{background:#111;border:2px solid #cc99ff;border-radius:20px;padding:30px}h1{color:#99ccff;margin-bottom:15px}</style></head>");
  html += F("<body><div class='box'><h1>CONFIGURATION SAVED</h1><p>Rebooting in 3 seconds...</p><p style='color:#cc99ff;margin-top:20px'>Open hailing frequencies!.</p></div></body></html>");
  return html;
}

// Global server pointer for handler access
WebServer* configServer = nullptr;

void handleConfigRoot() {
  configServer->send(200, "text/html", generateConfigPage());
}

void handleConfigSave() {
  if (configServer->hasArg("ssid")) strlcpy(config.wifiSsid, configServer->arg("ssid").c_str(), sizeof(config.wifiSsid));
  if (configServer->hasArg("pass")) strlcpy(config.wifiPass, configServer->arg("pass").c_str(), sizeof(config.wifiPass));
  if (configServer->hasArg("llmProvider")) strlcpy(config.llmProvider, configServer->arg("llmProvider").c_str(), sizeof(config.llmProvider));
  if (configServer->hasArg("llmKey")) strlcpy(config.llmApiKey, configServer->arg("llmKey").c_str(), sizeof(config.llmApiKey));
  if (configServer->hasArg("llmUrl")) strlcpy(config.llmUrl, configServer->arg("llmUrl").c_str(), sizeof(config.llmUrl));
  if (configServer->hasArg("haurl")) strlcpy(config.haUrl, configServer->arg("haurl").c_str(), sizeof(config.haUrl));
  if (configServer->hasArg("hatoken")) strlcpy(config.haToken, configServer->arg("hatoken").c_str(), sizeof(config.haToken));
  if (configServer->hasArg("tap")) config.tapThreshold = configServer->arg("tap").toFloat() / 10.0;
  if (configServer->hasArg("rec")) config.recordTime = configServer->arg("rec").toInt();
  if (configServer->hasArg("sleep")) config.sleepTimeout = configServer->arg("sleep").toInt();
  if (configServer->hasArg("vadSens")) config.vadSensitivity = configServer->arg("vadSens").toInt();
  
  // Clamp values
  if (config.tapThreshold < 2.0) config.tapThreshold = 2.0;
  if (config.tapThreshold > 4.5) config.tapThreshold = 4.5;
  if (config.recordTime < 1) config.recordTime = 1;
  if (config.recordTime > 6) config.recordTime = 6;
  if (config.sleepTimeout < 10) config.sleepTimeout = 10;
  if (config.sleepTimeout > 120) config.sleepTimeout = 120;
  if (config.vadSensitivity < 1) config.vadSensitivity = 1;
  if (config.vadSensitivity > 10) config.vadSensitivity = 10;
  
  // Auto-set URL based on provider if empty/invalid
  if (strcmp(config.llmProvider, "groq") == 0 && (strlen(config.llmUrl) == 0 || strstr(config.llmUrl, "groq") == nullptr)) {
    strcpy(config.llmUrl, "https://api.groq.com/openai/v1/audio/transcriptions");
  } else if (strcmp(config.llmProvider, "openai") == 0 && (strlen(config.llmUrl) == 0 || strstr(config.llmUrl, "openai") == nullptr)) {
    strcpy(config.llmUrl, "https://api.openai.com/v1/audio/transcriptions");
  }
  
  config.configured = true;
  saveConfig();
  
  configServer->send(200, "text/html", generateSavedPage());
  
  delay(3000);
  ESP.restart();
}

void startConfigMode() {
  Serial.println("\n════════════════════════════════════════");
  Serial.println("   CONFIGURATION MODE ACTIVATED");
  Serial.println("════════════════════════════════════════\n");
  
  playConfigBeep();
  
  // Free audio buffer if allocated (safety)
  if (audioBuffer) {
    free(audioBuffer);
    audioBuffer = nullptr;
  }
  
  // Close LLM client if open
  if (llmClient) {
    llmClient->stop();
    delete llmClient;
    llmClient = nullptr;
  }
  
  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CommBadge-Config", "starfleet");
  delay(100);
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("Access Point: CommBadge-Config\n");
  Serial.printf("Password: starfleet\n");
  Serial.printf("Navigate to: http://%s\n\n", apIP.toString().c_str());
  
  // Show on display
  M5.Display.wakeup();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_ORANGE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 10);
  M5.Display.println("CONFIG MODE");
  M5.Display.setCursor(5, 30);
  M5.Display.println("WiFi: CommBadge-Config");
  M5.Display.setCursor(5, 50);
  M5.Display.println("Pass: starfleet");
  M5.Display.setCursor(5, 70);
  M5.Display.print("http://");
  M5.Display.println(apIP.toString());
  
  // Start web server (global pointer for handler access)
  WebServer server(80);
  configServer = &server;
  server.on("/", HTTP_GET, handleConfigRoot);
  server.on("/save", HTTP_POST, handleConfigSave);
  server.begin();
  
  Serial.println("Standing by for configuration...\n");
  
  // Run config server until reboot
  while (true) {
    server.handleClient();
    M5.update();
    
    // Allow exit via 5-sec hold (safety)
    if (M5.BtnA.pressedFor(5000)) {
      Serial.println("Exiting config mode (no save)...");
      delay(500);
      ESP.restart();
    }
    
    delay(10);
  }
}

// ========== SOUND EFFECTS ==========

void playConfigBeep() {
  M5.Speaker.begin();
  M5.Speaker.tone(880, 80); delay(90);
  M5.Speaker.tone(1320, 120); delay(130);
  M5.Speaker.end();
}

void playActivationBeep() {
  M5.Speaker.begin();
  for (int b = 0; b < 3; b++) {
    for (int i = 0; i < (b == 2 ? 5 : 3); i++) {
      M5.Speaker.tone(1800 + i*400, 8);
      delay(10);
    }
    delay(60);
  }
  M5.Speaker.end();
}

void playSuccessBeep() {
  M5.Speaker.begin();
  M5.Speaker.tone(1800, 30); delay(35);
  M5.Speaker.tone(1200, 20); delay(105);
  M5.Speaker.tone(1800, 30); delay(35);
  M5.Speaker.tone(1200, 25); delay(110);
  M5.Speaker.tone(1600, 40); delay(45);
  M5.Speaker.tone(2000, 50); delay(55);
  M5.Speaker.tone(1800, 60); delay(70);
  M5.Speaker.end();
}

void playErrorBeep() {
  M5.Speaker.begin();
  M5.Speaker.tone(600, 100); delay(110);
  M5.Speaker.tone(400, 150); delay(160);
  M5.Speaker.end();
}

void playLowBatteryWarning() {
  M5.Speaker.begin();
  M5.Speaker.tone(300, 200); delay(220);
  M5.Speaker.tone(300, 200); delay(220);
  M5.Speaker.end();
}

// ========== BATTERY ==========

void printBatteryStatus() {
  int level = M5.Power.getBatteryLevel();
  float voltage = M5.Power.getBatteryVoltage() / 1000.0;
  bool charging = M5.Power.isCharging();
  
  Serial.printf("Battery: %d%% (%.2fV)%s\n", level, voltage, charging ? " [CHARGING]" : "");
  
  if (level < 20 && !charging) {
    Serial.println("⚠ LOW BATTERY");
    playLowBatteryWarning();
  }
}

// ========== MPU6886 ==========

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(MPU6886_ADDRESS);
  Wire1.write(reg);
  Wire1.write(val);
  Wire1.endTransmission();
}

uint8_t mpuRead(uint8_t reg) {
  Wire1.beginTransmission(MPU6886_ADDRESS);
  Wire1.write(reg);
  Wire1.endTransmission(false);
  Wire1.requestFrom(MPU6886_ADDRESS, 1);
  return Wire1.read();
}

void setupWakeOnMotion() {
  Wire1.begin(21, 22);
  delay(10);
  
  mpuWrite(MPU6886_ACCEL_CONFIG, 0x18);  // 16G
  delay(10);
  
  uint8_t pwr1 = mpuRead(MPU6886_PWR_MGMT_1);
  mpuWrite(MPU6886_PWR_MGMT_1, pwr1 & 0x8F);
  mpuWrite(MPU6886_PWR_MGMT_2, 0x07);
  mpuWrite(MPU6886_ACCEL_CONFIG2, 0x21);
  mpuWrite(MPU6886_INT_ENABLE, 0xE0);
  mpuWrite(MPU6886_WOM_THR_X, WOM_THRESHOLD);
  mpuWrite(MPU6886_WOM_THR_Y, WOM_THRESHOLD);
  mpuWrite(MPU6886_WOM_THR_Z, WOM_THRESHOLD);
  mpuWrite(MPU6886_ACCEL_INTEL_CTRL, 0xC2);
  mpuWrite(MPU6886_SMPLRT_DIV, 19);
  
  pwr1 = mpuRead(MPU6886_PWR_MGMT_1);
  mpuWrite(MPU6886_PWR_MGMT_1, pwr1 | 0x20);
  
  mpuRead(MPU6886_INT_STATUS);  // Clear
}

bool checkWoM() {
  return (mpuRead(MPU6886_INT_STATUS) & WOM_INT_MASK) != 0;
}

void clearWoM() {
  mpuRead(MPU6886_INT_STATUS);
}

// ========== TAP DETECTION ==========

bool detectTap() {
  static unsigned long spikeTime = 0;
  static bool waiting = false;
  
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrt(ax*ax + ay*ay + az*az);
  
  if ((millis() - lastTapTime) < DEBOUNCE_MS) return false;
  
  if (mag > config.tapThreshold && !waiting) {
    spikeTime = millis();
    waiting = true;
  }
  
  if (waiting) {
    if (mag < SETTLE_THRESHOLD) {
      waiting = false;
      lastTapTime = millis();
      return true;
    }
    if (millis() - spikeTime > SETTLE_TIME) {
      waiting = false;
    }
  }
  return false;
}

bool verifyTap() {
  M5.Imu.begin();
  
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrt(ax*ax + ay*ay + az*az);
  
  if (mag > config.tapThreshold) {
    unsigned long start = millis();
    while (millis() - start < SETTLE_TIME) {
      delay(10);
      M5.Imu.getAccel(&ax, &ay, &az);
      mag = sqrt(ax*ax + ay*ay + az*az);
      if (mag < SETTLE_THRESHOLD) return true;
    }
    return false;
  }
  
  if (mag < SETTLE_THRESHOLD) {
    delay(50);
    M5.Imu.getAccel(&ax, &ay, &az);
    mag = sqrt(ax*ax + ay*ay + az*az);
    return (mag < SETTLE_THRESHOLD);
  }
  
  return false;
}

// ========== WIFI ==========

bool connectWiFi() {
  Serial.printf("Connecting to %s", config.wifiSsid);
  WiFi.mode(WIFI_STA);  // STA mode only - critical for stability
  WiFi.begin(config.wifiSsid, config.wifiPass);
  WiFi.setSleep(false);
  
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected! IP: %s, RSSI: %d dBm\n", 
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("WiFi failed");
  return false;
}

// ========== SLEEP (with WiFi keep-alive) ==========

void goToSleep() {
  unsigned long idleTime = millis() - lastActivityTime;
  unsigned long deepSleepThreshold = config.sleepTimeout * 1000UL;
  
  // LIGHT IDLE (keep WiFi alive for fast repeat commands)
  if (idleTime < deepSleepThreshold) {
    Serial.printf("\n💤 Light idle (%lums until deep sleep)...\n", deepSleepThreshold - idleTime);
    Serial.flush();
    delay(100);
    
    // Monitor for tap while keeping WiFi alive
    unsigned long lightIdleStart = millis();
    while ((millis() - lightIdleStart) < (deepSleepThreshold - idleTime)) {
      M5.update();
      
      if (detectTap()) {
        Serial.println("⚡ Tap detected during light idle");
        lastTapTime = millis();
        handleVoiceCommand();
        return;
      }
      
      // Check button for config mode exit
      if (M5.BtnA.pressedFor(5000)) {
        Serial.println("Exiting to config mode...");
        delay(500);
        startConfigMode();
        return;
      }
      
      delay(20);  // Low power polling
    }
  }
  
  // DEEP SLEEP (WiFi off)
  Serial.println("\n🌙 Deep sleep (WiFi off)...");
  Serial.flush();
  delay(100);
  
  // Close LLM connection before sleep
  if (llmClient) {
    llmClient->stop();
    delete llmClient;
    llmClient = nullptr;
  }
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  setupWakeOnMotion();
  clearWoM();
  
  while (true) {
    esp_sleep_enable_timer_wakeup(50000);
    esp_light_sleep_start();
    
    if (checkWoM()) {
      clearWoM();
      
      if (verifyTap()) {
        Serial.begin(115200);
        Serial.println("\n⚡ Woke from deep sleep!");
        
        if (connectWiFi()) {
          lastActivityTime = millis();
          lastTapTime = millis();
          handleVoiceCommand();
          return;
        }
        Serial.println("WiFi failed, back to sleep");
      }
      
      setupWakeOnMotion();
      clearWoM();
    }
  }
}

// ========== SETUP ==========

void setup() {
  Serial.begin(115200);
  delay(500);
  bootCount++;
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("   M5StickC PLUS2 - HA Voice Control v3.8");
  Serial.println("   STARFLEET COMM BADGE");
  Serial.println("════════════════════════════════════════\n");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Imu.begin();
  M5.Display.sleep();
  
  // EARLY BOOT BUTTON CHECK (before M5.update initializes buttons)
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  delay(50);
  bool btnHeldAtBoot = (digitalRead(BUTTON_A_PIN) == LOW);
  
  loadConfig();
  
  // Enter config mode if button held at boot OR never configured
  if (btnHeldAtBoot || !config.configured) {
    Serial.println(">>> Entering CONFIGURATION MODE <<<\n");
    startConfigMode();  // Never returns - reboots on save
    return;
  }
  
  // Normal boot flow
  esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
  if (reason == ESP_SLEEP_WAKEUP_EXT0) Serial.println("Wake: EXT0");
  else if (reason == ESP_SLEEP_WAKEUP_TIMER) Serial.println("Wake: Timer");
  else Serial.printf("Boot #%d (reason: %d)\n", bootCount, reason);
  
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  printBatteryStatus();

  if (!connectWiFi()) {
    playErrorBeep();
    delay(2000);
    Serial.println("WiFi failed - entering config mode...");
    startConfigMode();
    return;
  }

  // Allocate audio buffer (max size = config.recordTime seconds)
  int maxSamples = SAMPLE_RATE * config.recordTime;
  audioBuffer = (int16_t*)malloc(maxSamples * sizeof(int16_t));
  if (!audioBuffer) {
    Serial.println("ERROR: Audio buffer allocation failed");
    playErrorBeep();
    while(1) delay(1000);
  }

  // Initialize LLM client (nullptr - will connect on first use)
  llmClient = nullptr;

  Serial.println("\n════════════════════════════════════════");
  Serial.println("           *** READY ***");
  Serial.printf("  Tap: %.1f | Max Record: %ds | Deep Sleep: %ds\n", 
    config.tapThreshold, config.recordTime, config.sleepTimeout);
  Serial.printf("  ⚡ VAD Sensitivity: %d (1=Very Sensitive | 10=Less Sensitive)\n", config.vadSensitivity);
  Serial.printf("  🤖 AI Provider: %s\n", config.llmProvider);
  Serial.println("════════════════════════════════════════\n");
  
  lastActivityTime = millis();
  
  if (reason == ESP_SLEEP_WAKEUP_EXT0) {
    delay(300);
    handleVoiceCommand();
  }
}

// ========== LOOP ==========

void loop() {
  M5.update();
  
  // Check for 5-second button hold (anytime)
  if (M5.BtnA.isPressed()) {
    if (!buttonHeld) {
      buttonHoldStart = millis();
      buttonHeld = true;
    } else if (millis() - buttonHoldStart > 5000) {
      Serial.println("\n>>> 5-second button hold detected - entering config mode <<<\n");
      startConfigMode();
      return;
    }
  } else {
    buttonHeld = false;
  }
  
  if (detectTap()) handleVoiceCommand();
  
  if (millis() - lastActivityTime > (config.sleepTimeout * 1000UL / 2)) {
    // Start checking more frequently as we approach deep sleep threshold
    goToSleep();
  }
  
  delay(10);
}

// ========== VOICE HANDLER ==========

void handleVoiceCommand() {
  lastActivityTime = millis();
  
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║     TAP DETECTED - LISTENING...    ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  playActivationBeep();

  // RECORD WITH VAD (adaptive stop on silence)
  unsigned long recordStart = millis();
  if (!recordAudioAdaptive()) {
    Serial.println("⚠ VAD timeout - using fixed 2s recording");
    if (!recordAudioFixed(2)) {
      playErrorBeep();
      return;
    }
  }
  unsigned long recordTime = millis() - recordStart;
  Serial.printf("🎤 Recorded %d samples (%.2fs)\n", recordedSamples, recordedSamples / (float)SAMPLE_RATE);

  Serial.println("→ Transcribing with " + String(config.llmProvider) + "...");
  unsigned long t0 = millis();
  String text = transcribeAudio();
  Serial.printf("  %lu ms\n", millis() - t0);

  if (text.length() < 2 || text.startsWith("ERROR")) {
    Serial.println("✗ Transcription failed: " + text);
    playErrorBeep();
    return;
  }

  // Strip "Computer" prefix if present
  String cmd = text;
  cmd.toLowerCase();
  if (cmd.startsWith("computer")) {
    text = text.substring(8);
    text.trim();
    Serial.println("  Stripped 'Computer' prefix");
  }

  Serial.println("→ Sending to Ship's Computer...");
  t0 = millis();
  bool ok = sendToHomeAssistant(text);
  Serial.printf("  %lu ms\n", millis() - t0);
  
  ok ? playSuccessBeep() : playErrorBeep();
  
  Serial.printf("\n✅ Total latency: %lu ms\n\n", millis() - lastActivityTime);
  lastActivityTime = millis();
}

// ========== AUDIO WITH VAD ==========

bool recordAudioAdaptive() {
  // Safety check - buffer must be large enough for max duration
  int maxSamples = SAMPLE_RATE * config.recordTime;
  if (!audioBuffer || maxSamples < (SAMPLE_RATE * 1)) {
    return false;
  }
  
  // Map VAD sensitivity (1-10) to silence frames (2-20)
  int silenceFrames = 2 + (10 - config.vadSensitivity) * 2;
  
  M5.Mic.begin();
  recordedSamples = 0;
  int silenceCounter = 0;
  bool speechStarted = false;
  unsigned long startTime = millis();
  unsigned long lastSpeechTime = millis();
  
  // Temporary buffer for chunked processing
  int16_t* chunkBuffer = (int16_t*)malloc(VAD_SAMPLE_CHUNK * sizeof(int16_t));
  if (!chunkBuffer) {
    M5.Mic.end();
    return false;
  }
  
  // Initial silence detection (wait for speech to start)
  while ((millis() - startTime) < 2000) {  // 2s max wait for speech start
    M5.Mic.record(chunkBuffer, VAD_SAMPLE_CHUNK, SAMPLE_RATE);
    while (M5.Mic.isRecording()) delay(1);
    
    // Compute RMS energy (lowered threshold for better sensitivity)
    float sum = 0;
    for (int i = 0; i < VAD_SAMPLE_CHUNK; i++) {
      int32_t sample = chunkBuffer[i];
      sum += (sample * sample);
    }
    float rms = sqrt(sum / VAD_SAMPLE_CHUNK);
    
    if (rms > VAD_ENERGY_THRESHOLD) {
      speechStarted = true;
      lastSpeechTime = millis();
      // Copy first speech chunk
      memcpy(&audioBuffer[recordedSamples], chunkBuffer, VAD_SAMPLE_CHUNK * sizeof(int16_t));
      recordedSamples += VAD_SAMPLE_CHUNK;
      break;
    }
    
    M5.update();
    if (M5.BtnA.wasPressed()) {  // Cancel on button press
      free(chunkBuffer);
      M5.Mic.end();
      return false;
    }
  }
  
  if (!speechStarted) {
    free(chunkBuffer);
    M5.Mic.end();
    return false;  // Timed out waiting for speech
  }
  
  // Record until silence or max duration
  while (true) {
    // Check timeouts
    if ((millis() - startTime) > (config.recordTime * 1000)) break;  // Max duration
    if ((millis() - lastSpeechTime) > 3000) break;  // 3s no speech
    
    M5.Mic.record(chunkBuffer, VAD_SAMPLE_CHUNK, SAMPLE_RATE);
    while (M5.Mic.isRecording()) delay(1);
    
    // Compute RMS energy
    float sum = 0;
    for (int i = 0; i < VAD_SAMPLE_CHUNK; i++) {
      int32_t sample = chunkBuffer[i];
      sum += (sample * sample);
    }
    float rms = sqrt(sum / VAD_SAMPLE_CHUNK);
    
    // Speech detected
    if (rms > VAD_ENERGY_THRESHOLD) {
      silenceCounter = 0;
      lastSpeechTime = millis();
      speechStarted = true;
      
      // Copy to main buffer if space available
      if (recordedSamples + VAD_SAMPLE_CHUNK <= maxSamples) {
        memcpy(&audioBuffer[recordedSamples], chunkBuffer, VAD_SAMPLE_CHUNK * sizeof(int16_t));
        recordedSamples += VAD_SAMPLE_CHUNK;
      } else {
        break;  // Buffer full
      }
    } 
    // Silence detected
    else {
      silenceCounter++;
      
      // Keep trailing silence (helps Whisper detect end of speech)
      if (silenceCounter <= VAD_TRAILING_SILENCE_FRAMES && 
          recordedSamples + VAD_SAMPLE_CHUNK <= maxSamples) {
        memcpy(&audioBuffer[recordedSamples], chunkBuffer, VAD_SAMPLE_CHUNK * sizeof(int16_t));
        recordedSamples += VAD_SAMPLE_CHUNK;
      }
      
      // Stop after sustained silence AND minimum duration met
      if (silenceCounter >= silenceFrames && 
          (recordedSamples * 1000 / SAMPLE_RATE) >= VAD_MIN_RECORD_MS) {
        break;
      }
    }
    
    M5.update();
    if (M5.BtnA.wasPressed()) {  // Cancel on button press
      free(chunkBuffer);
      M5.Mic.end();
      return false;
    }
  }
  
  M5.Mic.end();
  free(chunkBuffer);
  
  // Enforce minimum duration
  if ((recordedSamples * 1000 / SAMPLE_RATE) < VAD_MIN_RECORD_MS) {
    Serial.println("  ⚠ Recording too short - padding to minimum");
    int minSamples = (VAD_MIN_RECORD_MS * SAMPLE_RATE) / 1000;
    while (recordedSamples < minSamples && recordedSamples < maxSamples) {
      audioBuffer[recordedSamples++] = 0;
    }
  }
  
  return (recordedSamples > 0);
}

bool recordAudioFixed(int seconds) {
  int samples = SAMPLE_RATE * seconds;
  int maxSamples = SAMPLE_RATE * config.recordTime;
  if (samples > maxSamples) samples = maxSamples;
  
  M5.Mic.begin();
  for (int s = 0; s < seconds; s++) {
    if ((s * SAMPLE_RATE) >= maxSamples) break;
    M5.Mic.record(&audioBuffer[s * SAMPLE_RATE], SAMPLE_RATE, SAMPLE_RATE);
    while (M5.Mic.isRecording()) delay(10);
  }
  M5.Mic.end();
  
  recordedSamples = min(samples, maxSamples);
  return true;
}

// ========== WAV HEADER ==========

void createWavHeader(uint8_t* h, int size) {
  memcpy(h, "RIFF", 4);
  *(uint32_t*)(h+4) = 36 + size;
  memcpy(h+8, "WAVE", 4);
  memcpy(h+12, "fmt ", 4);
  *(uint32_t*)(h+16) = 16;
  *(uint16_t*)(h+20) = 1;
  *(uint16_t*)(h+22) = 1;
  *(uint32_t*)(h+24) = SAMPLE_RATE;
  *(uint32_t*)(h+28) = SAMPLE_RATE * 2;
  *(uint16_t*)(h+32) = 2;
  *(uint16_t*)(h+34) = 16;
  memcpy(h+36, "data", 4);
  *(uint32_t*)(h+40) = size;
}

// ========== TRANSCRIPTION (UNIFIED FOR ALL PROVIDERS) ==========

String transcribeAudio() {
  // Parse URL into host and path (critical for stability)
  String host, path;
  String url = String(config.llmUrl);
  
  if (!url.startsWith("https://")) {
    Serial.println("⚠ Invalid URL format - using Groq fallback");
    host = "api.groq.com";
    path = "/openai/v1/audio/transcriptions";
  } else {
    String cleanUrl = url.substring(8); // Remove "https://"
    int slashPos = cleanUrl.indexOf('/');
    if (slashPos > 0) {
      host = cleanUrl.substring(0, slashPos);
      path = cleanUrl.substring(slashPos);
    } else {
      host = cleanUrl;
      path = "/openai/v1/audio/transcriptions"; // Default Whisper path
    }
  }
  
  // Reuse connection if available and healthy
  if (!llmClient || !llmClient->connected()) {
    if (llmClient) {
      llmClient->stop();
      delete llmClient;
    }
    llmClient = new WiFiClientSecure();
    llmClient->setInsecure();
    
    if (!llmClient->connect(host.c_str(), 443)) {
      delete llmClient;
      llmClient = nullptr;
      return "ERROR: Connection failed to " + host;
    }
    Serial.println("  → New connection to: " + host);
  } else {
    Serial.println("  → Reusing connection to: " + host);
  }
  
  int audioSize = recordedSamples * sizeof(int16_t);
  uint8_t wav[44];
  createWavHeader(wav, audioSize);

  String boundary = "----ESP32Boundary7890";
  String bodyStart = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    "whisper-large-v3-turbo\r\n--" + boundary + "--\r\n";

  int len = bodyStart.length() + 44 + audioSize + bodyEnd.length();

  // CRITICAL: Proper HTTP/1.1 request with Host header
  llmClient->print("POST " + path + " HTTP/1.1\r\n"
    "Host: " + host + "\r\n"
    "Authorization: Bearer " + String(config.llmApiKey) + "\r\n"
    "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
    "Content-Length: " + String(len) + "\r\n"
    "Connection: keep-alive\r\n\r\n");

  llmClient->print(bodyStart);
  llmClient->write(wav, 44);
  
  uint8_t* p = (uint8_t*)audioBuffer;
  int rem = audioSize;
  while (rem > 0) {
    int chunk = min(2048, rem);
    llmClient->write(p, chunk);
    p += chunk;
    rem -= chunk;
    delay(1);
  }
  llmClient->print(bodyEnd);

  unsigned long t = millis();
  while (!llmClient->available()) {
    if (millis() - t > 30000) {
      llmClient->stop();
      delete llmClient;
      llmClient = nullptr;
      return "ERROR: Timeout waiting for response";
    }
    delay(20);
  }

  // Skip headers
  while (llmClient->connected()) {
    String line = llmClient->readStringUntil('\n');
    if (line == "\r") break;
  }

  String resp = llmClient->readString();
  
  // DON'T close connection - keep alive for next request
  
  int idx = resp.indexOf("\"text\"");
  if (idx < 0) {
    // Debug: show first 200 chars of response
    Serial.println("  Response preview: " + resp.substring(0, min(200, (int)resp.length())));
    return "ERROR: No 'text' field in response (check API key/URL)";
  }
  
  int start = resp.indexOf('"', idx + 6) + 1;
  String result;
  bool esc = false;
  for (unsigned int i = start; i < resp.length(); i++) {
    char c = resp[i];
    if (esc) { result += c; esc = false; }
    else if (c == '\\') esc = true;
    else if (c == '"') break;
    else result += c;
  }
  
  // Clean up result
  result.trim();
  if (result.length() == 0) return "ERROR: Empty transcription";
  
  Serial.println("  ✓ \"" + result + "\"");
  return result;
}

// ========== HOME ASSISTANT ==========

bool sendToHomeAssistant(const String &text) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(config.haUrl) + "/api/conversation/process";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(config.haToken));
  http.setTimeout(10000);

  StaticJsonDocument<512> doc;
  doc["text"] = text;
  doc["language"] = "en";
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("  ✗ HTTP %d\n", code);
    return false;
  }

  StaticJsonDocument<2048> r;
  if (deserializeJson(r, resp)) return false;

  const char* type = r["response"]["response_type"] | "";
  const char* speech = r["response"]["speech"]["plain"]["speech"] | "";
  Serial.printf("  %s: %s\n", type, speech);

  String t = String(type);
  if (t == "error") return false;
  if (t == "action_done" || t == "query_answer") return true;
  
  String s = String(speech);
  s.toLowerCase();
  if (s.indexOf("sorry") >= 0 || s.indexOf("don't understand") >= 0 ||
      s.indexOf("can't help") >= 0 || s.indexOf("not found") >= 0) return false;
  
  return true;
}