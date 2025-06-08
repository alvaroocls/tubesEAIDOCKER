from flask import Flask, render_template, request, jsonify
import mysql.connector
from mysql.connector import Error
import requests
from datetime import datetime, timedelta
import json
import os

app = Flask(__name__)

# ESP32 Configuration
ESP32_IP = os.environ.get('ESP32_IP', "192.168.89.42")

# MySQL Configuration - Untuk Docker
MYSQL_CONFIG = {
    'host': os.environ.get('MYSQL_HOST', 'localhost'),
    'database': os.environ.get('MYSQL_DATABASE', 'smart_classroom'),
    'user': os.environ.get('MYSQL_USER', 'root'),
    'password': os.environ.get('MYSQL_PASSWORD', ''),
    'port': int(os.environ.get('MYSQL_PORT', 3306))
}

def get_db_connection():
    """Get MySQL database connection dengan retry mechanism"""
    max_retries = 5
    retry_delay = 2
    
    for attempt in range(max_retries):
        try:
            connection = mysql.connector.connect(**MYSQL_CONFIG)
            return connection
        except Error as e:
            print(f"Database connection attempt {attempt + 1} failed: {e}")
            if attempt < max_retries - 1:
                print(f"Retrying in {retry_delay} seconds...")
                import time
                time.sleep(retry_delay)
            else:
                print("Max retries reached. Could not connect to database.")
                return None


# API untuk scan attendance - DIPERBAIKI
@app.route('/api/scan-once', methods=['POST'])
def scan_once():
    try:
        subject = request.form.get('subject')
        if not subject:
            return jsonify({'status': 'error', 'message': 'Subject is required'}), 400
        
        # Send to ESP32
        esp32_response = requests.post(f'http://{ESP32_IP}/scan-once', 
                                     data={'subject': subject}, 
                                     timeout=30)
        
        if esp32_response.status_code == 200:
            result = esp32_response.json()
            if result.get('status') == 'success':
                # Save to database
                connection = get_db_connection()
                if not connection:
                    return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
                    
                cursor = connection.cursor()
                
                fingerprint_id = result['fingerprint_id']
                
                # Check if user exists
                cursor.execute('SELECT id, name FROM users WHERE fingerprint_id = %s', (fingerprint_id,))
                user = cursor.fetchone()
                
                if user:
                    user_id, user_name = user
                    
                    # PERBAIKAN: Hanya simpan user_id, subject, dan timestamp
                    cursor.execute('''
                        INSERT INTO attendance (user_id, subject) 
                        VALUES (%s, %s)
                    ''', (user_id, subject))
                    
                    
                    connection.commit()
                    
                    return jsonify({
                        'status': 'success',
                        'fingerprint_id': fingerprint_id,
                        'user_id': user_id,
                        'user_name': user_name,
                        'subject': subject,
                        'message': 'Attendance recorded successfully'
                    })
                else:
                    # User tidak terdaftar - kembalikan error
                    return jsonify({
                        'status': 'error',
                        'message': f'Fingerprint ID {fingerprint_id} tidak terdaftar dalam sistem'
                    }), 404
            else:
                return jsonify(result), 400
        else:
            return jsonify({'status': 'error', 'message': 'ESP32 communication failed'}), 500
            
    except requests.exceptions.RequestException as e:
        return jsonify({'status': 'error', 'message': f'ESP32 connection error: {str(e)}'}), 500
    except Error as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500
    finally:
        if 'connection' in locals() and connection.is_connected():
            cursor.close()
            connection.close()

# API untuk mendapatkan recent attendance - DIPERBAIKI
@app.route('/api/recent-attendance')
def recent_attendance():
    try:
        connection = get_db_connection()
        if not connection:
            return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
        cursor = connection.cursor()
        
        # PERBAIKAN: Query yang benar tanpa fingerprint_id
        cursor.execute('''
            SELECT a.timestamp, a.subject, u.name, u.fingerprint_id
            FROM attendance a
            INNER JOIN users u ON a.user_id = u.id
            ORDER BY a.timestamp DESC
            LIMIT 20
        ''')
        
        attendance = []
        for row in cursor.fetchall():
            attendance.append({
                'timestamp': row[0].strftime('%H:%M:%S') if row[0] else None,
                'subject': row[1],
                'name': row[2],
                'fingerprint_id': row[3]
            })
        
        return jsonify(attendance)
        
    except Error as e:
        return jsonify({
            'status': 'error',
            'message': str(e)
        }), 500
    finally:
        if connection and connection.is_connected():
            cursor.close()
            connection.close()

# API untuk statistik hari ini - DIPERBAIKI
@app.route('/api/today-stats')
def today_stats():
    try:
        connection = get_db_connection()
        if not connection:
            return jsonify({'total_attendance': 0, 'unique_students': 0})
            
        cursor = connection.cursor()
        
        today = datetime.now().strftime('%Y-%m-%d')
        
        # Total attendance today
        cursor.execute('''
            SELECT COUNT(*) FROM attendance 
            WHERE DATE(timestamp) = %s
        ''', (today,))
        total_attendance = cursor.fetchone()[0] or 0
        
        # Unique students today
        cursor.execute('''
            SELECT COUNT(DISTINCT user_id) FROM attendance 
            WHERE DATE(timestamp) = %s
        ''', (today,))
        unique_students = cursor.fetchone()[0] or 0
        
        return jsonify({
            'total_attendance': total_attendance,
            'unique_students': unique_students
        })
        
    except Error as e:
        return jsonify({'total_attendance': 0, 'unique_students': 0})
    finally:
        if connection and connection.is_connected():
            cursor.close()
            connection.close()

# API untuk mendapatkan daftar user terdaftar
@app.route('/api/registered-users')
def get_registered_users():
    try:
        connection = get_db_connection()
        if not connection:
            return jsonify({
                'status': 'error',
                'message': 'Database connection failed'
            }), 500
            
        cursor = connection.cursor()
        
        # Get all users with their last activity
        cursor.execute('''
            SELECT u.id, u.name, u.fingerprint_id, u.created_at,
                   MAX(a.timestamp) as last_activity
            FROM users u
            LEFT JOIN attendance a ON u.id = a.user_id
            GROUP BY u.id, u.name, u.fingerprint_id, u.created_at
            ORDER BY u.fingerprint_id
        ''')
        
        users = []
        for row in cursor.fetchall():
            users.append({
                'id': row[0],
                'name': row[1],
                'fingerprint_id': row[2],
                'created_at': row[3].isoformat() if row[3] else None,
                'last_activity': row[4].isoformat() if row[4] else None
            })
        
        # Get statistics
        today = datetime.now().strftime('%Y-%m-%d')
        
        # Count active users today
        cursor.execute('''
            SELECT COUNT(DISTINCT user_id) 
            FROM attendance 
            WHERE DATE(timestamp) = %s
        ''', (today,))
        active_today = cursor.fetchone()[0] or 0
        
        # Get sensor memory info
        sensor_memory = f"{len(users)}/127"
        
        return jsonify({
            'status': 'success',
            'users': users,
            'statistics': {
                'active_today': active_today,
                'sensor_memory': sensor_memory
            }
        })
        
    except Error as e:
        return jsonify({
            'status': 'error',
            'message': str(e)
        }), 500
    finally:
        if connection and connection.is_connected():
            cursor.close()
            connection.close()

# API untuk menghapus user
@app.route('/api/delete-user', methods=['POST'])
def delete_user():
    try:
        data = request.get_json()
        user_id = data.get('user_id')
        fingerprint_id = data.get('fingerprint_id')
        
        if not user_id or not fingerprint_id:
            return jsonify({
                'status': 'error',
                'message': 'User ID dan Fingerprint ID diperlukan'
            }), 400
        
        connection = get_db_connection()
        if not connection:
            return jsonify({
                'status': 'error',
                'message': 'Database connection failed'
            }), 500
            
        cursor = connection.cursor()
        
        # Get user info before deletion
        cursor.execute('SELECT name FROM users WHERE id = %s', (user_id,))
        user = cursor.fetchone()
        
        if not user:
            return jsonify({
                'status': 'error',
                'message': 'User tidak ditemukan'
            }), 404
        
        user_name = user[0]
        
        # Delete from ESP32 sensor first
        try:
            esp32_response = requests.post(f'http://{ESP32_IP}/delete-fingerprint', 
                                         data={'fingerprint_id': fingerprint_id}, 
                                         timeout=10)
            
            if esp32_response.status_code != 200:
                return jsonify({
                    'status': 'error',
                    'message': 'Gagal menghapus dari sensor ESP32'
                }), 500
                
        except requests.exceptions.RequestException as e:
            return jsonify({
                'status': 'error',
                'message': f'Error komunikasi dengan ESP32: {str(e)}'
            }), 500
        
        # Delete attendance records (will cascade due to foreign key)
        cursor.execute('DELETE FROM attendance WHERE user_id = %s', (user_id,))
        
        # Delete user
        cursor.execute('DELETE FROM users WHERE id = %s', (user_id,))
        
        connection.commit()
        
        return jsonify({
            'status': 'success',
            'message': f'User {user_name} berhasil dihapus dari database dan sensor'
        })
        
    except Error as e:
        return jsonify({
            'status': 'error',
            'message': str(e)
        }), 500
    finally:
        if connection and connection.is_connected():
            cursor.close()
            connection.close()

# API untuk clear system (hapus semua data)
@app.route('/api/clear-sensor', methods=['POST'])
def clear_sensor():
    try:
        # Clear ESP32 sensor first
        try:
            esp32_response = requests.post(f'http://{ESP32_IP}/clear-all', timeout=10)
            
            if esp32_response.status_code != 200:
                return jsonify({
                    'status': 'error',
                    'message': 'Gagal menghapus data dari sensor ESP32'
                }), 500
                
        except requests.exceptions.RequestException as e:
            return jsonify({
                'status': 'error',
                'message': f'Error komunikasi dengan ESP32: {str(e)}'
            }), 500
        
        # Clear database
        connection = get_db_connection()
        if not connection:
            return jsonify({
                'status': 'error',
                'message': 'Database connection failed'
            }), 500
            
        cursor = connection.cursor()
        
        # Delete in correct order due to foreign key constraints
        cursor.execute('DELETE FROM attendance')
        cursor.execute('DELETE FROM users')
        
        connection.commit()
        
        return jsonify({
            'status': 'success',
            'message': 'Semua data berhasil dihapus dari sensor dan database'
        })
        
    except Error as e:
        return jsonify({
            'status': 'error',
            'message': str(e)
        }), 500
    finally:
        if connection and connection.is_connected():
            cursor.close()
            connection.close()

# Dashboard dengan data yang benar
@app.route('/dashboard')
def dashboard():
    try:
        connection = get_db_connection()
        if not connection:
            return render_template('dashboard.html', 
                                 stats={'total_today': 0, 'registered_today': 0, 'unregistered_today': 0, 'total_registered_users': 0},
                                 recent_data=[], subject_stats=[], weekly_data=[])
            
        cursor = connection.cursor()
        
        today = datetime.now().strftime('%Y-%m-%d')
        
        # Total attendance today
        cursor.execute('''
            SELECT COUNT(*) FROM attendance 
            WHERE DATE(timestamp) = %s
        ''', (today,))
        total_today = cursor.fetchone()[0] or 0
        
        # Registered users attended today (semua attendance sekarang adalah registered)
        registered_today = total_today
        
        # Unregistered fingerprints today (sekarang selalu 0 karena struktur baru)
        unregistered_today = 0
        
        # Total registered users
        cursor.execute('SELECT COUNT(*) FROM users')
        total_registered_users = cursor.fetchone()[0] or 0
        
        # Recent attendance data
        cursor.execute('''
            SELECT a.timestamp, u.name, u.fingerprint_id, a.subject
            FROM attendance a
            INNER JOIN users u ON a.user_id = u.id
            ORDER BY a.timestamp DESC
            LIMIT 10
        ''')
        recent_data = cursor.fetchall()
        
        # Subject statistics today
        cursor.execute('''
            SELECT a.subject, COUNT(*) as count
            FROM attendance a
            WHERE DATE(a.timestamp) = %s
            GROUP BY a.subject
            ORDER BY count DESC
        ''', (today,))
        subject_stats = cursor.fetchall()
        
        # Weekly data (last 7 days)
        cursor.execute('''
            SELECT DATE(timestamp) as date, COUNT(*) as count
            FROM attendance
            WHERE DATE(timestamp) >= DATE_SUB(CURDATE(), INTERVAL 6 DAY)
            GROUP BY DATE(timestamp)
            ORDER BY date
        ''')
        weekly_data = cursor.fetchall()
        
        stats = {
            'total_today': total_today,
            'registered_today': registered_today,
            'unregistered_today': unregistered_today,
            'total_registered_users': total_registered_users
        }
        
        return render_template('dashboard.html', 
                             stats=stats, 
                             recent_data=recent_data, 
                             subject_stats=subject_stats,
                             weekly_data=weekly_data)
        
    except Error as e:
        print(f"Dashboard error: {e}")
        return render_template('dashboard.html', 
                             stats={'total_today': 0, 'registered_today': 0, 'unregistered_today': 0, 'total_registered_users': 0},
                             recent_data=[], subject_stats=[], weekly_data=[])
    finally:
        if 'connection' in locals() and connection.is_connected():
            cursor.close()
            connection.close()

# Existing endpoints...
@app.route('/')
def index():
    return render_template('index.html')

@app.route('/register')
def register():
    return render_template('register.html')

@app.route('/attendance')
def attendance():
    return render_template('attendance.html')

@app.route('/api/esp32-status')
def esp32_status():
    try:
        response = requests.get(f'http://{ESP32_IP}/status', timeout=5)
        if response.status_code == 200:
            return response.json()
        else:
            return jsonify({'status': 'offline', 'error': 'ESP32 not responding'})
    except requests.exceptions.RequestException as e:
        return jsonify({'status': 'offline', 'error': str(e)})

@app.route('/api/enroll-fingerprint', methods=['POST'])
def enroll_fingerprint():
    try:
        name = request.form.get('name')
        if not name:
            return jsonify({'status': 'error', 'message': 'Name is required'}), 400
        
        # Send to ESP32
        esp32_response = requests.post(f'http://{ESP32_IP}/enroll', 
                                     data={'name': name}, 
                                     timeout=30)
        
        if esp32_response.status_code == 200:
            result = esp32_response.json()
            if result.get('status') == 'success':
                # Save to database
                connection = get_db_connection()
                if not connection:
                    return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
                    
                cursor = connection.cursor()
                
                cursor.execute('''
                    INSERT INTO users (name, fingerprint_id) 
                    VALUES (%s, %s)
                ''', (name, result['fingerprint_id']))
                
                connection.commit()
                
                return jsonify({
                    'status': 'success',
                    'name': name,
                    'fingerprint_id': result['fingerprint_id']
                })
            else:
                return jsonify(result), 400
        else:
            return jsonify({'status': 'error', 'message': 'ESP32 communication failed'}), 500
            
    except requests.exceptions.RequestException as e:
        return jsonify({'status': 'error', 'message': f'ESP32 connection error: {str(e)}'}), 500
    except Error as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500
    finally:
        if 'connection' in locals() and connection.is_connected():
            cursor.close()
            connection.close()

if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0', port=5000)