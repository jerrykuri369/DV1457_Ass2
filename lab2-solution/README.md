# Lab 2 math server: reference solution

A complete solution to DV1457/DV1578 Lab 2 that targets **grade A**: fork, poll and epoll servers, daemon mode, and both math problems parallelised with pthreads. It is meant for **comparing with your own work**. The course requires the code you submit to be your own and that you can explain every part of it.

## What is implemented

| Requirement (handout section) | Where | Verified by |
|---|---|---|
| Folder layout, single `make` (1.1) | `project/`, `project/Makefile` | build test: zero warnings, objects in `mathserver/objects/` |
| Parallel matrix inversion (1.2.1) | `mathserver/src/matinv-par.c` | identical output to sequential for 40 size/init/maxnum combinations |
| Parallel k-means (1.2.2) | `mathserver/src/kmeans-par.c` | identical results file and console output to sequential |
| Concurrent clients (1.3) | all three strategies | small job answered while a 1500×1500 job runs |
| No zombies (1.4) | `strategy_fork.c` (SIGCHLD + `waitpid` loop) | 0 zombies after 200 simultaneous clients |
| Options `-h -p -d -s`, exit code 3 (1.5) | `mathserver/src/server.c` | `-x`, `-s nope`, `-p`, `-p 70000` all return 3 |
| `fork` strategy (grade D) | `strategy_fork.c` | 200 simultaneous clients |
| Daemon `-d` (grade C) | `server_util.c` → `daemonize()` | prints PID, no terminal, serves clients, stops with `kill` |
| `muxbasic` with `poll()` (grade B) | `strategy_muxbasic.c` + `mux_core.c` | 200 clients, one process |
| `muxscale` with `epoll()` (grade A) | `strategy_muxscale.c` + `mux_core.c` | 1000 simultaneous clients, one process |
| Results returned as files, never overwritten (FAQ A3, A9, A10) | `job.c`, `client.c` | repeated commands give soln1, soln2, soln3; restart continues numbering |

## Build and run

```sh
cd project
make                       # builds server, client/client, matinvpar, kmeanspar

./server -p 9999           # default strategy: fork
./server -p 9999 -s muxscale
./server -p 9999 -d        # daemon; prints its PID, stop it with: kill <PID>

cd client
./client -ip 127.0.0.1 -p 9999
Connected to server (you are client 1)
Enter a command for the server: matinvpar -n 8 -I fast -P 1
Received the solution: matinv_client1_soln1.txt
Enter a command for the server: kmeanspar -f kmeans-data.txt -k 9
Sent the data file: kmeans-data.txt (37446 bytes)
Received the solution: kmeans_client1_soln1.txt
```

Meanwhile the server prints:

```
Listening for clients...
Connected with client 1
Client 1 commanded: matinvpar -n 8 -I fast -P 1
Sending solution: matinv_client1_soln1.txt
Client 1 commanded: kmeanspar -f kmeans-data.txt -k 9
Received the data file from client 1 (37446 bytes)
Sending solution: kmeans_client1_soln1.txt
Client 1 disconnected
```

The server saves results in `mathserver/computed_results/`. The client saves received files in the `results/` folder next to its executable. The client ends on end of input, `quit`, or Ctrl+C.

**The client executable is `client/client`.** The handout puts executables in the project folder, but that folder already contains a directory named `client`, and a file cannot have the same name (`ld: cannot open output file client: Is a directory`). Check with your TA which name they want; it is a single variable in the Makefile.

## Design

**Protocol** (`protocol.h`). Every message is an 8-byte header (type and payload length, network byte order) followed by the payload. TCP is a byte stream, so the length tells the receiver where a message ends. A k-means command is answered with `NEED_DATA`, because the data file is on the client's machine (FAQ A7). The client then uploads it, or cancels if it cannot read the file. A result is sent as `RESULT "<name>\n<contents>"`.

**Matrix inversion.** The pivot loop has to run in order, but within one pivot step every other row can be eliminated independently. Each thread owns a block of rows. Thread 0 normalises the pivot row; then comes a barrier; then every thread eliminates its own rows; then a second barrier. Every element goes through the same operations in the same order as in the sequential program, so the output is identical.

**k-means.** Each iteration has two parallel phases, each ending at a barrier.
- **Assignment:** each thread finds the nearest centroid for its own slice of points.
- **Update:** each thread recomputes the centroids it owns, scanning the points in order. The float sums are therefore added in the sequential order, and the clusters come out identical.

Each value is written by only one thread, so no locks are needed.

**Threads** (`parallel.c`). Worker threads wait at a "gate" until all of them have been created. If `pthread_create` fails, none of them has started work yet, so the solver safely falls back to one thread. Solver state lives in per-job structures, and random numbers come from `random_r` with the sequential program's seed. The solvers are re-entrant, so the multiplexing servers can run several jobs at once.

**fork strategy.** The parent only accepts connections and forks; each child serves one client with blocking I/O. A SIGCHLD handler reaps all finished children (in a loop, since one signal can stand for several children). The handler uses `SA_RESTART`, so `accept()` resumes after it runs. Output is flushed before `fork()` so that buffered text is not printed twice.

**muxbasic / muxscale.** Both use the same core (`mux_core.c`) and differ only in how they wait: `poll()` scans the whole list every time, while `epoll` returns only the ready sockets.
- **Event loop.** Each client is a small state machine (`WAIT_COMMAND → WAIT_DATA → COMPUTING`) with non-blocking sockets and its own input and output buffers. The loop asks for write readiness only while output is pending.
- **Computations.** These would block the loop for seconds, so they go to a job queue served by a few worker threads. A finished job is handed back through a pipe that the loop watches like any socket. It is still one process: FAQ A8 says the strategy should not use processes, and threads are only used for the math.
- **Closed clients.** Their memory is freed only after the current batch of events, so a stale event can never touch freed memory.

**Daemon.** The port is opened before daemonising, so errors still reach the terminal. Then: `fork` (the parent prints the PID and exits), `setsid`, `umask`, `chdir("/")`, standard streams to `/dev/null`, and log messages go to syslog (`journalctl -t mathserver`). Every path the server uses is absolute: it is resolved from `/proc/self/exe`.

**File names.** Names follow the handout's pattern, `matinv_client1_soln2.txt`, with separate counters for each client and each problem type. On start-up the server continues numbering after the highest client number already in `computed_results`, so a restarted server never overwrites earlier files. The client also opens files with `O_EXCL` and adds `_2`, `_3`, … if a name is taken.

## Assumptions about the sequential programs: check these against Canvas

I did not have `matrix_inverse.c` and `kmeans.c` from Canvas. The output formats are rebuilt from the handout's sample output; for example, `-n 4 -I fast` gives `0.27 / -0.06`, exactly as in the handout. `tests/reference/` contains these reconstructions, used only for testing. If your Canvas files differ, everything that matters is in one place:

- **matrix report text and `" %5.2f"`:** `matinv_write_report()` in `matinv-par.c`
- **defaults (`-n 5`, `fast`, `maxnum 15`, `-P 1`) and the random fill (`rand() % maxnum + 5/+1`):** top of `matinv-par.c` and `init_matrices()`
- **k-means seed `srand(0)`, the `<=` tie rule, float sums, and the output `"%.2f %.2f %d"`:** `kmeans-par.c`

To compare against the real files, run `SEQ_DIR=/path/to/canvas/files tests/run_tests.sh`. That folder should contain `matrix_inverse.c`, `kmeans.c` and optionally `kmeans-data.txt`.

## Tests

`tests/run_tests.sh` copies the project to a temporary folder, builds it, and checks everything in the table above:
- all three strategies: correctness against the sequential programs, concurrency, process model and zombies;
- load: 200 clients for fork and poll, 1000 for epoll;
- broken and vanishing clients, restart, daemon, and a non-loopback IP address.

| Run | Result |
|---|---|
| normal build | 69 / 69 passed |
| AddressSanitizer + UndefinedBehaviorSanitizer (`EXTRA_FLAGS="-fsanitize=address,undefined -g"`) | 70 / 70 passed, no reports |
| ThreadSanitizer (`EXTRA_FLAGS="-fsanitize=thread -g"`) | 69 / 69 passed, no data races; the fork zombie check is skipped because ThreadSanitizer delays signal handlers until `accept()` returns |
| Valgrind (solvers, client, poll server) | no memory errors, no definite leaks |

Speed: a 1024×1024 inversion took 0.53 s instead of 1.42 s on 4 cores (2.7×).

## Advantages and drawbacks (for the presentation)

- **fork:** simplest code (blocking I/O), and a crash only affects one client. But it costs a whole process per client and has the highest memory use; it suits hundreds of clients.
- **poll:** one process and small per-client state, with no 1024-descriptor limit (unlike `select`). But every call passes and scans the whole list, so it costs O(n) per wake-up. It needs non-blocking I/O and a state machine.
- **epoll:** the kernel keeps the list and returns only ready sockets, so it scales to thousands of mostly idle clients. The code is about the same as poll. It is Linux-only (`kqueue` is the BSD equivalent).
- **Threads for the math, even with fork:** with one or two clients, the solver's threads are the only way to use all the cores (FAQ A6). The speedup is below the core count because of the barriers, memory bandwidth and the sequential pivot step (Amdahl's law).
