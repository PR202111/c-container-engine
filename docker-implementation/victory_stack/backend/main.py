from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from datetime import datetime
import sqlite3
import platform

app = FastAPI()

# Allow the frontend (port 8080) to talk to the backend (port 8000)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

DB_PATH = "/db/data.db"

class LogEntry(BaseModel):
    content: str
    level: str = "INFO"

def get_db_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

@app.on_event("startup")
def startup():
    conn = get_db_connection()
    # Upgraded table to include timestamps and log levels
    conn.execute('''CREATE TABLE IF NOT EXISTS logs 
                    (id INTEGER PRIMARY KEY AUTOINCREMENT, 
                     timestamp TEXT, 
                     level TEXT, 
                     content TEXT)''')
    conn.commit()
    conn.close()
    print("[FastAPI] Enhanced Database initialized securely on mounted volume!")

@app.get("/api/status")
def get_status():
    # Showcasing system details inside the container namespace
    return {
        "engine": "MyDocker Orchestrator v1.0", 
        "status": "Online and Operational",
        "networking": "veth Bridge NAT Routing",
        "storage": "OverlayFS + Bind Mounts",
        "container_hostname": platform.node(),  # This will prove your sethostname() works!
        "python_version": platform.python_version()
    }

@app.post("/api/logs")
def add_log(log: LogEntry):
    conn = get_db_connection()
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    conn.execute('INSERT INTO logs (timestamp, level, content) VALUES (?, ?, ?)', 
                 (timestamp, log.level, log.content))
    conn.commit()
    conn.close()
    return {"status": "success"}

@app.get("/api/logs")
def get_logs():
    conn = get_db_connection()
    # Fetch the latest 50 logs
    logs = conn.execute('SELECT * FROM logs ORDER BY id DESC LIMIT 50').fetchall()
    conn.close()
    return [dict(row) for row in logs]