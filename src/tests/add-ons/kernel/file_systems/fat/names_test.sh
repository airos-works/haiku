#!/bin/bash
# Long file names outside ASCII in the FAT driver, on the build host
# (airos-works/haiku#144): the kernel add-on had no UTF-16 <-> UTF-8
# conversion, so such names came out mangled and could not be opened.
#
#   names_test.sh <build directory>/objects/linux/<arch>/release/tools/fat_shell/fat_shell
#
# fat_shell (jam "<build>fat_shell") is the kernel add-on's code. The test
# reads a FAT32 image made by mtools, then writes names with fat_shell and
# checks the directory entries raw (fat_long_names.py) and with fsck.vfat.
# Needs dosfstools, mtools and python3.
set -u
FAT_SHELL=${1:?usage: names_test.sh <fat_shell>}
HERE=$(dirname "$(readlink -f "$0")")
W=$(mktemp -d "${TMPDIR:-/tmp}/fat-names.XXXXXX"); trap 'rm -rf "$W"' EXIT
failed=0
check() {	# what, value, expected
	if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: '$2', expected '$3'"; failed=1; fi
}
# (Overwriting a file leaves fat_shell's volume busy at the end, and the next
# mount complains about the FAT copies; that is older than this test and has
# nothing to do with names, so the checks look for their line among the output.)
fssh() {	# commands on stdin; the image stays mounted for all of them
	{ cat; echo sync; echo quit; } | "$FAT_SHELL" "$W/card.img" 2>&1 \
		| sed 's/^\(fssh:\/> \)*//' | grep -a -v '^fat\['
}

truncate -s 96M "$W/card.img" && mkfs.vfat -F 32 -n TESTCARD "$W/card.img" >/dev/null || exit 1
mkdir "$W/src"; cd "$W/src" || exit 1
names=('Straße 日本.jpg' 'Ärger.txt' 'Очень длинное имя файла.txt' 'Ελληνικά.txt'
	'ÄÖÜ.TXT' 'Holiday photo 13.jpg' 'README.TXT'
	'日本語のとても長いファイル名日本語のとても長いファイル名日本語のとても長いファイル名日本語のとても長いファイル名.txt')
for name in "${names[@]}"; do
	echo "content of $name" > "$name"; mcopy -i "$W/card.img" "$name" :: || exit 1
done
mmd -i "$W/card.img" ::/Bücher && mcopy -i "$W/card.img" README.TXT '::/Bücher/Über uns.txt' || exit 1
echo payload > "$W/payload"

echo "== reading what mtools wrote"
listing=$(echo "ls /myfs" | fssh)
for name in "${names[@]}" Bücher; do
	check "listed: $name" "$(grep -c -F -- " $name" <<<"$listing")" 1
done
for name in "${names[@]}"; do
	check "opened: $name" "$(echo "cat \"/myfs/$name\"" | fssh | grep -c -x -F -- "content of $name")" 1
done
check "in a directory" "$(echo 'cat "/myfs/Bücher/Über uns.txt"' | fssh | grep -c -x -F -- "content of README.TXT")" 1
# FAT names differ in more than case, in every script
check "other case, Latin" "$(echo 'cat "/myfs/STRAßE 日本.JPG"' | fssh | grep -c -x -F -- "content of Straße 日本.jpg")" 1
check "other case, Latin-1" "$(echo 'cat /myfs/ärger.TXT' | fssh | grep -c -x -F -- "content of Ärger.txt")" 1
check "other case, short name only" "$(echo 'cat /myfs/äöü.txt' | fssh | grep -c -x -F -- "content of ÄÖÜ.TXT")" 1
check "other case, Cyrillic" "$(echo 'cat "/myfs/ОЧЕНЬ ДЛИННОЕ имя файла.TXT"' | fssh | grep -c -x -F -- "content of Очень длинное имя файла.txt")" 1
check "other case, Greek" "$(echo 'cat /myfs/ΕΛΛΗΝΙΚΆ.txt' | fssh | grep -c -x -F -- "content of Ελληνικά.txt")" 1
check "a name that is not there" "$(echo 'cat "/myfs/Strasse 日本.jpg"' | fssh | grep -c "No such file")" 1

echo "== writing"
# 😀 is outside the BMP: two UTF-16 units; in the second name they are the
# last unit of the first slot and the first unit of the second one
fssh >/dev/null <<END
cp :$W/payload "/myfs/Grüße aus Köln.txt"
cp :$W/payload "/myfs/😀 photo.jpg"
cp :$W/payload "/myfs/abcdefghijkl😀mnopqrstuvwxyz.txt"
cp :$W/payload "/myfs/ÑANDÚ.TXT"
cp :$W/payload "/myfs/日本語.txt"
mkdir /myfs/Фото
cp :$W/payload "/myfs/Фото/Снимок 😀🎉.jpg"
cp :$W/payload /myfs/Привет.txt
mv /myfs/Привет.txt "/myfs/Привет мир.txt"
cp :$W/payload /myfs/ärger2.txt
cp :$W/src/README.TXT /myfs/ÄRGER2.TXT
END
raw=$(python3 "$HERE/fat_long_names.py" "$W/card.img"; python3 "$HERE/fat_long_names.py" "$W/card.img" Фото)
entry() { grep -c -F -- "$1" <<<"$raw"; }
check "on disk: Latin-1" "$(entry " 18 units - 'Grüße aus Köln.txt'")" 1
check "on disk: a surrogate pair" "$(entry " 12 units - '😀 photo.jpg'")" 1
check "on disk: a surrogate pair across two slots" "$(entry " 32 units - 'abcdefghijkl😀mnopqrstuvwxyz.txt'")" 1
check "on disk: short name in code page 850" "$(entry "ÑANDÚ.TXT ")" 1
check "on disk: Japanese" "$(entry "  7 units - '日本語.txt'")" 1
check "on disk: a directory" "$(entry "  4 units D 'Фото'")" 1
check "on disk: in the directory" "$(entry " 15 units - 'Снимок 😀🎉.jpg'")" 1
check "on disk: renamed" "$(entry " 14 units - 'Привет мир.txt'")" 1
check "on disk: the old name is gone" "$(entry "'Привет.txt'")" 0
check "a name in another case is the same file" "$(entry "rger2.txt'")$(entry "RGER2.TXT'")" 10
check "and got the new content" "$(echo 'cat /myfs/ärger2.txt' | fssh | grep -c -x -F -- "content of README.TXT")" 1
check "read back after a new mount" "$(echo 'cat "/myfs/фото/снимок 😀🎉.JPG"' | fssh | grep -c -x -F -- payload)" 1
check "listed with its character" "$(echo 'ls /myfs' | fssh | grep -c -F ' 😀 photo.jpg')" 1
check "a name that is not UTF-8 is refused" "$(printf 'cp :%s "/myfs/bad\xff\xfename.txt"\nls /myfs\n' "$W/payload" | fssh | grep -a -c 'name.txt$')" 0
check "fsck.vfat finds nothing" "$(fsck.vfat -n "$W/card.img" >"$W/fsck.log" 2>&1; echo $?)" 0

[[ $failed == 0 ]] && echo "PASS" || { echo "FAILED"; cat "$W/fsck.log" 2>/dev/null | tail -5; }
exit $failed
