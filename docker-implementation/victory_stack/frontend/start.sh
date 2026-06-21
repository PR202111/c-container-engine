LOG_FILE="/www/startup.log"

{
    echo "[$(date)] Starting UI Boot..."
    
    export DEBIAN_FRONTEND=noninteractive
    
    echo "[UI] Updating and Installing..."
    apt-get update -y
    apt-get install -y -q python3
    
    echo "[UI] Launching HTTP Server..."
    cd /www
    python3 -m http.server 3000 --bind 0.0.0.0
} > "$LOG_FILE" 2>&1