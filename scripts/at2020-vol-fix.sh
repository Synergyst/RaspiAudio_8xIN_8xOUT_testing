#!/bin/sh
sleep 3

# Lock AT2020USB+ capture volume to 0.80 (69% alsamixer)
SOURCE_ID=$(wpctl status | grep -A 25 "Sources:" | grep -i "AT2020USB" | head -n 1 | awk -F'.' '{print $1}' | tr -cd '0-9')
if [ -n "$SOURCE_ID" ]; then
    wpctl set-volume "$SOURCE_ID" 0.80
fi
