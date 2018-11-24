#!/bin/sh
cd /home/pi/my2RFM24/
sudo screen -r nRF24Server -X quit
sudo pkill nRFserver
echo "Starting nRF server ..."
sudo screen -S nRF24Server -d -m ./nRFserver
