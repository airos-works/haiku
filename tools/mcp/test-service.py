#!/usr/bin/env python3
"""Exercise a fresh MCP service on an isolated Haiku test machine.

Requires Python 3 on the host, SSH to Haiku, and no pre-existing MCP settings.
The test changes the setting, creates a temporary file and leaves MCP off.
Pass --server /boot/home/mcp_server when testing an uninstalled binary.
"""
import argparse
import http.client
import json
import shlex
import socket
import subprocess
import urllib.parse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('url')
p.add_argument('--ssh', required=True, help='SSH command, including user@host')
p.add_argument('--server', default='/boot/system/servers/mcp_server')
a = p.parse_args()
ssh = shlex.split(a.ssh)
url = urllib.parse.urlsplit(a.url)
assert url.scheme == 'http'

def shell(command):
    return subprocess.check_output(ssh + [command], text=True, timeout=20)

def control(option):
    return shell(shlex.quote(a.server) + ' --' + option)

def connection():
    return http.client.HTTPConnection(url.hostname, url.port, timeout=10)

def request(method, value=None, headers=None):
    c = connection()
    c.request(method, url.path, json.dumps(value) if value is not None else None,
              headers or {})
    r = c.getresponse()
    status, data = r.status, r.read()
    c.close()
    return status, data

def inaccessible():
    try:
        request('GET')
    except (OSError, http.client.HTTPException):
        return True
    return False

initial = control('status')
assert 'MCP: off (saved setting: off)' in initial, initial
assert 'Bearer' not in initial
assert inaccessible(), 'default-off server accepted HTTP'
print('PASS fresh default off: no endpoint or credential')

try:
    enabled = control('enable')
    config = json.loads(enabled[enabled.index('{'):])['mcpServers']['airOS']
    headers = config['headers'] | {'Content-Type': 'application/json',
        'Accept': 'application/json, text/event-stream'}
    token = headers['Authorization'].removeprefix('Bearer ')
    assert len(token) == 64 and all(c in '0123456789abcdef' for c in token)
    assert config['url'].startswith('http://') and config['url'].endswith(':7780/mcp')
    assert '0.0.0.0' not in config['url']
    print('PASS enabled: reachable-address configuration and 256-bit bearer token')
    mode = shell('stat -c %a /boot/home/config/settings/mcp_server/settings').strip()
    assert mode == '600', mode
    print('PASS settings permissions 0600')

    assert request('POST', {'jsonrpc': '2.0', 'id': 1, 'method': 'ping'})[0] == 401
    assert request('POST', {}, {'Authorization': 'Bearer wrong'})[0] == 401
    assert request('POST', {}, headers | {'Origin': 'http://untrusted.invalid'})[0] == 403
    print('PASS missing/wrong credentials and browser origins rejected')

    seq = 0
    def rpc(method, params=None):
        global seq
        seq += 1
        body = {'jsonrpc': '2.0', 'id': seq, 'method': method}
        if params is not None:
            body['params'] = params
        status, data = request('POST', body, headers)
        assert status == 200, (status, data)
        response = json.loads(data)
        assert response.get('id') == seq and 'error' not in response, response
        return response['result']

    result = rpc('initialize', {'protocolVersion': '2025-03-26',
        'capabilities': {}, 'clientInfo': {'name': 'service-test', 'version': '1'}})
    assert result['serverInfo']['name'] == 'mcp_server', result
    assert request('POST', {'jsonrpc': '2.0', 'method': 'notifications/initialized'}, headers)[0] == 202
    assert request('GET', headers=headers)[0] == 405
    tools = {t['name'] for t in rpc('tools/list')['tools']}
    expected = {'syslog_query', 'syslog_mark', 'syslog_write', 'run_command',
        'run_start', 'run_status', 'run_stop', 'run_list', 'teams_list',
        'team_threads', 'team_kill', 'screenshot', 'screen_info', 'hey', 'app_list',
        'app_launch', 'driver_deploy', 'driver_status', 'hw_inventory',
        'package_list', 'package_info', 'package_install', 'package_uninstall',
        'file_put', 'file_get', 'file_list', 'file_hash', 'health_check'}
    assert tools == expected, tools ^ expected
    print('PASS MCP initialization, notifications and all 28 existing tools exposed')

    def tool(name, arguments):
        result = rpc('tools/call', {'name': name, 'arguments': arguments})
        assert not result.get('isError'), (name, result)
        return result.get('structuredContent') or result['content']

    assert 'mcp-service-ok' in str(tool('run_command', {'command': 'echo mcp-service-ok'}))
    assert 'mcp_server' in str(tool('teams_list', {'filter': 'mcp_server'}))
    assert 'width' in str(tool('screen_info', {}))
    assert 'apps' in str(tool('app_list', {}))
    tool('screenshot', {'max_width': 320, 'return_image': True})
    tool('syslog_query', {'tail': 2})
    tool('hw_inventory', {'sections': ['system', 'network']})
    tool('health_check', {'quick': True})
    print('PASS commands, teams, lazy GUI context, screenshots, syslog, inventory and health')

    # A keep-alive request plus a request stalled mid-header must both be closed.
    idle = connection()
    idle.request('POST', url.path, json.dumps({'jsonrpc': '2.0', 'id': 90, 'method': 'ping'}), headers)
    r = idle.getresponse()
    assert r.status == 200
    r.read()
    assert idle.sock is not None
    sock = idle.sock
    slow = socket.create_connection((url.hostname, url.port), timeout=5)
    slow.sendall(b'POST /mcp HTTP/1.1\r\nHost: test\r\n')
    disabled = control('disable')
    assert 'MCP: off (saved setting: off)' in disabled and 'Bearer' not in disabled
    assert inaccessible(), 'disabled server accepted HTTP'
    for client in [sock, slow]:
        client.settimeout(5)
        try:
            assert client.recv(1) == b''
        except ConnectionResetError:
            pass
        client.close()
    print('PASS disable closes listener, keep-alive and partial requests; details hidden')

    again = control('enable')
    assert token in again, 'credential changed unexpectedly'
    assert rpc('ping') == {}
    print('PASS re-enable retains credential and serves new connections')
finally:
    control('disable')
print('All service checks passed; MCP left off.')
