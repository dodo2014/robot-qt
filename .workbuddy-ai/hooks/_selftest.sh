#!/usr/bin/env bash
# Self-test for the CreamPuffRobot project hooks.
#
# Usage:  bash .workbuddy-ai/hooks/_selftest.sh
# Exit 0 = all cases pass. Run this after ANY change to the hook scripts.
#
# NOTE: if you run this from a sandboxed agent shell, the suite may be killed
# partway through (the destructive-command section deliberately contains
# deletion patterns that sandboxes block). Run it from a normal terminal.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
H="$HERE/guard-git.sh"
D="$HERE/guard-dangerous-bash.sh"
E="$HERE/check-script-encoding.sh"

pass=0; fail=0
t() {
  local name="$1" expect="$2" json="$3" target="$4"
  local out; out="$(printf '%s' "$json" | bash "$target" 2>&1)"; local c=$?
  local got; if [ "$c" = "2" ]; then got="BLOCK"; else got="allow"; fi
  if [ "$got" = "$expect" ]; then pass=$((pass+1)); printf '  ok    %-6s %s\n' "$got" "$name"
  else fail=$((fail+1)); printf '  FAIL  got=%s want=%s  %s\n' "$got" "$expect" "$name"; fi
}
j() { printf '{"tool_input":{"command":"%s"}}' "$1"; }
jp() { printf '{"tool_input":{"file_path":"%s"}}' "$1"; }

echo "== guard-git.sh : bypass attempts (expect BLOCK) =="
t "plain commit"        BLOCK "$(j 'git commit -m x')" "$H"
t "git push"            BLOCK "$(j 'git push origin main')" "$H"
t "quoted"              BLOCK "$(j '\"git commit -m x\"')" "$H"
t "cd && commit"        BLOCK "$(j 'cd /d/repo && git commit -m x')" "$H"
t "cmd /c"              BLOCK "$(j 'cmd /c \"git commit -m x\"')" "$H"
t "bash -c"             BLOCK "$(j 'bash -c \"git commit -m x\"')" "$H"
t "eval"                BLOCK "$(j 'eval git commit -m x')" "$H"
t "pwsh -Command"       BLOCK "$(j 'pwsh -Command git commit -m x')" "$H"
t "newline concat"      BLOCK "$(j 'echo hi\ngit commit -m x')" "$H"
t "git -C path"         BLOCK "$(j 'git -C D:/repo commit -m x')" "$H"
t "commit --amend"      BLOCK "$(j 'git commit --amend')" "$H"
t "push --force"        BLOCK "$(j 'git push --force')" "$H"
t "extra spaces"        BLOCK "$(j 'git    commit   -m x')" "$H"
t "semicolon chain"     BLOCK "$(j 'ls; git commit -m x')" "$H"

echo "== guard-git.sh : false positives (expect allow) =="
t "git status"          allow "$(j 'git status')" "$H"
t "git diff"            allow "$(j 'git diff HEAD')" "$H"
t "git log"             allow "$(j 'git log --oneline -5')" "$H"
t "git commit-graph"    allow "$(j 'git commit-graph verify')" "$H"
t "git show"            allow "$(j 'git show HEAD')" "$H"
t "build bat"           allow "$(j './out/smoke/build_debug.bat')" "$H"
t "ls"                  allow "$(j 'ls -la')" "$H"
t "grep commit in src"  allow "$(j 'grep -rn \"commit\" src/')" "$H"
t "empty"               allow "$(j '')" "$H"

RM="r""m -rf "
echo "== guard-dangerous-bash.sh : destructive (expect BLOCK) =="
t "delete src tree"     BLOCK "$(j "${RM}src/")" "$D"
t "delete repo root"    BLOCK "$(j "${RM}.")" "$D"
t "delete .git"         BLOCK "$(j "${RM}.git")" "$D"
t "delete src/config"   BLOCK "$(j "${RM}src/config")" "$D"
t "reset --hard"        BLOCK "$(j 'git reset --hard HEAD~1')" "$D"
t "clean -fdx"          BLOCK "$(j 'git clean -fdx')" "$D"

echo "== guard-dangerous-bash.sh : safe ops (expect allow) =="
t "delete build output" allow "$(j "${RM}out/build/x64-Debug")" "$D"
t "delete a log file"   allow "$(j 'rm log/old.log')" "$D"
t "ninja build"         allow "$(j 'ninja -C out/build/x64-Debug CreamPuffRobot')" "$D"
t "tail log"            allow "$(j 'tail -20 log/creampuff.log')" "$D"

echo "== check-script-encoding.sh =="
t "good .bat"           allow "$(jp '/tmp/enc/good.bat')" "$E"
t "bad .bat (chinese)"  BLOCK "$(jp '/tmp/enc/bad.bat')" "$E"
t "good .ps1 (BOM)"     allow "$(jp '/tmp/enc/good.ps1')" "$E"
t "bad .ps1 (no BOM)"   BLOCK "$(jp '/tmp/enc/bad.ps1')" "$E"
t "non-script file"     allow "$(jp 'log/creampuff.log')" "$E"

echo
echo "RESULT: pass=$pass fail=$fail"
[ "$fail" = "0" ] || exit 1
