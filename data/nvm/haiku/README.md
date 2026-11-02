# nvm on Haiku

This nvm (v0.40.8 plus Haiku support) is loaded into every login shell
(Terminal, SSH) by `profile.d/nvm.sh`. Node.js versions are installed per
user into `$NVM_DIR`, `~/.nvm` unless set otherwise.

    nvm install 24        # newest 24.x that has a Haiku build
    nvm install 26
    nvm install 10        # any major from 10 to 26
    nvm alias default 24
    nvm use 26
    nvm ls-remote         # releases with Haiku binaries

## Where the binaries come from

nodejs.org does not publish Haiku builds. On Haiku, nvm reads the release
index, the `node-vX.Y.Z-haiku-x64.tar.xz` archives and their
`SHASUMS256.txt` from air/OS's mirror, the releases of
https://github.com/airos-works/node-haiku, which has the newest release of
every major from 10 to 26. Another mirror in the nodejs.org layout, served
over https or read locally, can be set with `NVM_HAIKU_MIRROR`:

    export NVM_HAIKU_MIRROR=https://example.org/node-haiku/dist
    export NVM_HAIKU_MIRROR=file:///boot/home/node-dist

Source tarballs still come from nodejs.org (`NVM_NODEJS_ORG_MIRROR`).

## Releases without a Haiku binary

If a version is not on the Haiku mirror (an older point release, or a release
newer than the mirror), `nvm ls-remote <version>` and `nvm install <version>`
fall back to the nodejs.org index, and nvm builds the release from source: it
ports the source tree with `haiku/port/port-haiku.py` and runs configure with
`--dest-os=haiku`. That needs

    pkgman install gcc binutils make python3.10 haiku_devel

(nvm names whatever is missing) and takes from 15 minutes (Node.js 10) to over
an hour (26, which also builds Rust code when rust_bin is installed) on 12 cores,
much longer on slower machines. `nvm install -s <version>` forces a source
build. The port covers Node.js 10 and later; it is not a fixed patch but a
set of edits anchored on the upstream sources, so new releases of a major
usually build unchanged (if one does not, nvm prints the edits that found no
anchor). Node.js 12 to 19 build with Python 3.10 or 3.11 only, and 10 and 11
need Python 2.7, which HaikuPorts no longer has: install their binaries.
