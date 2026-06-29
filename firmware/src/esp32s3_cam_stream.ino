/*
 * ============================================================================
 *   AEROSENSE — ESP32-S3 OV2640 MJPEG Streaming Server
 * ============================================================================
 *
 *   Firmware for the drone-mounted camera module. Connects to a local Wi-Fi
 *   network, registers itself via mDNS as "esp32-cam.local", and serves a
 *   continuous MJPEG stream on Port 81 at the /stream endpoint.
 *
 *   The stream URL that the GCS dashboard expects is:
 *       http://esp32-cam.local:81/stream
 *
 *   Hardware:  ESP32-S3 DevKitC-1 + OV2640 (flex ribbon cable)
 *   Framework: Arduino (ESP-IDF under the hood)
 *
 *   Author:    Aerosense GCS Team
 *   Date:      2026-06-29
 * ============================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>           // mDNS so the device resolves as "esp32-cam.local"
#include "esp_http_server.h"

// ============================================================================
//  SECTION 1: NETWORK CONFIGURATION
// ============================================================================
// Replace these with your actual Wi-Fi credentials before flashing.
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// mDNS hostname — this MUST match the URL used in the GCS frontend dashboard.
// The frontend input field defaults to: http://esp32-cam.local:81/stream
const char* MDNS_HOSTNAME = "esp32-cam";

// Maximum number of Wi-Fi connection retry attempts before restarting
#define WIFI_MAX_RETRIES 30

// ============================================================================
//  SECTION 2: OV2640 CAMERA PIN MAPPING (ESP32-S3 WROOM CAM Module)
// ============================================================================
// These pin definitions correspond to the standard ESP32-S3 CAM dev board.
// If you are using a different board or a custom PCB, update these to match
// the physical wiring of your flex ribbon cable connector.
//
// Signal Name   | GPIO | Description
// --------------|------|----------------------------------------------
// PWDN          |  -1  | Power-down (not used on most S3 CAM boards)
// RESET         |  -1  | Hardware reset (not used, rely on software)
// XCLK          |  10  | External clock input to the OV2640 sensor
// SIOD (SDA)    |  40  | SCCB (I2C-like) data line for register config
// SIOC (SCL)    |  39  | SCCB clock line for register config
// D7 (Y9)       |  48  | Parallel data bit 7 (MSB)
// D6 (Y8)       |  11  | Parallel data bit 6
// D5 (Y7)       |  12  | Parallel data bit 5
// D4 (Y6)       |  14  | Parallel data bit 4
// D3 (Y5)       |  16  | Parallel data bit 3
// D2 (Y4)       |  18  | Parallel data bit 2
// D1 (Y3)       |  17  | Parallel data bit 1
// D0 (Y2)       |  15  | Parallel data bit 0 (LSB)
// VSYNC         |  38  | Vertical sync (marks the start of each frame)
// HREF          |  47  | Horizontal reference (marks valid pixel data)
// PCLK          |  13  | Pixel clock (drives data sampling timing)

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48   // D7
#define Y8_GPIO_NUM       11   // D6
#define Y7_GPIO_NUM       12   // D5
#define Y6_GPIO_NUM       14   // D4
#define Y5_GPIO_NUM       16   // D3
#define Y4_GPIO_NUM       18   // D2
#define Y3_GPIO_NUM       17   // D1
#define Y2_GPIO_NUM       15   // D0

#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ============================================================================
//  SECTION 3: XCLK FREQUENCY CONFIGURATION
// ============================================================================
// 20 MHz is the standard OV2640 clock speed for crisp, fast streaming.
// If you experience image artifacts or instability with longer flex cables,
// reduce this to 16 MHz or even 10 MHz for improved signal integrity.
#define XCLK_FREQ_HZ      20000000   // 20 MHz (default — best for short cables)
// #define XCLK_FREQ_HZ   16000000   // 16 MHz (safe fallback for medium cables)
// #define XCLK_FREQ_HZ   10000000   // 10 MHz (ultra-safe for long/noisy cables)

// ============================================================================
//  SECTION 4: STREAM SERVER CONFIGURATION
// ============================================================================
// The HTTP server listens on port 81 to avoid clashing with any port-80
// web configuration server. The GCS frontend connects to:
//    http://esp32-cam.local:81/stream
#define STREAM_SERVER_PORT 81

// MJPEG multipart boundary — this exact string is expected by standard
// MJPEG-consuming <img> tags and OpenCV VideoCapture clients.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Handle to the running HTTP server instance
httpd_handle_t stream_httpd = NULL;

// ============================================================================
//  SECTION 5: MJPEG STREAM HANDLER
// ============================================================================
// This is the core handler registered at GET /stream. It enters an infinite
// loop, capturing JPEG frames from the camera and pushing them as a
// multipart HTTP response. The loop exits cleanly if any send operation
// fails (e.g. the client disconnects).

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];

    Serial.println("[STREAM] Client connected — starting MJPEG stream");

    // Set the response content type to multipart MJPEG
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        Serial.println("[STREAM] Failed to set content type");
        return res;
    }

    // Allow cross-origin requests so the GCS dashboard (served from a
    // different origin or opened as a local file) can display the stream.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "24");

    // ---- Main streaming loop ----
    while (true) {
        // Capture a single JPEG frame from the OV2640
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[STREAM] ERROR: Camera capture returned NULL — skipping frame");
            // Don't immediately break; give the sensor a moment and try again
            delay(50);
            continue;
        }

        // Send the multipart boundary separator
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        }

        // Send the per-part JPEG header with content length
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HEADER, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }

        // Send the actual JPEG pixel data
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        // CRITICAL: Always return the frame buffer to the driver pool.
        // Failure to do this will exhaust PSRAM within seconds and crash the MCU.
        esp_camera_fb_return(fb);
        fb = NULL;

        // If any chunk send failed, the client likely disconnected — exit cleanly
        if (res != ESP_OK) {
            Serial.println("[STREAM] Client disconnected or send error — ending stream");
            break;
        }
    }

    return res;
}

// ============================================================================
//  SECTION 6: CAMERA INITIALIZATION
// ============================================================================
// Configures the OV2640 sensor with the pin mapping, clock speed, resolution,
// and JPEG compression quality defined above. Leverages PSRAM when available
// for double-buffered frame capture (smoother streaming).

bool initCamera() {
    camera_config_t config;

    // LEDC peripheral drives the XCLK signal to the camera sensor
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // Parallel data bus pins (D0–D7)
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    // Clock and sync pins
    config.pin_xclk  = XCLK_GPIO_NUM;
    config.pin_pclk  = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href  = HREF_GPIO_NUM;

    // SCCB (I2C-like) control bus for configuring OV2640 registers
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    // Power and reset (unused on most ESP32-S3 CAM boards)
    config.pin_pwdn  = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    // Clock and image format
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_JPEG;

    // Default: SVGA (800×600), quality factor 12 (lower = better quality)
    // These are conservative defaults that work without PSRAM.
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    // If PSRAM is available, upgrade to double-buffered mode for smoother
    // streaming and tighter JPEG compression (quality 10).
    if (psramFound()) {
        Serial.println("[CAMERA] PSRAM detected — enabling double-buffer mode");
        config.jpeg_quality = 10;       // Crisper images for AI analysis
        config.fb_count     = 2;        // Double buffer: one filling, one serving
        config.grab_mode    = CAMERA_GRAB_LATEST;  // Always serve the freshest frame
    } else {
        Serial.println("[CAMERA] WARNING: No PSRAM — falling back to single buffer in DRAM");
        config.frame_size  = FRAMESIZE_SVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // Initialize the camera driver
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAMERA] FATAL: esp_camera_init() failed with error 0x%x\n", err);
        Serial.println("[CAMERA] Check ribbon cable seating and pin definitions.");
        return false;
    }

    // Optional: Fine-tune sensor registers after init
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);     // -2 to 2
        s->set_contrast(s, 0);       // -2 to 2
        s->set_saturation(s, 0);     // -2 to 2
        s->set_whitebal(s, 1);       // 0 = disable, 1 = enable auto white balance
        s->set_awb_gain(s, 1);       // 0 = disable, 1 = enable AWB gain
        s->set_wb_mode(s, 0);        // 0 = auto, 1 = sunny, 2 = cloudy, etc.
        s->set_aec2(s, 1);           // Enable automatic exposure control (AEC DSP)
        s->set_ae_level(s, 0);       // -2 to 2 AE level
        s->set_gainceiling(s, (gainceiling_t)6);  // Max analogue gain
        Serial.println("[CAMERA] Sensor tuning applied");
    }

    Serial.printf("[CAMERA] Initialized — Resolution: %s | JPEG Quality: %d\n",
                  psramFound() ? "SVGA (800x600)" : "SVGA (800x600, DRAM)",
                  psramFound() ? 10 : 12);

    return true;
}

// ============================================================================
//  SECTION 7: HTTP STREAM SERVER STARTUP
// ============================================================================
// Registers the /stream endpoint on Port 81. The GCS dashboard constructs
// the full URL as:  http://esp32-cam.local:81/stream

void startStreamServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = STREAM_SERVER_PORT;
    config.ctrl_port   = STREAM_SERVER_PORT + 1;  // Control port (internal)

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    Serial.printf("[SERVER] Starting MJPEG stream server on port %d...\n", STREAM_SERVER_PORT);

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        Serial.println("[SERVER] Stream server started successfully");
    } else {
        Serial.println("[SERVER] FATAL: Failed to start HTTP server");
    }
}

// ============================================================================
//  SECTION 8: SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(1000);  // Allow serial monitor to attach

    Serial.println();
    Serial.println("============================================");
    Serial.println("  AEROSENSE — ESP32-S3 Camera Stream Node");
    Serial.println("============================================");

    // ---- Step 1: Initialize Camera ----
    if (!initCamera()) {
        Serial.println("[SETUP] Camera initialization failed. Halting.");
        while (true) { delay(1000); }  // Halt — no point continuing without a camera
    }

    // ---- Step 2: Connect to Wi-Fi ----
    Serial.printf("[WIFI] Connecting to \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setSleep(false);  // Disable Wi-Fi power-saving for consistent streaming

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        retries++;
        if (retries >= WIFI_MAX_RETRIES) {
            Serial.println("\n[WIFI] FATAL: Could not connect after max retries. Restarting...");
            ESP.restart();
        }
    }
    Serial.println();
    Serial.printf("[WIFI] Connected! IP Address: %s\n", WiFi.localIP().toString().c_str());

    // ---- Step 3: Start mDNS responder ----
    // This allows the GCS dashboard to reach the camera at "esp32-cam.local"
    // instead of needing to know the raw IP address.
    if (MDNS.begin(MDNS_HOSTNAME)) {
        Serial.printf("[MDNS] Responder started — hostname: %s.local\n", MDNS_HOSTNAME);
        // Advertise the HTTP service so network scanners can find it
        MDNS.addService("http", "tcp", STREAM_SERVER_PORT);
    } else {
        Serial.println("[MDNS] WARNING: mDNS responder failed to start");
        Serial.println("[MDNS] The stream will still work via raw IP address");
    }

    // ---- Step 4: Start the MJPEG stream server ----
    startStreamServer();

    // ---- Boot Summary ----
    Serial.println();
    Serial.println("--------------------------------------------");
    Serial.println("  BOOT COMPLETE — Stream endpoints:");
    Serial.printf("  mDNS : http://%s.local:%d/stream\n", MDNS_HOSTNAME, STREAM_SERVER_PORT);
    Serial.printf("  IP   : http://%s:%d/stream\n", WiFi.localIP().toString().c_str(), STREAM_SERVER_PORT);
    Serial.println("--------------------------------------------");
    Serial.println();
}

// ============================================================================
//  SECTION 9: MAIN LOOP
// ============================================================================
// The stream is served asynchronously by the HTTP server task running on
// a separate FreeRTOS core. The main loop only needs to monitor system
// health and handle Wi-Fi reconnection if the connection drops.

void loop() {
    // Monitor Wi-Fi connectivity — auto-reconnect if dropped
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Connection lost — attempting reconnection...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < WIFI_MAX_RETRIES) {
            delay(500);
            Serial.print(".");
            retries++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n[WIFI] Reconnection failed — restarting ESP32...");
            ESP.restart();
        }
    }

    delay(10000);  // Health check every 10 seconds
}
