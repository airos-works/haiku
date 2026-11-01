#!/bin/bash -l
# Exercise the nvm commands users rely on, on Haiku (run after the majors
# are installed, in a login shell so the profile.d hook set nvm up).
# Prints PASS/FAIL per check.
# (no set -u: nvm itself is not nounset-safe)
fail=0
check() {
	local out
	if out=$(eval "$2" 2>&1); then
		echo "PASS $1: $(echo "$out" | tail -n 1)"
	else
		echo "FAIL $1: $(echo "$out" | tail -n 3 | tr '\n' ' ')"
		fail=1
	fi
}
check "nvm --version" 'nvm --version'
check "ls-remote lists the Haiku releases" 'nvm ls-remote --no-colors | grep -c "v[0-9]"'
check "ls-remote --lts" 'nvm ls-remote --lts --no-colors | grep "Latest LTS"'
check "version-remote 24" 'nvm version-remote 24'
check "ls-remote falls back to nodejs.org for 26.12" 'nvm ls-remote --no-colors 26.12 | tail -n 1'
check "ls" 'nvm ls --no-colors | grep -c "v[0-9]"'
check "use 22" 'nvm use 22 >/dev/null && node --version'
check "current" 'nvm use 20 >/dev/null && nvm current'
check "which 24" 'nvm which 24'
check "run 18" 'nvm run 18 -e "console.log(process.version)" | tail -n 1'
check "exec 10" 'nvm exec 10 node -e "console.log(process.version)" | tail -n 1'
check "alias default 26 (new login shell)" 'nvm alias default 26 >/dev/null && bash -lc "node --version" | tail -n 1'
check "global npm install under 26" 'nvm use 26 >/dev/null && npm install -g --no-audit --no-fund cowsay@1 >/dev/null && cowsay hi >/dev/null && npm ls -g --depth=0 | grep cowsay'
check "install by LTS name (lts/iron)" 'nvm install lts/iron 2>&1 | tail -n 1'
check "uninstall and reinstall 21" 'nvm use 20 >/dev/null && nvm uninstall 21 >/dev/null && nvm install 21 >/dev/null 2>&1 && nvm use 21 >/dev/null && node --version'
exit $fail
