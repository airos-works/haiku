# MCP system service

`mcp_server` is part of `haiku.hpkg`, alongside the other Haiku servers.
**Preferences → MCP** turns network access on or off. A new installation is off:
there is no listener and no token until the user enables it. The setting persists
in `~/config/settings/mcp_server/settings` on writable installations. Changes on
a read-only live image last for that live session, like other Haiku preferences.

When enabled, the panel displays the machine's active IPv4 addresses, the
Streamable HTTP endpoint (`http://ADDRESS:7780/mcp`), the bearer authorization
header, and an example `mcpServers` JSON configuration. **Copy connection info**
copies all of this as text for an AI agent or client setup. Select a URL reachable
from the agent; virtual machines behind NAT need a port forward. An offline
machine shows a clearly labelled localhost URL. The display refreshes when
addresses or service state change.

Agents given the credential can run commands, access files and control the
system. The service uses HTTP, so use a trusted network or an encrypted tunnel.
Browser origins are rejected. Every MCP request requires the per-installation
256-bit token; there is no unauthenticated network mode. The settings directory
is mode 0700 and the settings file is mode 0600. Tokens are generated from
`/dev/urandom` and are never shipped in an image. Settings and entropy failures
leave the endpoint closed and appear in the panel.

Turning MCP off closes the listener and all existing connections, clears the
panel's connection details and disables the copy button. Operations already
executing can finish; disabling access does not undo commands or stop jobs that
an agent previously launched. Re-enabling retains the credential.

The service starts through `data/launch/system` after initial volumes mount,
outside safe mode, and remains idle while disabled. It is a native `BServer`
with local `BMessenger` control. It attaches to app_server only when a GUI tool
needs it, so other tools can still work when the desktop is unresponsive. The
preferences app can launch the service if it is not running.

Local administration (does not start a second server):

```sh
/boot/system/servers/mcp_server --status
/boot/system/servers/mcp_server --enable
/boot/system/servers/mcp_server --disable
```

`--status` and `--enable` print credentials only while the endpoint is on.
Do not put that output in public logs.

The 28 tools from the earlier MIT-licensed `apps/airos_mcp` implementation are
retained: syslog, commands/jobs, teams, screenshots, Haiku scripting, driver
deployment, inventory, package management, file transfer and health checks.
The new system service uses HTTP; the earlier standalone package's stdio and
one-shot command-line modes are not system-service entry points. Remove the
old `airos_mcp` package before enabling the replacement on a machine that
previously had the experimental package installed. Its independent listener
is not controlled by this preferences panel. The Pi lab image no longer adds
that package.

## Build and validation

The minimum image definition includes both `mcp_server` and `MCP`, plus the Media
Kit library used by inventory tools. Regular, test and bootstrap definitions
inherit it, as do the AirOS x86_64, ARM64 and Pi image profiles. No separately
built application package or local artifact is required.

From a configured Haiku build directory:

```sh
jam -j8 mcp_server MCP MCPJsonTest
jam -j8 haiku.hpkg
```

`MCPJsonTest` runs on Haiku. The same JSON regression tests can run on a build
host without Haiku libraries:

```sh
c++ -std=c++17 -Isrc/servers/mcp src/tests/servers/mcp/JsonTests.cpp \
    src/servers/mcp/Json.cpp -o /path/to/test-output/mcp-json-test
/path/to/test-output/mcp-json-test
```

On an isolated Haiku test installation, initially off, the host-side integration
check exercises default off, configuration, settings permissions, authentication,
initialization, all tool registrations, representative native tools, shutting
down active/partial connections and re-enabling. It leaves MCP off:

```sh
python3 tools/mcp/test-service.py http://TEST-HOST:7780/mcp --ssh 'ssh test-haiku'
```

Also verify the panel with the checkbox and copy button, and reboot after both
on and off to check persisted state. A read-only live image cannot prove
persistence on an installed system.

HTTP transport behavior follows the
[MCP Streamable HTTP specification](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports).
It uses stateless JSON responses and returns 405 for the optional SSE stream.

### Verification on 2026-10-09

- Jam compiled `mcp_server` and `MCP`, and built `haiku.hpkg`, for x86_64 and
  ARM64. Package listings contain the server, preference app, MIME registrations
  and Preferences menu link; the ARM64 minimum package also has `libmedia.so`.
- A newly built x86_64 nightly live image and ARM64 minimum disk image both booted
  with the service registered in launch_daemon and network access off.
- `test-service.py` passed on both architectures, including all 28 tool names and
  native screenshot, command, syslog, inventory and health calls.
- The native panel was inspected on both architectures. On x86_64 its checkbox
  enabled the endpoint and its copy button produced exactly the server's
  connection text and valid JSON configuration.
- On a writable x86_64 VM disk, enabled state and credential survived a reboot;
  disabled state survived another reboot. A failed settings save rolled back the
  listener and withheld connection details.
- JSON regression tests passed on the host and in Haiku x86_64.
- The x86_64, ARM64 and Pi images built from merge commit `418ca98031`
  passed the [image CI run](https://github.com/jmgasper/haiku/actions/runs/37872317743).
  The x86_64 and ARM64 gates booted QEMU; the Pi gate checked the SD layout and
  boot files, without claiming a native Pi boot.
- Each CI image's compressed and uncompressed checksums were verified against
  its manifest. Direct extraction confirmed the server, preference app, menu
  link and launch service, with no shipped MCP settings or legacy `airos_mcp`
  package. The server and preference binaries in all three images exactly match
  the corresponding locally tested x86_64 or ARM64 binaries.

Local build logs, private QEMU fixtures, screenshots and test output are retained
under `/mnt/HaikuWork/artifacts/mcp-service-20261009` and the adjacent
`mcp-build-*`, `mcp-package-*`, `mcp-image-*`, `mcp-disk-*` logs. The test fixtures
contain local access configuration and are not release images or committed files.
