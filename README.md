# Smart Classroom Attendance System with Docker

Sistem absensi berbasis fingerprint menggunakan ESP32 dan Flask, deployed dengan Docker Compose.

## 🏗️ Architecture

- **Frontend**: Flask web application dengan HTML templates
- **Backend**: Flask API + MySQL database  
- **Hardware**: ESP32 dengan fingerprint sensor
- **Deployment**: Docker Compose multi-container

## 🚀 Quick Start

### Prerequisites
- Docker Desktop installed
- ESP32 hardware di IP `192.168.89.42`

### Deployment
```bash
# Clone repository
git clone https://github.com/alvaroocls/tubesEAIDOCKER.git
cd tubesEAIDOCKER

# Deploy dengan Docker Compose
docker-compose up --build -d