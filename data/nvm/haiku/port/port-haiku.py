#!/usr/bin/env python3
"""Port an extracted Node.js source tree to Haiku (x86_64).

usage: port-haiku.py <node-source-dir> [ares_config.h]

nvm runs this on the source tree of every release it builds on Haiku, and
tools/nodejs-haiku/build-node.sh does for the binaries on the Haiku mirror.
Every edit is anchored on the upstream source and skipped when its marker
shows it is already there; an anchor that is missing is reported as MANUAL
(exit status 1) instead of guessed. Edits that only exist in some majors are
marked optional, so one script covers Node.js 10 to 26 and the later
releases of each major.

c-ares' own configure result on Haiku becomes deps/cares/config/haiku/
ares_config.h: the given file, or the one in ares/ next to this script that
matches the bundled c-ares (see make-ares-config.sh). With
PORT_HAIKU_ORIG=<dir> the original of every changed file is kept there.
"""
import os, re, shutil, sys

root = sys.argv[1]
manual = []
# PORT_HAIKU_ORIG=<dir>: keep a copy of every file before its first change
# (lets a patch be made with plain diff, without a git checkout).
orig_dir = os.environ.get("PORT_HAIKU_ORIG")


def backup(full):
    if not orig_dir:
        return
    rel = os.path.relpath(full, root)
    dst = os.path.join(orig_dir, rel)
    if not os.path.exists(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(full):
            shutil.copy2(full, dst)
        else:
            open(dst + ".NEW", "w").close()


def path(p):
    return os.path.join(root, p)


def edit(p, fn, what, done, optional=False):
    """Apply fn to file p unless the marker `done` shows it was applied."""
    full = path(p)
    if not os.path.exists(full):
        if not optional:
            manual.append("%s: missing file (%s)" % (p, what))
        return
    s = open(full, encoding="utf-8").read()
    if done in s:
        return
    try:
        t = fn(s)
    except LookupError as e:
        if not optional:
            manual.append("%s: %s (%s)" % (p, what, e))
        return
    if t == s:
        if not optional:
            manual.append("%s: no change for %s" % (p, what))
    else:
        backup(full)
        open(full, "w", encoding="utf-8").write(t)


def once(s, old, new):
    if s.count(old) != 1:
        raise LookupError("anchor found %d times: %r" % (s.count(old), old[:70]))
    return s.replace(old, new)


def resub(s, pattern, repl, flags=0):
    t, n = re.subn(pattern, repl, s, count=1, flags=flags)
    if n != 1:
        raise LookupError("pattern not found: %r" % pattern[:70])
    return t


# --- build system -----------------------------------------------------------

_ver = open(path("src/node_version.h")).read()
_major = int(re.search(r"#define NODE_MAJOR_VERSION (\d+)", _ver).group(1))

edit("configure.py",
     lambda s: resub(s, r"(valid_os = \((?:[^)]*?))\)", r"\1, 'haiku')", re.S),
     "valid_os += haiku", "'haiku')")



# Node 12 to 13 and 15: configure only accepts the Pythons of its day, the
# newest of which HaikuPorts no longer has; their build runs on 3.10 and 3.11
# (3.12 dropped distutils, which they import). 10 and 11 need Python 2.7.
def configure_python(s):
    s = once(s, "_=[ 'exec' '/bin/sh' '-c' '''\n",
             "_=[ 'exec' '/bin/sh' '-c' '''\n"
             "command -v python3.11 >/dev/null && exec python3.11 \"$0\" \"$@\"\n"
             "command -v python3.10 >/dev/null && exec python3.10 \"$0\" \"$@\"\n")
    if "acceptable_pythons = ((" in s:
        return once(s, "acceptable_pythons = ((", "acceptable_pythons = ((3, 11), (3, 10), (")
    return once(s, "if sys.version_info[:2] != (2, 7):", "if sys.version_info[:2] not in ((2, 7), (3, 10), (3, 11)):")


edit("configure", configure_python, "Python 3.10 and 3.11", "python3.10",
     optional=_major < 12)

edit("tools/gyp/pylib/gyp/common.py",
     lambda s: resub(s, r"(def GetFlavor(?:ByPlatform)?\((?:params)?\):.*?)(\n\n([ \t]+)return [\"']linux[\"'])",
                     r'\1\n\3if sys.platform.startswith("haiku"):\n\3    return "haiku"\2', re.S),
     "GetFlavor haiku", 'startswith("haiku")')

edit("tools/utils.py",
     lambda s: resub(s, r"(\n(\s+)elif id == 'FreeBSD':\n\s+return 'freebsd'\n)",
                     r"\1\2elif id == 'Haiku':\n\2  return 'haiku'\n"),
     "GuessOS haiku", "id == 'Haiku'")


def common_gypi(s):
    s = resub(s, r"\[ 'OS in \"(linux freebsd openbsd solaris aix[^\"]*)\"', \{\n(\s+)'cflags': \[ '-pthread' \]",
              r"""[ 'OS in "\1 haiku"', {\n\2'cflags': [ '-pthread' ]""")
    s = resub(s, r"\[ 'OS in \"(linux freebsd openbsd solaris android aix[^\"]*)\"', \{",
              r"""[ 'OS in "\1 haiku"', {""")
    return s


edit("common.gypi", common_gypi, "POSIX OS lists", ' haiku"')

# Node 19 and older predate gcc 13, whose C++ library headers no longer pull
# in <cstdint>, <utility> and friends: V8 and Node then miss declarations
# (std::exchange, uint8_t ...). Pre-include them, as gcc 12 effectively did.
GCC13_INCLUDES = ["cstdint", "cstddef", "cstring", "climits", "limits", "utility",
                  "memory", "string", "stdexcept", "algorithm"]
if _major < 20:
    edit("common.gypi",
         lambda s: resub(s, r"(\n(\s+)\[ 'OS in \"linux freebsd openbsd solaris aix[^\"]*haiku\"', \{\n\s+'cflags': \[ '-pthread' \])",
                         lambda m: "\n%s[ 'OS==\"haiku\"', {\n%s  'cflags_cc': [ %s ],\n%s}],%s" % (
                             m.group(2), m.group(2),
                             ", ".join("'-include', '%s'" % h for h in GCC13_INCLUDES),
                             m.group(2), m.group(1))),
         "gcc 13 headers", "'-include', 'cstdint'")

# V8 8.4 to 8.6 (Node 14, 15) ask is_subtype<T, U> about types that are only
# forward-declared there; its std::is_convertible<T, Object> is instantiated
# even when U is not MaybeObject, and gcc 13 rejects that for an incomplete
# T. std::conditional only instantiates it for MaybeObject (same result).
if _major < 16:
    edit("deps/v8/src/codegen/tnode.h",
         lambda s: once(s, "      std::is_base_of<U, T>::value || (std::is_same<U, MaybeObject>::value &&\n"
                           "                                       std::is_convertible<T, Object>::value);\n",
                        "      std::is_base_of<U, T>::value ||\n"
                        "      std::conditional<std::is_same<U, MaybeObject>::value,\n"
                        "                       std::is_convertible<T, Object>,\n"
                        "                       std::false_type>::type::value;\n"),
         "is_subtype for incomplete types", "std::conditional<std::is_same<U, MaybeObject>::value",
         optional=True)


# Node 12 to 19 bundle jinja2/markupsafe releases that import the container
# ABCs from collections, which Python 3.10 no longer allows.
if 12 <= _major < 20:
    for top in ("deps/v8/third_party", "tools"):
        for dirpath, dirs, files in os.walk(path(top)):
            if os.path.basename(dirpath) not in ("jinja2", "markupsafe"):
                continue
            for f in files:
                if f.endswith(".py"):
                    rel = os.path.relpath(os.path.join(dirpath, f), root)
                    edit(rel, lambda s: re.sub(
                        r"^(\s*)from collections import ((?:Mutable)?(?:Mapping|Set|Sequence)(?:, (?:Mutable)?(?:Mapping|Set|Sequence))*)$",
                        r"\1from collections.abc import \2", s, flags=re.M),
                         "collections.abc", "from collections.abc import", optional=True)


def node_gypi(s):
    s = resub(s, r"(\n    \[ 'OS==\"solaris\"', \{\n      'libraries': \[\n        '-lkstat',)",
              "\n    [ 'OS==\"haiku\"', {\n      'libraries': [\n        '-lbsd',\n        '-lnetwork',\n      ],\n"
              "      # libroot's locale backend loads Haiku's own ICU into the process;\n"
              "      # keep the bundled ICU out of the dynamic symbol table so the two\n"
              "      # copies cannot bind to each other (same-version names clash).\n"
              "      'ldflags': [\n        '-Wl,--exclude-libs,libicudata.a:libicui18n.a:libicuucx.a',\n      ],\n    }],\\1")
    # link all of V8 and OpenSSL into the binary, so that addons can use them
    s = resub(s, r'\(OS=="freebsd" or OS=="linux"((?: or OS=="\w+")*)\)', r'(OS=="freebsd" or OS=="linux"\1 or OS=="haiku")')
    s = resub(s, r"\['OS in \"linux freebsd([^\"]*)\" and node_shared==\"false\"', \{\n(\s+)'ldflags': \[\n\s+'-Wl,--whole-archive,?',?\n\s+'<\(obj_dir\)/deps/openssl/",
              lambda m: m.group(0).replace('"linux freebsd%s"' % m.group(1), '"linux freebsd%s haiku"' % m.group(1), 1))
    return s


# Node 22+: common_node.gypi switches Node's own sources to C++20 per OS.
edit("common_node.gypi",
     lambda s: resub(s, r"(\['OS in \"linux freebsd openbsd solaris android aix[^\"]*)(\"', \{\n\s+'cflags_cc!': \['-std=gnu\+\+17'\])",
                     r"\1 haiku\2"),
     "C++20 for Node core", ' haiku"', optional=True)

edit("node.gypi", node_gypi, "haiku libraries, whole-archive V8", '-lnetwork')

UV_HAIKU = """        [ 'OS=="haiku"', {
          'defines': [ '_BSD_SOURCE' ],
          'sources': [
            'src/unix/haiku.c',
            'src/unix/bsd-ifaddrs.c',
            'src/unix/no-fsevents.c',
            'src/unix/no-proctitle.c',
            'src/unix/posix-hrtime.c',
            'src/unix/posix-poll.c',
          ],
          'link_settings': {
            'libraries': [ '-lbsd', '-lnetwork' ],
          },
        }],
"""
edit("deps/uv/uv.gyp",
     lambda s: resub(s, r"(\n)(        \[ 'OS==\"freebsd\" or OS==\"dragonflybsd\"', \{\n)", r"\1" + UV_HAIKU.replace("\\", "\\\\") + r"\2"),
     "libuv haiku sources", 'src/unix/haiku.c')


# libuv before 1.29 (Node 11 bundles 1.27) has no Haiku support. Add libuv's
# own src/unix/haiku.c (from 1.34, as Node 10.24 bundles it, with the
# uv_free_cpu_info that 1.27 leaves to each platform and without
# uv_get_constrained_memory, which 1.27 does not have) and the Haiku cases
# libuv 1.34 has in the files 1.27 shares with it.
UV_HAIKU_C = r'''/* Copyright libuv project contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <FindDirectory.h> /* find_path() */
#include <OS.h>


void uv_loadavg(double avg[3]) {
  avg[0] = 0;
  avg[1] = 0;
  avg[2] = 0;
}


int uv_exepath(char* buffer, size_t* size) {
  char abspath[B_PATH_NAME_LENGTH];
  status_t status;
  ssize_t abspath_len;

  if (buffer == NULL || size == NULL || *size == 0)
    return UV_EINVAL;

  status = find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, abspath,
                     sizeof(abspath));
  if (status != B_OK)
    return UV__ERR(status);

  abspath_len = uv__strscpy(buffer, abspath, *size);
  *size -= 1;
  if (abspath_len >= 0 && *size > (size_t)abspath_len)
    *size = (size_t)abspath_len;

  return 0;
}


uint64_t uv_get_free_memory(void) {
  status_t status;
  system_info sinfo;

  status = get_system_info(&sinfo);
  if (status != B_OK)
    return 0;

  return (sinfo.max_pages - sinfo.used_pages) * B_PAGE_SIZE;
}


uint64_t uv_get_total_memory(void) {
  status_t status;
  system_info sinfo;

  status = get_system_info(&sinfo);
  if (status != B_OK)
    return 0;

  return sinfo.max_pages * B_PAGE_SIZE;
}



int uv_resident_set_memory(size_t* rss) {
  area_info area;
  ssize_t cookie;
  status_t status;
  thread_info thread;

  status = get_thread_info(find_thread(NULL), &thread);
  if (status != B_OK)
    return UV__ERR(status);

  cookie = 0;
  *rss = 0;
  while (get_next_area_info(thread.team, &cookie, &area) == B_OK)
    *rss += area.ram_size;

  return 0;
}


int uv_uptime(double* uptime) {
  /* system_time() returns time since booting in microseconds */
  *uptime = (double)system_time() / 1000000;
  return 0;
}


int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  cpu_topology_node_info* topology_infos;
  int i;
  status_t status;
  system_info system;
  uint32_t topology_count;
  uint64_t cpuspeed;
  uv_cpu_info_t* cpu_info;

  if (cpu_infos == NULL || count == NULL)
    return UV_EINVAL;

  status = get_cpu_topology_info(NULL, &topology_count);
  if (status != B_OK)
    return UV__ERR(status);

  topology_infos = uv__malloc(topology_count * sizeof(*topology_infos));
  if (topology_infos == NULL)
    return UV_ENOMEM;

  status = get_cpu_topology_info(topology_infos, &topology_count);
  if (status != B_OK) {
    uv__free(topology_infos);
    return UV__ERR(status);
  }

  cpuspeed = 0;
  for (i = 0; i < (int)topology_count; i++) {
    if (topology_infos[i].type == B_TOPOLOGY_CORE) {
      cpuspeed = topology_infos[i].data.core.default_frequency;
      break;
    }
  }

  uv__free(topology_infos);

  status = get_system_info(&system);
  if (status != B_OK)
    return UV__ERR(status);

  *cpu_infos = uv__calloc(system.cpu_count, sizeof(**cpu_infos));
  if (*cpu_infos == NULL)
    return UV_ENOMEM;

  /* CPU time and model are not exposed by Haiku. */
  cpu_info = *cpu_infos;
  for (i = 0; i < (int)system.cpu_count; i++) {
    cpu_info->model = uv__strdup("unknown");
    cpu_info->speed = (int)(cpuspeed / 1000000);
    cpu_info++;
  }
  *count = system.cpu_count;

  return 0;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++)
    uv__free(cpu_infos[i].model);

  uv__free(cpu_infos);
}
'''

if not os.path.exists(path("deps/uv/src/unix/haiku.c")):
    full = path("deps/uv/src/unix/haiku.c")
    backup(full)
    open(full, "w", encoding="utf-8").write(UV_HAIKU_C)

    edit("deps/uv/include/uv/unix.h",
         lambda s: once(s, '#elif defined(__GNU__)\n# include "uv/posix.h"\n',
                        '#elif defined(__GNU__)\n# include "uv/posix.h"\n'
                        '#elif defined(__HAIKU__)\n# include "uv/posix.h"\n'),
         "libuv 1.27: Haiku uses uv/posix.h", "__HAIKU__")

    def uv_core(s):
        # No FIOCLEX (uv__cloexec_fcntl is used) and a struct rusage with
        # only the times.
        s = once(s, "#if !defined(__CYGWIN__) && !defined(__MSYS__)\nint uv__cloexec_ioctl(",
                 "#if !defined(__CYGWIN__) && !defined(__MSYS__) && !defined(__HAIKU__)\nint uv__cloexec_ioctl(")
        return once(s, "#if !defined(__MVS__)\n  rusage->ru_maxrss",
                    "#if !defined(__MVS__) && !defined(__HAIKU__)\n  rusage->ru_maxrss")
    edit("deps/uv/src/unix/core.c", uv_core, "libuv 1.27: Haiku cloexec, rusage", "__HAIKU__")

    edit("deps/uv/src/unix/process.c",
         lambda s: once(s, "    if (n == SIGKILL || n == SIGSTOP)\n      continue;  /* Can't be changed. */\n",
                        "    if (n == SIGKILL || n == SIGSTOP)\n      continue;  /* Can't be changed. */\n\n"
                        "#if defined(__HAIKU__)\n    if (n == SIGKILLTHR)\n      continue;  /* Can't be changed. */\n#endif\n"),
         "libuv 1.27: Haiku SIGKILLTHR", "__HAIKU__")

    def uv_ifaddrs(s):
        s = once(s, "\nstatic int uv__ifaddr_exclude(",
                 "\n#if defined(__HAIKU__)\n#define IFF_RUNNING IFF_LINK\n#endif\n\nstatic int uv__ifaddr_exclude(")
        return once(s, "#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__)\n  /*\n   * On BSD getifaddrs",
                    "#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \\\n"
                    "    defined(__HAIKU__)\n  /*\n   * On BSD getifaddrs")
    edit("deps/uv/src/unix/bsd-ifaddrs.c", uv_ifaddrs, "libuv 1.27: Haiku interface addresses", "__HAIKU__")

    def uv_fs(s):
        # futimens() and utimensat() exist on Haiku.
        s = once(s, "#if defined(__linux__)                                                        \\\n    || defined(_AIX71)\n",
                 "#if defined(__linux__)                                                        \\\n    || defined(_AIX71)                                                         \\\n    || defined(__HAIKU__)\n")
        return once(s, "    || defined(_AIX71)                                                         \\\n    || defined(__sun)\n",
                    "    || defined(_AIX71)                                                         \\\n    || defined(__sun)                                                          \\\n    || defined(__HAIKU__)\n")
    edit("deps/uv/src/unix/fs.c", uv_fs, "libuv 1.27: Haiku futimens, utimensat", "__HAIKU__")

CARES_HAIKU = """        [ 'OS=="haiku"', {
          'include_dirs': [ 'config/haiku' ],
          'sources': [ 'config/haiku/ares_config.h' ],
          'direct_dependent_settings': {
            'libraries': [ '-lnetwork' ],
          },
        }],
"""
edit("deps/cares/cares.gyp",
     lambda s: resub(s, r"(        \[ 'OS==\"openbsd\"', \{\n          'include_dirs': \[ 'config/openbsd' \],\n.*?\n        \}\],\n)",
                     lambda m: m.group(1) + CARES_HAIKU, re.S),
     "c-ares haiku config", 'config/haiku')


def ares_config():
    """The c-ares configure result for the bundled c-ares, from ares/ next to
    this script (ares_config-X.Y.Z.h, made by make-ares-config.sh). A c-ares
    release without one takes the nearest older one of the same minor
    version, then the nearest older one at all: c-ares' configure checks
    hardly change between releases (1.34.5 to 1.34.9 give the same file)."""
    s = open(path("deps/cares/include/ares_version.h")).read()
    want = tuple(int(n) for n in re.search(r'ARES_VERSION_STR\s+"(\d+)\.(\d+)\.(\d+)', s).groups())
    have = {}
    adir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ares")
    for f in os.listdir(adir):
        m = re.match(r"ares_config-(\d+)\.(\d+)\.(\d+)\.h$", f)
        if m:
            have[tuple(int(n) for n in m.groups())] = os.path.join(adir, f)
    older = sorted(v for v in have if v <= want)
    same_minor = [v for v in older if v[:2] == want[:2]]
    pick = (same_minor or older or [None])[-1]
    if pick is None:
        manual.append("deps/cares/config/haiku/ares_config.h: no config for c-ares %d.%d.%d,"
                      " run make-ares-config.sh on Haiku" % want)
        return None
    if pick != want:
        print("note: c-ares %d.%d.%d uses the configure result of %d.%d.%d" % (want + pick))
    return have[pick]


config = sys.argv[2] if len(sys.argv) > 2 else ares_config()
if config:
    os.makedirs(path("deps/cares/config/haiku"), exist_ok=True)
    backup(path("deps/cares/config/haiku/ares_config.h"))
    shutil.copy(config, path("deps/cares/config/haiku/ares_config.h"))

OPENSSL_HAIKU = """    }, 'OS=="haiku"', {
      'cflags': ['-Wno-missing-field-initializers',],
      'defines': [
        'OPENSSLDIR="/boot/system/data/ssl"',
        'ENGINESDIR="/dev/null"',
        'TERMIOS',
      ],
      # Haiku has no libdl (dlopen() is in libroot); sockets are in libnetwork.
      'libraries!': ['-ldl -pthread', '-lm -ldl -pthread'],
      'libraries': ['-lm', '-pthread', '-lnetwork'],
"""
edit("deps/openssl/openssl_common.gypi",
     lambda s: resub(s, r"(        '__EXTENSIONS__'\n      \],\n)(    \}, \{\n)", lambda m: m.group(1) + OPENSSL_HAIKU + m.group(2)),
     "openssl haiku defines", 'OS=="haiku"')

# OpenSSL's per-arch headers are picked by the preprocessor; Haiku uses the
# Linux configurations (same ELF/gas/pthreads platform, same 64-bit longs).
cfg = path("deps/openssl/config")
n = 0
for f in sorted(os.listdir(cfg)):
    if f.endswith((".h", ".tmpl")):
        p = os.path.join(cfg, f)
        s = open(p).read()
        t = s.replace("#if defined(__linux) && !defined(__ANDROID__)\n",
                      "#if (defined(__linux) && !defined(__ANDROID__)) || defined(__HAIKU__)\n")
        if t != s:
            backup(p)
            open(p, "w").write(t)
            n += 1
if n == 0 and not any("defined(__HAIKU__)" in open(os.path.join(cfg, f)).read()
                      for f in os.listdir(cfg) if f.endswith(".h")):
    manual.append("deps/openssl/config: no arch-selection headers changed")

# --- Node.js ------------------------------------------------------------------

edit("src/node.cc",
     lambda s: resub(s, r"(\n(\s+)if \(nr == SIGKILL \|\| nr == SIGSTOP\)\n\s+continue;\n)",
                     r"\1#ifdef __HAIKU__\n\2// SIGKILLTHR cannot be caught or ignored either.\n\2if (nr == SIGKILLTHR)\n\2  continue;\n#endif\n"),
     "skip SIGKILLTHR", 'SIGKILLTHR')

# Node 16 resets the signal dispositions a second time, at exit.
edit("src/node_main_instance.cc",
     lambda s: resub(s, r"(\n(\s+)if \(nr == SIGKILL \|\| nr == SIGSTOP \|\| nr == SIGPROF\)\n\s+continue;\n)",
                     r"\1#ifdef __HAIKU__\n\2if (nr == SIGKILLTHR)\n\2  continue;\n#endif\n"),
     "skip SIGKILLTHR at exit", 'SIGKILLTHR', optional=True)

# npm 8 (Node 16) moves cache entries with link()+unlink(); BFS has no hard
# links (EPERM), so move the file instead.
edit("deps/npm/node_modules/cacache/lib/util/move-file.js",
     lambda s: resub(s, r"(\n  try \{\n    await fs\.link\(src, dest\)\n  \} catch \(err\) \{\n)(    if \(isWindows && err\.code === 'EPERM'\) \{)",
                     r"\1    if (process.platform === 'haiku' && err.code === 'EPERM') {\n"
                     r"      // Haiku's BFS has no hard links: move the file instead.\n"
                     r"      await move(src, dest)\n"
                     r"      return fs.chmod(dest, '0444')\n"
                     r"    }\n\2"),
     "npm cacache without hard links", "haiku", optional=True)

# Node 24+ (socket.setTypeOfService): Haiku has no IPV6_TCLASS; an invalid
# option makes setsockopt()/getsockopt() fail, which is reported to JS.
edit("src/tcp_wrap.cc",
     lambda s: s if "IPV6_TCLASS" not in s else resub(s, r"(\nnamespace node \{\n)",
                     r"\n#if defined(__HAIKU__) && !defined(IPV6_TCLASS)\n#define IPV6_TCLASS -1  // not supported by Haiku\n#endif\n\1"),
     "no IPV6_TCLASS", "__HAIKU__", optional=True)

edit("src/debug_utils.cc",
     lambda s: resub(s, r"(\n    defined\(_AIX\))(\n#define HAVE_EXECINFO_H 0)", r"\1 || defined(__HAIKU__)\2"),
     "no execinfo", '__HAIKU__')


def node_report(s):
    s = resub(s, r"#if !\(defined\(_AIX\) \|\| defined\(__sun\)\)\n(\s+\{\"max_locked_memory_bytes\")",
              r"#if !(defined(_AIX) || defined(__sun) || defined(__HAIKU__))\n\1")
    s = resub(s, r"#ifndef __sun\n(\s+\{\"max_memory_size_k?bytes\")", r"#if !(defined(__sun) || defined(__HAIKU__))\n\1")
    s = resub(s, r"#ifndef __sun\n(\s+\{\"max_user_processes\")", r"#if !(defined(__sun) || defined(__HAIKU__))\n\1")
    return s


edit("src/node_report.cc", node_report, "Haiku lacks RLIMIT_MEMLOCK/RSS/NPROC", '__HAIKU__',
     optional=not os.path.exists(path("src/node_report.cc")))


# ICU (bundled, full): Haiku has no /etc/localtime and TZ is normally unset;
# the zone picked in the Time preferences is kept by the kernel.
def icu_putil(s):
    s = resub(s, r"(\nU_CAPI const char\* U_EXPORT2\nuprv_tzname\(int n\)\n)",
              "\n#if U_PLATFORM == U_PF_HAIKU\n"
              "extern \"C\" int32_t _kern_get_timezone(int32_t* offset, char* name, size_t length);\n"
              "#endif\n\\1")
    s = resub(s, r"(\n    /\* else U_TZNAME will give a better result. \*/\n#endif\n)",
              "\\1\n#if U_PLATFORM == U_PF_HAIKU\n"
              "    /* The zone chosen in Haiku's Time preferences, as the kernel keeps it. */\n"
              "    {\n"
              "        static char haikuTimeZone[64];\n"
              "        if (_kern_get_timezone(nullptr, haikuTimeZone, sizeof(haikuTimeZone)) == 0\n"
              "                && isValidOlsonID(haikuTimeZone)) {\n"
              "            return haikuTimeZone;\n"
              "        }\n"
              "    }\n"
              "#endif\n")
    return s


edit("deps/icu-small/source/common/putil.cpp", icu_putil, "Haiku default time zone", '_kern_get_timezone')

# --- V8 -----------------------------------------------------------------------


def v8config(s):
    s = resub(s, r"(//  V8_OS_FREEBSD +- FreeBSD\n)", r"\1//  V8_OS_HAIKU         - Haiku\n")
    if "V8_OS_STRING" in s:
        s = resub(s, r"(# define V8_OS_STRING \"freebsd\"\n\n)",
                  r'\1#elif defined(__HAIKU__)\n# define V8_OS_HAIKU 1\n# define V8_OS_POSIX 1\n# define V8_OS_STRING "haiku"\n\n')
    else:  # V8 8 and older
        s = resub(s, r"(# define V8_OS_FREEBSD 1\n# define V8_OS_POSIX 1\n)",
                  r"\1#elif defined(__HAIKU__)\n# define V8_OS_HAIKU 1\n# define V8_OS_POSIX 1\n")
    return s


edit("deps/v8/include/v8config.h", v8config, "V8_OS_HAIKU", 'V8_OS_HAIKU')


def platform_posix(s):
    s = resub(s, r"(#if !defined\(_AIX\) && !defined\(V8_OS_FUCHSIA\)[^\n]*)(\n#include <sys/syscall.h>)",
              r"\1 && !defined(__HAIKU__)\2")
    # Haiku has its own Stack::ObtainCurrentThreadStackStart / GetStackStart
    # (V8 8.4 and later; before, V8 had no such function).
    if "GetStackStart" in s:
        s = resub(s, r"(\n#if !defined\(V8_OS_FREEBSD\) && !defined\(V8_OS_(?:DARWIN|MACOSX)\) && !defined\(_AIX\) && \\\n\s+!defined\(V8_OS_SOLARIS\))",
                  r"\1 && !defined(V8_OS_HAIKU)")
    return s


edit("deps/v8/src/base/platform/platform-posix.cc", platform_posix, "no sys/syscall.h, own stack start", '__HAIKU__')


def sampler(s):
    s = resub(s, r"(#if !V8_OS_QNX && !V8_OS_AIX[^\n]*)(\n#include <sys/syscall.h>)", r"\1 && !V8_OS_HAIKU\2")
    s = resub(s, r"(#elif[^\n]*!V8_OS_OPENBSD)\n#include <ucontext.h>", r"\1 && !V8_OS_HAIKU\n#include <ucontext.h>")
    s = resub(s, r"(\n#elif V8_OS_NETBSD\n#if V8_HOST_ARCH_IA32\n)",
              "\n#elif V8_OS_HAIKU\n#if V8_HOST_ARCH_X64\n"
              "  state->pc = reinterpret_cast<void*>(mcontext.rip);\n"
              "  state->sp = reinterpret_cast<void*>(mcontext.rsp);\n"
              "  state->fp = reinterpret_cast<void*>(mcontext.rbp);\n"
              "#endif  // V8_HOST_ARCH_*\\1")
    # V8 6.x and 7.x (Node 10, 11) hash pthread_t as an integer; on Haiku it
    # is a pointer, as on the BSDs.
    if "uint32_t ThreadHash(pthread_t thread_id)" in s:
        s = resub(s, r"(uint32_t ThreadHash\(pthread_t thread_id\) \{\n#if V8_OS_BSD)\n",
                  r"\1 || V8_OS_HAIKU\n")
    return s


edit("deps/v8/src/libsampler/sampler.cc", sampler, "Haiku ucontext registers", 'V8_OS_HAIKU')


def export_template(s):
    s = once(s, "#define EXPORT_TEMPLATE_DEFINE_MSVC_HACK(export, _) export\n",
             "#define EXPORT_TEMPLATE_DEFINE_MSVC_HACK(export, _) export\n"
             "#define EXPORT_TEMPLATE_TEST_MSVC_HACK_DEFAULT(...) true\n")
    s = once(s, "#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_MSVC_HACK\n",
             "#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_MSVC_HACK\n#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_DEFAULT\n")
    return s


edit("deps/v8/src/base/export-template.h", export_template, "__declspec is predefined on Haiku", 'MSVC_HACK_DEFAULT')

edit("deps/v8/src/trap-handler/trap-handler.h",
     lambda s: resub(s, r"#if defined\(V8_OS_AIX\)\n(// `thread_local` does not link on AIX)",
                     r"#if defined(V8_OS_AIX) || defined(V8_OS_HAIKU)\n\1"),
     "__thread for g_thread_in_wasm_code", 'V8_OS_HAIKU', optional=True)

# Node 10 and 11 build V8 with its own gyp files (deps/v8/gypfiles).
v8_own_gyp = not os.path.exists(path("tools/v8_gypfiles"))

edit("tools/v8_gypfiles/features.gypi",
     lambda s: resub(s, r"(\n(\s+)\['OS==\"android\"', \{ # GYP reverts OS to linux)",
                     r"""\n\2['OS == "haiku"', {\n\2  'is_haiku': 1,\n\2}, {\n\2  'is_haiku': 0,\n\2}],\1"""),
     "is_haiku", 'is_haiku', optional=v8_own_gyp)

edit("tools/v8_gypfiles/v8.gyp",
     lambda s: resub(s, r"(\n(\s+)\['OS == \"mac\" or (?:\(_toolset==\"host\" and host_os==\"mac\"\)|OS == \"ios\")', \{\n\s+'sources': \[\n\s+'<\(V8_ROOT\)/src/base/debug/stack_trace_posix.cc',)",
                     r"\n\2['is_haiku', {\n\2  'sources': [\n\2    '<(V8_ROOT)/src/base/debug/stack_trace_posix.cc',\n\2    '<(V8_ROOT)/src/base/platform/platform-haiku.cc',\n\2  ]\n\2}],\1"),
     "platform-haiku.cc", 'platform-haiku.cc', optional=v8_own_gyp)

V8_LIBBASE_HAIKU = """        ['OS=="haiku"', {
            'sources': [
              '../src/base/debug/stack_trace_posix.cc',
              '../src/base/platform/platform-haiku.cc',
              '../src/base/platform/platform-posix.h',
              '../src/base/platform/platform-posix.cc',
              '../src/base/platform/platform-posix-time.h',
              '../src/base/platform/platform-posix-time.cc',
            ],
          }
        ],
"""
if v8_own_gyp:
    edit("deps/v8/gypfiles/v8.gyp",
         lambda s: resub(s, r"(\n)(        \['OS==\"freebsd\"', \{\n            'link_settings': \{)",
                         lambda m: m.group(1) + V8_LIBBASE_HAIKU + m.group(2)),
         "platform-haiku.cc", 'platform-haiku.cc')

# abseil (V8 12 and later): a thread ID for Haiku, whose pthread_t is a pointer.
def absl_sysinfo(s):
    s = resub(s, r"(\n#ifdef __NetBSD__\n#include <lwp.h>\n#endif\n)", r"\1\n#ifdef __HAIKU__\n#include <OS.h>\n#endif\n")
    s = resub(s, r"(\n#elif defined\(__NetBSD__\)\n\npid_t GetTID\(\) \{ return static_cast<pid_t>\(_lwp_self\(\)\); \}\n)",
              r"\1\n#elif defined(__HAIKU__)\n\npid_t GetTID() { return find_thread(nullptr); }\n")
    return s


edit("deps/v8/third_party/abseil-cpp/absl/base/internal/sysinfo.cc", absl_sysinfo, "abseil GetTID", "__HAIKU__", optional=True)

# V8 13+: Haiku links executables like shared objects, so the "local-exec"
# TLS model V8 uses in non-component builds does not link; use the
# shared-library mode (local-dynamic behind a getter).
edit("deps/v8/src/common/thread-local-storage.h",
     lambda s: resub(s, r"#if defined\(COMPONENT_BUILD\) \|\| defined\(V8_TLS_USED_IN_LIBRARY\)\n",
                     "#if defined(COMPONENT_BUILD) || defined(V8_TLS_USED_IN_LIBRARY) || defined(__HAIKU__)\n"),
     "TLS library mode", "__HAIKU__", optional=True)

# Node 26: the Rust crates (Temporal) are a staticlib whose std needs
# Haiku's socket (libnetwork) and arc4random (libbsd) functions.
edit("deps/crates/crates.gyp",
     lambda s: resub(s, r"(\n(\s+)'conditions': \[\n\s+\['OS==\"win\"', \{\n\s+'libraries': \[\n\s+'-lntdll',\n\s+'-luserenv'\n\s+\],\n\s+\}\],\n)",
                     lambda m: m.group(1) + m.group(2) + "  ['OS==\"haiku\"', {\n" + m.group(2) + "    'libraries': [ '-lnetwork', '-lbsd' ],\n" + m.group(2) + "  }],\n"),
     "Rust std needs libnetwork/libbsd", "haiku", optional=True)

# Node 21 builds QUIC (OpenSSL 3.0.13+quic -> ngtcp2/nghttp3), whose gyp
# only includes <arpa/inet.h>/<netinet/in.h> on Linux; without them htonl()
# and friends (macros on Haiku) end up as undefined functions. Only needed,
# and only tested, where the QUIC deps are actually built (21 of our majors).
if _major == 21:
    edit("deps/ngtcp2/ngtcp2.gyp",
         lambda s: s.replace("['OS==\"linux\" or OS==\"android\"', {\n          'defines': [\n            'HAVE_ARPA_INET_H',",
                             "['OS==\"linux\" or OS==\"android\" or OS==\"haiku\"', {\n          'defines': [\n            'HAVE_ARPA_INET_H',"),
         "QUIC deps need arpa/inet.h", "haiku")

# One platform-haiku.cc serves V8 6.8 to 14.6; it sits next to this script.
backup(path("deps/v8/src/base/platform/platform-haiku.cc"))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "platform-haiku.cc"),
            path("deps/v8/src/base/platform/platform-haiku.cc"))

for m in manual:
    print("MANUAL:", m)
print("done, %d manual items" % len(manual))
sys.exit(1 if manual else 0)
