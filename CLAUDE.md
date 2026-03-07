# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Star Trek-style wearable comm badge built on the **M5StickC PLUS2** (ESP32-based). Tap the device to record a voice command, which is transcribed via a Whisper-compatible API (Groq/OpenAI/custom) and sent to **Home Assistant**'s conversation API.

## Architecture

This is a **single-file Arduino sketch**: `HA_Comm_Badge_V1/HA_Comm_Badge_V1.ino`. There is no build system beyond the Arduino IDE. The sketch contains all logic in one file, organized into labeled sections:

- **Hardware pin definitions & MPU6886 register constants** - accelerometer setup for tap detection and wake-on-motion
- **DeviceConfig struct & Preferences** - persistent configuration stored in ESP32 NVS flash
- **Audio/VAD constants** - sample rate (16kHz), voice activity detection parameters
- **Config portal** - a WebServer that serves an LCARS-themed HTML config page at 192.168.4.1 when in AP mode
- **Core flow**: `setup()` -> WiFi connect -> `loop()` polls for taps -> `handleVoiceCommand()` records audio with VAD -> sends WAV to Whisper API -> sends transcription to Home Assistant conversation API -> audio feedback beeps

## Build & Flash

1. Open `HA_Comm_Badge_V1/HA_Comm_Badge_V1.ino` in **Arduino IDE**
2. Select board: **M5StickC PLUS2**
3. Required libraries (install via Arduino Library Manager): M5Unified, WiFiClientSecure, HTTPClient, WebServer, ArduinoJson, Preferences, Wire
4. Upload to device

There are no tests, linters, or CI pipelines in this project.

## Key Technical Details

- **Target hardware**: M5StickC PLUS2 only (not original M5StickC - different microphone)
- **ESP32 platform**: 2.4GHz WiFi only, uses `RTC_DATA_ATTR` for boot count persistence across deep sleep
- **Config migration**: `migrateConfig()` handles upgrading from older config formats (e.g., `groqApiKey` -> unified `llmApiKey`)
- **HTTP keep-alive**: Uses persistent `WiFiClientSecure` connections for lower latency on repeated API calls
- **Audio format**: Raw 16-bit PCM at 16kHz, packaged as WAV for the Whisper API
- **Config mode entry**: Hold front button (GPIO 37) for 5 seconds while awake, or during reset
