#!/usr/bin/env python3
"""
jfedor2 Proxy Control API
Provides HTTP REST API for dynamic backend switching
Runs on localhost:8002 or Unix socket for security
"""

from flask import Flask, request, jsonify
import threading
import time
import json
import os

app = Flask(__name__)

# Shared state with q3proxy.py (will be imported by proxy)
active_sessions = {}  # session_id -> {client, backend, overlap_until, attached_at, state}

# Metrics
metrics = {
    'attach_total': 0,
    'attach_failures': 0,
    'detach_total': 0,
    'rebinds_total': 0
}

@app.route('/attach', methods=['POST'])
def attach():
    """
    Attach client to new backend

    Request body:
    {
        "session_id": "portal_1234567890",
        "client_ip": "203.0.113.5",
        "client_port": 54832,
        "backend_ip": "127.0.0.1",
        "backend_port": 29201,
        "overlap_seconds": 5
    }

    Response:
    {
        "status": "success",
        "session_id": "portal_1234567890",
        "message": "Client attached to backend"
    }
    """
    try:
        data = request.json

        # Validate required fields
        required = ['session_id', 'client_ip', 'client_port', 'backend_ip', 'backend_port']
        for field in required:
            if field not in data:
                metrics['attach_failures'] += 1
                return jsonify({
                    "status": "error",
                    "error": f"Missing required field: {field}"
                }), 400

        session_id = data['session_id']
        client_addr = (data['client_ip'], int(data['client_port']))
        backend_addr = (data['backend_ip'], int(data['backend_port']))
        overlap_sec = data.get('overlap_seconds', 5)

        # Create session entry
        now = time.time()
        active_sessions[session_id] = {
            'client': client_addr,
            'backend': backend_addr,
            'overlap_until': now + overlap_sec,
            'attached_at': now,
            'state': 'attaching',
            'packets_in': 0,
            'packets_out': 0
        }

        metrics['attach_total'] += 1

        print(f"[API] ATTACH: {client_addr[0]}:{client_addr[1]} -> {backend_addr[0]}:{backend_addr[1]} " +
              f"(overlap: {overlap_sec}s, session: {session_id})")

        return jsonify({
            "status": "success",
            "session_id": session_id,
            "client": f"{client_addr[0]}:{client_addr[1]}",
            "backend": f"{backend_addr[0]}:{backend_addr[1]}",
            "overlap_seconds": overlap_sec,
            "message": f"Client attached to backend"
        })

    except Exception as e:
        metrics['attach_failures'] += 1
        print(f"[API] ERROR in attach: {e}")
        return jsonify({
            "status": "error",
            "error": str(e)
        }), 500


@app.route('/detach', methods=['POST'])
def detach():
    """
    Detach client session

    Request body:
    {
        "session_id": "portal_1234567890"
    }
    OR
    {
        "client_ip": "203.0.113.5",
        "client_port": 54832
    }
    """
    try:
        data = request.json
        session_id = data.get('session_id')

        # Find by session_id
        if session_id and session_id in active_sessions:
            client = active_sessions[session_id]['client']
            del active_sessions[session_id]
            metrics['detach_total'] += 1

            print(f"[API] DETACH: {session_id} (client: {client[0]}:{client[1]})")

            return jsonify({
                "status": "success",
                "session_id": session_id,
                "client": f"{client[0]}:{client[1]}"
            })

        # Find by client IP:port
        client_ip = data.get('client_ip')
        client_port = data.get('client_port')
        if client_ip and client_port:
            client_addr = (client_ip, int(client_port))
            for sid, sess in list(active_sessions.items()):
                if sess['client'] == client_addr:
                    del active_sessions[sid]
                    metrics['detach_total'] += 1

                    print(f"[API] DETACH: {sid} (client: {client_ip}:{client_port})")

                    return jsonify({
                        "status": "success",
                        "session_id": sid,
                        "client": f"{client_ip}:{client_port}"
                    })

        return jsonify({
            "status": "not_found",
            "error": "Session not found"
        }), 404

    except Exception as e:
        print(f"[API] ERROR in detach: {e}")
        return jsonify({
            "status": "error",
            "error": str(e)
        }), 500


@app.route('/status', methods=['GET'])
def status():
    """Get all active sessions and proxy status"""
    now = time.time()

    # Clean up expired sessions
    expired = []
    for sid, sess in active_sessions.items():
        if now - sess['attached_at'] > 3600:  # 1 hour timeout
            expired.append(sid)
    for sid in expired:
        del active_sessions[sid]

    return jsonify({
        "status": "running",
        "active_sessions": len(active_sessions),
        "metrics": metrics,
        "sessions": {
            sid: {
                "client": f"{s['client'][0]}:{s['client'][1]}",
                "backend": f"{s['backend'][0]}:{s['backend'][1]}",
                "state": s['state'],
                "uptime_seconds": round(now - s['attached_at'], 2),
                "overlap_remaining": max(0, round(s['overlap_until'] - now, 2)),
                "packets_in": s.get('packets_in', 0),
                "packets_out": s.get('packets_out', 0)
            } for sid, s in active_sessions.items()
        }
    })


@app.route('/health', methods=['GET'])
def health():
    """Simple health check"""
    return jsonify({
        "status": "healthy",
        "service": "jfproxy-control-api",
        "sessions": len(active_sessions)
    })


@app.route('/metrics', methods=['GET'])
def get_metrics():
    """Prometheus-style metrics"""
    output = []
    output.append("# HELP jfproxy_sessions_active Current active client sessions")
    output.append("# TYPE jfproxy_sessions_active gauge")
    output.append(f"jfproxy_sessions_active {len(active_sessions)}")
    output.append("")

    output.append("# HELP jfproxy_attach_total Total attach requests")
    output.append("# TYPE jfproxy_attach_total counter")
    output.append(f"jfproxy_attach_total {metrics['attach_total']}")
    output.append("")

    output.append("# HELP jfproxy_attach_failures_total Failed attach requests")
    output.append("# TYPE jfproxy_attach_failures_total counter")
    output.append(f"jfproxy_attach_failures_total {metrics['attach_failures']}")
    output.append("")

    output.append("# HELP jfproxy_detach_total Total detach requests")
    output.append("# TYPE jfproxy_detach_total counter")
    output.append(f"jfproxy_detach_total {metrics['detach_total']}")
    output.append("")

    return "\n".join(output), 200, {'Content-Type': 'text/plain; charset=utf-8'}


def run_api(host='127.0.0.1', port=8002, unix_socket=None):
    """
    Start Flask API

    Args:
        host: IP to bind to (default localhost)
        port: TCP port (default 8002)
        unix_socket: Optional Unix socket path (overrides host:port)
    """
    if unix_socket:
        # Unix socket mode (more secure)
        if os.path.exists(unix_socket):
            os.remove(unix_socket)

        print(f"[API] Starting control API on Unix socket: {unix_socket}")

        # Flask doesn't natively support Unix sockets, use gunicorn or werkzeug
        from werkzeug.serving import run_simple
        import socket as sock_module

        sock = sock_module.socket(sock_module.AF_UNIX, sock_module.SOCK_STREAM)
        sock.bind(unix_socket)
        os.chmod(unix_socket, 0o660)  # rw for owner+group

        app.run(host='unix://' + unix_socket, port=0, debug=False, use_reloader=False)
    else:
        # TCP mode
        print(f"[API] Starting control API on http://{host}:{port}")
        app.run(host=host, port=port, debug=False, use_reloader=False, threaded=True)


if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(description='jfproxy Control API')
    parser.add_argument('--host', default='127.0.0.1', help='Host to bind to (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=8002, help='Port to bind to (default: 8002)')
    parser.add_argument('--unix-socket', help='Unix socket path (overrides host:port)')

    args = parser.parse_args()

    run_api(host=args.host, port=args.port, unix_socket=args.unix_socket)
