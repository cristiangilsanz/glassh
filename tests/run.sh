#!/usr/bin/env bash
set -u

GLASSH="$(cd "$(dirname "${1:-./glassh}")" && pwd)/$(basename "${1:-./glassh}")"
WORK="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
export NO_COLOR=1

passed=0
failed=0

pass() {
    passed=$((passed + 1))
}

fail() {
    failed=$((failed + 1))
    printf 'FAIL %s\n' "$1"
    printf '  expected: %s\n' "$2"
    printf '  actual:   %s\n' "$3"
}

same() {
    local name="$1" expected="$2" line="$3"
    local actual
    actual="$("$GLASSH" -c "$line" 2>&1)"
    if [ "$actual" = "$expected" ]; then pass; else fail "$name" "$expected" "$actual"; fi
}

quiet() {
    local name="$1" expected="$2" line="$3"
    local actual
    actual="$("$GLASSH" -c "$line" 2>/dev/null)"
    if [ "$actual" = "$expected" ]; then pass; else fail "$name" "$expected" "$actual"; fi
}

status() {
    local name="$1" expected="$2" line="$3"
    "$GLASSH" -c "$line" >/dev/null 2>&1
    local actual=$?
    if [ "$actual" = "$expected" ]; then pass; else fail "$name" "status $expected" "status $actual"; fi
}

matches() {
    local name="$1" pattern="$2"
    shift 2
    local actual
    actual="$("$GLASSH" "$@" 2>&1)"
    if printf '%s\n' "$actual" | grep -Eq -- "$pattern"; then pass; else fail "$name" "/$pattern/" "$actual"; fi
}

same "runs a simple command" "hello world" "echo hello world"
same "keeps arguments apart" "a|b|c" "printf '%s|%s|%s\n' a b c"
same "connects a pipe" "HELLO" "echo hello | tr a-z A-Z"
same "connects long pipelines" "3" "printf 'c\nb\na\nb\n' | sort | uniq | cat | cat | wc -l | tr -d ' '"
same "redirects output" "saved" "echo saved > out.txt; cat out.txt"
same "appends output" "$(printf 'one\ntwo')" "echo one > log.txt; echo two >> log.txt; cat log.txt"
same "redirects input" "2" "printf 'x\ny\n' > in.txt; wc -l < in.txt | tr -d ' '"
same "redirects errors" "$(printf '1\n1')" "cat /missing 2> err.txt; echo \$?; grep -c 'No such file' err.txt"
same "accepts the original error syntax" "captured" "ls /missing !> err.txt && echo no || grep -q missing err.txt && echo captured"
same "merges errors into output" "1" "ls /missing > both.txt 2>&1; grep -c missing both.txt"
same "applies redirects inside pipelines" "b" "printf 'b\na\n' > in.txt; sort -r < in.txt | head -1"
same "runs sequences" "$(printf '1\n2')" "echo 1; echo 2"
same "short-circuits and" "" "false && echo no"
same "short-circuits or" "yes" "false || echo yes"
same "chains and with or" "fallback" "false && echo no || echo fallback"
same "reports the last status" "1 0" "false; a=\$?; true; echo \$a \$?"
same "returns the status of the last stage" "0" "false | true; echo \$?"
same "keeps single quotes literal" '$HOME *' "echo '\$HOME *'"
same "expands inside double quotes" "x=1 2" "export V='1 2'; echo \"x=\$V\""
same "expands braces" "ab" "export A=a; echo \${A}b"
same "drops empty expansions" "[]" "echo [\$UNDEFINED_GLASSH_VARIABLE]"
same "escapes characters" 'a b $c' 'echo a\ b \$c'
same "expands the home directory" "$HOME/x" "echo ~/x"
same "expands globs" "a.txt b.txt" "mkdir -p globs; cd globs; touch a.txt b.txt; echo *.txt"
same "keeps unmatched globs" "*.none" "echo *.none"
same "sets prefix assignments for one command" "1 []" "X=1 sh -c 'printf \"%s \" \$X'; echo [\$X]"
same "sets plain assignments" "kept" "Y=kept; echo \$Y"
same "ignores comments" "visible" "echo visible # hidden"
same "reports missing commands" "glassh: nope-glassh: command not found" "nope-glassh"
status "returns 127 for missing commands" 127 "nope-glassh"
status "returns 126 for non-executable files" 126 "touch plain; ./plain"
status "returns 2 for syntax errors" 2 "echo |"
same "explains unterminated quotes" "glassh: unterminated quote" "echo 'open"
same "rejects a leading pipe" "glassh: syntax error near |" "| ls"
same "rejects a dangling and" "glassh: syntax error near end of line" "ls &&"
same "rejects ambiguous redirects" "glassh: *.txt: ambiguous redirect" "touch a.txt b.txt; echo x > *.txt"
same "reports unreadable input" "glassh: none.txt: No such file or directory" "cat < none.txt"
status "exits with a code" 7 "exit 7"
status "exits with the last status" 1 "false; exit"
same "stops after exit" "" "exit 0; echo never"
same "changes directory" "$WORK/sub" "mkdir -p sub; cd sub; pwd"
same "goes back with cd -" "$WORK" "mkdir -p sub; cd sub; cd - > /dev/null; pwd"
same "reports bad directories" "glassh: cd: /missing-glassh: No such file or directory" "cd /missing-glassh"
same "exports variables to children" "shared" "export SHARED=shared; sh -c 'echo \$SHARED'"
same "unsets variables" "[]" "export GONE=1; unset GONE; echo [\$GONE]"
same "copies files" "$(printf '[OK] Copied in.txt to out.txt (6 B)\nsame')" "printf 'hello\n' > in.txt; copy in.txt out.txt; cmp -s in.txt out.txt && echo same"
same "refuses to copy a file onto itself" "[ERROR] in.txt and in.txt are the same file" "touch in.txt; copy in.txt in.txt"
same "reports missing copy sources" "[ERROR] cannot open none.txt: No such file or directory" "copy none.txt out.txt"
same "runs built-ins inside pipelines" "1" "help | grep -c 'copy SRC DST'"
quiet "runs jobs in the background" "done" "sleep 0.1 & wait; echo done"
matches "prints the background pid" '^\[1\] [0-9]+$' -c "sleep 0.1 &"
quiet "runs chains in the background" "$(printf 'a\nb')" "echo a > x.txt && echo b >> x.txt & wait; cat x.txt"
quiet "lists jobs" "[1]  Running   sleep 1" "sleep 1 & jobs 2>/dev/null"
quiet "toggles tracing" "trace is on" "trace on; trace"
matches "narrates fork and exec" 'fork\(\) = [0-9]+  →  echo hi' --trace -c "echo hi"
matches "narrates dup2 in the child" '╰─ [0-9]+ dup2\([0-9]+, 1\)' --trace -c "echo hi | cat"
matches "counts bytes through each pipe" '└─▶ 5 B' --trace -c "printf hello | cat"
matches "reports every process" '┌─ 3 processes in' --trace -c "printf hello | cat | cat"
matches "reports the exit status" '└─ status 1' --trace -c "true | false"
matches "names deadly signals" 'SIGPIPE' --trace -c "yes | head -1"
matches "narrates built-ins" 'cd is a built-in, it runs inside the shell' --trace -c "cd ."
quiet "keeps traced output intact" "$(seq 1 20000 | wc -c | tr -d ' ')" "trace on; seq 1 20000 | cat | wc -c | tr -d ' '"
matches "explains pipelines" 'pipeline of 2 commands' -c "explain 'ls | wc'"
matches "explains redirects" 'stdout >> log.txt' -c "explain 'echo hi >> log.txt'"
matches "explains conditions" 'if the previous failed' -c "explain 'false || true'"
matches "lists built-ins" 'explain LINE' -c "help"
same "reads scripts" "$(printf 'one\ntwo')" "printf 'echo one\necho two\n' > script.sh; $GLASSH script.sh"
same "reads standard input" "piped" "echo 'echo piped' | $GLASSH"
matches "prints the version" '^glassh [0-9]+\.[0-9]+\.[0-9]+$' --version

printf '\n%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
