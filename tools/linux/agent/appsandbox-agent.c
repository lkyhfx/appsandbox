/*
 * appsandbox-agent.c — Linux guest agent for App Sandbox.
 *
 * Listens on AF_VSOCK port 1 for the host control channel. Speaks the
 * line-based text protocol from docs/linux-idd-implementation-plan.md
 * section 3 (Port 1 — agent). Compatible with the existing host code in
 * src/backend_win/vm_agent.c.
 *
 * Single-client at a time. On accept: send "hello", start a heartbeat
 * thread emitting "heartbeat" every 5s, run a command dispatch loop on
 * the main thread. On client disconnect, join heartbeat thread and accept
 * the next connection.
 *
 * Build:    gcc -O2 -Wall -Wextra -pthread -D_GNU_SOURCE \
 *               -o appsandbox-agent appsandbox-agent.c
 * Install:  /usr/local/bin/appsandbox-agent
 * Run as:   systemd system service (root).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <openssl/evp.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/vm_sockets.h>

/* ---- Constants ---- */

#define AGENT_PORT              1
#define HEARTBEAT_INTERVAL_SEC  5
#define LINE_BUF_MAX            4096
#define REPLY_MAX               256
#define GUEST_UPDATER_PATH      "/usr/local/libexec/appsandbox-guest-updater"
#define UPDATE_TXID_MAX         39
#define BOOTSTRAP_PORT          10
#define BOOTSTRAP_MAX_SIZE      (64ULL * 1024ULL * 1024ULL)
#define BOOTSTRAP_SERVICE_PATH  "/etc/systemd/system/appsandbox-guest-updater.service"
#define BOOTSTRAP_WATCH_PATH    "/etc/systemd/system/appsandbox-guest-update-watch.service"
#define DISPLAY_PROFILE_PATH    "/etc/appsandbox/display-profile"
#define DISPLAY_ENV_PATH        "/etc/appsandbox/display.env"
#define DISPLAY_MODE_PATH       "/etc/modprobe.d/zz-appsandbox-display-profile.conf"

static int read_active_display_profile(char *out, size_t cap);

#pragma pack(push, 1)
typedef struct BootstrapHeader {
    char magic[8];
    uint32_t protocol;
    uint32_t header_size;
    uint64_t payload_size;
    unsigned char sha256[32];
} BootstrapHeader;
typedef struct BootstrapPayloadHeader {
    uint64_t updater_size;
    uint64_t service_size;
    uint64_t watch_size;
} BootstrapPayloadHeader;
#pragma pack(pop)

/* ---- Global state for the currently-active client connection ---- */

static volatile sig_atomic_t g_stop      = 0;
static volatile int          g_client_fd = -1;
static pthread_mutex_t       g_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t       g_bootstrap_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int          g_bootstrap_armed = 0;
static uint64_t              g_bootstrap_size = 0;
static char                  g_bootstrap_sha[65] = {0};
static pthread_t              g_bootstrap_thread;
static volatile int           g_bootstrap_running = 0;

/* ---- Logging (timestamped, goes to stderr → systemd journal) ---- */

static void agent_log(const char *fmt, ...)
{
    char    ts[32];
    time_t  t = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- Line I/O ---- */

/* Send a line + '\n'. Thread-safe (heartbeat thread and command-reply
 * code share the write end of the same socket). */
static int send_line(int fd, const char *line)
{
    size_t  len = strlen(line);
    int     ok;
    ssize_t n1, n2;

    if (fd < 0) return -1;

    pthread_mutex_lock(&g_send_lock);
    n1 = write(fd, line, len);
    n2 = (n1 == (ssize_t)len) ? write(fd, "\n", 1) : -1;
    ok = (n1 == (ssize_t)len && n2 == 1);
    pthread_mutex_unlock(&g_send_lock);

    return ok ? 0 : -1;
}

/* Read one line (up to '\n') into buf. Strips '\r'. Null-terminates.
 * Returns: number of chars in buf, 0 on EOF, -1 on error. */
static int recv_line(int fd, char *buf, int max)
{
    int pos = 0;
    int overflow = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) { buf[pos] = '\0'; return 0; }       /* EOF */
        if (n < 0) {
            if (errno == EINTR) continue;
            buf[pos] = '\0';
            return -1;
        }
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    if (pos == max - 1) {
        char c;
        overflow = 1;
        while (read(fd, &c, 1) == 1 && c != '\n') { }
    }
    buf[pos] = '\0';
    return overflow ? -2 : pos;
}

static int recv_all_bytes(int fd, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void hex_digest(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}

static int bootstrap_digest(const void *data, size_t len, unsigned char out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int out_len = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, data, len) == 1 &&
             EVP_DigestFinal_ex(ctx, out, &out_len) == 1 && out_len == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int bootstrap_write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int bootstrap_fsync_parent(const char *path)
{
    char parent[512];
    char *slash;
    int fd;
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) return -1;
    slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = 0;
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fsync(fd) < 0 && errno != EINVAL) { close(fd); return -1; }
    close(fd);
    return 0;
}

static int install_bootstrap_file(const unsigned char *data, size_t len,
                                  const char *path, mode_t mode)
{
    char tmp[512];
    int fd = -1, ok = -1;
    if (snprintf(tmp, sizeof(tmp), "%s.bootstrap.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) return -1;
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    if (bootstrap_write_all(fd, data, len) == 0 && fchmod(fd, mode) == 0 && fsync(fd) == 0 &&
        close(fd) == 0 && rename(tmp, path) == 0 && bootstrap_fsync_parent(path) == 0) ok = 0;
    else if (fd >= 0) close(fd);
    if (ok < 0) unlink(tmp);
    return ok;
}

static int bootstrap_run_systemd(void)
{
    static char *const reload[] = { (char *)"/usr/bin/systemctl", (char *)"daemon-reload", NULL };
    static char *const enable[] = { (char *)"/usr/bin/systemctl", (char *)"enable", (char *)"--now",
                                    (char *)"appsandbox-guest-updater.service",
                                    (char *)"appsandbox-guest-update-watch.service", NULL };
    pid_t pid;
    int status = 0;
    char *const *commands[] = { reload, enable };
    size_t i;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) { execv(commands[i][0], (char *const *)commands[i]); _exit(127); }
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    }
    return 0;
}

static int bootstrap_receive(int fd)
{
    BootstrapHeader header;
    BootstrapPayloadHeader payload_header;
    unsigned char *payload = NULL, digest[32];
    uint64_t expected_size;
    char expected_sha[65];
    uint64_t total;
    size_t off;
    int ok = -1;
    pthread_mutex_lock(&g_bootstrap_lock);
    expected_size = g_bootstrap_size;
    snprintf(expected_sha, sizeof(expected_sha), "%s", g_bootstrap_sha);
    if (!g_bootstrap_armed) expected_size = 0;
    g_bootstrap_armed = 0;
    pthread_mutex_unlock(&g_bootstrap_lock);
    if (expected_size == 0 || expected_size > BOOTSTRAP_MAX_SIZE ||
        recv_all_bytes(fd, &header, sizeof(header)) < 0 ||
        memcmp(header.magic, "ASBBST1", 7) != 0 || header.protocol != 1 ||
        header.header_size != sizeof(header) || header.payload_size != expected_size) return -1;
    payload = malloc((size_t)header.payload_size);
    if (!payload || recv_all_bytes(fd, payload, (size_t)header.payload_size) < 0 ||
        bootstrap_digest(payload, (size_t)header.payload_size, digest) < 0) goto done;
    {
        char received_sha[65];
        hex_digest(digest, received_sha);
        if (strcmp(received_sha, expected_sha) != 0) goto done;
        if (memcmp(digest, header.sha256, sizeof(digest)) != 0) goto done;
    }
    if (header.payload_size < sizeof(payload_header)) goto done;
    memcpy(&payload_header, payload, sizeof(payload_header));
    total = sizeof(payload_header);
    if (payload_header.updater_size > BOOTSTRAP_MAX_SIZE ||
        payload_header.service_size > 1024 * 1024 || payload_header.watch_size > 1024 * 1024 ||
        payload_header.updater_size > UINT64_MAX - total) goto done;
    total += payload_header.updater_size;
    if (payload_header.service_size > UINT64_MAX - total) goto done;
    total += payload_header.service_size;
    if (payload_header.watch_size > UINT64_MAX - total) goto done;
    total += payload_header.watch_size;
    if (total != header.payload_size) goto done;
    off = sizeof(payload_header);
    if (install_bootstrap_file(payload + off, (size_t)payload_header.updater_size,
                               GUEST_UPDATER_PATH, 0755) < 0) goto done;
    off += (size_t)payload_header.updater_size;
    if (install_bootstrap_file(payload + off, (size_t)payload_header.service_size,
                               BOOTSTRAP_SERVICE_PATH, 0644) < 0) goto done;
    off += (size_t)payload_header.service_size;
    if (install_bootstrap_file(payload + off, (size_t)payload_header.watch_size,
                               BOOTSTRAP_WATCH_PATH, 0644) < 0) goto done;
    if (bootstrap_run_systemd() < 0) goto done;
    ok = 0;
done:
    free(payload);
    send_line(fd, ok == 0 ? "ok" : "error:bootstrap_failed");
    return ok;
}

static void *bootstrap_listener_proc(void *arg)
{
    int ls;
    struct sockaddr_vm addr = {0};
    (void)arg;
    ls = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0) return NULL;
    addr.svm_family = AF_VSOCK; addr.svm_cid = VMADDR_CID_ANY; addr.svm_port = BOOTSTRAP_PORT;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 1) < 0) {
        close(ls); return NULL;
    }
    while (g_bootstrap_running) {
        struct pollfd pfd = { ls, POLLIN, 0 };
        int r = poll(&pfd, 1, 1000);
        if (r <= 0) continue;
        if (pfd.revents & POLLIN) {
            int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) { bootstrap_receive(fd); close(fd); }
        }
    }
    close(ls);
    return NULL;
}

/* ---- Subprocess helpers ---- */

/* Run a shell command synchronously, return exit code (-1 on spawn fail). */
static int run_sync(const char *cmd)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Spawn a shell command, double-fork to detach it from us. Returns 0
 * if the intermediate fork succeeded (doesn't wait for the real child). */
static int spawn_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (setsid() < 0) _exit(127);
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);  /* reap intermediate */
    return 0;
}

/* ---- Clipboard helper spawn ----
 *
 * Mirrors the Windows-agent pattern (tools/agent/agent.c
 * clipboard_monitor_thread + spawn_clipboard_in_session): the agent
 * binds the privileged vsock ports as root, then forks+execs a
 * user-context helper with the fds inherited at 3/4 (LISTEN_FDS
 * convention). A background monitor thread polls every 3s for user
 * session presence and helper liveness and respawns as needed —
 * idd_connect alone is a one-shot trigger and doesn't survive helper
 * crashes / window-server restarts. */

static pid_t           g_clipboard_helper_pid = 0;
static pthread_mutex_t g_clipboard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_clipboard_monitor_thread;
static volatile int    g_clipboard_monitor_running = 0;

static int vsock_bind_listen_privileged(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("clipboard: bind vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("clipboard: listen vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    return s;
}

/* Find the active user's Mutter-XWayland auth cookie. Sets *out to a
 * NUL-terminated path on success, leaves it untouched otherwise.
 * Caller must already be in the user's uid context if checking
 * readability matters. */
static void find_mutter_xauth(uid_t uid, char *out, size_t cap)
{
    char xdg_rt[64];
    snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)uid);
    DIR *d = opendir(xdg_rt);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, ".mutter-Xwaylandauth.", 21) == 0) {
            snprintf(out, cap, "%s/%s", xdg_rt, de->d_name);
            break;
        }
    }
    closedir(d);
}

/* True if the UID looks like a regular human user we can spawn the
 * clipboard helper as — has a /home/... directory and a real login shell.
 *
 * Without this filter we'd accept system users like gdm (pw_dir=/var/lib/gdm3,
 * pw_shell=/usr/sbin/nologin). The greeter's own mutter creates a
 * /run/user/<gdm-uid>/.mutter-Xwaylandauth.* cookie for the login screen,
 * which would match the xauth probe. Spawning the clipboard helper as gdm
 * crashloops (exit 1) until the user actually logs in. The pw_dir + shell
 * checks below reject gdm regardless of its UID. */
static int uid_is_real_user(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (!pw || !pw->pw_dir || !pw->pw_shell) return 0;
    if (strncmp(pw->pw_dir, "/home/", 6) != 0) return 0;
    /* Reject nologin/false. Accept anything else as a real shell. */
    const char *shell = pw->pw_shell;
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if (strcmp(base, "nologin") == 0 || strcmp(base, "false") == 0)
        return 0;
    return 1;
}

/* Scan /run/user for the UID whose runtime dir has a Mutter XWayland auth
 * cookie AND who looks like a real user (not gdm). That's the user whose
 * graphical session we want to spawn the clipboard helper into. Returns
 * 0 (not found) or a valid uid_t.
 *
 * We can't hardcode a username — the create-VM modal lets the user pick
 * any account name. Looking for the live mutter xauth file is more robust
 * than parsing /etc/passwd because it implicitly filters for "user who
 * actually has a graphical session right now". */
static uid_t find_graphical_session_uid(void)
{
    DIR *d = opendir("/run/user");
    if (!d) return 0;
    struct dirent *de;
    uid_t found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char *end = NULL;
        unsigned long v = strtoul(de->d_name, &end, 10);
        if (!end || *end != '\0' || v < 1000 || v >= 65534) continue;
        if (!uid_is_real_user((uid_t)v)) continue;
        char xauth[512] = {0};
        find_mutter_xauth((uid_t)v, xauth, sizeof(xauth));
        if (xauth[0]) { found = (uid_t)v; break; }
    }
    closedir(d);
    return found;
}

/* User session is "ready" once Mutter has dropped its XWayland auth
 * cookie. Before that, xcb_connect would fail with the no-auth-protocol
 * error we saw on early spawns. */
static int is_user_session_ready(uid_t uid)
{
    char xauth[512] = {0};
    find_mutter_xauth(uid, xauth, sizeof(xauth));
    if (xauth[0] == '\0') return 0;
    struct stat st;
    return stat(xauth, &st) == 0 && S_ISREG(st.st_mode);
}

static void kill_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid <= 0) return;
    kill(g_clipboard_helper_pid, SIGTERM);
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) != 0) break;
        usleep(100 * 1000);
    }
    /* If still alive after 1s, SIGKILL. */
    if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) == 0) {
        kill(g_clipboard_helper_pid, SIGKILL);
        waitpid(g_clipboard_helper_pid, &status, 0);
    }
    g_clipboard_helper_pid = 0;
}

/* Spawn helper if no live one. Caller must hold g_clipboard_lock. */
static void spawn_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid > 0) {
        int status;
        pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
        if (r == 0) return;                  /* still running */
        g_clipboard_helper_pid = 0;
    }

    uid_t uid = find_graphical_session_uid();
    if (uid == 0) {
        /* No graphical session yet — retry on next monitor tick. */
        return;
    }
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        agent_log("clipboard: getpwuid(%u) failed: %s",
                  (unsigned)uid, strerror(errno));
        return;
    }

    if (!is_user_session_ready(pw->pw_uid)) {
        /* Will retry on the next monitor tick. */
        return;
    }

    int writer_fd = vsock_bind_listen_privileged(5);
    int reader_fd = vsock_bind_listen_privileged(6);
    if (writer_fd < 0 || reader_fd < 0) {
        if (writer_fd >= 0) close(writer_fd);
        if (reader_fd >= 0) close(reader_fd);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        agent_log("clipboard: fork failed: %s", strerror(errno));
        close(writer_fd); close(reader_fd);
        return;
    }

    if (pid == 0) {
        /* Child: prepare fds at 3 and 4 (systemd LISTEN_FDS convention). */
        if (dup2(writer_fd, 3) < 0 || dup2(reader_fd, 4) < 0) {
            fprintf(stderr, "clipboard child: dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (writer_fd != 3) close(writer_fd);
        if (reader_fd != 4) close(reader_fd);

        if (setgid(pw->pw_gid) < 0 ||
            initgroups(pw->pw_name, pw->pw_gid) < 0 ||
            setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "clipboard child: drop privs failed: %s\n",
                    strerror(errno));
            _exit(127);
        }

        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("DISPLAY", ":0", 1);

        char xdg_rt[64];
        snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)pw->pw_uid);
        setenv("XDG_RUNTIME_DIR", xdg_rt, 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

        char xauth[512] = {0};
        find_mutter_xauth(pw->pw_uid, xauth, sizeof(xauth));
        if (xauth[0] == '\0')
            snprintf(xauth, sizeof(xauth), "%s/.Xauthority", pw->pw_dir);
        setenv("XAUTHORITY", xauth, 1);

        char fds_str[16], pid_str[16];
        snprintf(fds_str, sizeof(fds_str), "%d", 2);
        snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
        setenv("LISTEN_FDS", fds_str, 1);
        setenv("LISTEN_PID", pid_str, 1);

        execl("/usr/local/bin/appsandbox-clipboard",
              "appsandbox-clipboard", (char *)NULL);
        fprintf(stderr, "clipboard child: execl failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(writer_fd);
    close(reader_fd);
    g_clipboard_helper_pid = pid;
    agent_log("clipboard helper spawned (pid=%d, uid=%u, xauth_ok=1)",
              (int)pid, (unsigned)pw->pw_uid);
}

/* Kill+respawn variant used from the idd_connect handler — guarantees a
 * fresh helper bound to a fresh socket so we don't accidentally serve
 * stale state when the host display reconnects. */
static void respawn_clipboard_helper(void)
{
    pthread_mutex_lock(&g_clipboard_lock);
    kill_clipboard_helper_locked();
    spawn_clipboard_helper_locked();
    pthread_mutex_unlock(&g_clipboard_lock);
}

/* Monitor thread: 3s polling, like tools/agent/agent.c
 * clipboard_monitor_thread. Respawns the helper if it died and spawns
 * one the first time the user session becomes ready. */
static void *clipboard_monitor_proc(void *arg)
{
    (void)arg;
    while (g_clipboard_monitor_running) {
        pthread_mutex_lock(&g_clipboard_lock);
        if (g_clipboard_helper_pid > 0) {
            int status;
            pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
            if (r > 0) {
                agent_log("clipboard helper exited (pid=%d, status=%d), "
                          "will respawn", (int)g_clipboard_helper_pid, status);
                g_clipboard_helper_pid = 0;
            }
        }
        if (g_clipboard_helper_pid == 0) {
            spawn_clipboard_helper_locked();
        }
        pthread_mutex_unlock(&g_clipboard_lock);

        for (int i = 0; i < 6 && g_clipboard_monitor_running; i++)
            usleep(500 * 1000);
    }
    return NULL;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = 1;
    if (pthread_create(&g_clipboard_monitor_thread, NULL,
                       clipboard_monitor_proc, NULL) != 0) {
        agent_log("clipboard: failed to start monitor thread: %s",
                  strerror(errno));
        g_clipboard_monitor_running = 0;
    }
}

/* ---- Reply helper: prepends the sequence tag if present ---- */

static void send_reply(int fd, const char *tag, const char *msg)
{
    if (tag && tag[0]) {
        char buf[REPLY_MAX];
        snprintf(buf, sizeof(buf), "%s%s", tag, msg);
        send_line(fd, buf);
    } else {
        send_line(fd, msg);
    }
}

/* ---- Fixed guest-update verbs ----------------------------------------- */

static int update_token(const char *s, size_t min_len, size_t max_len, int hex_only)
{
    size_t i, n;
    if (!s) return 0;
    n = strlen(s);
    if (n < min_len || n > max_len) return 0;
    for (i = 0; i < n; i++) {
        int ok = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
                 (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.';
        if (hex_only) ok = (s[i] >= '0' && s[i] <= '9') ||
                           (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F');
        if (!ok) return 0;
    }
    return 1;
}

static void handle_bootstrap_arm(int fd, const char *tag, const char *args)
{
    char copy[160], *save = NULL, *size_text, *sha, *extra;
    char *end;
    unsigned long long size;
    if (!args || strlen(args) >= sizeof(copy)) {
        send_reply(fd, tag, "error:bad_bootstrap_command");
        return;
    }
    strcpy(copy, args);
    size_text = strtok_r(copy, " \t", &save);
    sha = strtok_r(NULL, " \t", &save);
    extra = strtok_r(NULL, " \t", &save);
    if (!size_text || !sha || extra || !update_token(size_text, 1, 20, 0) ||
        !update_token(sha, 64, 64, 1)) {
        send_reply(fd, tag, "error:bad_bootstrap_command");
        return;
    }
    errno = 0;
    size = strtoull(size_text, &end, 10);
    if (errno || end == size_text || *end || size == 0 || size > BOOTSTRAP_MAX_SIZE) {
        send_reply(fd, tag, "error:bad_bootstrap_size");
        return;
    }
    pthread_mutex_lock(&g_bootstrap_lock);
    g_bootstrap_size = (uint64_t)size;
    snprintf(g_bootstrap_sha, sizeof(g_bootstrap_sha), "%s", sha);
    g_bootstrap_armed = 1;
    pthread_mutex_unlock(&g_bootstrap_lock);
    send_reply(fd, tag, "bootstrap_ready");
}

/* Execute only the installed updater with a fixed verb and already-validated
 * argv. The host cannot cause a shell command to run through this path. */
static int updater_exec(char *const argv[], char *output, size_t output_cap)
{
    int p[2], status;
    pid_t pid;
    size_t used = 0;
    if (!argv || !argv[0] || pipe(p) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]); close(p[1]);
        execv(GUEST_UPDATER_PATH, argv);
        _exit(127);
    }
    close(p[1]);
    if (output_cap) output[0] = '\0';
    while (output_cap > 1 && used + 1 < output_cap) {
        ssize_t n = read(p[0], output + used, output_cap - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(p[0]);
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    while (used && (output[used - 1] == '\n' || output[used - 1] == '\r' || output[used - 1] == ' '))
        output[--used] = '\0';
    return used ? 0 : -1;
}

static void send_guest_update_metadata(int fd)
{
    char version[96] = "unknown", graphics[96] = "unknown";
    FILE *f;
    if (access(GUEST_UPDATER_PATH, X_OK) != 0) {
        send_line(fd, "guest_caps:none");
        return;
    }
    f = fopen("/opt/appsandbox/guest/current/RELEASE", "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "version=", 8)) sscanf(line + 8, "%95[0-9A-Za-z.-]", version);
            else if (!strncmp(line, "graphics_version=", 17))
                sscanf(line + 17, "%95[0-9A-Za-z.-]", graphics);
        }
        fclose(f);
    }
    /* Graphics-only transactions keep the runtime RELEASE stable while the
     * Mesa A/B pointer moves. Prefer the active slot marker when present. */
    f = fopen("/opt/wsl-mesa/current/GRAPHICS", "r");
    if (f) {
        char line[128];
        if (fgets(line, sizeof(line), f))
            sscanf(line, "%95[0-9A-Za-z.+-]", graphics);
        fclose(f);
    }
    {
        char caps[256] = "guest_caps:update-v1,graphics-v1,health-v1,display-profile-v1,hevc444-v1";
        FILE *gate = fopen("/opt/wsl-mesa/current/BUILDINFO", "r");
        char gate_line[160];
        if (gate) {
            while (fgets(gate_line, sizeof(gate_line), gate))
                if (!strncmp(gate_line, "production-4k60: true", 21)) {
                    strcat(caps, ",production-4k60-v1");
                    break;
                }
            fclose(gate);
        }
        send_line(fd, caps);
    }
    {
        char line[128];
        snprintf(line, sizeof(line), "guest_version:%s", version);
        send_line(fd, line);
        snprintf(line, sizeof(line), "graphics_version:%s", graphics);
        send_line(fd, line);
        {
            char value[32];
            read_active_display_profile(value, sizeof(value));
            snprintf(line, sizeof(line), "display_profile:%s", value);
            send_line(fd, line);
        }
    }
}

static const char *display_profile_name(const char *value)
{
    if (!value) return NULL;
    if (!strcmp(value, "standard")) return "standard";
    if (!strcmp(value, "high_performance")) return "high_performance";
    return NULL;
}

static int read_display_profile(char *out, size_t cap)
{
    FILE *f;
    if (!out || cap < 32) return -1;
    snprintf(out, cap, "standard");
    f = fopen(DISPLAY_PROFILE_PATH, "r");
    if (!f) return 0;
    if (fscanf(f, "%31s", out) != 1 || !display_profile_name(out))
        snprintf(out, cap, "standard");
    fclose(f);
    return 0;
}

static int read_active_display_profile(char *out, size_t cap)
{
    FILE *width_file, *height_file;
    unsigned width = 0, height = 0;
    if (!out || cap < 32) return -1;
    snprintf(out, cap, "standard");
    width_file = fopen("/sys/module/asb_drm/parameters/width", "r");
    height_file = fopen("/sys/module/asb_drm/parameters/height", "r");
    if (width_file) {
        (void)fscanf(width_file, "%u", &width);
        fclose(width_file);
    }
    if (height_file) {
        (void)fscanf(height_file, "%u", &height);
        fclose(height_file);
    }
    if (width >= 3840 && height >= 2160)
        snprintf(out, cap, "high_performance");
    return 0;
}

static int write_display_profile(const char *profile)
{
    int fd;
    char env[96];
    char mode_config[128];
    const char *codec_mode = !strcmp(profile, "high_performance") ? "hevc444" : "hevc420";
    if (!display_profile_name(profile) ||
        (mkdir("/etc/appsandbox", 0755) < 0 && errno != EEXIST))
        return -1;
    fd = open(DISPLAY_PROFILE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    if (dprintf(fd, "%s\n", profile) < 0 || close(fd) < 0) return -1;
    fd = open(DISPLAY_ENV_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    snprintf(env, sizeof(env), "APPSANDBOX_DISPLAY_CODEC_MODE=%s\n", codec_mode);
    if (write(fd, env, strlen(env)) != (ssize_t)strlen(env) || close(fd) < 0) return -1;
    fd = open(DISPLAY_MODE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    snprintf(mode_config, sizeof(mode_config),
             "options asb_drm width=%s height=%s refresh=60\n",
             !strcmp(profile, "high_performance") ? "3840" : "1920",
             !strcmp(profile, "high_performance") ? "2160" : "1080");
    if (write(fd, mode_config, strlen(mode_config)) != (ssize_t)strlen(mode_config) ||
        close(fd) < 0) return -1;
    return 0;
}

static void handle_display_profile(int fd, const char *tag, const char *cmd)
{
    char current[32], active[32];
    const char *profile;
    if (!strcmp(cmd, "display_profile_get")) {
        char response[96];
        read_display_profile(current, sizeof(current));
        read_active_display_profile(active, sizeof(active));
        snprintf(response, sizeof(response), "profile=%s;reboot_required=%d",
                 active, strcmp(current, active) != 0 ? 1 : 0);
        send_reply(fd, tag, response);
        return;
    }
    if (strncmp(cmd, "display_profile_set ", 21) != 0 ||
        strlen(cmd + 21) >= sizeof(current) || strchr(cmd + 21, ' ') ||
        strchr(cmd + 21, '\t')) {
        send_reply(fd, tag, "error:bad_display_profile_command");
        return;
    }
    profile = display_profile_name(cmd + 21);
    if (!profile) {
        send_reply(fd, tag, "error:unsupported_display_profile");
        return;
    }
    read_display_profile(current, sizeof(current));
    read_active_display_profile(active, sizeof(active));
    if (!strcmp(current, profile) && !strcmp(active, profile)) {
        send_reply(fd, tag, !strcmp(profile, "high_performance")
            ? "profile=high_performance;reboot_required=0"
            : "profile=standard;reboot_required=0");
        return;
    }
    if (write_display_profile(profile) < 0) {
        send_reply(fd, tag, "error:display_profile_write_failed");
        return;
    }
    send_reply(fd, tag, !strcmp(profile, "high_performance")
        ? "profile=high_performance;reboot_required=1"
        : "profile=standard;reboot_required=1");
}

static void handle_guest_update(int fd, const char *tag, const char *cmd)
{
    char copy[LINE_BUF_MAX], *save = NULL, *verb, *a, *b, *c;
    char output[REPLY_MAX];
    char *argv[6] = { (char *)GUEST_UPDATER_PATH, NULL, NULL, NULL, NULL, NULL };
    if (strlen(cmd) >= sizeof(copy)) { send_reply(fd, tag, "error:command_too_long"); return; }
    strcpy(copy, cmd);
    verb = strtok_r(copy, " \t", &save);
    a = strtok_r(NULL, " \t", &save);
    b = strtok_r(NULL, " \t", &save);
    c = strtok_r(NULL, " \t", &save);
    if (strtok_r(NULL, " \t", &save) != NULL || !verb) {
        send_reply(fd, tag, "error:bad_update_command"); return;
    }
    if (!strcmp(verb, "update_query") && !a) {
        send_reply(fd, tag, access(GUEST_UPDATER_PATH, X_OK) == 0 ? "ok" : "error:unsupported");
        return;
    }
    if (!strcmp(verb, "update_health") && !a) {
        argv[1] = "--health";
    } else if (!strcmp(verb, "update_migrate") && !a) {
        argv[1] = "--migrate";
    } else if (!strcmp(verb, "update_begin") && a && b && c &&
        update_token(a, 8, UPDATE_TXID_MAX, 0) && update_token(b, 1, 12, 0) &&
        update_token(c, 64, 64, 1)) {
        argv[1] = "--begin"; argv[2] = a; argv[3] = b; argv[4] = c;
    } else if ((!strcmp(verb, "update_apply") || !strcmp(verb, "update_status") ||
                !strcmp(verb, "update_cancel")) && a && update_token(a, 8, UPDATE_TXID_MAX, 0)) {
        argv[1] = !strcmp(verb, "update_apply") ? "--submit" :
                  !strcmp(verb, "update_status") ? "--status" : "--cancel";
        argv[2] = a;
    } else if (!strcmp(verb, "update_rollback") && !a) {
        argv[1] = "--rollback";
    } else {
        send_reply(fd, tag, "error:bad_update_command"); return;
    }
    if (updater_exec(argv, output, sizeof(output)) < 0) {
        send_reply(fd, tag, "error:update_failed");
        return;
    }
    send_reply(fd, tag, output);
}

/* ---- Heartbeat thread ---- */

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    int idd_last = 0;   /* last reported display-driver readiness (so we only send on change) */
    while (!g_stop) {
        /* Sleep first so the very first heartbeat is at +5s, after hello. */
        for (int s = 0; s < HEARTBEAT_INTERVAL_SEC && !g_stop; s++)
            sleep(1);
        if (g_stop) break;
        int fd = g_client_fd;
        if (fd < 0) break;          /* client disconnected */
        if (send_line(fd, "heartbeat") < 0) {
            agent_log("heartbeat: send failed, exiting thread");
            break;
        }
        /* Report display readiness to the host (the same idd_status the Windows
         * agent sends). The display is serveable once the user session is up --
         * Mutter is compositing, so asb_drm has an active framebuffer for a host
         * consumer to capture. The host latches this as the display-open gate, so
         * it never has to probe the frame channel to find out. send_line is
         * mutex-locked, so this is safe alongside the command thread's replies. */
        {
            uid_t uid = find_graphical_session_uid();
            int ready = (uid != 0 && is_user_session_ready(uid));
            if (ready != idd_last) {
                send_line(fd, ready ? "idd_status:ok" : "idd_status:not_found");
                idd_last = ready;
            }
        }
    }
    return NULL;
}

/* ---- Command handlers ---- */

/* set_ip:<ip>/<prefix>:<gw>
 *   e.g. set_ip:192.168.42.2/24:192.168.42.1
 *
 * Mirrors the Windows agent's `netsh ip set static` clobber semantics:
 *   1. Disable cloud-init's network module so it stops re-writing
 *      /etc/netplan/50-cloud-init.yaml at next boot.
 *   2. Remove every other *.yaml in /etc/netplan so the merge has only
 *      our file to consider — no coexistence games.
 *   3. Write /etc/netplan/99-appsandbox.yaml with the host-assigned
 *      address + gateway + DNS (gateway primary, 8.8.8.8 fallback —
 *      same DNS layout the Windows agent uses).
 *   4. `netplan apply` then `systemctl restart systemd-networkd` so the
 *      kernel actually drops any stale addresses from a prior config.
 *
 * Reply: "ok" on success, "error:..." on failure. */
static void handle_set_ip(int fd, const char *tag, const char *args)
{
    char ip[64], prefix[8], gw[64];
    const char *slash  = strchr(args, '/');
    const char *colon2 = slash ? strchr(slash, ':') : NULL;
    size_t ip_len, pfx_len;
    char cmd[2400];
    int n, rc;

    if (!slash || !colon2) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    ip_len  = (size_t)(slash - args);
    pfx_len = (size_t)(colon2 - slash - 1);
    if (ip_len >= sizeof(ip) || pfx_len >= sizeof(prefix)) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    memcpy(ip,     args,        ip_len);  ip[ip_len]   = '\0';
    memcpy(prefix, slash + 1,   pfx_len); prefix[pfx_len] = '\0';
    snprintf(gw, sizeof(gw), "%s", colon2 + 1);

    n = snprintf(cmd, sizeof(cmd),
        /* Pick the renderer by probing whether the NetworkManager service
         * is active (Ubuntu Desktop); otherwise default to systemd-networkd
         * (Server). Writing a netplan with the wrong renderer makes the
         * active one stop managing the NIC entirely (which is why "no
         * network in Ubuntu Settings" happened). */
        "RENDERER=networkd; "
        "if systemctl is-active --quiet NetworkManager; then "
        "  RENDERER=NetworkManager; "
        "fi; "
        /* Disable cloud-init's network module so it stops re-writing
         * /etc/netplan/50-cloud-init.yaml on every boot. Idempotent. */
        "mkdir -p /etc/cloud/cloud.cfg.d && "
        "printf 'network: {config: disabled}\\n' "
        "  > /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg && "
        /* Remove every other netplan dropfile so merge can't reintroduce
         * a conflicting interface key (e.g. cloud-init's `enp0s5: dhcp4`
         * + our `appsbnic` both binding the same NIC). */
        "find /etc/netplan -maxdepth 1 -type f -name '*.yaml' "
        "  ! -name '99-appsandbox.yaml' -delete; "
        /* Our authoritative config. Heredoc is unquoted so $RENDERER
         * expands; nothing else in the YAML uses '$'. */
        "umask 077 && cat > /etc/netplan/99-appsandbox.yaml <<EOF\n"
        "network:\n"
        "  version: 2\n"
        "  renderer: $RENDERER\n"
        "  ethernets:\n"
        "    appsbnic:\n"
        "      match: { name: \"e*\" }\n"
        "      dhcp4: false\n"
        "      dhcp6: false\n"
        "      addresses: [\"%s/%s\"]\n"
        "      routes:\n"
        "        - to: default\n"
        "          via: %s\n"
        "      nameservers:\n"
        "        addresses: [%s, 8.8.8.8]\n"
        "EOF\n"
        /* Apply, then restart the renderer to drop stale leases / state
         * left behind by cloud-init or a previous run. netplan apply
         * alone is sometimes a no-op against a NIC that already has an
         * address the renderer hasn't released.
         *
         * IMPORTANT: reset umask BEFORE `netplan apply`. netplan's
         * systemd-networkd backend writes the generated
         * /run/systemd/network/10-netplan-*.network file via Python
         * open() with no explicit mode, so the file ends up at
         * (0666 & ~umask). With our 077 umask the generated file is 600
         * (root:systemd-network), and systemd-networkd — running as
         * `systemd-network` user, only in the systemd-network group —
         * gets "Permission denied" trying to read it, falls back to the
         * dracut catch-all `.network`, and eth0 ends up with no IP.
         * 022 → generated .network is 644 → systemd-network can read it. */
        "chmod 600 /etc/netplan/99-appsandbox.yaml && "
        "umask 022 && "
        "netplan apply 2>&1; "
        "if [ \"$RENDERER\" = NetworkManager ]; then "
        "  systemctl restart NetworkManager 2>&1; "
        "else "
        "  systemctl restart systemd-networkd 2>&1; "
        "fi; "
        /* Verify the address actually materialised. netplan apply / NM
         * reload are async — the host might try to SSH before the new
         * IP claims the wire. Poll for up to 5 seconds. */
        "for i in 1 2 3 4 5 6 7 8 9 10; do "
        "  ip -4 addr show | grep -q '%s/' && exit 0; "
        "  sleep 0.5; "
        "done; "
        "echo 'set_ip: address never appeared'; ip -4 addr show; exit 1",
        ip, prefix, gw, gw, ip);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        send_reply(fd, tag, "error:cmd_too_long");
        return;
    }
    rc = run_sync(cmd);
    agent_log("set_ip: %s/%s via %s → rc=%d", ip, prefix, gw, rc);
    send_reply(fd, tag, rc == 0 ? "ok" : "error:netplan_failed");
}

/* ---- SSH proxy: AF_VSOCK :7 ↔ localhost:22 ----
 *
 * Mirror of tools/agent/agent.c's ssh_proxy_thread + ssh_relay_thread.
 * The host's vm_ssh_proxy.c connects to AF_HYPERV service GUID :0007 —
 * hcs_service_guid(os_type, 7, ...) translates that to AF_VSOCK port 7
 * for Linux VMs — so we listen there and bridge each accepted vsock
 * connection to a fresh TCP socket on 127.0.0.1:22 (sshd).
 *
 * Port 7 is privileged (<1024); the agent runs as root so the bind
 * goes through without extra capability work. Per-connection relays
 * run as detached pthreads so we don't have to track them. */

#define SSH_RELAY_BUF   8192u
#define SSH_VSOCK_PORT  7u
#define SSH_TCP_PORT    22u

static pthread_t        g_ssh_proxy_pthread;
static volatile int     g_ssh_proxy_running = 0;

typedef struct {
    int vsock_fd;
    int tcp_fd;
} ssh_relay_ctx_t;

static int ssh_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *ssh_relay_proc(void *arg)
{
    ssh_relay_ctx_t *r = (ssh_relay_ctx_t *)arg;
    uint8_t buf[SSH_RELAY_BUF];
    struct pollfd pfd[2];

    pfd[0].fd = r->vsock_fd; pfd[0].events = POLLIN;
    pfd[1].fd = r->tcp_fd;   pfd[1].events = POLLIN;

    for (;;) {
        int n = poll(pfd, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfd[0].revents & POLLIN) {
            ssize_t m = recv(r->vsock_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->tcp_fd, buf, (size_t)m) < 0) break;
        }
        if (pfd[1].revents & POLLIN) {
            ssize_t m = recv(r->tcp_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->vsock_fd, buf, (size_t)m) < 0) break;
        }
        if ((pfd[0].revents | pfd[1].revents) & (POLLHUP | POLLERR | POLLNVAL))
            break;
    }
    close(r->vsock_fd);
    close(r->tcp_fd);
    free(r);
    return NULL;
}

static void *ssh_proxy_proc(void *arg)
{
    (void)arg;

    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) {
        agent_log("ssh proxy: socket(AF_VSOCK) failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = SSH_VSOCK_PORT;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("ssh proxy: bind vsock :%u failed: %s",
                  SSH_VSOCK_PORT, strerror(errno));
        close(s);
        return NULL;
    }
    if (listen(s, 4) < 0) {
        agent_log("ssh proxy: listen failed: %s", strerror(errno));
        close(s);
        return NULL;
    }
    agent_log("ssh proxy: listening on AF_VSOCK :%u", SSH_VSOCK_PORT);

    while (g_ssh_proxy_running) {
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("ssh proxy: accept failed: %s", strerror(errno));
            continue;
        }

        /* Per-connection TCP socket → localhost:22. Done eagerly so we
         * surface refused/timeout immediately rather than after one
         * round of relay polling. */
        int t = socket(AF_INET, SOCK_STREAM, 0);
        if (t < 0) { close(c); continue; }
        struct sockaddr_in ta = {0};
        ta.sin_family      = AF_INET;
        ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ta.sin_port        = htons(SSH_TCP_PORT);
        if (connect(t, (struct sockaddr *)&ta, sizeof(ta)) < 0) {
            agent_log("ssh proxy: connect 127.0.0.1:%u failed: %s",
                      SSH_TCP_PORT, strerror(errno));
            close(t); close(c);
            continue;
        }

        ssh_relay_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) { close(t); close(c); continue; }
        ctx->vsock_fd = c;
        ctx->tcp_fd   = t;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t rt;
        if (pthread_create(&rt, &attr, ssh_relay_proc, ctx) != 0) {
            agent_log("ssh proxy: relay pthread_create failed: %s",
                      strerror(errno));
            close(t); close(c); free(ctx);
        }
        pthread_attr_destroy(&attr);
    }

    close(s);
    agent_log("ssh proxy: stopped");
    return NULL;
}

static void start_ssh_proxy(void)
{
    if (g_ssh_proxy_running) return;
    g_ssh_proxy_running = 1;
    if (pthread_create(&g_ssh_proxy_pthread, NULL, ssh_proxy_proc, NULL) != 0) {
        agent_log("ssh proxy: pthread_create failed: %s", strerror(errno));
        g_ssh_proxy_running = 0;
    }
}

/* Find the primary interactive account (the one firstboot's useradd created):
 * a real /home user with a login shell. The username is user-chosen, so we scan
 * the passwd db rather than hardcode it. Returns 1 on success. */
static int find_primary_user(char *home, size_t hsz, uid_t *uid, gid_t *gid)
{
    struct passwd *pw;
    int found = 0;
    setpwent();
    while ((pw = getpwent())) {
        const char *sh, *b;
        if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;
        if (!pw->pw_dir || strncmp(pw->pw_dir, "/home/", 6) != 0) continue;
        sh = pw->pw_shell ? pw->pw_shell : "";
        b = strrchr(sh, '/'); b = b ? b + 1 : sh;
        if (strcmp(b, "nologin") == 0 || strcmp(b, "false") == 0) continue;
        snprintf(home, hsz, "%s", pw->pw_dir);
        *uid = pw->pw_uid; *gid = pw->pw_gid; found = 1;
        break;
    }
    endpwent();
    return found;
}

/* ssh_deploy_key: write the AppSandbox public key into the primary user's
 * ~/.ssh/authorized_keys (0700 dir, 0600 file, owned by the user). */
static int deploy_ssh_key(const char *pubkey)
{
    char home[256], sshdir[320], authk[400];
    uid_t uid; gid_t gid;
    FILE *f;
    if (!pubkey || !*pubkey) return 0;
    if (!find_primary_user(home, sizeof(home), &uid, &gid)) {
        agent_log("ssh key: no primary /home user found");
        return 0;
    }
    snprintf(sshdir, sizeof(sshdir), "%s/.ssh", home);
    snprintf(authk, sizeof(authk), "%s/authorized_keys", sshdir);
    mkdir(sshdir, 0700);
    f = fopen(authk, "w");
    if (!f) { agent_log("ssh key: cannot write %s: %s", authk, strerror(errno)); return 0; }
    fprintf(f, "%s\n", pubkey);
    fclose(f);
    chmod(sshdir, 0700);
    chmod(authk, 0600);
    if (chown(sshdir, uid, gid) != 0 || chown(authk, uid, gid) != 0)
        agent_log("ssh key: chown warning: %s", strerror(errno));
    agent_log("ssh key: deployed to %s", authk);
    return 1;
}

/* ssh_enable: enable+start openssh-server, then start the vsock:7 →
 * localhost:22 proxy. Reply: ssh_ready/ssh_failed. Idempotent — repeat
 * calls just confirm state. */
static void handle_ssh_enable(int fd, const char *tag)
{
    if (run_sync("systemctl is-active --quiet ssh") == 0) {
        agent_log("ssh: already running");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
        return;
    }
    int rc = run_sync("systemctl enable --now ssh 2>&1");
    if (rc == 0) {
        agent_log("ssh: enabled and started");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
    } else {
        agent_log("ssh: enable failed (rc=%d)", rc);
        send_reply(fd, tag, "ssh_failed");
    }
}

/* gpu_query_response:N — host sent N share descriptors after the header.
 *
 * Wire format per share line: "share_name|guest_path|filter\n" where
 *   share_name = "AppSandbox.HostLxssLib" or "AppSandbox.Drv.N"
 *   guest_path = Windows-format target path (we ignore it except to
 *                lift the leaf folder name for AppSandbox.Drv.N)
 *   filter     = ";"-separated filename whitelist (unused on Linux —
 *                we mount the whole share read-only instead of copying)
 *
 * For each share we open AF_VSOCK to (CID_HOST=2, port=50001), the HCS
 * Plan9 server, and hand the socket fd to the kernel's 9p driver via
 * mount(2) with "trans=fd,rfdno=N,wfdno=N,aname=<share_name>". The
 * kernel 9p client does Tversion + Tattach over the fd; userspace can
 * close its copy after mount() returns (kernel fget()'d the file).
 *
 * After all mounts succeed, write /etc/ld.so.conf.d/wsl.conf with one
 * line per mount path that contains .so files, run ldconfig, and send
 * gpu_copy_done back to the host (best-effort status reply — host
 * doesn't block on it).
 *
 * Mount paths follow the WSL convention:
 *   AppSandbox.HostLxssLib  → /usr/lib/wsl/lib
 *   AppSandbox.Drv.N        → /usr/lib/wsl/drivers/<windows-leaf>
 */

#define VSOCK_PLAN9_PORT  50001u
#define WSL_LIB_DIR       "/usr/lib/wsl/lib"
#define WSL_DRV_BASE      "/usr/lib/wsl/drivers"

/* Recursive mkdir like `mkdir -p`. Best-effort: ignores existing dirs
 * but reports if the final mkdir fails. */
static int mkpath_p(const char *path)
{
    char tmp[1024];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    /* Strip trailing slash (except root). */
    if (len > 1 && tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                agent_log("mkdir %s: %s", tmp, strerror(errno));
                /* keep going — parent might already exist */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Translate a share name (and the Windows guest_path we're handed) into
 * the Linux mount target. Returns 0 if the share is one we recognise,
 * -1 otherwise. */
static int derive_mount_target(const char *share_name,
                               const char *wpath,
                               char *out, size_t outsz)
{
    if (strcmp(share_name, "AppSandbox.HostLxssLib") == 0) {
        snprintf(out, outsz, "%s", WSL_LIB_DIR);
        return 0;
    }
    if (strncmp(share_name, "AppSandbox.Drv.", 15) == 0) {
        /* Extract the leaf folder from the Windows guest_path.
         * Tolerate either backslash or forward slash separators. */
        const char *bs = strrchr(wpath, '\\');
        const char *fs = strrchr(wpath, '/');
        const char *leaf = bs > fs ? bs : fs;
        leaf = leaf ? leaf + 1 : wpath;
        if (!*leaf) return -1;
        snprintf(out, outsz, "%s/%s", WSL_DRV_BASE, leaf);
        return 0;
    }
    return -1;
}

/* Mount one Plan9 share. Returns 0 on success, -1 otherwise. Logs the
 * outcome either way. */
static int mount_plan9_share(const char *share_name, const char *target)
{
    int v;
    struct sockaddr_vm sa = {0};
    char opts[512];

    /* mkdir -p target first — mount(2) requires it to exist. */
    if (mkpath_p(target) < 0) {
        agent_log("gpu_mount: mkdir(%s): %s", target, strerror(errno));
        return -1;
    }

    /* If already mounted (re-run on second agent connect), umount first
     * so the new mount replaces stale state. MNT_DETACH lets the kernel
     * drop refs lazily without blocking on outstanding accesses. */
    if (umount2(target, MNT_DETACH) == 0)
        agent_log("gpu_mount: lazy-unmounted stale %s", target);

    v = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (v < 0) {
        agent_log("gpu_mount: socket(AF_VSOCK): %s", strerror(errno));
        return -1;
    }
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_HOST;   /* 2 = host parent partition */
    sa.svm_port   = VSOCK_PLAN9_PORT;
    if (connect(v, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("gpu_mount: vsock connect (2,%u): %s",
                  VSOCK_PLAN9_PORT, strerror(errno));
        close(v);
        return -1;
    }

    snprintf(opts, sizeof(opts),
        "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,"
        "aname=%s,access=any,msize=65536,cache=loose",
        v, v, share_name);

    if (mount("none", target, "9p", MS_RDONLY | MS_NODEV | MS_NOSUID,
              opts) < 0) {
        agent_log("gpu_mount: mount %s -> %s: %s",
                  share_name, target, strerror(errno));
        close(v);
        return -1;
    }

    /* Kernel held its own reference via fget; we can drop ours. */
    close(v);
    agent_log("gpu_mount: %s -> %s OK", share_name, target);
    return 0;
}

/* Update /etc/ld.so.conf.d/wsl.conf to list every mount path that holds
 * .so files, then run ldconfig. Idempotent — overwrites each time. */
static void refresh_ldconfig_for_mounts(char (*paths)[1024], int n_paths)
{
    FILE *f = fopen("/etc/ld.so.conf.d/wsl.conf", "w");
    if (!f) {
        agent_log("ldconfig: open wsl.conf: %s", strerror(errno));
        return;
    }
    fprintf(f, "# Auto-generated by appsandbox-agent. Do not edit.\n");
    for (int i = 0; i < n_paths; i++)
        fprintf(f, "%s\n", paths[i]);
    fclose(f);

    int rc = run_sync("ldconfig 2>&1");
    agent_log("ldconfig refreshed (%d paths, rc=%d)", n_paths, rc);
}

static void handle_gpu_query_response(int fd, int n_shares)
{
    if (n_shares <= 0 || n_shares > 64) {
        agent_log("gpu_query: bad share count %d", n_shares);
        return;
    }

    char ldpaths[64][1024];
    int  n_ldpaths = 0;
    int  n_mounted = 0;

    for (int i = 0; i < n_shares; i++) {
        char line[LINE_BUF_MAX];
        int n = recv_line(fd, line, sizeof(line));
        if (n <= 0) {
            agent_log("gpu_query: short read on share %d", i);
            return;
        }
        agent_log("gpu_share[%d]: %s", i, line);

        /* Parse "share_name|guest_path|filter". filter may be empty. */
        char *p1 = strchr(line, '|');
        if (!p1) { agent_log("gpu_query: malformed: %s", line); continue; }
        *p1++ = '\0';
        char *p2 = strchr(p1, '|');
        if (p2) *p2++ = '\0';

        const char *share = line;        /* before first '|' */
        const char *wpath = p1;          /* between '|'s */
        (void)p2;                        /* filter — unused on Linux */

        char target[1024];
        if (derive_mount_target(share, wpath, target, sizeof(target)) < 0) {
            agent_log("gpu_query: unknown share name '%s', skipping", share);
            continue;
        }

        if (mount_plan9_share(share, target) < 0)
            continue;

        if (n_ldpaths < (int)(sizeof(ldpaths) / sizeof(ldpaths[0]))) {
            strncpy(ldpaths[n_ldpaths], target, sizeof(ldpaths[0]) - 1);
            ldpaths[n_ldpaths][sizeof(ldpaths[0]) - 1] = '\0';
            n_ldpaths++;
        }
        n_mounted++;
    }

    if (n_ldpaths > 0)
        refresh_ldconfig_for_mounts(ldpaths, n_ldpaths);

    /* Status reply to host. vm_agent.c just logs it; we send anyway so
     * the host log line "GPU copy complete for ..." fires and the user
     * sees confirmation that the share pipeline finished. */
    char ack[64];
    snprintf(ack, sizeof(ack), "gpu_copy_done:%d", n_mounted);
    send_line(g_client_fd, ack);
    agent_log("gpu_query: mounted %d/%d shares", n_mounted, n_shares);
}

/* ---- Per-client command dispatch ---- */

static void handle_client(int fd)
{
    pthread_t hb;
    int       hb_started = 0;
    char      line[LINE_BUF_MAX];

    g_client_fd = fd;
    agent_log("client connected (fd=%d)", fd);

    if (send_line(fd, "hello") < 0) {
        agent_log("hello: send failed");
        goto out;
    }
    /* Keep the first line exactly "hello" for old hosts. */
    send_guest_update_metadata(fd);

    if (pthread_create(&hb, NULL, heartbeat_thread, NULL) != 0) {
        agent_log("heartbeat: pthread_create failed: %s", strerror(errno));
    } else {
        hb_started = 1;
    }

    while (!g_stop) {
        int   n;
        char  tag[32] = "";
        const char *cmd;
        char *colon;

        n = recv_line(fd, line, sizeof(line));
        if (n == -2) {
            send_line(fd, "error:line_too_long");
            break;
        }
        if (n <= 0) break;

        /* Strip optional "<digits>:" sequence tag */
        cmd   = line;
        colon = strchr(line, ':');
        if (colon && colon > line && (size_t)(colon - line) < sizeof(tag) - 1) {
            int is_seq = 1;
            for (char *p = line; p < colon; p++) {
                if (*p < '0' || *p > '9') { is_seq = 0; break; }
            }
            if (is_seq) {
                size_t t = (size_t)(colon - line + 1);
                memcpy(tag, line, t);
                tag[t] = '\0';
                cmd = colon + 1;
            }
        }

        agent_log("cmd: %s", line);

        if (strcmp(cmd, "ping") == 0) {
            send_reply(fd, tag, "ok");
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("shutdown requested");
            spawn_detached("sleep 1 && systemctl poweroff");
        }
        else if (strcmp(cmd, "restart") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("restart requested");
            spawn_detached("sleep 1 && systemctl reboot");
        }
        else if (strncmp(cmd, "set_ip:", 7) == 0) {
            handle_set_ip(fd, tag, cmd + 7);
        }
        else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(fd, tag);
        }
        else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0) {
            send_reply(fd, tag, deploy_ssh_key(cmd + 15) ? "ssh_key_deployed" : "ssh_key_failed");
        }
        else if (strcmp(cmd, "idd_connect") == 0) {
            /* Mirror Windows handle_idd_connect (tools/agent/agent.c:1485):
             * kill+respawn the helper so a fresh CLDY handshake happens
             * for the new host display session. The monitor thread keeps
             * it alive after that. */
            respawn_clipboard_helper();
            send_reply(fd, tag, "ok");
        }
        else if (!strcmp(cmd, "display_profile_get") ||
                 !strncmp(cmd, "display_profile_set ", 21)) {
            handle_display_profile(fd, tag, cmd);
        }
        else if (strncmp(cmd, "gpu_query_response:", 19) == 0) {
            int n_shares = atoi(cmd + 19);
            handle_gpu_query_response(fd, n_shares);
            /* No reply — host sends this fire-and-forget */
        }
        else if (!strncmp(cmd, "bootstrap_updater ", 18)) {
            handle_bootstrap_arm(fd, tag, cmd + 18);
        }
        else if (!strncmp(cmd, "update_", 7)) {
            handle_guest_update(fd, tag, cmd);
        }
        else if (strcmp(cmd, "gpu_none") == 0) {
            /* Host reports no GPU-PV assignment — nothing to do */
        }
        else if (strcmp(cmd, "gpu_copy") == 0) {
            /* Host asks us to re-trigger the share enumeration.
             * Not implemented in v1. */
            send_reply(fd, tag, "error:not_implemented");
        }
        else {
            send_reply(fd, tag, "error:unknown");
        }
    }

out:
    agent_log("client disconnected");
    g_client_fd = -1;
    if (hb_started) {
        /* heartbeat thread checks g_client_fd and exits on its own */
        pthread_join(hb, NULL);
    }
}

/* ---- Listener setup ---- */

static int listen_vsock(void)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    struct sockaddr_vm addr;

    if (s < 0) {
        agent_log("socket(AF_VSOCK) failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid    = VMADDR_CID_ANY;
    addr.svm_port   = AGENT_PORT;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        agent_log("bind(vsock:%d) failed: %s", AGENT_PORT, strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("listen failed: %s", strerror(errno));
        close(s);
        return -1;
    }
    agent_log("listening on AF_VSOCK port %d", AGENT_PORT);
    return s;
}

/* ---- Signal handling ---- */

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    /* Best-effort: tell host we're stopping before systemd kills us.
     * Use raw write — async-signal-safe. */
    int fd = g_client_fd;
    if (fd >= 0) {
        static const char msg[] = "service_stopping\n";
        ssize_t w = write(fd, msg, sizeof(msg) - 1);
        (void)w;
    }
}

/* ---- Main ---- */

int main(int argc, char **argv)
{
    struct sigaction sa;
    int ls;

    (void)argc; (void)argv;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Don't die from SIGPIPE if peer goes away mid-write */
    signal(SIGPIPE, SIG_IGN);

    ls = listen_vsock();
    if (ls < 0) return 1;

    /* Legacy guests expose a fixed, one-purpose bootstrap channel.  It never
     * accepts a destination path or command from the host; the payload format
     * has exactly three files and installs only the pinned updater paths. */
    g_bootstrap_running = 1;
    if (pthread_create(&g_bootstrap_thread, NULL, bootstrap_listener_proc, NULL) != 0)
        g_bootstrap_running = 0;

    /* Start the clipboard monitor thread BEFORE accepting any control
     * clients. It'll spawn the helper as soon as the user session is
     * ready, independent of host-driven idd_connect. */
    start_clipboard_monitor();

    while (!g_stop) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("accept failed: %s", strerror(errno));
            break;
        }
        handle_client(c);
        close(c);
    }

    close(ls);
    g_bootstrap_running = 0;
    if (g_bootstrap_thread) pthread_join(g_bootstrap_thread, NULL);
    agent_log("agent stopped");
    return 0;
}
