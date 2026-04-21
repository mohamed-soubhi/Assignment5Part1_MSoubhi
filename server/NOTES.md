# Assignment 5 Part 1 — aesdsocket Server

## Implementation Checklist

- [x] **Step 1** — Makefile with cross-compilation support
- [x] **Step 2** — Socket create, bind to port 9000, listen (return -1 on failure)
- [x] **Step 3** — Accept loop + syslog "Accepted/Closed connection from XXX"
- [x] **Step 4** — Receive data → append `/var/tmp/aesdsocketdata` on newline
- [x] **Step 5** — Send full file back to client after each complete packet
- [x] **Step 6** — Signal handler (SIGINT/SIGTERM) → syslog, cleanup, exit
- [x] **Step 7** — Daemon mode via `-d` argument (fork after bind)
- [x] **Step 8** — `sockettest.sh` verified on restart (clean state confirmed)
- [x] **Step 9** — `full-test.sh` passes: `Test of assignment assignment5 complete with success`
- [x] **Step 10** — Tagged `assignment-5-complete` and pushed

---

## What to Learn from Each Step

### Step 1 — Makefile / Cross-Compilation

`CC ?= gcc` allows overriding the compiler at call time:

```bash
make CC=arm-linux-gnueabihf-gcc
```

Cross-compilation means the same source compiles for a different target architecture (e.g. x86 host → ARM target) by swapping the toolchain binary. The `?=` operator is the idiomatic way to make a Makefile cross-compile-friendly without modifying it.

---

### Step 2 — Socket Create + Bind + Listen

```c
socket(AF_INET, SOCK_STREAM, 0)   // IPv4 TCP
setsockopt(..., SO_REUSEADDR, ...) // avoid "Address already in use"
bind(...)                          // assign port 9000
listen(...)                        // mark socket as passive
```

**Key concepts:**
- `AF_INET / SOCK_STREAM` = IPv4 TCP. Use `AF_INET6` for IPv6, `SOCK_DGRAM` for UDP.
- `SO_REUSEADDR` — prevents 60-second TIME_WAIT delay when you restart the server. Without it, `bind()` fails immediately after a crash or restart.
- `htons(PORT)` — converts host byte order to network byte order (big-endian). Always required for port numbers sent over the wire.

---

### Step 3 — Accept Loop + Syslog Logging

```c
accept(server_fd, &client_addr, &client_len)  // blocks until client connects
inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip))  // binary → string IP
syslog(LOG_INFO, "Accepted connection from %s", ip)
```

**Key concepts:**
- `accept()` blocks until a client connects and returns a *new* fd for that client. The original `server_fd` stays open for the next client.
- `inet_ntop()` converts binary `in_addr` to human-readable string (e.g. `"192.168.1.5"`). Preferred over deprecated `inet_ntoa()` (not thread-safe).
- `syslog()` writes to the system log (`/var/log/syslog` or `journalctl`). `openlog()` sets the ident tag (shown in log) and facility.

---

### Step 4 — Receive Data, Newline Packet Detection, File Append

```c
recv(fd, buf, sizeof(buf), 0)           // partial data is normal in TCP
realloc(packet, packet_len + n + 1)     // grow buffer dynamically
strchr(packet, '\n')                    // find packet boundary
fopen(DATA_FILE, "a") → fwrite(...)    // append complete packet to file
memmove(packet, packet + line_len, remaining)  // shift leftover bytes
```

**Key concepts:**
- TCP is a *stream* protocol — `recv()` can return partial data. You must accumulate bytes and scan for your application-level delimiter (here: `'\n'`).
- `realloc()` resizes heap memory. Always assign to a temp pointer first — if it fails it returns NULL and the original pointer is still valid (prevents memory leak).
- `memmove()` (not `memcpy`) is safe when source and destination overlap, which happens when shifting the remaining tail after consuming a packet.

---

### Step 5 — Send Full File Back After Each Packet

```c
f = fopen(DATA_FILE, "r");
while ((bytes = fread(send_buf, 1, sizeof(send_buf), f)) > 0)
    send(fd, send_buf, bytes, 0);
```

**Key concepts:**
- The file size is unbounded (spec says it may exceed available RAM), so never load the whole file into memory. Stream it in fixed-size chunks with `fread()` + `send()` in a loop.
- This is the standard pattern for sending large files over a socket without exhausting the heap.

---

### Step 6 — Signal Handling (SIGINT / SIGTERM)

```c
struct sigaction sa;
sa.sa_handler = signal_handler;
sigaction(SIGINT, &sa, NULL);
sigaction(SIGTERM, &sa, NULL);

static volatile sig_atomic_t g_running = 1;

void signal_handler(int signo) {
    syslog(LOG_INFO, "Caught signal, exiting");
    g_running = 0;
    shutdown(server_fd, SHUT_RDWR);  // wake blocked accept()
}
```

Cleanup in main after loop exits:

```c
close(server_fd);
remove(DATA_FILE);
closelog();
```

**Key concepts:**
- Use `sigaction()` not `signal()` — POSIX-portable, no undefined re-entry behavior.
- `volatile sig_atomic_t` is the only type guaranteed safe to read/write from both a signal handler and main code simultaneously.
- `shutdown(fd, SHUT_RDWR)` wakes a blocked `accept()` call (which would otherwise sleep forever), letting the main loop check `g_running` and exit cleanly. Setting a flag alone is not enough.
- `remove(DATA_FILE)` deletes `/var/tmp/aesdsocketdata` on exit as required by the spec.

---

## Files

| File | Purpose |
|------|---------|
| `aesdsocket.c` | Full server implementation |
| `Makefile` | Build system with cross-compilation support |

## Build

```bash
# Native build
make

# Cross-compile for ARM
make CC=arm-linux-gnueabihf-gcc

# Clean
make clean
```

## Quick Test

```bash
# Terminal 1 — run server
sudo ./aesdsocket

# Terminal 2 — send data
echo "hello world" | nc localhost 9000

# Check file
cat /var/tmp/aesdsocketdata
```

## Daemon Mode (-d flag)

Fork happens **after** successful `bind()` — parent sees bind errors before exiting.

```bash
# Start as daemon
sudo ./aesdsocket -d

# Verify running
ps aux | grep aesdsocket

# Stop
sudo pkill aesdsocket
```

| Daemonize step | Why |
|----------------|-----|
| `fork()` + parent `exit()` | Shell gets prompt back; child orphaned to init |
| `setsid()` | New session leader — detached from terminal, can't receive terminal signals |
| `dup2(..., /dev/null)` | Prevents accidental terminal read/write |
| `chdir("/")` | Stops daemon holding a mount point open |
| Fork **after** bind | Parent sees bind errors before exiting — no silent failures |

---

## Verification — sockettest.sh (Step 4)

Run each time server is closed and restarted to prove clean state on exit.

**Terminal 1:**
```bash
cd server && make && sudo ./aesdsocket
# Ctrl+C to stop → signal handler deletes /var/tmp/aesdsocketdata
# Restart: sudo ./aesdsocket
```

**Terminal 2:**
```bash
sudo bash assignment-autotest/test/assignment5/sockettest.sh
```

**Result:** `Tests complete with success!` on every run. ✓

**Why it works across restarts:** SIGINT triggers `remove(DATA_FILE)` in signal handler. Each fresh server start has no stale data — sockettest.sh assumes clean state, so it passes repeatably.

---

## Full Test Result

```bash
sudo bash full-test.sh
# → Test of assignment assignment5 complete with success
```

---

## Assignment Completion

Tagged and pushed:

```bash
git tag assignment-5-complete
git push && git push origin assignment-5-complete
```
