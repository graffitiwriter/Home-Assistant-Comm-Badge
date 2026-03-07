#!/usr/bin/env python3
"""Sync LCARS_CSS from the .ino into preview.html."""

import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
INO = ROOT / "HA_Comm_Badge_V1.ino"
PREVIEW = ROOT / "preview.html"

TEMPLATE = """\
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>COMM BADGE CONFIG</title>
  <style>
  </style>
</head>
<body>

<div class="lcars-frame">

  <div class="lcars-header">
    <h1>COMM BADGE CONFIGURATION</h1>
    <span>STARFLEET ISSUE</span>
  </div>

  <div class="lcars-body">

    <div class="lcars-sidebar">
      <div class="lcars-pill pill-orange" style="flex:2">LCARS</div>
      <div class="lcars-pill pill-blue">47</div>
      <div class="lcars-pill pill-purple">148</div>
      <div class="lcars-pill pill-tan" style="flex:3">CONFIG</div>
      <div class="lcars-pill pill-blue">22</div>
      <div class="lcars-pill pill-orange" style="flex:2">1701</div>
    </div>

    <div class="lcars-content">
      <form action="#" method="POST" onsubmit="return false">

        <!-- Status bar (hardcoded for preview) -->
        <div class="status-bar">
          <div class="status-item"><span class="lbl">POWER:</span><span class="val">82%</span></div>
          <div class="status-item"><span class="lbl">SIGNAL:</span><span class="val">-58 dBm</span></div>
          <div class="status-item"><span class="lbl">VERSION:</span><span class="val">3.8</span></div>
        </div>

        <!-- WiFi -->
        <div class="section blue">
          <h2>SUBSPACE RELAY (WIFI)</h2>
          <div class="row">
            <div class="field">
              <label>NETWORK SSID</label>
              <input name="ssid" value="EnterpriseD">
            </div>
            <div class="field">
              <label>ACCESS CODE</label>
              <input type="password" name="pass" value="password">
            </div>
          </div>
        </div>

        <!-- LLM -->
        <div class="section purple">
          <h2>UNIVERSAL TRANSLATOR (SPEECH RECOGNITION)</h2>
          <div class="field">
            <label>AI PROVIDER</label>
            <select name="llmProvider" id="llmProvider">
              <option value="groq" selected>GROQ (RECOMMENDED)</option>
              <option value="openai">OPENAI</option>
              <option value="custom">CUSTOM WHISPER ENDPOINT</option>
            </select>
            <div class="provider-warning" id="providerWarning">&#x2705; Official Groq Whisper endpoint (fastest)</div>
          </div>
          <div class="field">
            <label>API KEY</label>
            <input type="password" name="llmKey" value="gsk_examplekey">
          </div>
          <div class="field">
            <label>API URL</label>
            <input name="llmUrl" id="llmUrl" value="https://api.groq.com/openai/v1/audio/transcriptions">
          </div>
          <div class="field">
            <label>VOICE ACTIVITY DETECTION SENSITIVITY</label>
            <input type="range" name="vadSens" min="1" max="10" value="5" style="width:100%">
            <div style="display:flex;justify-content:space-between;font-size:12px">
              <span>Very Sensitive</span><span>Less Sensitive</span>
            </div>
            <div style="color:#666;font-size:11px;margin-top:5px">1=stops quickly after speech | 10=waits longer</div>
          </div>
        </div>

        <!-- Home Assistant -->
        <div class="section tan">
          <h2>SHIP'S COMPUTER (HOME ASSISTANT)</h2>
          <div class="field">
            <label>SHIP'S COMPUTER ADDRESS</label>
            <div style="display:flex;gap:8px;align-items:center">
              <select name="haProto" id="haProto" style="width:auto;flex:0 0 auto;appearance:none;-webkit-appearance:none" onchange="updateHaProtoWarn()">
                <option value="https://" selected>https://</option>
                <option value="http://">http://</option>
              </select>
              <input name="haHost" placeholder="10.0.0.2 or home.example.com" value="home.example.com" style="flex:1">
              <input type="number" name="haPort" placeholder="8123" min="1" max="65535" value="8123" style="flex:0 0 120px;width:120px">
            </div>
            <div id="haProtoWarn" class="provider-warning" style="display:none">
              &#x26A0; HTTP only &mdash; use for local network servers (e.g. 192.168.x.x). Never over the internet.
            </div>
          </div>
          <div class="field">
            <label>LONG-LIVED (and prosper) ACCESS TOKEN</label>
            <input type="password" name="hatoken" value="eyexampletoken">
          </div>
        </div>

        <!-- Sensor Calibration -->
        <div class="section blue">
          <h2>SENSOR CALIBRATION</h2>
          <div class="field">
            <label>TAP SENSITIVITY <span id="tapVal">3.4</span></label>
            <input type="range" name="tap" min="20" max="45" step="1" value="34" style="width:100%"
              oninput="document.getElementById('tapVal').textContent=(this.value/10).toFixed(1)">
            <div style="color:#666;font-size:11px;margin-top:5px">Gentle tap &#x2190; &#x2192; Firm tap</div>
          </div>
          <div class="row">
            <div class="field">
              <label>MAX RECORD TIME (sec)</label>
              <input type="number" min="1" max="6" name="rec" value="4">
            </div>
            <div class="field">
              <label>DEEP SLEEP TIMEOUT (sec)</label>
              <input type="number" min="10" max="120" name="sleep" value="60">
            </div>
          </div>
        </div>

        <button type="submit" class="btn btn-purple">ENGAGE</button>
        <button type="button" class="btn btn-orange" onclick="alert('Reset to factory defaults?')">RESET</button>

      </form>
    </div>
  </div>

  <div class="lcars-footer">
    <span>CHIEF ENGINEER: LIEUTENANT COMMANDER SPANNER  -  https://sthi.space</span>
    <span>LCARS ACCESS 47988</span>
  </div>

</div>

<script>
  function updateHaProtoWarn() {
    var w = document.getElementById('haProtoWarn');
    w.style.display = document.getElementById('haProto').value === 'http://' ? 'block' : 'none';
  }

  document.getElementById('llmProvider').onchange = function() {
    const urlField = document.getElementById('llmUrl');
    const warn = document.getElementById('providerWarning');
    switch(this.value) {
      case 'groq':
        urlField.value = 'https://api.groq.com/openai/v1/audio/transcriptions';
        warn.textContent = '\\u2705 Official Groq Whisper endpoint (fastest)';
        warn.style.color = '#99ccff';
        break;
      case 'openai':
        urlField.value = 'https://api.openai.com/v1/audio/transcriptions';
        warn.textContent = '\\u2705 Official OpenAI Whisper endpoint';
        warn.style.color = '#99ccff';
        break;
      case 'custom':
        urlField.value = '';
        warn.textContent = '\\U0001f527 Enter any Whisper-compatible API endpoint (e.g., proxy)';
        warn.style.color = '#ff9966';
        break;
    }
  };
</script>

</body>
</html>
"""

ino = open(INO).read()

css_match = re.search(r'R"rawliteral\((.*?)\)rawliteral"', ino, re.DOTALL)
if not css_match:
    print("ERROR: Could not find LCARS_CSS rawliteral block in .ino", file=sys.stderr)
    sys.exit(1)

css = css_match.group(1).strip()

created = False
if not PREVIEW.exists():
    PREVIEW.write_text(TEMPLATE)
    print("Created preview.html")
    created = True

preview = PREVIEW.read_text()

updated = re.sub(
    r'(<style>).*?(</style>)',
    lambda m: m.group(1) + "\n    " + css + "\n  " + m.group(2),
    preview,
    flags=re.DOTALL
)

if updated == preview and not created:
    print("preview.html already up to date")
else:
    PREVIEW.write_text(updated)
    if not created:
        print("Synced LCARS_CSS -> preview.html")

if created:
    subprocess.run(["open", str(PREVIEW)])
