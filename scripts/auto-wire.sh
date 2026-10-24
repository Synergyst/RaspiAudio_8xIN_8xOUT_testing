#!/bin/sh
# Wait for cm5audio and audio nodes to fully initialize
sleep 5

# --- 1. RaspiAudio DAC8xADC8x (Built-in Audio Pro) Cross-Mapping ---
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX0 cm5audio:input_AUX0 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX1 cm5audio:input_AUX1 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX2 cm5audio:input_FL 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX3 cm5audio:input_FLC 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX4 cm5audio:input_FC 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX5 cm5audio:input_FR 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX6 cm5audio:input_FRC 2>/dev/null || true
pw-link alsa_input.platform-soc_107c000000_sound.pro-input-0:capture_AUX7 cm5audio:input_RC 2>/dev/null || true

pw-link cm5audio:output_FL  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX0 2>/dev/null || true
pw-link cm5audio:output_FLC alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX1 2>/dev/null || true
pw-link cm5audio:output_FC  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX2 2>/dev/null || true
pw-link cm5audio:output_FR  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX3 2>/dev/null || true
pw-link cm5audio:output_FRC alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX4 2>/dev/null || true
pw-link cm5audio:output_RC  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX5 2>/dev/null || true
pw-link cm5audio:output_AUX0  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX6 2>/dev/null || true
pw-link cm5audio:output_AUX1  alsa_output.platform-soc_107c000000_sound.pro-output-0:playback_AUX7 2>/dev/null || true

# --- 2. AT2020USB+ Microphone Connections (cm5audio-1) ---
pw-link alsa_input.usb-audio-technica_AT2020USB_-00.analog-stereo:capture_FL cm5audio-1:input_FL 2>/dev/null || true
pw-link alsa_input.usb-audio-technica_AT2020USB_-00.analog-stereo:capture_FR cm5audio-1:input_FR 2>/dev/null || true

# --- 3. UAC2 Gadget 1 Connections (Capture -> cm5audio-3, Playback <- cm5audio-2) ---
pw-link alsa_input.platform-1000480000.usb.stereo-fallback:capture_FL cm5audio-3:input_FL 2>/dev/null || true
pw-link alsa_input.platform-1000480000.usb.stereo-fallback:capture_FR cm5audio-3:input_FR 2>/dev/null || true
pw-link cm5audio-2:output_FL alsa_output.platform-1000480000.usb.stereo-fallback:playback_FL 2>/dev/null || true
pw-link cm5audio-2:output_FR alsa_output.platform-1000480000.usb.stereo-fallback:playback_FR 2>/dev/null || true

# --- 4. UAC2 Gadget 2 Connections (Capture -> cm5audio-2, Playback <- cm5audio-1) ---
pw-link alsa_input.platform-1000480000.usb.2.stereo-fallback:capture_FL cm5audio-2:input_FL 2>/dev/null || true
pw-link alsa_input.platform-1000480000.usb.2.stereo-fallback:capture_FR cm5audio-2:input_FR 2>/dev/null || true
pw-link cm5audio-1:output_FL alsa_output.platform-1000480000.usb.2.stereo-fallback:playback_FL 2>/dev/null || true
pw-link cm5audio-1:output_FR alsa_output.platform-1000480000.usb.2.stereo-fallback:playback_FR 2>/dev/null || true
