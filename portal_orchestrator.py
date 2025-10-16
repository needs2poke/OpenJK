#!/usr/bin/env python3
"""
Portal Orchestrator Daemon
Monitors hub server logs for portal touch events
Spawns Docker containers and calls proxy attach API
"""

import subprocess
import requests
import time
import re
import sys
import json
import os
from datetime import datetime

# Configuration
SHARD_API = "http://localhost:8001"
PROXY_API = "http://localhost:8002"
HUB_LOG = "/tmp/hub.log"

# RCON configuration (for client IP:port discovery fallback)
RCON_PASSWORD = None  # Will read from env or config
RCON_HOST = "127.0.0.1"
RCON_PORT = 29078

# Test client mapping (for development/testing)
TEST_CLIENT_PORTS = {
    # Add your test client IP here:
    # "203.0.113.5": 12345,
}


def log(message):
    """Timestamped logging"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] [ORCH] {message}", flush=True)


def spawn_docker_backend(account_id, map_name="mp/ffa3"):
    """
    Spawn Docker container via shard-manager API

    Returns:
        dict: {"instance_id": int, "port": int, "container_id": str, "status": str}
        None: on failure
    """
    try:
        log(f"Requesting Docker spawn for account {account_id}, map {map_name}")

        resp = requests.post(f"{SHARD_API}/api/spawn_instance", json={
            "instance_type": "mission",
            "owner_account_id": account_id,
            "map_name": map_name,
            "max_players": 8
        }, timeout=15)

        if resp.status_code == 200:
            data = resp.json()
            log(f"Docker spawned: instance_id={data['instance_id']}, port={data['port']}, container={data['container_id'][:12]}")
            return data
        else:
            log(f"ERROR: Shard API returned {resp.status_code}: {resp.text}")
            return None

    except requests.exceptions.ConnectionError:
        log("ERROR: Cannot connect to Shard Manager API - is it running?")
        return None
    except Exception as e:
        log(f"ERROR spawning Docker: {e}")
        return None


def wait_for_port_ready(port, timeout=30):
    """
    Wait for UDP port to be listening (check via ss command locally)

    Returns:
        bool: True if port is ready, False on timeout
    """
    log(f"Waiting for backend port {port} to be ready...")
    start = time.time()

    while time.time() - start < timeout:
        try:
            # Check if port is listening locally (no SSH needed)
            result = subprocess.run(
                ['ss', '-lunp'],
                capture_output=True,
                timeout=2,
                text=True
            )

            if f':{port}' in result.stdout:
                log(f"Backend port {port} is READY")
                return True

        except subprocess.TimeoutExpired:
            log(f"Warning: timeout checking port {port}")
        except Exception as e:
            log(f"Warning: Error checking port {port}: {e}")

        time.sleep(0.5)

    log(f"TIMEOUT waiting for port {port} after {timeout}s")
    return False


def get_client_port_from_rcon(client_ip, player_name=None):
    """
    Fallback: Query server via RCON to get client port

    Args:
        client_ip: Client IP address
        player_name: Optional player name to match

    Returns:
        int: Client port number or None
    """
    if not RCON_PASSWORD:
        return None

    try:
        # TODO: Implement RCON query
        # For now, return None to skip this fallback
        log(f"RCON fallback not yet implemented for {client_ip}")
        return None

    except Exception as e:
        log(f"RCON query failed: {e}")
        return None


def get_client_port(client_ip, player_name=None):
    """
    3-tier fallback for discovering client port:
    1. Portal log provides IP:port
    2. RCON status query
    3. Test mapping (development)

    Returns:
        int: Client port or None
    """
    # Tier 3: Test mapping (development)
    if client_ip in TEST_CLIENT_PORTS:
        port = TEST_CLIENT_PORTS[client_ip]
        log(f"Using TEST mapping for {client_ip} → port {port}")
        return port

    # Tier 2: RCON fallback (if available)
    if RCON_PASSWORD:
        port = get_client_port_from_rcon(client_ip, player_name)
        if port:
            log(f"Got port {port} from RCON for {client_ip}")
            return port

    # Tier 1: Should be provided in portal log (future enhancement)
    log(f"WARNING: No client port available for {client_ip}")
    return None


def attach_client_to_backend(client_ip, client_port, backend_port, session_id):
    """
    Call proxy attach API to redirect client to backend

    Returns:
        bool: True on success, False on failure
    """
    try:
        log(f"Calling proxy attach: {client_ip}:{client_port} → backend:{backend_port}")

        resp = requests.post(f"{PROXY_API}/attach", json={
            "session_id": session_id,
            "client_ip": client_ip,
            "client_port": client_port,
            "backend_ip": "127.0.0.1",
            "backend_port": backend_port,
            "overlap_seconds": 5
        }, timeout=5)

        if resp.status_code == 200:
            data = resp.json()
            log(f"Proxy attach SUCCESS: session {session_id}")
            return True
        else:
            log(f"Proxy attach FAILED: {resp.status_code} - {resp.text}")
            return False

    except requests.exceptions.ConnectionError:
        log("ERROR: Cannot connect to Proxy Control API - is it running?")
        return False
    except Exception as e:
        log(f"ERROR calling proxy API: {e}")
        return False


def handle_portal_touch(client_ip, client_port, account_id, instance_id, backend_port):
    """
    Handle a portal touch event - orchestrate the entire flow

    Args:
        client_ip: Client IP address
        client_port: Client port (0 if unknown)
        account_id: Player's account ID
        instance_id: Portal instance ID
        backend_port: Expected backend port (from portal entity)
    """
    log("=" * 60)
    log("PORTAL TOUCH DETECTED")
    log(f"Client: {client_ip}:{client_port if client_port else '???'}")
    log(f"Account: {account_id}, Instance: {instance_id}, Port: {backend_port}")

    # Step 1: Discover client port if not provided
    if not client_port or client_port == 0:
        log("Client port not in log, attempting fallback discovery...")
        client_port = get_client_port(client_ip)

        if not client_port:
            log("ABORT: Cannot determine client port - cannot attach")
            log("=" * 60)
            return

    # Step 2: Check if backend already exists (portal pre-spawned)
    if wait_for_port_ready(backend_port, timeout=2):
        log(f"Backend already running on port {backend_port} (pre-spawned)")
    else:
        # Step 3: Spawn new Docker container
        log(f"Backend not found - spawning new Docker container...")
        instance = spawn_docker_backend(account_id)

        if not instance:
            log("ABORT: Failed to spawn Docker container")
            log("=" * 60)
            return

        # Update backend port from actual spawned instance
        backend_port = instance['port']

        # Step 4: Wait for backend to be ready
        if not wait_for_port_ready(backend_port, timeout=30):
            log(f"ABORT: Backend failed to start on port {backend_port}")
            # TODO: Kill the failed container
            log("=" * 60)
            return

    # Step 5: Attach client to backend via proxy
    session_id = f"portal_{int(time.time())}_{client_port}"

    if attach_client_to_backend(client_ip, client_port, backend_port, session_id):
        log("SUCCESS: Player should now be seamlessly transferred to shard!")
        log(f"Session ID: {session_id}")
    else:
        log("FAILED: Proxy attach error - player will see manual connect message")

    log("=" * 60)


def tail_server_log():
    """
    Tail hub server log and detect portal touch events

    Log format expected:
    ^5[PORTAL] Attach request: client=IP:PORT accountID=X instanceID=Y port=Z

    Alternative formats supported:
    ^5[PORTAL] client=IP:PORT accountID=X instanceID=Y port=Z
    [PORTAL_TOUCH] client=IP:PORT ...
    """
    log(f"Starting log monitor: {HUB_LOG}")
    log("Waiting for portal touch events...")
    log("Press Ctrl+C to stop")

    # Start tailing log (runs locally on server, no SSH needed)
    try:
        proc = subprocess.Popen(
            ['tail', '-F', HUB_LOG],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            bufsize=1
        )

        for line in proc.stdout:
            # Strip color codes
            line = re.sub(r'\^\d', '', line)

            # Match portal touch patterns
            # Pattern 1: [PORTAL] Attach request: client=IP:PORT accountID=X instanceID=Y port=Z
            match = re.search(
                r'\[PORTAL\].*client=([^:]+):(\d+).*accountID=(\d+).*instanceID=(\d+).*port=(\d+)',
                line
            )

            if not match:
                # Pattern 2: [PORTAL] client=IP:PORT (without "Attach request")
                match = re.search(
                    r'\[PORTAL\].*client=([^:]+):(\d+).*accountID=(\d+).*port=(\d+)',
                    line
                )

            if match:
                client_ip = match.group(1)
                client_port = int(match.group(2)) if match.group(2) != '0' else 0
                account_id = int(match.group(3))

                # instance_id might not be in pattern 2
                if len(match.groups()) >= 5:
                    instance_id = int(match.group(4))
                    backend_port = int(match.group(5))
                else:
                    instance_id = 0
                    backend_port = int(match.group(4))

                # Handle the portal touch
                handle_portal_touch(client_ip, client_port, account_id, instance_id, backend_port)

    except KeyboardInterrupt:
        log("Shutting down (Ctrl+C)")
        proc.terminate()
        sys.exit(0)
    except Exception as e:
        log(f"FATAL ERROR: {e}")
        sys.exit(1)


def check_services():
    """Check that required services are running"""
    log("Checking required services...")

    # Check Proxy API
    try:
        resp = requests.get(f"{PROXY_API}/health", timeout=2)
        if resp.status_code == 200:
            log("✓ Proxy Control API is running")
        else:
            log("✗ Proxy Control API returned error")
            return False
    except:
        log("✗ Proxy Control API is NOT running (check localhost:8002)")
        return False

    # Check Shard Manager API
    try:
        resp = requests.get(f"{SHARD_API}/", timeout=2)
        log("✓ Shard Manager API is running")
    except:
        log("⚠ Shard Manager API is NOT running (Docker spawn will fail)")
        log("  Start it with: cd /home/ubuntu/shard-manager && docker-compose up -d")

    return True


def main():
    """Main entry point"""
    log("=" * 60)
    log("Portal Orchestrator Starting")
    log("=" * 60)

    # Check services
    if not check_services():
        log("FATAL: Required services not available")
        sys.exit(1)

    log("")

    # Start monitoring
    tail_server_log()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        log("Shutting down...")
        sys.exit(0)
