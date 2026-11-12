# The arm64 package repository

HaikuPorts publishes no arm64 repository: `https://eu.hpkg.haiku-os.org/haikuports/master`
lists only riscv64, x86_64 and x86_gcc2, and its `arm64/current` answers every
request with an empty listing. The only arm64 packages HaikuPorts has are the
bootstrap build packages the build system downloads
(`build/jam/repositories/HaikuPorts/arm64`, from `…/master/build-packages/packages/`),
and that location has no `repo` or `repo.sha256`, so it cannot be a repository.

Until November 2026 the arm64 images still named `…/haikuports/master/arm64/current`
in `system/settings/package-repositories/HaikuPorts`. HaikuDepot and pkgman
listed the packages of the image's cached package list, but every download
failed with "Resource not found", and every refresh with an I/O error
(airos-works/haiku#118: gcc on a Pi 400).

## What air/OS does

- **The repository.** airos-ci `images/package-repo.py` builds a repository of
  the bootstrap packages after each arm64 image build: gcc, binutils, make, m4,
  bison, flex, texinfo, python3.10, the `_devel` packages of the base libraries
  and the rest of the list, minus source packages, minus packages the image
  has its own build of (CI's icu74, ncurses6, freetype, libffi, noto, the
  newer ca_root_certificates), minus packages whose files an image package of
  another name has (curl: `airos_curl`), and minus packages whose
  requirements nothing meets (the bootstrap freetype_devel wants FreeType
  2.6.3). It keeps the name HaikuPorts, its identifier and the vendor
  "Haiku Project" (package_repo accepts one vendor per repository, so CI's own
  packages, vendor air/OS, are not in it). CI uploads it to the `repo-arm64`
  release of this repository, and airos-works/airos-website
  (`scripts/repo.py`) serves it at **https://airos.works/repo/arm64/**.
- **The images.** `src/data/repository_infos/haikuports_arm64` replaces the
  generic HaikuPorts template for arm64 (`build/jam/RepositoryRules` takes a
  `haikuports_<arch>` template where one exists), so the images' HaikuPorts
  entry names `https://airos.works/repo/arm64`. The image's cached list is
  still HaikuPorts' build-packages list; HaikuDepot's first start and pkgman's
  `install` refresh it from the new repository.
- **haiku_devel and binutils.** CI's arm64 images
  (`tools/airos/ci/UserBuildConfig`) have `haiku_devel` and `binutils`
  installed: x86_64 offers them among the Installer's optional packages, but
  the Pi and the Cubie start their image as it is, and the bootstrap gcc
  requires neither, so HaikuDepot would install a gcc that cannot assemble,
  link or find a header. They are system packages of the release, so System
  Updater keeps `haiku_devel` at the `haiku` package's version (it requires
  exactly that version, which is why it is not in the repository).
- **Older installs.** The `airos_release` package of arm64 releases carries
  `boot/post-install/airos_haikuports_arm64.sh`
  (`tools/airos/ci/post-install/`) as a post-install script, which runs after
  each update System Updater installs (at the restart). Where the HaikuPorts
  entry still names HaikuPorts' server it names air/OS's instead and deletes
  the cached list, so the next refresh fetches the new one.

## Building programs on an arm64 machine

```
pkgman install gcc make
```

(or gcc in HaikuDepot); `haiku_devel` and `binutils` are already installed,
and a machine that updates from an older release gets them with the update.
Not there yet: jam, mkdepend (so no
`makefile_engine`), autotools, pkg-config and perl; the bootstrap packages are
the only arm64 packages HaikuPorts has.

The bootstrap packages change when HaikuPorts rebootstraps arm64; the next
image build after `build/jam/repositories/HaikuPorts/arm64` follows publishes
the new set, and machines get it at their next refresh.
