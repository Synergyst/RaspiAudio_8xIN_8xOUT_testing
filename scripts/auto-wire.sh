#!/bin/sh
# Wait for cm5audio and audio nodes to fully initialize
sleep 15

# --- UAC2 Gadget 1 Connections ---
pw-link alsa_input.platform-1000480000.usb.stereo-fallback:capture_FL cm5audio:input_FL 2>/dev/null || true
pw-link alsa_input.platform-1000480000.usb.stereo-fallback:capture_FR cm5audio:input_FR 2>/dev/null || true
pw-link cm5audio:output_FL alsa_output.platform-1000480000.usb.stereo-fallback:playback_FL 2>/dev/null || true
pw-link cm5audio:output_FR alsa_output.platform-1000480000.usb.stereo-fallback:playback_FR 2>/dev/null || true

# --- UAC2 Gadget 2 Connections ---
pw-link alsa_input.platform-1000480000.usb.2.stereo-fallback:capture_FL cm5audio:input_FL 2>/dev/null || true
pw-link alsa_input.platform-1000480000.usb.2.stereo-fallback:capture_FR cm5audio:input_FR 2>/dev/null || true
pw-link cm5audio:output_FL alsa_output.platform-1000480000.usb.2.stereo-fallback:playback_FL 2>/dev/null || true
pw-link cm5audio:output_FR alsa_output.platform-1000480000.usb.2.stereo-fallback:playback_FR 2>/dev/null || true

# --- AT2020USB+ Microphone Connections ---
pw-link alsa_input.usb-audio-technica_AT2020USB_-00.analog-stereo:capture_FL cm5audio:input_FL 2>/dev/null || true
pw-link alsa_input.usb-audio-technica_AT2020USB_-00.analog-stereo:capture_FR cm5audio:input_FR 2>/dev/null || true
