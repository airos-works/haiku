# Node.js for Haiku: building and publishing nvm's binaries

air/OS ships nvm (the `nvm` package, from `data/nvm`), and nvm on Haiku
installs Node.js binaries from air/OS's mirror: the GitHub releases of
[airos-works/node-haiku](https://github.com/airos-works/node-haiku). The
mirror has the newest release of every major from 10 to 26. Releases that
are not on the mirror are built from source by nvm itself, with the same
Haiku port. User documentation: `data/nvm/haiku/README.md`.

## Where things are

| Path | What |
|---|---|
| `data/nvm/nvm.sh` | nvm v0.40.8 with the Haiku changes (see below) |
| `data/nvm/haiku/port/port-haiku.py` | ports a pristine Node.js source tree (10 and later) to Haiku |
| `data/nvm/haiku/port/platform-haiku.cc` | V8's Haiku platform file, installed by the port |
| `data/nvm/haiku/port/ares/ares_config-X.Y.Z.h` | c-ares' own configure result on Haiku, per bundled c-ares version |
| `data/nvm/haiku/port/make-ares-config.sh` | Haiku: makes one of those configs from a c-ares release |
| `data/nvm/haiku/profile.d/nvm.sh` | login-shell hook |
| `build/jam/packages/NVM`, `src/data/package_infos/any/nvm` | the `nvm` package |
| `tools/nodejs-haiku/build-node.sh` | Haiku: configure, make and package one ported tree |
| `tools/nodejs-haiku/build-queue.sh` | Haiku: extract, port and build several releases in a row |
| `tools/nodejs-haiku/make-dist-index.py` | host: SHASUMS256.txt, index.json and index.tab for a dist directory |
| `tools/nodejs-haiku/publish-mirror.sh` | host: publish a dist directory as the mirror's GitHub releases |
| `tools/nodejs-haiku/verify-major.sh`, `verify-nvm-features.sh`, `test-node.js` | the checks below |
| `tools/nodejs-haiku/vm/` | the build VM's start script and ssh config |

The port is not a patch: every edit is anchored on the upstream source, is
skipped when already applied, and edits that exist only in some majors are
optional. A missing anchor is reported as MANUAL and the script exits 1,
so a new release either ports cleanly or says exactly what moved.

## Building a release

The build machine is an air/OS x86_64 install (a VM is fine) with gcc,
binutils, make, python3.10 and haiku_devel; Node 26 also needs rust_bin
(Temporal), and Node 10 and 11 need Python 2.7, which HaikuPorts dropped:
build it from HaikuPorts' last python-2.7.18 recipe and put it first on
PATH for those two.

    tar xJf node-vX.Y.Z.tar.xz
    python3.10 data/nvm/haiku/port/port-haiku.py node-vX.Y.Z
    sh tools/nodejs-haiku/build-node.sh node-vX.Y.Z dist

`build-node.sh` writes `dist/node-vX.Y.Z-haiku-x64.tar.{gz,xz}` in the
nodejs.org layout. Everything is bundled (OpenSSL, ICU, libuv, c-ares,
nghttp2, brotli, zlib), so the result needs only libroot, libbsd,
libnetwork and gcc_syslibs. If the release bundles a c-ares version with no
config in `ares/`, run `make-ares-config.sh X.Y.Z data/nvm/haiku/port/ares`
on Haiku first.

The VM used for the published builds (`vm/start.sh`: KVM, an air/OS CI
install on one NVMe disk and a 200 GB build volume on a second one,
`mount /dev/disk/nvme/1/raw /build` after each boot) runs
`build-queue.sh` as `/build/q.sh`, reading the port from `/build/port`
and the source tarballs from `/build/official`.

## Publishing

On the host, with each release in `dist/vX.Y.Z/`:

    python3 tools/nodejs-haiku/make-dist-index.py dist
    sh tools/nodejs-haiku/publish-mirror.sh dist

`make-dist-index.py` copies each release's metadata (npm, V8, LTS name,
security ...) from nodejs.org's index.json and lists only the files the
mirror holds. `publish-mirror.sh` makes one release per version (tag
`vX.Y.Z`) and the release `index` with index.tab and index.json; nvm maps
`$MIRROR/vX.Y.Z/<file>` and `$MIRROR/index/index.tab` onto them. Another
mirror in the plain nodejs.org layout works too, through
`NVM_HAIKU_MIRROR` (`https://...` or `file:///path`).

## Checks

In a login shell on air/OS (nvm loaded by the profile.d hook):

- `verify-major.sh <major>`: `nvm install <major>` (checksum verified),
  then `test-node.js` (fs, child_process, crypto, TLS via https and fetch,
  dns, zlib/brotli, Intl with full ICU, worker_threads, os), npm/npx, an
  `npm install` into a scratch project, and no crash reports.
- `verify-nvm-features.sh`: ls-remote (also --lts and the nodejs.org
  fallback), version-remote, ls, use, current, which, run, exec, a default
  alias seen by a new login shell, `npm install -g`, install by LTS name,
  uninstall and reinstall.

## nvm changes (against v0.40.8)

- `nvm_get_os` knows Haiku; the make-jobs count falls back to `nproc`.
- On Haiku the version index, binaries and SHASUMS256.txt come from the
  Haiku mirror (`NVM_HAIKU_MIRROR`, default the GitHub releases above);
  source tarballs still come from nodejs.org. `nvm ls-remote` lists the
  mirror's releases and falls back to nodejs.org's index for a version the
  mirror lacks, which nvm then builds from source.
- Source builds on Haiku check the build tools (and Python 2.7 for 10-11,
  Python 3.10/3.11 for 12-19, whose build systems need distutils), run
  `port-haiku.py` on the extracted tree and configure with
  `--dest-os=haiku --dest-cpu=x64`.
- `nvm exec`/`nvm run` find nvm-exec next to nvm.sh when nvm is outside
  `$NVM_DIR` (the system package).

## What the port changes

All majors: configure, gyp and the build files know Haiku (ELF/gcc POSIX
like the BSDs; -lbsd -lnetwork; V8 and OpenSSL linked whole for addons);
libuv's Haiku source list; c-ares' Haiku config; OpenSSL picks the Linux
per-arch configs (OPENSSLDIR /boot/system/data/ssl); V8_OS_HAIKU with
platform-haiku.cc (thread_info.stack_end is the stack start; images via
get_next_image_info), the sampler reading rip/rsp/rbp from Haiku's
mcontext; Node skips SIGKILLTHR when resetting signals and has no execinfo;
ICU's time zone comes from the kernel (`_kern_get_timezone`), since Haiku
has neither TZ nor /etc/localtime; and the bundled ICU stays out of the
dynamic symbol table (`--exclude-libs`), because libroot loads Haiku's own
ICU into every process and its calls would otherwise bind to Node's copy
(npm deadlocked at start on 18 and 21).

By major:
- 10, 11: V8's ThreadHash() treats pthread_t (a pointer on Haiku) as an
  integer. 11's libuv 1.27 predates libuv's Haiku support: the port adds
  libuv 1.34's haiku.c and its Haiku cases.
- 10, 11, 13, 15 (c-ares before 1.18): c-ares' cmake leaves HAVE_RECV,
  HAVE_RECVFROM and HAVE_SEND undefined on Haiku; the configs define them.
  (Without them setup_once.h emits a bare `Error ...` line, and the error
  that shows is an "incomplete type" in net/if.h further down.)
- 12-19: configure runs on Python 3.10/3.11; the bundled jinja2/markupsafe
  import the ABCs from collections.abc.
- 10-19: gcc 13's headers no longer include <cstdint>, <utility> etc.
  transitively; they are pre-included.
- 14, 15: V8's is_subtype instantiated std::is_convertible on incomplete
  types, which gcc 13 rejects.
- 16: a second signal-reset loop at exit; npm 8's cacache uses rename
  instead of link() (BFS has no hard links).
- 21: QUIC deps include <arpa/inet.h> on Haiku too.
- 22+: C++20 for ncrypto; abseil's GetTID() uses find_thread().
- 24+: no IPV6_TCLASS; OpenSSL 3.5's link flags; V8's shared-library TLS
  mode (Haiku links executables like shared objects).
- 26: Temporal on: the vendored Rust crates are built with rust_bin; the
  staticlib needs -lnetwork -lbsd.

Known limits: `os.cpus()` reports model "unknown" and no CPU times; no
native backtraces in fatal errors; native addons cannot use Node's ICU.

## Traps

- The build host (airos-build) is shared with CI: two build VMs of 20 and
  14 GB plus CI pushed it into a global OOM on 2026-11-02 that killed a CI
  runner and the build VM. Keep the VMs' total around 24 GB and watch
  MemAvailable; one V8 compile job needs about 1 GB.
- The VM disks use cache=unsafe: after QEMU dies, object files written in
  the last minutes may be truncated. Delete the newest `out/` objects
  before resuming a build.
- `_kern_set_timezone` moves the system clock (it once broke make's
  timestamps).
- A copied .hpkg must be moved into a packages directory, not written into
  it (package_daemon reads half-written files).

## Releases on the mirror

The newest release of every major, 2026-11-02. "Port" builds were made with
`port-haiku.py` (each with every fix its major needs);
the 16, 18, 20, 21 and 23 binaries are the 2026-09-27 builds from the
earlier per-major patches (the same edits; port-haiku.py made those
patches), which are already the last releases of their majors. Verified on
the X399 (air/OS hrev60207+2114) with `verify-major.sh` through nvm from a
copy of the mirror: all PASS; `verify-nvm-features.sh`: all PASS.

| Major | Version | V8 | npm | OpenSSL | Built by |
|---|---|---|---|---|---|
| 10 | 10.24.1 | 6.8.275 | 6.14.12 | 1.1.1k | port |
| 11 | 11.15.0 | 7.0.276 | 6.7.0 | 1.1.1b | port |
| 12 | 12.22.12 | 7.8.279 | 6.14.16 | 1.1.1n | port |
| 13 | 13.14.0 | 7.9.317 | 6.14.4 | 1.1.1g | port |
| 14 | 14.21.3 | 8.4.371 | 6.14.18 | 1.1.1t | port |
| 15 | 15.14.0 | 8.6.395 | 7.7.6 | 1.1.1k | port |
| 16 | 16.20.2 | 9.4.146 | 8.19.4 | 1.1.1v | patch (09-27) |
| 17 | 17.9.1 | 9.6.180 | 8.11.0 | 3.0.3 | port |
| 18 | 18.20.8 | 10.2.154 | 10.8.2 | 3.0.16 | patch (09-27) |
| 19 | 19.9.0 | 10.8.168 | 9.6.3 | 3.0.8 | port |
| 20 | 20.20.2 | 11.3.244 | 10.8.2 | 3.0.19 | patch (09-27) |
| 21 | 21.7.3 | 11.8.172 | 10.5.0 | 3.0.13 | patch (09-27) |
| 22 | 22.23.4 | 12.4.254 | 10.9.9 | 3.5.9 | port |
| 23 | 23.11.1 | 12.9.202 | 10.9.2 | 3.0.16 | patch (09-27) |
| 24 | 24.22.2 | 13.6.233 | 11.19.1 | 3.5.9 | port |
| 25 | 25.9.0 | 14.1.146 | 11.12.1 | 3.5.5 | port |
| 26 | 26.13.0 | 14.6.202 | 11.20.0 | 3.5.9 | port (Temporal on) |

Intl: 10 and 11 have Node's small ICU (English only); 12 and later full
ICU. 10 and 11 have no global fetch; 10's worker_threads needs
--experimental-worker. The build dist is kept in
`/data3/airos/node-haiku/dist` on airos-build (1.1 GB with the .tar.gz).
