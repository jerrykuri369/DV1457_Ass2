#!/usr/bin/env bash
# End-to-end tests for the math server, mirroring the grading scenarios.
#
#   tests/run_tests.sh
#
# Environment variables:
#   SEQ_DIR      folder with the sequential matrix_inverse.c and kmeans.c
#                (and optionally kmeans-data.txt) to compare against.
#                Default: the reconstructions in tests/reference.
#   EXTRA_FLAGS  extra compiler flags, e.g. -fsanitize=thread
#   LOAD         number of simultaneous clients in the load test (default 200;
#                muxscale uses 5x this)
#
# The project is copied to a temporary folder first, so your own
# computed_results and results folders are not touched.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SEQ_DIR=${SEQ_DIR:-$HERE/reference}
LOAD=${LOAD:-200}
WORK=$(mktemp -d)
PROJECT=$WORK/project
CLIENT=$PROJECT/client/client
RESULTS=$PROJECT/client/results
COMPUTED=$PROJECT/mathserver/computed_results
PORT=$((20000 + RANDOM % 20000))
PASSED=0
FAILED=0
SERVER_PID=

pass() { echo "PASS  $*"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }
check() { local name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# ---------------------------------------------------------------- setup
echo "== Build"
cp -r "$HERE/../project" "$PROJECT"
rm -f "$COMPUTED"/*.txt "$RESULTS"/*.txt
make -C "$PROJECT" clean >/dev/null
if make -C "$PROJECT" EXTRA_FLAGS="${EXTRA_FLAGS:-}" >"$WORK/build.log" 2>&1; then
    pass "make builds everything"
else
    fail "make builds everything"; cat "$WORK/build.log"; exit 1
fi
check "no compiler warnings" bash -c "! grep -qi warning '$WORK/build.log'"
check "object files in mathserver/objects" bash -c "ls '$PROJECT'/mathserver/objects/*.o >/dev/null"

mkdir -p "$WORK/seq" "$WORK/client_cwd"
gcc -w -O2 -o "$WORK/seq/matinv" "$SEQ_DIR/matrix_inverse.c"
gcc -w -O2 -o "$WORK/seq/kmeans" "$SEQ_DIR/kmeans.c"
if [ -f "$SEQ_DIR/kmeans-data.txt" ]; then
    cp "$SEQ_DIR/kmeans-data.txt" "$WORK/seq/"
else
    python3 "$HERE/gen_kmeans_data.py" "$WORK/seq/kmeans-data.txt"
fi
cp "$WORK/seq/kmeans-data.txt" "$WORK/client_cwd/"
(cd "$WORK/seq" && ./kmeans >/dev/null)
"$WORK/seq/matinv" -n 8 -I fast -P 1 >"$WORK/seq/fast8.txt"
"$WORK/seq/matinv" -n 64 -I rand -P 1 >"$WORK/seq/rand64.txt"

# ---------------------------------------------------------------- helpers
start_server() { # strategy
    PORT=$((PORT + 1))
    local log=$WORK/server-$1-$PORT.log # a new log for every start
    "$PROJECT/server" -p "$PORT" -s "$1" >"$log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 100); do
        grep -q "Listening for clients" "$log" && return 0
        sleep 0.1
    done
    cat "$log"
    return 1
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
}

client() { # commands... ; runs one client session, prints its output
    printf '%s\n' "$@" | (cd "$WORK/client_cwd" && "$CLIENT" -ip 127.0.0.1 -p "$PORT")
}

client_number() { sed -n 's/.*you are client \([0-9]*\).*/\1/p' "$1"; }

server_alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

# Children that have exited are reaped within moments by the SIGCHLD handler.
no_zombies() {
    for _ in $(seq 30); do
        [ "$(ps -o stat= --ppid "$SERVER_PID" 2>/dev/null | grep -c '^Z')" -eq 0 ] && return 0
        sleep 0.1
    done
    ps -o pid,stat,cmd --ppid "$SERVER_PID"
    return 1
}

# ---------------------------------------------------------------- options
echo "== Command-line options"
"$PROJECT/server" -h >/dev/null; check "server -h exits 0" test $? -eq 0
"$PROJECT/server" -x 2>/dev/null; check "server -x exits 3" test $? -eq 3
"$PROJECT/server" -s nope 2>/dev/null; check "server -s nope exits 3" test $? -eq 3
"$PROJECT/server" -p 2>/dev/null; check "server -p without a value exits 3" test $? -eq 3
"$PROJECT/server" -p 70000 2>/dev/null; check "server -p 70000 exits 3" test $? -eq 3
"$CLIENT" 2>/dev/null; check "client without options exits 3" test $? -eq 3

# ---------------------------------------------------------------- per strategy
for strategy in fork muxbasic muxscale; do
    echo "== Strategy: $strategy"
    if ! start_server "$strategy"; then
        fail "$strategy: server starts"; continue
    fi

    # 1. A whole session: correct results, unique names, errors handled.
    out=$WORK/session-$strategy.txt
    client "matinvpar -n 8 -I fast -P 1" \
           "matinvpar -n 64 -I rand -P 0" \
           "kmeanspar -f kmeans-data.txt -k 9" \
           "matinvpar -n 8 -I fast -P 1" \
           "bogus command" \
           "matinvpar -n 0" \
           "kmeanspar -f missing.txt" \
           "matinvpar -h" >"$out"
    c=$(client_number "$out")
    check "$strategy: three matinv + one kmeans solution received" \
        test "$(grep -c 'Received the solution' "$out")" -eq 4
    check "$strategy: matinv -n 8 fast identical to sequential" \
        cmp -s "$RESULTS/matinv_client${c}_soln1.txt" "$WORK/seq/fast8.txt"
    check "$strategy: matinv -n 64 rand identical to sequential (file always has the matrix)" \
        cmp -s "$RESULTS/matinv_client${c}_soln2.txt" "$WORK/seq/rand64.txt"
    check "$strategy: repeated command saved as soln3, not overwritten" \
        cmp -s "$RESULTS/matinv_client${c}_soln3.txt" "$WORK/seq/fast8.txt"
    check "$strategy: kmeans identical to sequential" \
        cmp -s "$RESULTS/kmeans_client${c}_soln1.txt" "$WORK/seq/kmeans-results.txt"
    check "$strategy: server keeps the same files in computed_results" \
        cmp -s "$COMPUTED/kmeans_client${c}_soln1.txt" "$RESULTS/kmeans_client${c}_soln1.txt"
    check "$strategy: unknown command reported" grep -q "Server error: Unknown command 'bogus'" "$out"
    check "$strategy: invalid size reported" grep -q "Invalid value '0' for -n" "$out"
    check "$strategy: missing data file handled on the client" grep -q "Cannot read the data file 'missing.txt'" "$out"
    check "$strategy: -h answered" grep -q "HELP: try matinvpar -u" "$out"

    # 2. Concurrency: a second client is served while a big job runs.
    client "matinvpar -n 1500 -I rand -P 0" >"$WORK/big-$strategy.txt" &
    big=$!
    sleep 0.5
    if [ "$strategy" = fork ]; then
        check "fork: one child process per connected client" test "$(pgrep -c -P "$SERVER_PID")" -ge 1
    else
        check "$strategy: still a single process while computing" test "$(pgrep -c -P "$SERVER_PID")" -eq 0
        check "$strategy: solver threads running inside it" test "$(ls /proc/"$SERVER_PID"/task | wc -l)" -gt 1
    fi
    client "matinvpar -n 8 -I fast -P 1" >"$WORK/small-$strategy.txt"
    check "$strategy: small job answered while the big one is still running" kill -0 "$big"
    check "$strategy: small job correct" grep -q "Received the solution" "$WORK/small-$strategy.txt"
    wait "$big"
    check "$strategy: big job finished too" grep -q "Received the solution" "$WORK/big-$strategy.txt"

    # 3. Load: many simultaneous clients.
    n=$LOAD
    [ "$strategy" = muxscale ] && n=$((LOAD * 5))
    pids=()
    for i in $(seq "$n"); do
        client "matinvpar -n 16 -I rand -P 0" >"$WORK/load-$strategy-$i.txt" 2>&1 &
        pids+=($!)
    done
    wait "${pids[@]}" # not a bare "wait": that would also wait for the server
    got=$(cat "$WORK"/load-"$strategy"-*.txt | grep -c "Received the solution")
    check "$strategy: $n simultaneous clients all answered ($got/$n)" test "$got" -eq "$n"
    if [[ "${EXTRA_FLAGS:-}" == *thread* && "$strategy" == fork ]]; then
        # ThreadSanitizer defers signal handlers until accept() returns, so
        # children are only reaped when the next client connects.
        echo "SKIP  fork: zombie check (not meaningful under ThreadSanitizer)"
    else
        check "$strategy: no zombie processes" no_zombies
    fi

    # 4. Misbehaving clients must not bring the server down.
    python3 - "$PORT" <<'EOF'
import socket, struct, sys, time
port = int(sys.argv[1])
def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.recv(64); return s
s = conn(); s.close()                                          # leaves at once
s = conn(); s.sendall(b"\x00\x00"); s.close()                  # half a header
s = conn(); s.sendall(struct.pack("!II", 2, 50_000_000)); time.sleep(0.2); s.close()  # huge command
s = conn(); s.sendall(struct.pack("!II", 99, 3) + b"abc"); time.sleep(0.2); s.close()  # wrong type
EOF
    client "matinvpar -n 1500 -I rand -P 0" >/dev/null &
    victim=$!
    sleep 0.3
    pkill -P "$victim" client 2>/dev/null; kill "$victim" 2>/dev/null
    sleep 0.2
    client "matinvpar -n 8 -I fast -P 1" >"$WORK/after-$strategy.txt"
    check "$strategy: survives broken and vanished clients" \
        bash -c "kill -0 $SERVER_PID && grep -q 'Received the solution' '$WORK/after-$strategy.txt'"

    stop_server
done

# ---------------------------------------------------------------- restart
echo "== Restart"
check "server starts again" start_server fork
client "matinvpar -n 8 -I fast -P 1" >"$WORK/restart.txt"
c=$(client_number "$WORK/restart.txt")
check "restarted server continues client numbering (client $c), nothing overwritten" \
    test "$(ls "$COMPUTED" | grep -c "_client${c}_")" -eq 1
stop_server

# ---------------------------------------------------------------- daemon
echo "== Daemon"
PORT=$((PORT + 1))
line=$("$PROJECT/server" -d -p "$PORT" -s muxscale)
pid=$(echo "$line" | sed -n 's/.*PID \([0-9]*\).*/\1/p')
check "daemon prints its PID" test -n "$pid"
sleep 0.3
check "daemon has no terminal" test "$(ps -o tty= -p "$pid" | tr -d ' ')" = "?"
check "daemon serves clients" bash -c "printf 'matinvpar -n 8 -I fast -P 1\n' | (cd '$WORK/client_cwd' && '$CLIENT' -ip 127.0.0.1 -p $PORT) | grep -q 'Received the solution'"
kill "$pid"
daemon_stopped() { # gone, or dead and waiting to be reaped by init
    for _ in $(seq 30); do
        state=$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')
        case "$state" in "" | Z*) return 0 ;; esac
        sleep 0.1
    done
    return 1
}
check "daemon stops with kill" daemon_stopped

# ---------------------------------------------------------------- network
echo "== Non-loopback address"
ip=$(hostname -I 2>/dev/null | awk '{print $1}')
if [ -n "$ip" ]; then
    start_server muxbasic
    out=$(printf 'matinvpar -n 8 -I fast -P 1\n' | (cd "$WORK/client_cwd" && "$CLIENT" -ip "$ip" -p "$PORT"))
    check "client reaches the server via $ip" grep -q "Received the solution" <<<"$out"
    stop_server
fi

# ---------------------------------------------------------------- sanitizer reports
if [ -n "${EXTRA_FLAGS:-}" ]; then
    check "no sanitizer reports in server logs" bash -c "! grep -l 'Sanitizer' '$WORK'/server-*.log"
fi

echo
echo "Passed: $PASSED   Failed: $FAILED"
[ "$FAILED" -eq 0 ]
