import sys
import cv2

# Usage: python read_camera.py <esp32_ip>
cap = cv2.VideoCapture(f"http://{sys.argv[1]}:81/stream")
while True:
    ok, frame = cap.read()
    if not ok:
        break
    cv2.imshow("OV7670", frame)
    if cv2.waitKey(1) == 27:
        break
