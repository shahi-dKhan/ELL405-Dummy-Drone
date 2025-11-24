#!/bin/bash

echo "===== CAMERA DEBUGGER ====="

echo
echo "STEP 1: List all video devices"
echo "-----------------------------"
ls -l /dev/video* || echo "No /dev/video devices found."


echo
echo "STEP 2: Show detailed camera capabilities"
echo "----------------------------------------"
for cam in /dev/video*; do
    echo
    echo "--- Capabilities for $cam ---"
    v4l2-ctl -d $cam --all || echo "Could not query $cam"
done


echo
echo "STEP 3: List supported formats"
echo "------------------------------"
for cam in /dev/video*; do
    echo
    echo "--- Formats for $cam ---"
    v4l2-ctl -d $cam --list-formats-ext
done


echo
echo "STEP 4: Test raw V4L2 capture (1 frame)"
echo "---------------------------------------"
for cam in /dev/video*; do
    echo
    echo "Capturing from $cam..."
    v4l2-ctl -d $cam --stream-mmap=3 --stream-count=1 --stream-to=/tmp/frame.raw || \
        echo "Failed to stream from $cam"
done


echo
echo "STEP 5: Test GStreamer pipeline"
echo "-------------------------------"
gst-launch-1.0 v4l2src device=/dev/video0 ! videoconvert ! autovideosink -v \
    2>&1 | tee gst_test.log


echo
echo "STEP 6: Test OpenCV access"
echo "--------------------------"
python3 << 'EOF'
import cv2

print("\nTesting OpenCV VideoCapture(0):")

cap = cv2.VideoCapture(0)
if not cap.isOpened():
    print("❌ OpenCV could NOT open the camera")
else:
    print("✅ OpenCV opened the camera")
    ret, frame = cap.read()
    if ret:
        print("✅ OpenCV successfully grabbed a frame:", frame.shape)
    else:
        print("❌ OpenCV failed to read a frame")

cap.release()
EOF

echo
echo "===== END OF CAMERA DEBUG ====="
