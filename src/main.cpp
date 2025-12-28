#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

// --- CONFIGURATION ---
String ap_ssid = "ESP32-Ducky-Pro";
String ap_pass = "password123";
String sta_ssid = "";
String sta_pass = "";
int typeDelay = 10; 
int ledBrightness = 50;

// --- HARDWARE ---
// Adjust Pin 38 for your specific S3 board (48 is common for S3 Zero/DevKit)
#define LED_PIN 38
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- OBJECTS ---
USBHIDKeyboard Keyboard;
AsyncWebServer server(80);

// --- MEMORY & CONCURRENCY ---
char *psramBuffer = NULL;
size_t bufferIndex = 0;
const size_t BUFFER_SIZE = 1024 * 1024 * 2; // 2MB

volatile bool isWorkerBusy = false; 
volatile bool stopScriptFlag = false; // Flag to interrupt typing

// --- JOB QUEUE ---
struct DuckyJob {
  size_t length;
  bool isRawText;
};
QueueHandle_t jobQueue;

// --- LED HELPERS ---
void setStatus(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

// --- SETTINGS ---
void loadSettings() {
  if (LittleFS.exists("/settings.json")) {
    File file = LittleFS.open("/settings.json", "r");
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, file);
    file.close();
    if(doc.containsKey("ap_ssid")) ap_ssid = doc["ap_ssid"].as<String>();
    if(doc.containsKey("ap_pass")) ap_pass = doc["ap_pass"].as<String>();
    if(doc.containsKey("sta_ssid")) sta_ssid = doc["sta_ssid"].as<String>();
    if(doc.containsKey("sta_pass")) sta_pass = doc["sta_pass"].as<String>();
    if(doc.containsKey("delay")) typeDelay = doc["delay"];
    if(doc.containsKey("bright")) ledBrightness = doc["bright"];
    pixels.setBrightness(ledBrightness);
  }
}

void saveSettings(String json) {
  File file = LittleFS.open("/settings.json", "w");
  file.print(json);
  file.close();
  loadSettings(); 
}

// --- TYPING ENGINE ---
void typeTextInternal(size_t startIndex, size_t length) {
  delay(100); 
  
  for (size_t i = 0; i < length; i++) {
    // EMERGENCY STOP CHECK
    if (stopScriptFlag) return;

    char c = psramBuffer[startIndex + i];
    Keyboard.write(c);
    
    int d = (typeDelay < 5) ? 5 : typeDelay;
    delay(d); 
    
    // Throttling for OS buffer
    if (i > 0 && i % 15 == 0) {
      delay(20); 
      vTaskDelay(1); 
    }
    
    if (c == '\n') delay(100);
  }
}

void parseAndExecuteInternal(size_t totalLength) {
  size_t i = 0;
  int lineCount = 0;
  
  while (i < totalLength) {
    if (stopScriptFlag) break; // EMERGENCY STOP

    size_t lineStart = i;
    size_t lineEnd = i;
    while (lineEnd < totalLength && psramBuffer[lineEnd] != '\n') {
      lineEnd++;
    }
    
    size_t lineLen = lineEnd - lineStart;
    char lineBuf[512]; 
    memset(lineBuf, 0, 512);
    
    size_t copyLen = (lineLen > 511) ? 511 : lineLen;
    memcpy(lineBuf, psramBuffer + lineStart, copyLen);
    String line = String(lineBuf);
    line.trim();

    lineCount++;
    if (lineCount % 5 == 0) vTaskDelay(1); // Anti-Watchdog bite

    // --- PARSING ---
    if (line.equals("BLOCK")) {
      size_t searchPos = lineEnd;
      size_t blockEnd = 0;
      
      while (searchPos < totalLength - 8) {
        if (strncmp(psramBuffer + searchPos, "ENDBLOCK", 8) == 0) {
          blockEnd = searchPos;
          break;
        }
        searchPos++;
      }

      if (blockEnd > 0) {
        size_t blockStart = lineEnd + 1; 
        if (blockStart < blockEnd) {
           typeTextInternal(blockStart, blockEnd - blockStart);
        }
        size_t nextLine = blockEnd;
        while (nextLine < totalLength && psramBuffer[nextLine] != '\n') nextLine++;
        i = nextLine + 1;
        continue;
      }
    }

    if (line.startsWith("STRING ")) {
      if (lineLen > 7) typeTextInternal(lineStart + 7, lineLen - 7);
    }
    else if (line.startsWith("DELAY ")) delay(line.substring(6).toInt());
    else if (line.equals("ENTER")) Keyboard.press(KEY_RETURN);
    else if (line.equals("TAB")) Keyboard.press(KEY_TAB);
    else if (line.equals("GUI") || line.equals("WINDOWS")) { Keyboard.press(KEY_LEFT_GUI); delay(200); } 
    else if (line.startsWith("GUI ") && line.length() > 4) { Keyboard.press(KEY_LEFT_GUI); Keyboard.print(line.substring(4, 5)); delay(200); }
    // Add more keys as needed here (SHIFT, ALT, etc.)

    Keyboard.releaseAll();
    i = lineEnd + 1; 
    delay(20); 
  }
}

// --- WORKER TASK ---
void duckyWorkerTask(void * parameter) {
  DuckyJob job;
  for(;;) {
    if (xQueueReceive(jobQueue, &job, portMAX_DELAY)) {
      isWorkerBusy = true;
      stopScriptFlag = false; // Reset stop flag on new job
      setStatus(0, 0, 255); // Blue
      
      delay(500); 

      if (job.isRawText) {
        typeTextInternal(0, job.length);
      } else {
        parseAndExecuteInternal(job.length);
      }
      
      setStatus(255, 255, 255); // White
      vTaskDelay(500);
      setStatus(0, 255, 0); // Green
      isWorkerBusy = false;
    }
  }
}

void queueJob(bool isRaw) {
  DuckyJob job = { bufferIndex, isRaw };
  xQueueSend(jobQueue, &job, portMAX_DELAY);
}

// --- HTML FRONTEND (Updated with Stop Button) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ESP32 Ducky OS</title>
  <style>
    :root { --bg: #121212; --panel: #1e1e1e; --border: #333; --accent: #007acc; --text: #e0e0e0; --success: #2e7d32; --danger: #c62828; }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: 'Segoe UI', sans-serif; background: var(--bg); color: var(--text); display: flex; height: 100dvh; overflow: hidden; width: 100vw; }
    #sidebar { width: 60px; background: var(--panel); border-right: 1px solid var(--border); display: flex; flex-direction: column; align-items: center; padding-top: 10px; transition: width 0.2s; z-index: 100; height: 100%; flex-shrink: 0; }
    #sidebar:hover { width: 180px; }
    #sidebar:hover .nav-label { display: inline; opacity: 1; }
    .nav-item { width: 100%; padding: 15px 0; cursor: pointer; display: flex; justify-content: center; align-items: center; color: #aaa; transition: 0.2s; white-space: nowrap; overflow: hidden; position: relative;}
    .nav-item:hover, .nav-item.active { background: #2d2d2d; color: white; border-left: 3px solid var(--accent); }
    #sidebar:hover .nav-item { justify-content: flex-start; padding-left: 20px; }
    .nav-icon { display: flex; align-items: center; justify-content: center; width: 24px; height: 24px; flex-shrink: 0;}
    .nav-icon svg { width: 24px; height: 24px; fill: currentColor; }
    .nav-label { margin-left: 15px; font-size: 14px; opacity: 0; transition: opacity 0.2s; }
    #main { flex-grow: 1; position: relative; display: flex; flex-direction: column; height: 100%; overflow: hidden; width: 100%; }
    .view { display: none; height: 100%; width: 100%; flex-direction: column; overflow: hidden; }
    .view.active { display: flex; }
    .scroll-container { padding: 20px; overflow-y: auto; height: 100%; -webkit-overflow-scrolling: touch; padding-bottom: 80px; }
    .editor-container { display: flex; flex-grow: 1; height: 100%; overflow: hidden; }
    .file-sidebar { width: 200px; background: #181818; border-right: 1px solid var(--border); display: flex; flex-direction: column; flex-shrink: 0; }
    .file-header { padding: 10px; border-bottom: 1px solid var(--border); font-weight: bold; font-size: 14px; background: var(--panel); display: flex; justify-content: space-between; align-items: center;}
    .file-list { flex-grow: 1; overflow-y: auto; }
    .file-item { padding: 10px 15px; cursor: pointer; border-bottom: 1px solid #222; font-size: 13px; color: #ccc;}
    .file-item:hover { background: #252526; color: white; }
    .file-item.selected { background: #37373d; color: white; border-left: 3px solid var(--accent); }
    .editor-main { flex-grow: 1; display: flex; flex-direction: column; background: #1e1e1e; overflow: hidden; width: 0; }
    .toolbar { height: 50px; background: #252526; border-bottom: 1px solid var(--border); display: flex; align-items: center; padding: 0 10px; gap: 8px; overflow-x: auto; flex-shrink: 0; }
    .tool-btn { background: #333; border: 1px solid #444; color: white; padding: 6px 12px; border-radius: 4px; cursor: pointer; font-size: 12px; display: flex; align-items: center; gap: 5px; white-space: nowrap; flex-shrink: 0; }
    .tool-btn:hover { background: #444; }
    .btn-run { background: var(--success); border-color: var(--success); }
    .btn-del { background: var(--danger); border-color: var(--danger); }
    .btn-save { background: var(--accent); border-color: var(--accent); }
    #code-area { flex-grow: 1; background: #1e1e1e; color: #d4d4d4; border: none; padding: 15px; font-family: 'Consolas', monospace; font-size: 14px; resize: none; outline: none; line-height: 1.5; white-space: pre; overflow-wrap: normal; overflow: auto; }
    .status-bar { height: 25px; background: #007acc; color: white; font-size: 11px; display: flex; align-items: center; padding: 0 10px; justify-content: space-between; flex-shrink: 0; }
    .panel-box { background: var(--panel); padding: 20px; border-radius: 8px; max-width: 800px; margin: 0 auto; width: 100%; margin-bottom: 20px; }
    .kb-row { display: flex; justify-content: center; gap: 4px; margin-bottom: 4px; }
    .key { background: #333; color: white; border-radius: 4px; padding: 12px 0; flex-grow: 1; text-align: center; cursor: pointer; user-select: none; font-weight: bold; box-shadow: 0 2px 0 #111; font-size: 14px; min-width: 25px;}
    .key:active { transform: translateY(2px); box-shadow: none; background: var(--accent); }
    .key.active { background: var(--success); border: 1px solid #4caf50; }
    .key-wide { flex-grow: 1.5; } .key-space { flex-grow: 6; }
    .remote-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-top: 10px; }
    .key-btn { background: #333; padding: 15px; border-radius: 6px; text-align: center; cursor: pointer; user-select: none; font-weight: bold; }
    .key-btn:active { background: var(--accent); transform: scale(0.98); }
    .input-group { margin-bottom: 15px; position: relative; }
    label { display: block; margin-bottom: 5px; color: #888; font-size: 12px; font-weight: bold; }
    input { width: 100%; padding: 10px; background: #111; border: 1px solid #444; color: white; border-radius: 4px; }
    .eye-icon { position: absolute; right: 10px; top: 32px; cursor: pointer; color: #888; }
    textarea.remote-input { width: 100%; height: 80px; background: #222; border: 1px solid #444; color: white; margin-bottom: 10px; padding: 5px; }
    .section-title { font-size: 12px; color: var(--accent); text-transform: uppercase; letter-spacing: 1px; margin-top: 10px; margin-bottom: 10px; border-bottom: 1px solid #333; padding-bottom: 5px;}
    #live-status { text-align: center; margin-top: 10px; font-weight: bold; font-size: 13px; min-height: 20px;}
    .status-ok { color: var(--success); } .status-busy { color: #ffa726; }
    @media (max-width: 600px) { .file-sidebar { display: none; } .editor-container { flex-direction: column; } .file-header { display:flex; } }
  </style>
</head>
<body>
  <div id="sidebar">
    <div class="nav-item active" onclick="setView('editor')"><div class="nav-icon"><svg viewBox="0 0 24 24"><path d="M14 2H6c-1.1 0-1.99.9-1.99 2L4 20c0 1.1.89 2 1.99 2H18c1.1 0 2-.9 2-2V8l-6-6zm2 16H8v-2h8v2zm0-4H8v-2h8v2zm-3-5V3.5L18.5 9H13z"/></svg></div><span class="nav-label">Editor</span></div>
    <div class="nav-item" onclick="setView('keyboard')"><div class="nav-icon"><svg viewBox="0 0 24 24"><path d="M20 5H4c-1.1 0-1.99.9-1.99 2L2 17c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2zm-9 3h2v2h-2V8zm0 3h2v2h-2v-2zM8 8h2v2H8V8zm0 3h2v2H8v-2zm-1 2H5v-2h2v2zm0-3H5V8h2v2zm9 7H8v-2h8v2zm0-4h-2v-2h2v2zm0-3h-2V8h2v2zm3 3h-2v-2h2v2zm0-3h-2V8h2v2z"/></svg></div><span class="nav-label">Keyboard</span></div>
    <div class="nav-item" onclick="setView('remote')"><div class="nav-icon"><svg viewBox="0 0 24 24"><path d="M21 6H3c-1.1 0-2 .9-2 2v8c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2zm-10 7H8v3H6v-3H3v-2h3V8h2v3h3v2zm4.5 2c-.83 0-1.5-.67-1.5-1.5s.67-1.5 1.5-1.5 1.5.67 1.5 1.5-.67 1.5-1.5 1.5zm4 0c-.83 0-1.5-.67-1.5-1.5s.67-1.5 1.5-1.5 1.5.67 1.5 1.5-.67 1.5-1.5 1.5z"/></svg></div><span class="nav-label">Remote</span></div>
    <div class="nav-item" onclick="setView('settings')"><div class="nav-icon"><svg viewBox="0 0 24 24"><path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.488.488 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.484.484 0 0 0-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg></div><span class="nav-label">Settings</span></div>
  </div>

  <div id="main">
    <div id="view-editor" class="view active">
      <div class="editor-container">
        <div class="file-sidebar">
          <div class="file-header">EXPLORER <button onclick="newFile()" style="background:none; border:none; color:white; cursor:pointer; font-size:18px;">+</button></div>
          <div id="file-list" class="file-list"></div>
        </div>
        <div class="editor-main">
          <div class="toolbar">
            <button class="tool-btn btn-save" onclick="saveFile()">💾 Save</button>
            <button class="tool-btn btn-run" onclick="runScript()">▶ Run</button>
            <button class="tool-btn btn-del" onclick="stopScript()">⏹ Stop</button>
            <button class="tool-btn" onclick="downloadFile()">⬇ Download</button>
            <button class="tool-btn btn-del" onclick="delCurrent()">🗑 Delete</button>
            <div style="flex-grow:1"></div>
            <span id="current-filename" style="color:#aaa; font-size:12px; margin-right:10px;">Untitled.txt</span>
          </div>
          <textarea id="code-area" spellcheck="false" placeholder="// Select a file or create new..."></textarea>
          <div class="status-bar"><span id="status-msg">Ready</span><span>ESP32-S3 Ducky</span></div>
        </div>
      </div>
    </div>

    <div id="view-keyboard" class="view">
      <div class="scroll-container">
        <div class="panel-box">
          <h2 style="text-align:center">Virtual Keyboard</h2>
          <div id="kb-container">
            <div class="kb-row"><div class="key" onclick="type('!')">!</div><div class="key" onclick="type('@')">@</div><div class="key" onclick="type('#')">#</div><div class="key" onclick="type('$')">$</div><div class="key" onclick="type('%')">%</div><div class="key" onclick="type('^')">^</div><div class="key" onclick="type('&')">&</div><div class="key" onclick="type('*')">*</div><div class="key" onclick="type('(')">(</div><div class="key" onclick="type(')')">)</div><div class="key" onclick="type('_')">_</div><div class="key" onclick="type('+')">+</div></div>
             <div class="kb-row"><div class="key" onclick="sendKey(177)">ESC</div><div class="key" onclick="type('1')">1</div><div class="key" onclick="type('2')">2</div><div class="key" onclick="type('3')">3</div><div class="key" onclick="type('4')">4</div><div class="key" onclick="type('5')">5</div><div class="key" onclick="type('6')">6</div><div class="key" onclick="type('7')">7</div><div class="key" onclick="type('8')">8</div><div class="key" onclick="type('9')">9</div><div class="key" onclick="type('0')">0</div><div class="key" onclick="sendKey(178)">⌫</div></div>
             <div class="kb-row"><div class="key key-wide" onclick="sendKey(179)">TAB</div><div class="key" onclick="type('q')">Q</div><div class="key" onclick="type('w')">W</div><div class="key" onclick="type('e')">E</div><div class="key" onclick="type('r')">R</div><div class="key" onclick="type('t')">T</div><div class="key" onclick="type('y')">Y</div><div class="key" onclick="type('u')">U</div><div class="key" onclick="type('i')">I</div><div class="key" onclick="type('o')">O</div><div class="key" onclick="type('p')">P</div></div>
             <div class="kb-row"><div id="key-caps" class="key key-wide" onclick="toggleCaps()">CAPS</div><div class="key" onclick="type('a')">A</div><div class="key" onclick="type('s')">S</div><div class="key" onclick="type('d')">D</div><div class="key" onclick="type('f')">F</div><div class="key" onclick="type('g')">G</div><div class="key" onclick="type('h')">H</div><div class="key" onclick="type('j')">J</div><div class="key" onclick="type('k')">K</div><div class="key" onclick="type('l')">L</div><div class="key key-wide" onclick="sendKey(176)">ENTER</div></div>
             <div class="kb-row"><div id="key-shift" class="key key-wide" onclick="toggleMod('shift')">SHIFT</div><div class="key" onclick="type('z')">Z</div><div class="key" onclick="type('x')">X</div><div class="key" onclick="type('c')">C</div><div class="key" onclick="type('v')">V</div><div class="key" onclick="type('b')">B</div><div class="key" onclick="type('n')">N</div><div class="key" onclick="type('m')">M</div><div class="key" onclick="type(',')">,</div><div class="key" onclick="type('.')">.</div><div id="key-shift-r" class="key key-wide" onclick="toggleMod('shift')">SHIFT</div></div>
             <div class="kb-row"><div id="key-ctrl" class="key" onclick="toggleMod('ctrl')">CTRL</div><div class="key" onclick="sendKey(131)">WIN</div><div id="key-alt" class="key" onclick="toggleMod('alt')">ALT</div><div class="key key-space" onclick="type(' ')">SPACE</div><div class="key" onclick="sendKey(216)">⬅</div><div class="key" onclick="sendKey(218)">⬆</div><div class="key" onclick="sendKey(217)">⬇</div><div class="key" onclick="sendKey(215)">➡</div></div>
          </div>
        </div>
      </div>
    </div>

    <div id="view-remote" class="view">
      <div class="scroll-container">
        <div class="panel-box">
          <h2>Live Control</h2>
          <textarea class="remote-input" id="live-text" placeholder="Paste text here..."></textarea>
          <div style="display:flex; gap:10px;">
             <button id="btn-inject" class="tool-btn btn-save" style="flex:1; justify-content:center; padding:10px;" onclick="sendLiveText()">Inject</button>
             <button class="tool-btn btn-del" style="width:80px; justify-content:center; padding:10px;" onclick="stopScript()">Stop</button>
          </div>
          <div id="live-status"></div>
          <label style="margin-top:15px">Shortcuts</label>
          <div class="remote-grid">
             <div class="key-btn" style="background:#0d47a1" onclick="sendCombo('a')">Select All</div>
             <div class="key-btn" style="background:#0d47a1" onclick="sendCombo('c')">Copy</div>
             <div class="key-btn" style="background:#0d47a1" onclick="sendCombo('v')">Paste</div>
          </div>
          <label style="margin-top:15px">Navigation</label>
          <div class="remote-grid">
            <div class="key-btn" onclick="sendKey(177)">ESC</div><div class="key-btn" onclick="sendKey(218)">⬆</div><div class="key-btn" onclick="sendKey(179)">TAB</div>
            <div class="key-btn" onclick="sendKey(216)">⬅</div><div class="key-btn" onclick="sendKey(217)">⬇</div><div class="key-btn" onclick="sendKey(215)">➡</div>
            <div class="key-btn" onclick="sendKey(131)">WIN</div><div class="key-btn" onclick="sendKey(176)">ENTER</div><div class="key-btn" onclick="sendKey(178)">⌫</div>
          </div>
        </div>
      </div>
    </div>

    <div id="view-settings" class="view">
      <div class="scroll-container">
        <div class="panel-box">
          <h2>Settings</h2>
          <div class="section-title">Access Point (Hotspot)</div>
          <div class="input-group"><label>AP SSID</label><input type="text" id="conf-ap-ssid"></div>
          <div class="input-group"><label>AP Password</label><input type="password" id="conf-ap-pass"><span class="eye-icon" onclick="togglePass('conf-ap-pass')">👁</span></div>
          <div class="section-title">Station (Router Connection)</div>
          <div class="input-group"><label>Router SSID</label><input type="text" id="conf-sta-ssid"></div>
          <div class="input-group"><label>Router Password</label><input type="password" id="conf-sta-pass"><span class="eye-icon" onclick="togglePass('conf-sta-pass')">👁</span></div>
          <div class="section-title">Preferences</div>
          <div class="input-group"><label>Typing Delay (ms)</label><input type="number" id="conf-delay"></div>
          <div class="input-group"><label>LED Brightness (0-255)</label><input type="number" id="conf-bright"></div>
          <button class="tool-btn btn-save" style="width:100%; justify-content:center; padding:10px; margin-top:10px;" onclick="saveSettings()">Save & Apply</button>
          <button class="tool-btn btn-del" style="width:100%; justify-content:center; padding:10px; margin-top:10px;" onclick="reboot()">Reboot Device</button>
        </div>
      </div>
    </div>
  </div>

<script>
  let currentFile = "";
  let checkInterval = null;
  let mods = { shift: false, ctrl: false, alt: false };
  let caps = false;

  function setView(id) {
    document.querySelectorAll('.view').forEach(e => e.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(e => e.classList.remove('active'));
    document.getElementById('view-'+id).classList.add('active');
    event.currentTarget.classList.add('active');
    if(id === 'settings') loadSettings();
  }

  function status(msg) { document.getElementById('status-msg').innerText = msg; }
  
  function loadFiles() {
    fetch('/list').then(r=>r.json()).then(files => {
      const list = document.getElementById('file-list'); list.innerHTML = "";
      files.forEach(f => {
        if(f.name.endsWith("settings.json")) return;
        let d = document.createElement('div'); d.className = 'file-item';
        if(currentFile === f.name) d.classList.add('selected');
        d.innerText = f.name.replace('/',''); d.onclick = () => loadFile(f.name);
        list.appendChild(d);
      });
    });
  }

  function loadFile(n) { currentFile = n; document.getElementById('current-filename').innerText = n; status("Loading..."); fetch('/load?name='+n).then(r=>r.text()).then(t => { document.getElementById('code-area').value = t; status("Loaded"); loadFiles(); }); }
  function newFile() { let n = prompt("Filename:"); if(!n) return; if(!n.startsWith("/")) n = "/"+n; currentFile=n; document.getElementById('current-filename').innerText=n; document.getElementById('code-area').value="GUI r\nDELAY 500\nSTRING notepad\nENTER"; }
  function saveFile() { if(!currentFile) return newFile(); let d = new FormData(); d.append("data", new Blob([document.getElementById('code-area').value]), currentFile); status("Saving..."); fetch('/edit', { method: 'POST', body: d }).then(() => { status("Saved"); loadFiles(); }); }
  function downloadFile() { if(!currentFile) return; const b = new Blob([document.getElementById('code-area').value], {type:'text/plain'}); const u = URL.createObjectURL(b); const a = document.createElement('a'); a.href=u; a.download=currentFile.replace('/',''); a.click(); }
  function delCurrent() { if(!currentFile) return; if(confirm("Delete?")) fetch('/delete?name='+currentFile, {method:'DELETE'}).then(() => { currentFile=""; document.getElementById('code-area').value=""; loadFiles(); }); }
  
  function runScript() { 
    if(!document.getElementById('code-area').value) return; 
    status("Queued..."); 
    fetch('/run', { method: 'POST', body: document.getElementById('code-area').value })
    .then(r => { 
       if(r.status === 503) status("Device Busy!");
       else status("Running...");
    }); 
  }

  function stopScript() {
    fetch('/stop', { method: 'POST' }).then(() => status("Stopped"));
  }

  function toggleMod(m) {
    mods[m] = !mods[m];
    if(m==='ctrl') document.getElementById('key-ctrl').classList.toggle('active', mods[m]);
    if(m==='shift') { document.getElementById('key-shift').classList.toggle('active', mods[m]); document.getElementById('key-shift-r').classList.toggle('active', mods[m]); }
    if(m==='alt') document.getElementById('key-alt').classList.toggle('active', mods[m]);
  }
  function toggleCaps() { caps = !caps; document.getElementById('key-caps').classList.toggle('active', caps); }

  function type(char) {
    if (mods.ctrl || mods.alt) {
        let payload = { char: char };
        if(mods.ctrl) payload.ctrl = true; if(mods.alt) payload.alt = true; if(mods.shift) payload.shift = true;
        mods.ctrl=false; mods.alt=false; mods.shift=false; updateModVisuals();
        fetch('/live_combo', { method: 'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({char: char}) });
    } else {
        if (mods.shift || caps) { char = char.toUpperCase(); }
        fetch('/live_text', { method: 'POST', body: char });
        if(mods.shift) { mods.shift = false; updateModVisuals(); }
    }
  }

  function updateModVisuals() {
    document.getElementById('key-ctrl').classList.toggle('active', mods.ctrl);
    document.getElementById('key-shift').classList.toggle('active', mods.shift);
    document.getElementById('key-shift-r').classList.toggle('active', mods.shift);
    document.getElementById('key-alt').classList.toggle('active', mods.alt);
  }

  function sendKey(code) { fetch('/live_key', { method: 'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({code:code}) }); }
  function sendCombo(c) { fetch('/live_combo', { method: 'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({char:c}) }); }
  
  function sendLiveText() {
    const txt = document.getElementById('live-text').value; if(!txt) return;
    document.getElementById('btn-inject').disabled = true; document.getElementById('btn-inject').innerText = "Sending...";
    fetch('/live_text', { method: 'POST', body: txt }).then(r => {
      if (r.status === 503) {
         alert("Device is busy running another script.");
         document.getElementById('btn-inject').disabled = false;
         document.getElementById('btn-inject').innerText = "Inject";
      } else {
         document.getElementById('live-text').value = ""; document.getElementById('btn-inject').innerText = "Queued";
         startProgressCheck();
      }
    });
  }

  function startProgressCheck() {
    if(checkInterval) clearInterval(checkInterval);
    const s = document.getElementById('live-status'); s.innerHTML = "<span class='status-busy'>Typing...</span>";
    checkInterval = setInterval(() => {
      fetch('/status').then(r=>r.json()).then(d => {
        if(!d.busy) { clearInterval(checkInterval); s.innerHTML = "<span class='status-ok'>Finished!</span>"; document.getElementById('btn-inject').innerText = "Inject"; document.getElementById('btn-inject').disabled = false; setTimeout(() => s.innerHTML="", 3000); }
      });
    }, 1000);
  }

  function loadSettings() { fetch('/get_settings').then(r=>r.json()).then(d => { document.getElementById('conf-ap-ssid').value=d.ap_ssid||""; document.getElementById('conf-ap-pass').value=d.ap_pass||""; document.getElementById('conf-sta-ssid').value=d.sta_ssid||""; document.getElementById('conf-sta-pass').value=d.sta_pass||""; document.getElementById('conf-delay').value=d.delay||5; document.getElementById('conf-bright').value=d.bright||50; }); }
  function saveSettings() { const d={ap_ssid:document.getElementById('conf-ap-ssid').value, ap_pass:document.getElementById('conf-ap-pass').value, sta_ssid:document.getElementById('conf-sta-ssid').value, sta_pass:document.getElementById('conf-sta-pass').value, delay:parseInt(document.getElementById('conf-delay').value), bright:parseInt(document.getElementById('conf-bright').value)}; fetch('/save_settings', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(d)}).then(()=>alert("Saved")); }
  function togglePass(id) { const e=document.getElementById(id); e.type=(e.type==="password")?"text":"password"; }
  function reboot() { if(confirm("Reboot?")) fetch('/reboot', { method: 'POST' }); }
  window.onload = loadFiles;
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  if(!LittleFS.begin(true)){ Serial.println("LittleFS Error"); }
  loadSettings(); 
  pixels.begin(); pixels.setBrightness(ledBrightness); setStatus(0, 0, 255); 
  
  // ALLOCATE 2MB BUFFER IN PSRAM
  psramBuffer = (char*) heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM);
  if (!psramBuffer) {
    Serial.println("PSRAM MALLOC FAILED!");
    setStatus(255, 0, 0); // Red Error
    while(1);
  }

  USB.begin(); Keyboard.begin();
  
  jobQueue = xQueueCreate(10, sizeof(DuckyJob));
  xTaskCreatePinnedToCore(duckyWorkerTask, "DuckyWorker", 16384, NULL, 1, NULL, 1);

  bool staConnected = false;
  if (sta_ssid != "") {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
    long start = millis();
    while (millis() - start < 10000) {
      if (WiFi.status() == WL_CONNECTED) { staConnected = true; break; }
      delay(500);
    }
  }

  if (staConnected) { WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str()); } 
  else { WiFi.mode(WIFI_AP); WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str()); }
  setStatus(0, 255, 0); 

  // ROUTES
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html", index_html); });
  
  // -- RUN ENDPOINT WITH LOCK --
  server.on("/run", HTTP_POST, [](AsyncWebServerRequest *r){ 
      if(isWorkerBusy) r->send(503, "text/plain", "Busy");
      else r->send(200); 
    }, NULL, 
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
      if (isWorkerBusy) return; // Prevent overwriting while working
      if (index == 0) bufferIndex = 0;
      if (bufferIndex + len < BUFFER_SIZE) {
        memcpy(psramBuffer + bufferIndex, data, len);
        bufferIndex += len;
      }
      if (index + len == total) {
        psramBuffer[bufferIndex] = '\0'; 
        queueJob(false); 
      }
  });
  
  // -- LIVE TEXT ENDPOINT WITH LOCK --
  server.on("/live_text", HTTP_POST, [](AsyncWebServerRequest *r){ 
      if(isWorkerBusy) r->send(503, "text/plain", "Busy");
      else r->send(200); 
    }, NULL, 
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
      if (isWorkerBusy) return;
      if (index == 0) bufferIndex = 0;
      if (bufferIndex + len < BUFFER_SIZE) {
        memcpy(psramBuffer + bufferIndex, data, len);
        bufferIndex += len;
      }
      if (index + len == total) {
        psramBuffer[bufferIndex] = '\0';
        queueJob(true); 
      }
  });

  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest *r){ stopScriptFlag = true; r->send(200); });
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *r){ File root=LittleFS.open("/"); File f=root.openNextFile(); String j="["; while(f){ if(j!="[") j+=","; j+="{\"name\":\""+String(f.name())+"\"}"; f=root.openNextFile(); } j+="]"; r->send(200, "application/json", j); });
  server.on("/load", HTTP_GET, [](AsyncWebServerRequest *r){ if(r->hasParam("name")) r->send(LittleFS, r->getParam("name")->value(), "text/plain"); });
  server.on("/delete", HTTP_DELETE, [](AsyncWebServerRequest *r){ if(r->hasParam("name")) LittleFS.remove(r->getParam("name")->value()); r->send(200); });
  server.on("/edit", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); }, [](AsyncWebServerRequest *r, String f, size_t index, uint8_t *data, size_t len, bool final){ if(!index) r->_tempFile = LittleFS.open(f, "w"); if(r->_tempFile) r->_tempFile.write(data, len); if(final) r->_tempFile.close(); });
  server.on("/live_key", HTTP_POST, [](AsyncWebServerRequest *r){}, NULL, [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) { DynamicJsonDocument doc(256); deserializeJson(doc, data); Keyboard.press(doc["code"].as<int>()); delay(150); Keyboard.releaseAll(); r->send(200); });
  server.on("/live_combo", HTTP_POST, [](AsyncWebServerRequest *r){}, NULL, [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) { DynamicJsonDocument doc(256); deserializeJson(doc, data); String c = doc["char"]; Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(c[0]); delay(150); Keyboard.releaseAll(); r->send(200); });
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){ String json = isWorkerBusy ? "{\"busy\":true}" : "{\"busy\":false}"; r->send(200, "application/json", json); });
  server.on("/get_settings", HTTP_GET, [](AsyncWebServerRequest *r){ DynamicJsonDocument doc(512); doc["ap_ssid"]=ap_ssid; doc["ap_pass"]=ap_pass; doc["sta_ssid"]=sta_ssid; doc["sta_pass"]=sta_pass; doc["delay"]=typeDelay; doc["bright"]=ledBrightness; String json; serializeJson(doc, json); r->send(200, "application/json", json); });
  server.on("/save_settings", HTTP_POST, [](AsyncWebServerRequest *r){}, NULL, [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) { static String jsonSettings; if (index == 0) jsonSettings = ""; for(size_t i=0; i<len; i++) jsonSettings += (char)data[i]; if (index + len == total) { saveSettings(jsonSettings); r->send(200); } });
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); delay(500); ESP.restart(); });
  server.begin();
}

void loop() { vTaskDelay(1000); }