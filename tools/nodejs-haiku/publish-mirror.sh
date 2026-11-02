#!/bin/sh
# publish-mirror.sh <dist-dir> [owner/repo]
#
# Publish a Haiku Node.js mirror (the nodejs.org layout that build-node.sh and
# make-dist-index.py produce) as GitHub releases, which is where nvm's default
# NVM_HAIKU_MIRROR points (https://github.com/<repo>/releases/download):
# one release per version, tagged like its directory (v26.13.0) and holding
# that directory's files, and the release "index" holding index.tab and
# index.json. Existing releases get their files replaced. Needs gh, logged in
# with write access to the repository (default airos-works/node-haiku).
set -e
DIST=$(cd "$1" && pwd)
REPO=${2:-airos-works/node-haiku}

[ -f "$DIST/index.tab" ] || { echo "$DIST/index.tab missing: run make-dist-index.py $DIST"; exit 1; }

publish() {
	tag=$1 title=$2 notes=$3
	shift 3
	if gh release view "$tag" -R "$REPO" >/dev/null 2>&1; then
		gh release upload "$tag" -R "$REPO" --clobber "$@"
	else
		gh release create "$tag" -R "$REPO" --title "$title" --notes "$notes" --latest=false "$@"
	fi
	echo "$tag: $(echo "$@" | wc -w) files"
}

for dir in "$DIST"/v*/; do
	version=$(basename "$dir")
	publish "$version" "Node.js $version for Haiku (x86_64)" \
"Node.js $version built for Haiku x86_64 (air/OS) from the unmodified nodejs.org source tarball with the Haiku port in airos-works/haiku data/nvm/haiku/port; OpenSSL, ICU, libuv and the other dependencies are bundled. Install it with nvm on air/OS: \`nvm install ${version#v}\`." \
		"$dir"*.tar.xz "$dir"*.tar.gz "$dir"SHASUMS256.txt
done
publish index "Release index" \
"index.tab and index.json of the Haiku Node.js releases here, in the nodejs.org format; nvm reads index.tab." \
	"$DIST/index.tab" "$DIST/index.json"
