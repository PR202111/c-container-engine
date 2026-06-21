LOG_FILE="/app/startup.log"

{
    echo "[$(date)] Starting Backend Boot..."
    apt-get update -y
    apt-get install -y python3 python3-pip
    
    echo "[$(date)] Installing requirements..."
    pip3 install -r /app/requirements.txt --break-system-packages
    
    echo "[$(date)] Launching Uvicorn..."
    cd /app
    uvicorn main:app --host 0.0.0.0 --port 8000 --reload
} > "$LOG_FILE" 2>&1