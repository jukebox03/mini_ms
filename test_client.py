import socket
import json
import time
import sys
import threading

# Configuration
CONCURRENT_REQUESTS = 100
TARGET_HOST = 'localhost'
TARGET_PORT = 30080

NAMES = ["alice", "bob", "charlie", "diana", "eve", "frank", "grace", "henry", "unknown"]

results = {"success": 0, "fail": 0, "latencies": []}
lock = threading.Lock()

def send_request(client_id, name):
    """
    Sends a single HTTP GET request and measures latency.
    """
    request = (
        f"GET /lookup?name={name} HTTP/1.1\r\n"
        f"Host: {TARGET_HOST}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    )

    start_time = time.time()
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((TARGET_HOST, TARGET_PORT))
        sock.sendall(request.encode())

        # Read full response
        buffer = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buffer += chunk

        sock.close()
        latency = (time.time() - start_time) * 1000  # ms

        if buffer:
            # Parse HTTP response body (after \r\n\r\n)
            parts = buffer.split(b"\r\n\r\n", 1)
            body = parts[1].decode() if len(parts) > 1 else ""
            status_line = parts[0].decode().split("\r\n")[0]
            status_code = int(status_line.split(" ")[1])

            with lock:
                results["latencies"].append(latency)
                if status_code == 200:
                    results["success"] += 1
                else:
                    results["fail"] += 1

            resp = json.loads(body) if body else {}
            if status_code == 200:
                print(f"[Client {client_id:02d}] OK name={name} -> id={resp.get('id')} attended={resp.get('attended')} | {latency:.1f}ms")
            else:
                print(f"[Client {client_id:02d}] {status_code} name={name} -> {body.strip()} | {latency:.1f}ms")
        else:
            with lock:
                results["fail"] += 1
            print(f"[Client {client_id:02d}] FAIL empty response")

    except Exception as e:
        with lock:
            results["fail"] += 1
        print(f"[Client {client_id:02d}] ERROR {e}")

def run_load_test():
    print(f"=== mini_ms Load Test: {CONCURRENT_REQUESTS} concurrent requests ===")
    print(f"Target: {TARGET_HOST}:{TARGET_PORT}")
    print()

    threads = []
    start_total = time.time()

    for i in range(CONCURRENT_REQUESTS):
        name = NAMES[i % len(NAMES)]
        t = threading.Thread(target=send_request, args=(i + 1, name))
        threads.append(t)
        t.start()
        time.sleep(0.01)

    for t in threads:
        t.join()

    total_time = time.time() - start_total

    print()
    print("=" * 50)
    lats = sorted(results["latencies"])
    if lats:
        print(f"Success: {results['success']}  Fail: {results['fail']}")
        print(f"Total time: {total_time:.2f}s")
        print(f"Throughput: {len(lats) / total_time:.1f} req/s")
        print(f"Latency  min={lats[0]:.1f}ms  median={lats[len(lats)//2]:.1f}ms  p99={lats[int(len(lats)*0.99)]:.1f}ms  max={lats[-1]:.1f}ms")
    else:
        print("No successful requests")
    print("=" * 50)

if __name__ == "__main__":
    if len(sys.argv) > 1:
        TARGET_HOST = sys.argv[1]
    if len(sys.argv) > 2:
        TARGET_PORT = int(sys.argv[2])
    if len(sys.argv) > 3:
        CONCURRENT_REQUESTS = int(sys.argv[3])
    run_load_test()
