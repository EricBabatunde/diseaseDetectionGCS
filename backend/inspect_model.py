import torch

# Load the file, mapping to CPU so it doesn't crash if your laptop lacks a GPU
file_path = 'models/tomato_model.pth'
try:
    checkpoint = torch.load(file_path, map_location=torch.device('cpu'))
    
    print(f"--- Model Inspection ---")
    print(f"Type of loaded object: {type(checkpoint)}\n")

    # Check if it's a checkpoint dictionary (containing weights, labels, epochs, etc.)
    if isinstance(checkpoint, dict):
        print("Keys found in the dictionary:")
        for key in checkpoint.keys():
            print(f" - {key}")
            
        # Print custom classes if they exist
        for label_key in ['classes', 'class_to_idx', 'labels']:
            if label_key in checkpoint:
                print(f"\nFound classes under '{label_key}':")
                print(checkpoint[label_key])

        # Get the state_dict (the actual weights)
        state_dict = checkpoint.get('state_dict', checkpoint.get('model_state_dict', checkpoint))
    else:
        # If it's not a dict, it's likely just the raw state_dict
        state_dict = checkpoint

    # Inspect the layers to confirm architecture and custom head
    if isinstance(state_dict, dict) or hasattr(state_dict, 'items'):
        print("\n--- Architecture Check ---")
        layers = list(state_dict.items())
        
        print("First 3 layers (Confirms EfficientNet base):")
        for name, param in layers[:3]:
            print(f" - {name} : {param.shape}")
            
        print("\nLast 3 layers (Confirms Custom Classifier & Class Count):")
        for name, param in layers[-3:]:
            print(f" - {name} : {param.shape}")
            
except Exception as e:
    print(f"Error loading the model: {e}")