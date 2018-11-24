#!/bin/sh
cd /home/pi/nRF24_MQTT
sudo screen -r nRF24Server -X quit
sudo pkill nRFserver
echo "Starting nRF server ..."
sudo screen -S nRF24Server -d -m ./nRFserver
