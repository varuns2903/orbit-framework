import time
import subprocess
import urllib.request
import urllib.error
import sys
import os

print("==========================================")
print("       Orbit Full Coverage Runner         ")
print("==========================================")

def run_server(exe_path, port, test_func):
    print(f"--- Starting {exe_path} on port {port} ---")
    proc = subprocess.Popen([exe_path, "--port", str(port)])
    time.sleep(2) # wait for startup
    
    if proc.poll() is not None:
        print(f"[ERROR] {exe_path} failed to start.")
        return False
        
    try:
        test_func(port)
    except Exception as e:
        print(f"[ERROR] while testing {exe_path}: {e}")
        
    proc.terminate()
    proc.wait()
    print(f"--- Stopped {exe_path} ---\n")
    return True

def request(method, url, data=None, headers={}):
    req = urllib.request.Request(url, method=method, headers=headers)
    if data:
        req.data = data.encode('utf-8')
    try:
        with urllib.request.urlopen(req) as response:
            return response.status, response.read().decode('utf-8'), dict(response.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8'), dict(e.headers)
    except Exception as e:
        print(f"Failed to connect to {url}: {e}")
        return 0, "", {}

def test_fastapi_server(port):
    request("GET", f"http://127.0.0.1:{port}/api/users/123")
    request("POST", f"http://127.0.0.1:{port}/api/echo", data='{"test":true}', headers={"Content-Type":"application/json"})

def test_basic_server(port):
    request("GET", f"http://127.0.0.1:{port}/api/data")
    request("GET", f"http://127.0.0.1:{port}/error")
    request("GET", f"http://127.0.0.1:{port}/users/999")
    for i in range(110):
        request("GET", f"http://127.0.0.1:{port}/")

def test_rest_api(port):
    request("GET", f"http://127.0.0.1:{port}/api/v1/users")

def test_graphql_server(port):
    query = '{"query": "{ user(id: \\"1\\") { id name email } }"}'
    request("POST", f"http://127.0.0.1:{port}/graphql", data=query, headers={"Content-Type":"application/json"})

def test_orm_server(port):
    request("GET", f"http://127.0.0.1:{port}/users")
    
# We will run these locally
run_server("./build/fastapi_server", 8080, test_fastapi_server)
run_server("./build/basic_server", 8080, test_basic_server)
run_server("./build/rest_api", 8080, test_rest_api)
run_server("./build/graphql_server", 8080, test_graphql_server)
run_server("./build/orm_server", 8080, test_orm_server)

print("--- Running GoogleTests ---")
subprocess.run(["./build/http_server_tests"])

print("Tests completed. Run gcovr!")
