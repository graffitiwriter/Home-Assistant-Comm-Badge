# Home Assistant Comm Badge

**Working Star Trek communications badge using M5StickC PLUS2 and Home Assistant. Tap your badge, issue voice commands, control your ship... er, home. Based on arduinohw but significantly evolved.**

A wearable voice control device that uses tap detection, voice recording, AI transcription, and Home Assistant integration to control your smart home with natural voice commands; all while looking like a proper Starfleet comm badge, and costing under 25 quid.
I used Groq as the free tier for Whisper never seems to hit the limit for home auto commands. Even with heavy use, you don't send that many commands, so I've not yet hit the limit. You can choose your own AI provider though.
The M5StickC PLUS2 has a magnet built into its case, so it combines with the cheap magnetic Star Trek comm badge to stick to your shirt, while still detecting the taps through the badge (tap sensitivity is adjustable).
I'd like to make it a little faster yet, but it's currently trading that off against battery life.

## Acknowledgements

This project was inspired by and based on Shay Moradi's (@organised) excellent [M5Stick with OpenAI Access project](https://github.com/organised/arduinohw). I went this route because I had an old M5StickC PLUS2 with a broken screen, so it was no use for written responses from the LLM. But I wanted to repeat what Shay had done, and this seemed like a way to use the knackered M5Stick for something!

## Features

### Core Functionality
- **Tap-to-Activate**: Tap the device to start recording. Uses the MPU6886 accelerometer for tap detection
- **Voice Activity Detection (VAD)**: Automatically stops recording when you finish speaking, and/or times out (adjustable)
- **AI Transcription**: Supports multiple LLM providers (Groq, OpenAI, or custom) for speech-to-text using Whisper models
- **Home Assistant Integration**: Sends transcribed commands directly to Home Assistant's conversation API
- **Audio Feedback**: Different beep patterns for activation, success, errors, etc

### Power Management
- **Wake-on-Motion**: Device sleeps deeply and wakes when movement is detected
- **Configurable Sleep Timer**: Set how long the device stays awake after use

### Configuration
- **Web-Based Setup**: Hold the front button during reset (or for 5 seconds while awake) to enter config mode
- **LCARS-Style Interface**: Because if you're building a comm badge, what other choice do you have?
- **Configurable Parameters**:
  - WiFi credentials
  - Home Assistant URL and API token
  - LLM provider (Groq/OpenAI/Custom) and API key
  - Tap sensitivity threshold
  - Maximum recording duration
  - VAD sensitivity (how quickly it stops after silence)
  - Deep sleep timeout

### Technical Features
- **Keep-Alive Connections**: HTTP connection pooling for faster response times
- **Adaptive Recording**: VAD stops recording after detecting silence, with minimum 0.8s duration
- **Config Migration**: Automatically migrates from older configuration formats
- **Fixed Recording Mode**: Optional fixed-duration recording as fallback

## Hardware Requirements

- **M5StickC PLUS2** (not compatible with original M5StickC)
- Built-in components used:
  - MPU6886 accelerometer (tap detection & wake-on-motion)
  - Built-in microphone
  - LCD display
  - Front button
  - Speaker (audio feedback)
- Star Trek comm badge. The one I got is magnetic, and since the M5StickC PLUS2 has a magnet built into the case, the two got together great.

## Software Dependencies

Install these libraries through the Arduino IDE Library Manager:
- M5Unified
- WiFiClientSecure
- HTTPClient
- WebServer
- ArduinoJson
- Preferences
- Wire

## Setup

### 1. Flash the Firmware
1. Open `HA_Comm_Badge_V1.ino` in Arduino IDE
2. Select **M5StickC PLUS2** as your board
3. Upload the sketch

### 2. Initial Configuration
On first boot (or by holding the front button during reset, or for 5 seconds while the M5 is awake), the device enters config mode:

1. The device creates a WiFi access point called **"CommBadge-Config"**. The default AP password is **starfleet**
2. Connect to this network
3. Open your web browser and navigate to **192.168.4.1**
4. Fill in the configuration form:
   - **WiFi Credentials**: Your home network details
   - **Home Assistant**: 
     - URL (e.g., `https://your-ha-instance.local:8123`)
     - Long-lived access token (generate this in HA profile settings)
   - **LLM Provider**: 
     - Select Groq, OpenAI, or Custom
     - Enter your API key
     - (Optional) Customise the API endpoint URL
   - **Advanced Settings**: 
     - Tap sensitivity (2.0-4.5, default 3.4)
     - Max recording time (default 4 seconds)
     - VAD sensitivity (1-10, default 5)
     - Sleep timeout (default 60 seconds)
5. Save the configuration
6. The device will restart and connect to your WiFi

### 3. Home Assistant Setup
Create a long-lived access token:
   - Go to your HA profile
   - Scroll down to "Long-lived access tokens"
   - Click "Create Token"
   - Give it a name (e.g., "Comm Badge")
   - Copy the token and paste it into the badge configuration

### 4. LLM API Setup

**Option 1: Groq (Recommended - Fast & Free Tier)**
1. Sign up at [groq.com](https://groq.com)
2. Get your API key from the console
3. Use the default URL: `https://api.groq.com/openai/v1/audio/transcriptions`

**Option 2: OpenAI**
1. Get an API key from [platform.openai.com](https://platform.openai.com)
2. Use URL: `https://api.openai.com/v1/audio/transcriptions`

**Option 3: Custom**
- Any Whisper-compatible API endpoint
- Must accept OpenAI-style multipart/form-data audio transcription requests

## Usage

1. **Tap the device** - You'll hear an activation beep when recording begins
2. **Speak your command** - The device records and uses VAD to detect when you've finished
3. **Wait for processing** - The audio is transcribed by Groq and sent to Home Assistant
4. **Success!** - A success beep confirms the command was understood and executed

The device will automatically enter deep sleep after the configured timeout to save battery. So far, it's lasting really well.

### Example Commands
- "Turn on the living room lights"
- "Set bedroom temperature to 20 degrees"
- "Turn the TV on"

## Configuration Reference
### Tap Threshold (2.0-4.5)
Controls how hard you need to tap the device. Lower = more sensitive.
- **2.0-2.5**: Very sensitive (might trigger from movement)
- **3.0-3.5**: Normal (recommended)
- **4.0-4.5**: Requires a good slap

### Recording Time (1-10 seconds)
Maximum duration of a recording (safety ceiling). VAD usually stops it before this.
- **Default**: 4 seconds (sufficient for most commands, I've found)

### VAD Sensitivity (1-10)
How quickly the system stops recording after detecting silence.
- **1-3**: Very sensitive (stops quickly, may cut off slow speakers)
- **4-6**: Normal (recommended)
- **7-10**: Less sensitive (keeps recording longer, good for slow/hesitant speech)

### Sleep Timeout (10-300 seconds)
How long the device stays awake after last activity before entering deep sleep.
- **Default**: 60 seconds
- **Lower**: Better battery life, a little slower to respond to taps
- **Higher**: More responsive, uses more battery

## Customisation
The code is commented (though I might have missed some stuff, or changed it without labelling properly) and hopefully structured for easy modification:
- **Audio feedback tones**: Modify the `play*Beep()` functions
- **Display**: Update the LCD rendering in `handleVoiceCommand()`
- **Recording behaviour**: Adjust VAD constants at the top of the sketch
- **UI styling**: Modify the LCARS_CSS in the config portal section

## Known Limitations

- M5StickC PLUS2 only (original M5StickC not supported due to different microphone)
- 2.4GHz WiFi only (ESP32 limitation)
- Requires internet connection for transcription
- Home Assistant must be reachable from the device

## Future Ideas
- Local wake word detection
- Wireless charging
- Size reduction
- Local LLM

## Contributing
Feel free to fork, modify, and submit pull requests! This is a hobby project born from Star Trek fandom and smart home enthusiasm.

## Licence
Not affiliated in any way with Paramount or Star Trek brands, obviously.

## Support

For issues, questions, or just to show off your own build, open an issue on this repository! I'd love to see how this can be improved.

---

**"Starfleet out."** 🖖 https://sthi.space  (<-- my Star Trek podcast website)
