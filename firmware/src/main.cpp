/*
 * ============================================================================
 *   AEROSENSE — ESP32-S3 OV2640 MJPEG Streaming Server (AP Mode)
 * ============================================================================
 *
 *   Drone payload firmware. The ESP32-S3 boots as a standalone Wi-Fi Access
 *   Point broadcasting "AEROSENSE-PAYLOAD". The GCS laptop connects to this
 *   network and accesses the MJPEG stream at:
 *
 *       http://esp32-cam.local:81/stream
 *
 *   mDNS binds the hostname "esp32-cam" so the frontend dashboard's default
 *   URL resolves without the operator needing to know the raw AP gateway IP.
 *
 *   Hardware:  ESP32-S3 DevKitC-1 + OV2640 (flex ribbon cable)
 *   Framework: Arduino (ESP-IDF under the hood)
 *   Build:     PlatformIO
 *
 *   Author:    Aerosense GCS Team
 *   Date:      2026-06-29
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "camera_pins.h"        // Pin definitions from include/camera_pins.h

// ============================================================================
//  ACCESS POINT CONFIGURATION
// ============================================================================
// The ESP32 creates its own Wi-Fi network. The GCS operator connects their
// laptop/tablet to this SSID, then opens the dashboard which points at
// http://esp32-cam.local:81/stream by default.

static const char* AP_SSID     = "AEROSENSE-PAYLOAD";
static const char* AP_PASSWORD = "aerosense123";      // Min 8 chars for WPA2
static const int   AP_CHANNEL  = 1;                   // Wi-Fi channel (1-13)
static const int   AP_MAX_CONN = 2;                   // Max simultaneous clients

// The AP gateway IP. Clients connecting to this AP are assigned addresses
// in the 192.168.4.x subnet by the built-in DHCP server.
// Default gateway: 192.168.4.1

// ============================================================================
//  mDNS CONFIGURATION
// ============================================================================
// Hostname MUST match the URL in the GCS frontend dashboard input field.
// Frontend default: http://esp32-cam.local:81/stream
static const char* MDNS_HOSTNAME = "esp32-cam";

// ============================================================================
//  STREAM SERVER CONFIGURATION
// ============================================================================
// Port 81 avoids collision with any port-80 config server and matches
// the URL the GCS dashboard is hardcoded to connect to.
static constexpr uint16_t STREAM_PORT = 81;

// MJPEG multipart boundary string — this exact value is expected by
// standard <img> tag MJPEG consumers and OpenCV VideoCapture.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// HTTP server handle
static httpd_handle_t g_stream_httpd = nullptr;

// ============================================================================
//  MJPEG STREAM HANDLER  —  GET /stream
// ============================================================================
// Continuously captures JPEG frames from the OV2640 and pushes them over
// HTTP as a multipart response. The loop exits cleanly when the client
// disconnects or any send operation fails.

static esp_err_t stream_handler(httpd_req_t* req) {
    camera_fb_t* fb  = nullptr;
    esp_err_t    res = ESP_OK;
    char part_buf[128];

    Serial.println("[STREAM] Client connected — starting MJPEG stream");

    // Set multipart MJPEG content type
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        Serial.println("[STREAM] Failed to set content type");
        return res;
    }

    // CORS header so the GCS dashboard (opened as a local file or from a
    // different origin) can display the stream without being blocked.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "24");

    // ---- Main streaming loop ----
    while (true) {
        // Acquire a single JPEG frame from the OV2640
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[STREAM] WARNING: fb_get() returned NULL — retrying");
            delay(50);  // Brief cooldown before retry
            continue;
        }

        // 1. Send multipart boundary separator
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));

        // 2. Send per-part header with JPEG content length
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HEADER, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }

        // 3. Send raw JPEG pixel data
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        }

        // CRITICAL: Always return the frame buffer to the driver's PSRAM pool.
        // Skipping this call leaks ~100-400 KB per frame and crashes within seconds.
        esp_camera_fb_return(fb);
        fb = nullptr;

        // If any send failed, the client likely disconnected — exit gracefully
        if (res != ESP_OK) {
            Serial.println("[STREAM] Client disconnected — ending stream");
            break;
        }
    }

    return res;
}

// ============================================================================
//  CAMERA INITIALISATION
// ============================================================================
// Configures the OV2640 sensor using pin macros from camera_pins.h.
// Activates PSRAM double-buffering when available for smooth streaming.

static bool initCamera() {
    camera_config_t config = {};

    // LEDC peripheral provides the XCLK clock signal to the camera
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // Parallel data bus (D0–D7) from camera_pins.h
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;

    // Clock and sync signals
    config.pin_xclk  = CAM_PIN_XCLK;
    config.pin_pclk  = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href  = CAM_PIN_HREF;

    // SCCB (I2C-like) register control bus
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;

    // Power and reset (not wired on most ESP32-S3 CAM boards)
    config.pin_pwdn  = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;

    // Clock frequency and pixel format
    config.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_JPEG;

    // Conservative baseline (works even without PSRAM)
    config.frame_size   = FRAMESIZE_SVGA;     // 800×600
    config.jpeg_quality = 12;                 // Lower number = higher quality
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    // Upgrade to double-buffered mode if PSRAM is available
    if (psramFound()) {
        Serial.println("[CAMERA] PSRAM detected — enabling double-buffer mode");
        config.jpeg_quality = 10;                     // Crisper frames for AI analysis
        config.fb_count     = 2;                      // Double buffer: fill + serve
        config.grab_mode    = CAMERA_GRAB_LATEST;     // Always serve the freshest frame
    } else {
        Serial.println("[CAMERA] WARNING: No PSRAM — single buffer in DRAM");
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // Initialise the camera driver
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAMERA] FATAL: esp_camera_init() failed with error 0x%x\n", err);
        Serial.println("[CAMERA] Check ribbon cable seating and pin definitions.");
        return false;
    }

    // Fine-tune sensor image parameters after initialisation
    sensor_t* s = esp_camera_sensor_get();
    if (s != nullptr) {
        s->set_brightness(s, 0);                        // Range: -2 to +2
        s->set_contrast(s, 0);                          // Range: -2 to +2
        s->set_saturation(s, 0);                        // Range: -2 to +2
        s->set_whitebal(s, 1);                          // 1 = enable auto white balance
        s->set_awb_gain(s, 1);                          // 1 = enable AWB gain
        s->set_wb_mode(s, 0);                           // 0 = auto
        s->set_aec2(s, 1);                              // Enable AEC DSP
        s->set_ae_level(s, 0);                          // AE level: -2 to +2
        s->set_gainceiling(s, static_cast<gainceiling_t>(6));  // Max analogue gain
        Serial.println("[CAMERA] Sensor tuning applied");
    }

    Serial.printf("[CAMERA] Initialised — Resolution: SVGA (800x600) | Quality: %d | Buffers: %d\n",
                  psramFound() ? 10 : 12,
                  psramFound() ? 2  : 1);

    return true;
}

// ============================================================================
//  HTTP STREAM SERVER STARTUP
// ============================================================================
// Registers the /stream endpoint on the configured port. The GCS frontend
// connects to: http://esp32-cam.local:81/stream

static void startStreamServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = STREAM_PORT;
    config.ctrl_port   = STREAM_PORT + 1;   // Internal control port

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = nullptr
    };

    Serial.printf("[SERVER] Starting MJPEG stream server on port %u...\n", STREAM_PORT);

    if (httpd_start(&g_stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(g_stream_httpd, &stream_uri);
        Serial.println("[SERVER] Stream server started successfully");
    } else {
        Serial.println("[SERVER] FATAL: Failed to start HTTP server");
    }
}

// ============================================================================
//  ACCESS POINT STARTUP
// ============================================================================
// Configures the ESP32-S3 as a standalone Wi-Fi hotspot. The GCS operator
// connects their laptop to "AEROSENSE-PAYLOAD", then the dashboard at
// http://esp32-cam.local:81/stream resolves via mDNS.

static void startAccessPoint() {
    Serial.printf("[AP] Broadcasting SSID: \"%s\" on channel %d\n", AP_SSID, AP_CHANNEL);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
    WiFi.setSleep(false);   // Disable power-saving for consistent streaming

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[AP] Access Point active — Gateway IP: %s\n", apIP.toString().c_str());
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(1000);    // Allow serial monitor to attach

    Serial.println();
    Serial.println("============================================");
    Serial.println("  AEROSENSE — ESP32-S3 Camera Payload Node");
    Serial.println("  Mode: Wi-Fi Access Point (AP)");
    Serial.println("============================================");

    // ---- Step 1: Initialise Camera ----
    if (!initCamera()) {
        Serial.println("[SETUP] Camera initialisation failed. Halting.");
        while (true) { delay(1000); }   // Halt — no point continuing without a camera
    }

    // ---- Step 2: Start Wi-Fi Access Point ----
    startAccessPoint();

    // ---- Step 3: Start mDNS Responder ----
    // Binds "esp32-cam.local" so the GCS dashboard resolves the stream URL
    // without needing the raw 192.168.4.1 gateway address.
    if (MDNS.begin(MDNS_HOSTNAME)) {
        Serial.printf("[MDNS] Responder started — hostname: %s.local\n", MDNS_HOSTNAME);
        MDNS.addService("http", "tcp", STREAM_PORT);
    } else {
        Serial.println("[MDNS] WARNING: mDNS responder failed to start");
        Serial.println("[MDNS] Stream will still work via raw IP: 192.168.4.1");
    }

    // ---- Step 4: Start MJPEG Stream Server ----
    startStreamServer();

    // ---- Boot Summary ----
    Serial.println();
    Serial.println("--------------------------------------------");
    Serial.println("  BOOT COMPLETE — Stream endpoints:");
    Serial.printf("  mDNS : http://%s.local:%u/stream\n", MDNS_HOSTNAME, STREAM_PORT);
    Serial.printf("  IP   : http://%s:%u/stream\n", WiFi.softAPIP().toString().c_str(), STREAM_PORT);
    Serial.printf("  SSID : %s\n", AP_SSID);
    Serial.println("--------------------------------------------");
    Serial.println();
}

// ============================================================================
//  MAIN LOOP
// ============================================================================
// The MJPEG stream is served asynchronously by the HTTP server task on a
// separate FreeRTOS core. The main loop monitors connected client count
// for diagnostic purposes.

void loop() {
    // Periodic health check — log connected client count
    int clients = WiFi.softAPgetStationNum();
    Serial.printf("[AP] Connected clients: %d\n", clients);

    delay(10000);   // Health check every 10 seconds
}