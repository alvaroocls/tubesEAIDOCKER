#include <Adafruit_Fingerprint.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsServer.h>

// Hardware Serial for fingerprint sensor
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// WiFi credentials - Update these with your network details
const char* ssid = "hotspot";
const char* password = "aaaaaaaa";

// Flask server IP - Update with your Flask server IP
const char* flaskServerIP = "192.168.89.114";
const int flaskServerPort = 5000;

// Web server on port 80
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// Global variables
bool scanningMode = false;
String currentSubject = "";
unsigned long lastScanTime = 0;
const unsigned long scanCooldown = 10000;
bool isScanning = false;
bool isEnrolling = false;

// TAMBAHAN BARU: Message deduplication
String lastMessage = "";
unsigned long lastMessageTime = 0;
const unsigned long messageCooldown = 500; // 500ms cooldown between same messages

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== SISTEM ABSENSI FINGERPRINT ESP32 ===");
    Serial.println("Initializing...");
    
    // Initialize fingerprint sensor
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    
    if (finger.verifyPassword()) {
        Serial.println("✅ Sensor fingerprint TERDETEKSI!");
        finger.getParameters();
        Serial.print("Kapasitas sensor: "); Serial.println(finger.capacity);
        Serial.print("Jumlah template tersimpan: "); Serial.println(finger.templateCount);
    } else {
        Serial.println("❌ Sensor TIDAK terdeteksi!");
        Serial.println("Periksa koneksi sensor dan restart ESP32");
        while(1);
    }
    
    // Initialize WiFi
    initWiFi();
    
    // Setup web server routes
    setupWebServer();
    
    // Initialize WebSocket
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    
    Serial.println("\n=== SISTEM SIAP DIGUNAKAN ===");
    Serial.println("IP Address: " + WiFi.localIP().toString());
    Serial.println("Web Server: http://" + WiFi.localIP().toString());
    Serial.println("WebSocket: ws://" + WiFi.localIP().toString() + ":81");
    Serial.println("===============================\n");
}

void loop() {
    server.handleClient();
    webSocket.loop();
    delay(100);
}

// WebSocket Event Handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] WebSocket Disconnected!\n", num);
            break;
        case WStype_CONNECTED:
            Serial.printf("[%u] WebSocket Connected from %s\n", num, webSocket.remoteIP(num).toString().c_str());
            break;
        case WStype_TEXT:
            Serial.printf("[%u] WebSocket Received: %s\n", num, payload);
            break;
        default:
            break;
    }
}

// FUNGSI DIPERBAIKI: Send message with deduplication
void broadcastMessage(String type, String message, String status = "info") {
    // Check for duplicate message within cooldown period
    if (message == lastMessage && (millis() - lastMessageTime) < messageCooldown) {
        return; // Skip duplicate message
    }
    
    // Update last message tracking
    lastMessage = message;
    lastMessageTime = millis();
    
    // Create JSON message
    DynamicJsonDocument doc(500);
    doc["type"] = type;
    doc["message"] = message;
    doc["status"] = status;
    doc["timestamp"] = millis();
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Send to WebSocket clients
    webSocket.broadcastTXT(jsonString);
    
    // Also show in Serial Monitor (but only once)
    Serial.println("📡 [" + type + "] " + message);
}

void initWiFi() {
    Serial.print("Menghubungkan ke WiFi: ");
    Serial.println(ssid);
    
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi terhubung!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n❌ WiFi gagal terhubung!");
        Serial.println("Periksa SSID dan password WiFi");
        while(1);
    }
}

void setupWebServer() {
    // CORS headers for all responses
    server.onNotFound([]() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
        server.send(404, "text/plain", "Not Found");
    });
    
    // Handle preflight OPTIONS requests
    server.on("/", HTTP_OPTIONS, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
        server.send(200);
    });
    
    // Status endpoint
    server.on("/status", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        
        DynamicJsonDocument response(300);
        response["status"] = "online";
        response["sensor"] = finger.verifyPassword() ? "connected" : "disconnected";
        response["templates"] = finger.templateCount;
        response["capacity"] = finger.capacity;
        response["scanning"] = isScanning;
        response["enrolling"] = isEnrolling;
        response["subject"] = currentSubject;
        response["websocket_port"] = 81;
        
        String jsonString;
        serializeJson(response, jsonString);
        server.send(200, "application/json", jsonString);
    });
    
    // Manual attendance (single scan)
    server.on("/scan-once", HTTP_POST, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        
        if (server.hasArg("subject")) {
            String subject = server.arg("subject");
            
            broadcastMessage("scan", "🔍 Memulai scan manual untuk: " + subject, "info");
            
            int result = performSingleScan();
            
            if (result > 0) {
                DynamicJsonDocument response(300);
                response["status"] = "success";
                response["fingerprint_id"] = result;
                response["subject"] = subject;
                response["message"] = "Attendance recorded successfully";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(200, "application/json", jsonString);
                
            } else if (result == -2) {
                DynamicJsonDocument response(200);
                response["status"] = "error";
                response["message"] = "Scanner busy or cooldown active";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(429, "application/json", jsonString);
                
            } else if (result == 0) {
                DynamicJsonDocument response(200);
                response["status"] = "error";
                response["message"] = "No finger detected";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(400, "application/json", jsonString);
                
            } else {
                DynamicJsonDocument response(200);
                response["status"] = "error";
                response["message"] = "Fingerprint not found";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(404, "application/json", jsonString);
            }
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Subject required\"}");
        }
    });
    
    // Enroll new fingerprint
    server.on("/enroll", HTTP_POST, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        
        if (server.hasArg("name")) {
            String name = server.arg("name");
            
            broadcastMessage("enroll", "📝 Memulai enrollment untuk: " + name, "info");
            
            int newId = enrollNewFingerprint();
            if (newId > 0) {
                DynamicJsonDocument response(300);
                response["status"] = "success";
                response["fingerprint_id"] = newId;
                response["name"] = name;
                response["message"] = "Fingerprint enrolled successfully";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(200, "application/json", jsonString);
            } else {
                DynamicJsonDocument response(200);
                response["status"] = "error";
                response["message"] = "Enrollment failed";
                
                String jsonString;
                serializeJson(response, jsonString);
                server.send(400, "application/json", jsonString);
            }
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Name required\"}");
        }
    });
    
    // Delete all fingerprints
    server.on("/clear-all", HTTP_POST, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        
        broadcastMessage("clear", "🗑️ Menghapus semua fingerprint...", "warning");
        finger.emptyDatabase();
        broadcastMessage("clear", "✅ Semua fingerprint berhasil dihapus!", "success");
        
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"All fingerprints cleared\"}");
    });
    
    server.begin();
    Serial.println("🌐 Web server started");
}

int performSingleScan() {
    if (isScanning) {
        broadcastMessage("scan", "⚠️ Scan sedang berlangsung, harap tunggu...", "warning");
        return -2;
    }
    
    if (millis() - lastScanTime < scanCooldown) {
        unsigned long remaining = (scanCooldown - (millis() - lastScanTime)) / 1000;
        broadcastMessage("scan", "⏳ Cooldown aktif, tunggu " + String(remaining) + " detik lagi", "warning");
        return -2;
    }
    
    isScanning = true;
    broadcastMessage("scan", "👆 Letakkan jari pada sensor...", "info");
    
    int timeout = 0;
    int lastProgressUpdate = 0;
    
    while (finger.getImage() != FINGERPRINT_OK && timeout < 200) {
        delay(50);
        timeout++;
        
        // Send progress update every 2 seconds (every 40 iterations)
        if (timeout % 40 == 0 && timeout != lastProgressUpdate) {
            int remaining = (200 - timeout) / 4;
            broadcastMessage("scan", "⏰ Menunggu jari... " + String(remaining) + " detik tersisa", "info");
            lastProgressUpdate = timeout;
        }
    }
    
    if (timeout >= 200) {
        broadcastMessage("scan", "⏰ Timeout - tidak ada jari terdeteksi", "error");
        isScanning = false;
        return 0;
    }
    
    broadcastMessage("scan", "📷 Memproses gambar fingerprint...", "info");
    
    if (finger.image2Tz() != FINGERPRINT_OK) {
        broadcastMessage("scan", "❌ Gagal memproses gambar", "error");
        isScanning = false;
        return -1;
    }
    
    broadcastMessage("scan", "🔍 Mencari kecocokan fingerprint...", "info");
    
    if (finger.fingerFastSearch() == FINGERPRINT_OK) {
        broadcastMessage("scan", "✅ Fingerprint ditemukan! ID: " + String(finger.fingerID) + " (Confidence: " + String(finger.confidence) + ")", "success");
        lastScanTime = millis();
        isScanning = false;
        return finger.fingerID;
    } else {
        broadcastMessage("scan", "❌ Fingerprint tidak terdaftar", "error");
        isScanning = false;
        return -1;
    }
}

int enrollNewFingerprint() {
    isEnrolling = true;
    
    int id = getNextAvailableID();
    if (id == -1) {
        broadcastMessage("enroll", "❌ Memori penuh!", "error");
        isEnrolling = false;
        return -1;
    }
    
    broadcastMessage("enroll", "📋 Menggunakan ID: " + String(id), "info");
    broadcastMessage("enroll", "👆 Letakkan jari pada sensor untuk enrollment...", "info");
    
    // Take first image
    int p = -1;
    int timeout = 0;
    int lastProgressUpdate = 0;
    
    while (p != FINGERPRINT_OK && timeout < 200) {
        p = finger.getImage();
        delay(50);
        timeout++;
        
        // Progress update every 2 seconds
        if (timeout % 40 == 0 && timeout != lastProgressUpdate) {
            int remaining = (200 - timeout) / 4;
            broadcastMessage("enroll", "⏰ Menunggu jari pertama... " + String(remaining) + " detik tersisa", "info");
            lastProgressUpdate = timeout;
        }
    }
    
    if (timeout >= 200) {
        broadcastMessage("enroll", "⏰ Timeout pada gambar pertama", "error");
        isEnrolling = false;
        return -1;
    }
    
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
        broadcastMessage("enroll", "❌ Gagal memproses gambar pertama", "error");
        isEnrolling = false;
        return -1;
    }
    
    broadcastMessage("enroll", "📷 Gambar pertama berhasil", "success");
    broadcastMessage("enroll", "🔄 Angkat jari, lalu letakkan lagi...", "info");
    
    delay(2000);
    
    // Wait for finger removal
    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        delay(50);
    }
    
    broadcastMessage("enroll", "✋ Jari diangkat, siap untuk scan kedua", "info");
    broadcastMessage("enroll", "👆 Letakkan jari yang SAMA pada sensor lagi...", "info");
    
    // Take second image
    p = -1;
    timeout = 0;
    lastProgressUpdate = 0;
    
    while (p != FINGERPRINT_OK && timeout < 200) {
        p = finger.getImage();
        delay(50);
        timeout++;
        
        // Progress update every 2 seconds
        if (timeout % 40 == 0 && timeout != lastProgressUpdate) {
            int remaining = (200 - timeout) / 4;
            broadcastMessage("enroll", "⏰ Menunggu jari kedua... " + String(remaining) + " detik tersisa", "info");
            lastProgressUpdate = timeout;
        }
    }
    
    if (timeout >= 200) {
        broadcastMessage("enroll", "⏰ Timeout pada gambar kedua", "error");
        isEnrolling = false;
        return -1;
    }
    
    if (finger.image2Tz(2) != FINGERPRINT_OK) {
        broadcastMessage("enroll", "❌ Gagal memproses gambar kedua", "error");
        isEnrolling = false;
        return -1;
    }
    
    broadcastMessage("enroll", "📷 Gambar kedua berhasil", "success");
    broadcastMessage("enroll", "🔄 Membuat template fingerprint...", "info");
    
    if (finger.createModel() != FINGERPRINT_OK) {
        broadcastMessage("enroll", "❌ Gagal membuat template", "error");
        isEnrolling = false;
        return -1;
    }
    
    broadcastMessage("enroll", "💾 Menyimpan fingerprint ke memori...", "info");
    
    if (finger.storeModel(id) == FINGERPRINT_OK) {
        broadcastMessage("enroll", "✅ Fingerprint berhasil disimpan dengan ID: " + String(id), "success");
        isEnrolling = false;
        return id;
    } else {
        broadcastMessage("enroll", "❌ Gagal menyimpan fingerprint", "error");
        isEnrolling = false;
        return -1;
    }
}

int getNextAvailableID() {
    for (int id = 1; id <= 127; id++) {
        int p = finger.loadModel(id);
        if (p == FINGERPRINT_PACKETRECIEVEERR) {
            continue;
        } else if (p != FINGERPRINT_OK) {
            return id;
        }
    }
    return -1;
}

// void sendAttendanceToServer(int fingerprintID, String subject) {
//     if (WiFi.status() != WL_CONNECTED) {
//         broadcastMessage("server", "❌ WiFi tidak terhubung", "error");
//         return;
//     }
    
//     HTTPClient http;
//     String url = "http://" + String(flaskServerIP) + ":" + String(flaskServerPort) + "/absen";
    
//     http.begin(url);
//     http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
//     String postData = "fingerprint_id=" + String(fingerprintID) + "&subject=" + subject;
    
//     broadcastMessage("server", "📤 Mengirim absensi ke server...", "info");
    
//     int httpResponseCode = http.POST(postData);
    
//     if (httpResponseCode > 0) {
//         String response = http.getString();
//         broadcastMessage("server", "✅ Server Response: " + response, "success");
//     } else {
//         broadcastMessage("server", "❌ Error Code: " + String(httpResponseCode), "error");
//     }
    
//     http.end();
// }

// void sendRegistrationToServer(int fingerprintID, String name) {
//     if (WiFi.status() != WL_CONNECTED) {
//         broadcastMessage("server", "❌ WiFi tidak terhubung", "error");
//         return;
//     }
    
//     HTTPClient http;
//     String url = "http://" + String(flaskServerIP) + ":" + String(flaskServerPort) + "/register";
    
//     http.begin(url);
//     http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
//     String postData = "name=" + name + "&fingerprint_id=" + String(fingerprintID);
    
//     broadcastMessage("server", "📤 Mengirim registrasi ke server...", "info");
    
//     int httpResponseCode = http.POST(postData);
    
//     if (httpResponseCode > 0) {
//         String response = http.getString();
//         broadcastMessage("server", "✅ Server Response: " + response, "success");
//     } else {
//         broadcastMessage("server", "❌ Error Code: " + String(httpResponseCode), "error");
//     }
    
//     http.end();
// }
