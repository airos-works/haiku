#!/usr/bin/env python3
"""Check the agent UI tools of mcp_server (ui_*) against AgentUITestApp.

Starts src/tests/kits/interface/accessibility/AgentUITestApp on the Haiku
machine over SSH, reads its window with ui_tree, acts on every kind of
control with ui_act and checks, in the program's output, that each action
arrived and was marked as an agent's (be:agent). Quits the test program at
the end. MCP must be on with "Let agents see and use applications".

  python3 tools/mcp/test-ui.py http://HOST:7780/mcp --token TOKEN \\
      --ssh 'ssh user@host' --app /boot/home/tests/AgentUITestApp

--library-path runs the test program with another libbe.so (an uninstalled
build), e.g. --library-path /boot/home/agent-ui/lib.
"""
import argparse
import json
import re
import shlex
import subprocess
import sys
import time
import urllib.request

p = argparse.ArgumentParser(description=__doc__,
    formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('url')
p.add_argument('--token', required=True)
p.add_argument('--ssh', required=True, help='SSH command, including user@host')
p.add_argument('--app', required=True, help='AgentUITestApp on the machine')
p.add_argument('--library-path', help='directory with the libbe.so to test')
a = p.parse_args()
ssh = shlex.split(a.ssh)
log = '/tmp/agent_ui_test.log'
seq = 0
failures = 0


def shell(command, timeout=30):
    return subprocess.check_output(ssh + [command], text=True, timeout=timeout)


def call(name, arguments):
    global seq
    seq += 1
    body = json.dumps({'jsonrpc': '2.0', 'id': seq, 'method': 'tools/call',
        'params': {'name': name, 'arguments': arguments}}).encode()
    request = urllib.request.Request(a.url, body, {
        'Content-Type': 'application/json',
        'Accept': 'application/json, text/event-stream',
        'Authorization': 'Bearer ' + a.token})
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.loads(response.read())['result']
    text = ''.join(c.get('text', '') for c in result['content'])
    return text, result.get('isError', False)


def check(condition, what, detail=''):
    global failures
    if condition:
        print('PASS', what)
    else:
        failures += 1
        print('FAIL', what)
        if detail:
            print('   ', detail.replace('\n', '\n    '))


def app_output():
    return shell('cat ' + log)


def got(line):
    """Waits a little for the test program to print line."""
    for _ in range(20):
        output = app_output()
        if line in output:
            return True
        time.sleep(0.1)
    return False


# start the test program
environment = ''
if a.library_path:
    environment = ('env LIBRARY_PATH=' + shlex.quote(a.library_path)
        + ':%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib'
        + ':/boot/system/non-packaged/lib:/boot/system/lib ')
shell('(nohup %s%s > %s 2>&1 < /dev/null &)'
    % (environment, shlex.quote(a.app), log))

text, error = call('ui_wait', {'window': 'Agent test', 'timeout_s': 15})
check(not error, 'ui_wait finds the window', text)
team = re.search(r'\[(\d+):\d+\]', text).group(1)
app = {'app': team}

text, error = call('ui_windows', {'app': 'AgentUITestApp'})
check(not error and 'window "Agent test" [' + team in text,
    'ui_windows lists the window with its ref', text)

time.sleep(0.5)
tree, error = call('ui_tree', app)
check(not error, 'ui_tree reads the window', tree)
for expected, what in [
        ('menu item "File"', 'menu bar item'),
        ('menu item "New"', 'closed menu item'),
        ('Alt+N', 'menu shortcut'),
        ('pop-up button "Size"', 'menu field'),
        ('= "Medium"', 'menu field value'),
        ('menu item "Large"', 'menu field choices'),
        ('text field "Name"', 'text field'),
        ('= "Ada"', 'text field value'),
        ('text field "Password"', 'password field'),
        ('(password)', 'password state'),
        ('check box "Notify me"', 'check box'),
        ('slider "Volume"', 'slider'),
        ('range 0..100', 'slider range'),
        ('list "Colors"', 'list named by the application'),
        ('list item "Green"', 'list item that draws its own text'),
        ('tab "Advanced"', 'tab'),
        ('class StatusCanvas', 'custom view class'),
        ('| Temperature 21 C', 'custom view drawn text'),
        ('NEEDS CONFIRMATION', 'confirmation mark')]:
    check(expected in tree, 'tree: ' + what, tree if expected not in tree
        else '')
check('Export' in tree and 'PDF' not in tree,
    'tree: deeper closed menus are summarized')
text, error = call('ui_find', {'text': 'pdf', 'app': team})
check(not error and 'menu item "PDF…"' in text,
    'ui_find finds an item of a closed submenu', text)

steps = [
    ({'name': 'Name', 'action': 'set_value', 'value': 'Grace'},
        'got name text=Grace agent=1', 'set_value on a text field'),
    ({'name': 'Password', 'action': 'set_value', 'value': 'secret'},
        'got pass length=6 agent=1', 'set_value on a password field'),
    ({'name': 'Notify me', 'action': 'set_value', 'value': True},
        'got chck value=1 agent=1', 'set_value on a check box'),
    ({'name': 'Volume', 'action': 'set_value', 'value': 75},
        'got sldr value=75 agent=1', 'set_value on a slider'),
    ({'name': 'Size', 'action': 'set_value', 'value': 'large'},
        'got size size=Large agent=1', 'set_value on a menu field'),
    ({'name': 'Colors', 'action': 'set_value', 'value': 'Blue'},
        'got list index=2 agent=1', 'set_value on a list'),
    ({'name': 'PNG', 'action': 'press'},
        'got xpng agent=1', 'press an item of a closed submenu'),
    ({'name': 'StatusCanvas', 'action': 'click'},
        'got cnvc agent=1', 'click a custom view named by its class'),
    ({'name': 'Temperature', 'action': 'key', 'key': 'x'},
        "got key 'x' agent=1", 'key to a custom view named by its text'),
    ({'name': 'Name', 'action': 'key', 'key': 'n', 'modifiers': 'command'},
        'got new_', 'menu shortcut through a key'),
]
for step, line, what in steps:
    step = dict(step, **app)
    text, error = call('ui_act', {'steps': [step]})
    check(not error and got(line), what, text + '\n' + app_output())

text, error = call('ui_act', {'steps': [dict({'name': 'Name',
    'action': 'type', 'text': ' Hopper'}, **app)]})
check(not error and '= "Grace Hopper"' in text,
    'type inserts at the end of a text field', text)

text, error = call('ui_act', {'steps': [dict({'name': 'Delete everything',
    'action': 'press'}, **app)]})
check(error and 'NEEDS CONFIRMATION' in text and 'got dele' not in
    app_output(), 'a marked button refuses without confirmed', text)
text, error = call('ui_act', {'steps': [dict({'name': 'Delete everything',
    'action': 'press', 'confirmed': True}, **app)]})
check(not error and got('got dele agent=1'),
    'a marked button presses with confirmed', text)

text, error = call('ui_act', {'steps': [dict({'name': 'Nothing like it',
    'action': 'press'}, **app)]})
check(error and 'nothing named' in text, 'an unknown name is refused', text)
text, error = call('ui_act', {'steps': [dict({'role': 'menu item',
    'action': 'press'}, **app)]})
check(error and 'ambiguous' in text, 'an ambiguous step lists candidates',
    text)

text, error = call('ui_text', dict({'name': 'Password'}, **app))
check(error and 'secret' not in text, 'password text is never read', text)

text, error = call('ui_focus', app)
check(not error and 'focus:' in text, 'ui_focus names the focused control',
    text)
text, error = call('ui_events', {'app': 'AgentUITestApp'})
check(not error and 'focus' in text, 'ui_events reports focus changes', text)

text, error = call('ui_tree', dict({'format': 'json'}, **app))
check(not error and '"role": "slider"' in text, 'ui_tree as JSON', text[:300])

# quit the test program through its window
call('ui_act', {'steps': [dict({'role': 'window', 'name': 'Agent test',
    'action': 'activate'}, **app)]})
call('ui_act', {'steps': [dict({'name': 'Name', 'action': 'key', 'key': 'q',
    'modifiers': 'command'}, **app)]})
text, error = call('ui_wait', {'window': 'Agent test', 'gone': True,
    'timeout_s': 10})
check(not error, 'the window closed after Command+Q', text)

print('%s: %d failed' % ('PASS' if failures == 0 else 'FAIL', failures))
sys.exit(1 if failures else 0)
