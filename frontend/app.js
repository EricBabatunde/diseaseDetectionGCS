document.addEventListener('DOMContentLoaded', () => {
    // Clock updating logic
    const timeEl = document.getElementById('sys-time');
    setInterval(() => {
        timeEl.textContent = new Date().toTimeString().split(' ')[0];
    }, 1000);

    // DOM Elements Mapping
    const streamUrlInput = document.getElementById('stream-url');
    const connectBtn = document.getElementById('connect-btn');
    const videoStream = document.getElementById('video-stream');
    const captureBtn = document.getElementById('capture-btn');
    const flashOverlay = document.getElementById('flash-overlay');
    const snapshotsGrid = document.getElementById('snapshots-grid');
    
    // AI Panel Elements Mapping
    const aiIdle = document.getElementById('ai-idle');
    const aiLoading = document.getElementById('ai-loading');
    const aiResult = document.getElementById('ai-result');
    const aiStatusIndicator = document.getElementById('ai-status-indicator');
    const streamFps = document.getElementById('stream-fps');
    const streamRes = document.getElementById('stream-res');

    let isConnected = false;

    // ==========================================
    // ESP32 MJPEG STREAM CONNECTION LOGIC
    // ==========================================
    // TODO: In production, the backend FastAPI server might proxy this stream to avoid CORS, 
    // or the browser connects directly to the ESP32 IP if on the same local network.
    connectBtn.addEventListener('click', () => {
        const url = streamUrlInput.value.trim();
        if (!url) return;
        
        if (!isConnected) {
            // Connect State
            videoStream.src = url;
            
            // Update Button UI
            connectBtn.textContent = 'DISCONNECT';
            connectBtn.classList.replace('bg-gcs-border', 'bg-red-500/20');
            connectBtn.classList.replace('hover:bg-zinc-700', 'hover:bg-red-500/40');
            connectBtn.classList.add('text-red-400', 'border', 'border-red-500/50');
            isConnected = true;
            
            // Mock connection metadata updating
            setTimeout(() => {
                streamFps.textContent = '24';
                streamRes.textContent = '800 x 600';
            }, 1000);
        } else {
            // Disconnect State
            videoStream.src = 'https://placehold.co/1280x720/09090b/27272a?text=NO+SIGNAL\\nWAITING+FOR+STREAM';
            
            // Reset Button UI
            connectBtn.textContent = 'CONNECT';
            connectBtn.className = 'bg-gcs-border hover:bg-zinc-700 text-white px-4 py-2 rounded text-sm font-semibold transition-colors whitespace-nowrap';
            isConnected = false;
            
            // Reset metadata
            streamFps.textContent = '--';
            streamRes.textContent = '-- x --';
        }
    });

    // ==========================================
    // FRAME CAPTURE & AI INFERENCE LOGIC
    // ==========================================
    captureBtn.addEventListener('click', () => {
        // 1. Trigger visual UI flash effect
        flashOverlay.classList.remove('flash');
        void flashOverlay.offsetWidth; // Trigger DOM reflow to restart animation
        flashOverlay.classList.add('flash');

        // 2. Add thumbnail to grid
        // TODO: To capture an actual frame from the MJPEG stream, you will draw the <img> 
        // to a hidden <canvas> and call canvas.toDataURL('image/jpeg').
        // For this mock UI, we use the stream URL or a placeholder image.
        addSnapshotToGrid(videoStream.src);

        // 3. Trigger the AI Analysis UI flow
        // TODO: Here you will construct a fetch() request to your Python FastAPI backend
        // e.g., fetch('http://localhost:8000/api/detect', { method: 'POST', body: frameBlob })
        simulateAIInference();
    });

    function addSnapshotToGrid(imageSrc) {
        const snapshotDiv = document.createElement('div');
        snapshotDiv.className = 'aspect-video bg-black rounded border border-zinc-800 overflow-hidden relative group cursor-pointer';
        
        // Use a realistic tomato leaf mock image if we are offline/using placeholder URL
        const displaySrc = (imageSrc.includes('placehold.co') || isConnected) 
            ? 'https://images.unsplash.com/photo-1592841200221-a6898f307baa?auto=format&fit=crop&w=300&q=80' // Mock tomato plant
            : imageSrc;

        snapshotDiv.innerHTML = `
            <img src="${displaySrc}" class="w-full h-full object-cover opacity-70 group-hover:opacity-100 transition-opacity">
            <div class="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/80 to-transparent p-1">
                <div class="text-[9px] font-mono text-zinc-300">${new Date().toLocaleTimeString()}</div>
            </div>
        `;
        
        // Prepend so the newest image is top-left
        snapshotsGrid.prepend(snapshotDiv);
    }

    // Mocks the backend response delay and UI state changes
    function simulateAIInference() {
        // Set UI to loading state
        aiIdle.classList.add('hidden');
        aiResult.classList.add('hidden');
        aiLoading.classList.remove('hidden');
        aiStatusIndicator.className = 'w-2 h-2 rounded-full bg-yellow-500 animate-pulse';

        // Simulate network/inference delay (1.5 seconds)
        setTimeout(() => {
            // Remove loading state
            aiLoading.classList.add('hidden');
            aiResult.classList.remove('hidden');
            
            // Mock Inference Results 
            // TODO: Replace this with the actual JSON response from FastAPI
            const mockClasses = [
                { name: 'Early Blight Detected', color: 'text-red-400', barColor: 'bg-red-500', conf: 92 },
                { name: 'Healthy Leaf', color: 'text-emerald-400', barColor: 'bg-emerald-500', conf: 98 },
                { name: 'Septoria Leaf Spot', color: 'text-orange-400', barColor: 'bg-orange-500', conf: 85 }
            ];
            // Pick a random mock result
            const outcome = mockClasses[Math.floor(Math.random() * mockClasses.length)];
            
            // Update DOM with results
            const issueText = document.getElementById('ai-issue-text');
            const confBar = document.getElementById('ai-confidence-bar');
            const confText = document.getElementById('ai-confidence-text');

            issueText.textContent = outcome.name;
            issueText.className = `${outcome.color} font-bold text-xs lg:text-sm`;
            
            confText.textContent = `${outcome.conf}%`;
            confText.className = `font-bold ${outcome.color}`;
            
            // Reset bar width then animate
            confBar.style.width = '0%';
            confBar.className = `h-full transition-all duration-1000 ease-out ${outcome.barColor}`;
            setTimeout(() => { confBar.style.width = `${outcome.conf}%`; }, 50);

            // Update Top Right Status Indicator Bubble
            if(outcome.name === 'Healthy Leaf') {
                aiStatusIndicator.className = 'w-2 h-2 rounded-full bg-emerald-500 shadow-[0_0_8px_rgba(16,185,129,0.8)]';
            } else {
                aiStatusIndicator.className = 'w-2 h-2 rounded-full bg-red-500 shadow-[0_0_8px_rgba(239,68,68,0.8)]';
            }
            
        }, 1500);
    }
});
