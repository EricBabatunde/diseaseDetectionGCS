"""
============================================================================
  AEROSENSE — FastAPI Backend for Tomato Disease Detection
============================================================================

  Serves a PyTorch EfficientNet-B3 model trained on tomato leaf diseases.
  Accepts image uploads from the GCS frontend dashboard and returns a
  JSON diagnosis with confidence score.

  Endpoint:   POST /api/analyze
  Model:      backend/models/tomato_model.pth
  Framework:  PyTorch + torchvision

  Run with:   uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
============================================================================
"""

import io
import logging
from pathlib import Path
from contextlib import asynccontextmanager

import torch
import torch.nn.functional as F
from torchvision import models, transforms
from PIL import Image

from fastapi import FastAPI, File, UploadFile, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

# ============================================================================
#  CONFIGURATION
# ============================================================================

# Path to the trained model checkpoint (relative to the backend/ directory)
MODEL_PATH = Path(__file__).resolve().parent.parent / "models" / "tomato_model.pth"

# EfficientNet-B3 architecture constants
EFFICIENTNET_IN_FEATURES = 1536    # Output features from EfficientNet-B3 backbone
NUM_CLASSES = 10                   # Number of tomato disease classes

# Device selection — use GPU if available, otherwise CPU
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# ============================================================================
#  IMAGE PREPROCESSING PIPELINE
# ============================================================================
# Matches the EfficientNet-B3 training pipeline:
#   1. Resize to 300×300 (EfficientNet-B3 native input resolution)
#   2. CenterCrop to 300×300 (ensures consistent aspect ratio)
#   3. Convert to tensor (scales pixel values to [0, 1])
#   4. Normalize with ImageNet mean and std

preprocess = transforms.Compose([
    transforms.Resize(300),
    transforms.CenterCrop(300),
    transforms.ToTensor(),
    transforms.Normalize(
        mean=[0.485, 0.456, 0.406],
        std=[0.229, 0.224, 0.225]
    ),
])

# ============================================================================
#  GLOBAL STATE (populated at startup)
# ============================================================================

model = None          # PyTorch model instance
class_names = None    # List of disease class name strings

# ============================================================================
#  MODEL LOADING
# ============================================================================

def load_model():
    """
    Loads the EfficientNet-B3 architecture, replaces the classifier head to
    match the training configuration (1536 → 10), and loads the saved
    state_dict from the .pth checkpoint.

    The .pth file is a dictionary containing:
      - 'model_state_dict': The trained weights
      - 'class_names':      Python list of disease label strings
    """
    global model, class_names

    logging.info(f"[MODEL] Loading checkpoint from: {MODEL_PATH}")

    if not MODEL_PATH.exists():
        raise FileNotFoundError(
            f"Model file not found at {MODEL_PATH}. "
            "Place 'tomato_model.pth' in the backend/models/ directory."
        )

    # 1. Load the checkpoint dictionary
    checkpoint = torch.load(MODEL_PATH, map_location=DEVICE, weights_only=False)

    # 2. Extract the class name list from the checkpoint
    class_names = checkpoint["class_names"]
    logging.info(f"[MODEL] Loaded {len(class_names)} classes: {class_names}")

    # 3. Build the EfficientNet-B3 architecture (no pretrained weights — we load our own)
    model = models.efficientnet_b3(weights=None)

    # 4. Replace the default 1000-class ImageNet classifier with our 10-class head.
    #    This Sequential layout MUST exactly match the structure used during training:
    #      [0] BatchNorm1d  → keys: classifier.0.weight, .bias, .running_mean, .running_var
    #      [1] Dropout      → (no learnable params)
    #      [2] Linear       → keys: classifier.2.weight, .bias
    model.classifier = torch.nn.Sequential(
        torch.nn.BatchNorm1d(num_features=EFFICIENTNET_IN_FEATURES),
        torch.nn.Dropout(p=0.3),
        torch.nn.Linear(in_features=EFFICIENTNET_IN_FEATURES, out_features=NUM_CLASSES),
    )

    # 5. Load the trained weights into the model
    model.load_state_dict(checkpoint["model_state_dict"])

    # 6. Move to device and set to evaluation mode (disables dropout, batchnorm updates)
    model.to(DEVICE)
    model.eval()

    logging.info(f"[MODEL] EfficientNet-B3 loaded successfully on {DEVICE}")


# ============================================================================
#  FASTAPI APPLICATION LIFECYCLE
# ============================================================================
# The model is loaded once at startup and persists in memory for the
# lifetime of the server process. This avoids reloading the ~43 MB
# checkpoint on every request.

@asynccontextmanager
async def lifespan(app: FastAPI):
    """Load the PyTorch model when the server starts."""
    load_model()
    logging.info("[SERVER] Aerosense backend is ready for inference")
    yield
    logging.info("[SERVER] Shutting down")


app = FastAPI(
    title="Aerosense Disease Detection API",
    description="PyTorch EfficientNet-B3 inference backend for tomato leaf disease classification",
    version="1.0.0",
    lifespan=lifespan,
)

# ============================================================================
#  CORS MIDDLEWARE
# ============================================================================
# Allow all origins so the GCS frontend dashboard (served as a local file
# or from a different host) can freely send requests to this backend.

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ============================================================================
#  LOGGING CONFIGURATION
# ============================================================================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)-7s | %(message)s",
    datefmt="%H:%M:%S",
)

# ============================================================================
#  ROUTES
# ============================================================================

@app.get("/")
def health_check():
    """Basic health check endpoint."""
    return {
        "status": "Aerosense Backend is running",
        "model": "EfficientNet-B3",
        "classes": len(class_names) if class_names else 0,
        "device": str(DEVICE),
    }


@app.post("/api/analyze")
async def analyze_image(file: UploadFile = File(...)):
    """
    Accepts an uploaded image, runs it through the EfficientNet-B3 tomato
    disease classifier, and returns the predicted diagnosis with confidence.

    Returns:
        {
            "target": "Tomato Leaf",
            "diagnosis": "<Detected Class Name>",
            "confidence": <Percentage as float>
        }
    """
    # Validate that a model is loaded
    if model is None or class_names is None:
        raise HTTPException(status_code=503, detail="Model not loaded yet")

    # Validate file type
    if file.content_type and not file.content_type.startswith("image/"):
        raise HTTPException(status_code=400, detail=f"Expected an image file, got {file.content_type}")

    try:
        # 1. Read the uploaded image bytes and open with PIL
        contents = await file.read()
        image = Image.open(io.BytesIO(contents)).convert("RGB")
        logging.info(f"[INFERENCE] Received image: {file.filename} ({image.size[0]}x{image.size[1]})")

        # 2. Apply the preprocessing pipeline and add batch dimension
        input_tensor = preprocess(image).unsqueeze(0).to(DEVICE)

        # 3. Run inference (no gradient computation needed)
        with torch.no_grad():
            logits = model(input_tensor)

        # 4. Apply softmax to convert logits → probabilities
        probabilities = F.softmax(logits, dim=1)

        # 5. Get the top prediction
        confidence, predicted_idx = torch.max(probabilities, dim=1)
        predicted_class = class_names[predicted_idx.item()]
        confidence_pct = round(confidence.item() * 100, 2)

        logging.info(f"[INFERENCE] Result: {predicted_class} ({confidence_pct}%)")

        # 6. Return the structured JSON response
        return JSONResponse(content={
            "target": "Tomato Leaf",
            "diagnosis": predicted_class,
            "confidence": confidence_pct,
        })

    except Exception as e:
        logging.error(f"[INFERENCE] Error processing image: {e}")
        raise HTTPException(status_code=500, detail=str(e))


# ============================================================================
#  STANDALONE ENTRY POINT
# ============================================================================
# Allows running directly with: python -m app.main

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app.main:app", host="0.0.0.0", port=8000, reload=True)
