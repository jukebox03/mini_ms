import socket
import json
import time
import sys
import threading

# Configuration
CONCURRENT_REQUESTS = 100
TARGET_HOST = 'localhost'
TARGET_PORT = 5060  # DPU Bridge Port

NAMES = ["alice", "bob", "charlie", "diana", "eve", "frank", "grace", "henry", "unknown"]

results = {"success": 0, "fail": 0, "latencies": []}
lock = threading.Lock()

def send_request_dpumesh(client_id, name):
    """
    Sends a single DPUmesh-style JSON request over TCP.
    """
    # DPUmesh Bridge Request Format
    req_obj = {
        'method': 'GET',
        'path': '/lookup',
        'target_service': 'frontend',
        'query_string': f'name={name}',
        'body': ''
    }
    request = json.dumps(req_obj)

    start_time = time.time()
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((TARGET_HOST, TARGET_PORT))
        sock.sendall(request.encode())

        # Read full response (DPUmesh bridge returns JSON)
        buffer = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buffer += chunk

        sock.close()
        latency = (time.time() - start_time) * 1000  # ms

        if buffer:
            resp_data = json.loads(buffer.decode())
            status_code = resp_data.get('status', 500)
            body_str = resp_data.get('body', '{}')
            
            # Response body is also a JSON string
            resp = json.loads(body_str)

            with lock:
                results["latencies"].append(latency)
                if status_code == 200:
                    results["success"] += 1
                else:
                    results["fail"] += 1

            if status_code == 200:
                print(f"[Client {client_id:02d}] OK name={name} -> id={resp.get('id')} attended={resp.get('attended')} | {latency:.1f}ms")
            else:
                print(f"[Client {client_id:02d}] {status_code} name={name} -> {body_str.strip()} | {latency:.1f}ms")
        else:
            with lock:
                results["fail"] += 1
            print(f"[Client {client_id:02d}] FAIL empty response")

    except Exception as e:
        with lock:
            results["fail"] += 1
        print(f"[Client {client_id:02d}] ERROR {e}")

def run_load_test():
    print(f"=== mini_ms DPUmesh Load Test: {CONCURRENT_REQUESTS} concurrent requests ===")
    print(f"Target (DPU Bridge): {TARGET_HOST}:{TARGET_PORT}")
    print()

    threads = []
    start_total = time.time()

    for i in range(CONCURRENT_REQUESTS):
        name = NAMES[i % len(NAMES)]
        t = threading.Thread(target=send_request_dpumesh, args=(i + 1, name))
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
