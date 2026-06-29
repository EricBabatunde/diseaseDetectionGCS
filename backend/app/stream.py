import cv2
import requests
import numpy as np

# This is a helper script if you ever need the backend to proxy the stream
# Currently, the frontend connects directly to the ESP32 IP for MJPEG streaming.

def get_frame_from_stream(url: str):
    """
    Fetches a single frame from the MJPEG stream for server-side processing if needed.
    """
    try:
        stream = requests.get(url, stream=True, timeout=5)
        bytes_data = bytes()
        for chunk in stream.iter_content(chunk_size=1024):
            bytes_data += chunk
            a = bytes_data.find(b'\xff\xd8')
            b = bytes_data.find(b'\xff\xd9')
            if a != -1 and b != -1:
                jpg = bytes_data[a:b+2]
                bytes_data = bytes_data[b+2:]
                img = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
                return img
    except Exception as e:
        print(f"Error fetching stream: {e}")
        return None
