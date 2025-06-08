# Dockerfile
FROM python:3.9-slim

# Set working directory di dalam container
WORKDIR /app

# Install system dependencies yang dibutuhkan MySQL
RUN apt-get update && apt-get install -y \
    gcc \
    default-libmysqlclient-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Copy file requirements dan install dependencies Python
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy semua file aplikasi ke container
COPY . .

# Buat folder untuk uploads jika diperlukan
RUN mkdir -p uploads

# Port yang akan diexpose
EXPOSE 5000

# Environment variables
ENV FLASK_APP=app.py
ENV FLASK_ENV=production

# Command untuk menjalankan aplikasi
CMD ["python", "app.py"]