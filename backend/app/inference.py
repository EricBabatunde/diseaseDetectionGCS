import time
import random
from PIL import Image
import numpy as np

# Placeholder for AI Model
# In production, load your ONNX or PyTorch model here:
# import onnxruntime as ort
# session = ort.InferenceSession("../models/tomato_disease_model.onnx")

def preprocess_image(image: Image.Image) -> np.ndarray:
    """
    Resizes and normalizes the image for the neural network.
    """
    image = image.resize((224, 224))
    img_array = np.array(image).astype('float32')
    # Normalize assuming standard ImageNet mean and std
    img_array /= 255.0
    mean = np.array([0.485, 0.456, 0.406])
    std = np.array([0.229, 0.224, 0.225])
    img_array = (img_array - mean) / std
    # Transpose to CHW format typically used by PyTorch/ONNX
    img_array = np.transpose(img_array, (2, 0, 1))
    return np.expand_dims(img_array, axis=0)

def run_inference(image: Image.Image) -> dict:
    """
    Simulates the AI inference pipeline.
    """
    # 1. Preprocess
    tensor = preprocess_image(image)
    
    # 2. Inference (Mocked)
    # outputs = session.run(None, {"input": tensor})
    
    # Simulate processing delay
    time.sleep(0.5) 
    
    classes = [
        {"name": "Early Blight", "confidence": random.uniform(85.0, 98.0), "status": "alert"},
        {"name": "Healthy", "confidence": random.uniform(90.0, 99.0), "status": "ok"},
        {"name": "Septoria Leaf Spot", "confidence": random.uniform(70.0, 95.0), "status": "alert"}
    ]
    
    # Mock return the most likely class
    prediction = random.choice(classes)
    
    return {
        "diagnosis": prediction["name"],
        "confidence": round(prediction["confidence"], 2),
        "status": prediction["status"]
    }
