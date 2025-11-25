#!/bin/bash
# cleanup_drone.sh - Kill all drone-related processes

echo "=== DRONE CLEANUP ==="
echo ""

echo "1. Killing drone_core..."
sudo pkill -9 drone_core
sudo killall -9 drone_core 2>/dev/null

echo "2. Killing rpicam-vid..."
sudo pkill -9 rpicam-vid
sudo killall -9 rpicam-vid 2>/dev/null

echo "3. Killing test programs..."
sudo pkill -9 udp_test_receiver
sudo killall -9 udp_test_receiver 2>/dev/null

echo "4. Freeing port 8080..."
PORT_PID=$(sudo lsof -ti:8080 2>/dev/null)
if [ ! -z "$PORT_PID" ]; then
    sudo kill -9 $PORT_PID
    echo "   Killed process $PORT_PID"
else
    echo "   Port 8080 is free"
fi

echo "5. Freeing port 8888..."
PORT_PID=$(sudo lsof -ti:8888 2>/dev/null)
if [ ! -z "$PORT_PID" ]; then
    sudo kill -9 $PORT_PID
    echo "   Killed process $PORT_PID"
else
    echo "   Port 8888 is free"
fi

sleep 1

echo ""
echo "✓ Cleanup complete!"
echo ""
echo "Checking remaining processes..."
ps aux | grep -E "drone_core|rpicam|udp_test" | grep -v grep

if [ $? -ne 0 ]; then
    echo "✓ All clear - no drone processes running"
fi