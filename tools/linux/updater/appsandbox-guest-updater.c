/*
 * appsandbox-guest-updater - independent Linux guest runtime updater.
 *
 * This binary is deliberately separate from appsandbox-agent.  The agent only
 * invokes the fixed verbs below; it never writes its own executable or parses
 * an archive.  The updater owns the update state, the binary VSOCK transport,
 * signature verification, safe extraction, activation, and rollback watchdog.
 *
 * Bundle format (the outer stream is a zstd-compressed POSIX tar archive):
 *   manifest.json
 *   manifest.sig                 detached Ed25519 signature, raw or hex
 *   payload/<manifest path>      versioned guest runtime
 *
 * The implementation intentionally uses execv/execl with fixed argv arrays.
 * No update input is ever passed through a shell.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define UPDATER_VERSION       "1.0.0"
#define UPDATE_SCHEMA         1
#define UPDATE_PROTOCOL       1
#define UPDATE_PORT            9
#define UPDATE_MAX_BUNDLE     (512ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_MANIFEST   (4ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_UNCOMPRESSED (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_FILE_SIZE  (512ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_FILES      4096
#define UPDATE_HEALTH_SECONDS 90

#define UPDATE_ROOT            "/opt/appsandbox/guest"
#define RELEASES_ROOT          UPDATE_ROOT "/releases"
#define STAGING_ROOT           UPDATE_ROOT "/.staging"
#define STATE_ROOT             "/var/lib/appsandbox-update"
#define STATE_FILE             STATE_ROOT "/state.json"
#define BUNDLE_ROOT            STATE_ROOT "/bundles"
#define UPDATER_PATH           "/usr/local/libexec/appsandbox-guest-updater"

/* Build systems must inject the release-signing public key.  There is
 * intentionally no checked-in fallback: a build without an explicit trust
 * anchor must fail rather than silently producing a test build. */
#ifndef ASB_UPDATE_PUBLIC_KEY_HEX
#error "ASB_UPDATE_PUBLIC_KEY_HEX must be supplied by the release build"
#endif

#pragma pack(push, 1)
typedef struct UpdateStreamHeader {
    char     magic[8];
    uint32_t protocol;
    uint32_t header_size;
    char     txid[40];
    uint64_t bundle_size;
    unsigned char sha256[32];
} UpdateStreamHeader;
#pragma pack(pop)

typedef struct JsonToken {
    int type;                 /* 1 object, 2 array, 3 string, 4 primitive */
    int start;
    int end;
    int parent;
} JsonToken;

typedef struct PayloadFile {
    char path[PATH_MAX];
    char sha256[65];
    char component[32];
    uint64_t size;
    unsigned mode;
} PayloadFile;

typedef struct Manifest {
    int schema;
    char version[96];
    char commit[128];
    char arch[32];
    char os[64];
    int host_protocol_min;
    int host_protocol_max;
    char updater_min_version[32];
    char graphics_version[96];
    int reboot_required;
    int allow_downgrade;
    int kernel_components_present;
    int file_count;
    PayloadFile files[UPDATE_MAX_FILES];
} Manifest;

typedef struct ExtractedFile {
    char path[PATH_MAX];
} ExtractedFile;

typedef struct UpdateState {
    char state[32];
    char txid[40];
    char version[96];
    char graphics_version[96];
    char sha256[65];
    char previous[PATH_MAX];
    char target[PATH_MAX];
    char old_runtime_target[PATH_MAX];
    char new_runtime_target[PATH_MAX];
    char old_graphics_target[PATH_MAX];
    char new_graphics_target[PATH_MAX];
    char old_config_backup[PATH_MAX];
    char error[160];
    uint64_t bundle_size;
    uint64_t deadline;
    int reboot_required;
    int runtime_changed;
    int graphics_changed;
} UpdateState;

static int write_all(int fd, const void *data, size_t len)
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

static int read_all(int fd, void *data, size_t len)
{
    unsigned char *p = (unsigned char *)data;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int skip_bytes(int fd, uint64_t len)
{
    unsigned char buf[512];
    while (len) {
        size_t n = len > sizeof(buf) ? sizeof(buf) : (size_t)len;
        if (read_all(fd, buf, n) < 0) return -1;
        len -= n;
    }
    return 0;
}

static int mkdir_one(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t i, n;
    if (!path || !path[0] || strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    n = strlen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir_one(tmp, mode) < 0) return -1;
        tmp[i] = '/';
    }
    return mkdir_one(tmp, mode);
}

static int fsync_parent(const char *path)
{
    char parent[PATH_MAX];
    char *slash;
    int fd;
    if (strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (!slash) return -1;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fsync(fd) < 0 && errno != EINVAL) { close(fd); return -1; }
    close(fd);
    return 0;
}

static int safe_component(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || n > NAME_MAX || (n == 1 && s[0] == '.') ||
        (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '\\' || c == ':') return 0;
    }
    return 1;
}

static int safe_relpath(const char *path)
{
    const char *p, *start;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\0' ||
        strlen(path) >= PATH_MAX) return 0;
    start = path;
    for (p = path;; p++) {
        if (*p != '/' && *p != '\0') continue;
        if (!safe_component(start, (size_t)(p - start))) return 0;
        if (*p == '\0') break;
        start = p + 1;
    }
    return 1;
}

static int join_path(char *out, size_t cap, const char *root, const char *rel)
{
    if (!safe_relpath(rel)) return -1;
    if (snprintf(out, cap, "%s/%s", root, rel) >= (int)cap) return -1;
    return 0;
}

static int append_path(char *out, size_t cap, const char *base, const char *suffix)
{
    size_t a = strlen(base), b = strlen(suffix);
    if (a >= cap || b >= cap - a || a + b + 1 > cap) return -1;
    memcpy(out, base, a);
    memcpy(out + a, suffix, b + 1);
    return 0;
}

static int join_rel_path(char *out, size_t cap, const char *base, const char *rel)
{
    size_t a = strlen(base), b = strlen(rel);
    if (!b || a >= cap || b >= cap - a || a + b + 2 > cap) return -1;
    memcpy(out, base, a);
    out[a] = '/';
    memcpy(out + a + 1, rel, b + 1);
    return 0;
}

static int ensure_parent(const char *root, const char *rel)
{
    char path[PATH_MAX], *slash;
    if (join_path(path, sizeof(path), root, rel) < 0) return -1;
    slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    *slash = '\0';
    return mkdir_p(path, 0700);
}

static int file_read_limited(const char *path, unsigned char **out, size_t *out_len,
                             size_t max_len)
{
    struct stat st;
    unsigned char *buf;
    size_t got = 0;
    int fd;
    if (stat(path, &st) < 0 || st.st_size < 0 || (uint64_t)st.st_size > max_len)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    buf = (unsigned char *)malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return -1; }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(buf); close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    *out = buf;
    *out_len = got;
    return 0;
}

static int atomic_write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    int fd, ok = -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp))
        return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    if (write_all(fd, data, len) == 0 && fsync(fd) == 0 && close(fd) == 0 &&
        rename(tmp, path) == 0 && fsync_parent(path) == 0)
        ok = 0;
    else close(fd);
    if (ok < 0) unlink(tmp);
    return ok;
}

/* ---- Small JSON tokenizer.  Manifests are generated by the release tool and
 * are parsed strictly enough to reject malformed or ambiguous metadata. ---- */

static int json_tokenize(const char *s, size_t len, JsonToken *t, int cap)
{
    int n = 0, top = -1, i = 0;
    int stack[128];
    while ((size_t)i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '{' || c == '[') {
            if (n >= cap || top + 1 >= (int)(sizeof(stack) / sizeof(stack[0]))) return -1;
            t[n] = (JsonToken){ c == '{' ? 1 : 2, i, -1, top };
            stack[++top] = n++;
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (top < 0 || (c == '}' && t[stack[top]].type != 1) ||
                (c == ']' && t[stack[top]].type != 2)) return -1;
            t[stack[top]].end = i + 1;
            top--;
            i++;
            continue;
        }
        if (c == '"') {
            int start = ++i;
            while ((size_t)i < len && s[i] != '"') {
                if ((unsigned char)s[i] < 0x20) return -1;
                if (s[i] == '\\') {
                    /* Escapes are allowed, but Unicode escapes are rejected
                     * below for security-sensitive path/version fields. */
                    i++;
                    if ((size_t)i >= len || (s[i] == 'u')) return -1;
                }
                i++;
            }
            if ((size_t)i >= len || n >= cap) return -1;
            t[n++] = (JsonToken){3, start, i, top};
            i++;
            continue;
        }
        if (strchr(":,", c)) { i++; continue; }
        {
            int start = i;
            while ((size_t)i < len && !strchr(" \t\r\n,]}", s[i])) i++;
            if (i == start || n >= cap) return -1;
            t[n++] = (JsonToken){4, start, i, top};
        }
    }
    return top == -1 ? n : -1;
}

static int token_equals(const char *s, const JsonToken *t, const char *value)
{
    size_t n = (size_t)(t->end - t->start);
    return t->type == 3 && strlen(value) == n && memcmp(s + t->start, value, n) == 0;
}

static int object_value(const char *s, const JsonToken *t, int nt, int object,
                        const char *key)
{
    int i;
    if (object < 0 || object >= nt || t[object].type != 1) return -1;
    for (i = object + 1; i + 1 < nt; i++) {
        if (t[i].parent != object || !token_equals(s, &t[i], key)) continue;
        if (t[i + 1].parent != object) return -1;
        return i + 1;
    }
    return -1;
}

static int token_copy(const char *s, const JsonToken *t, char *out, size_t cap)
{
    size_t n;
    if (!t || t->type != 3) return -1;
    n = (size_t)(t->end - t->start);
    if (!cap || n >= cap || memchr(s + t->start, '\\', n)) return -1;
    memcpy(out, s + t->start, n);
    out[n] = '\0';
    return 0;
}

static int token_u64(const char *s, const JsonToken *t, uint64_t *out)
{
    char buf[64], *end;
    unsigned long long v;
    size_t n;
    if (!t || (t->type != 3 && t->type != 4)) return -1;
    n = (size_t)(t->end - t->start);
    if (!n || n >= sizeof(buf)) return -1;
    memcpy(buf, s + t->start, n); buf[n] = 0;
    errno = 0; v = strtoull(buf, &end, 10);
    if (errno || end == buf || *end) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int token_bool(const char *s, const JsonToken *t, int *out)
{
    if (!t || t->type != 4) return -1;
    if (t->end - t->start == 4 && !memcmp(s + t->start, "true", 4)) *out = 1;
    else if (t->end - t->start == 5 && !memcmp(s + t->start, "false", 5)) *out = 0;
    else return -1;
    return 0;
}

static int parse_manifest(const unsigned char *data, size_t len, Manifest *m)
{
    JsonToken *t;
    int nt, root, v, i;
    uint64_t u;
    if (!data || !len || len > UPDATE_MAX_MANIFEST || !m) return -1;
    memset(m, 0, sizeof(*m));
    t = calloc(8192, sizeof(*t));
    if (!t) return -1;
    nt = json_tokenize((const char *)data, len, t, 8192);
    root = 0;
    if (nt <= 0 || t[root].type != 1 || t[root].start != 0 ||
        t[root].end != (int)len)
        goto fail;
    v = object_value((const char *)data, t, nt, root, "schema");
    if (v < 0 || token_u64((const char *)data, &t[v], &u) < 0 || u != UPDATE_SCHEMA) goto fail;
    m->schema = (int)u;
#define REQ_STR(name, field) do { \
        v = object_value((const char *)data, t, nt, root, name); \
        if (v < 0 || token_copy((const char *)data, &t[v], field, sizeof(field)) < 0) goto fail; \
    } while (0)
    REQ_STR("version", m->version);
    REQ_STR("commit", m->commit);
    REQ_STR("arch", m->arch);
    REQ_STR("os", m->os);
    REQ_STR("updater_min_version", m->updater_min_version);
    v = object_value((const char *)data, t, nt, root, "graphics_version");
    if (v >= 0 && token_copy((const char *)data, &t[v], m->graphics_version,
                              sizeof(m->graphics_version)) < 0) goto fail;
#undef REQ_STR
    v = object_value((const char *)data, t, nt, root, "host_protocol_min");
    if (v < 0 || token_u64((const char *)data, &t[v], &u) < 0 || u > 32) goto fail;
    m->host_protocol_min = (int)u;
    v = object_value((const char *)data, t, nt, root, "host_protocol_max");
    if (v < 0 || token_u64((const char *)data, &t[v], &u) < 0 || u > 32) goto fail;
    m->host_protocol_max = (int)u;
    v = object_value((const char *)data, t, nt, root, "reboot_required");
    if (v < 0 || token_bool((const char *)data, &t[v], &m->reboot_required) < 0) goto fail;
    v = object_value((const char *)data, t, nt, root, "allow_downgrade");
    if (v >= 0 && token_bool((const char *)data, &t[v], &m->allow_downgrade) < 0) goto fail;
    v = object_value((const char *)data, t, nt, root, "kernel_components_present");
    if (v >= 0 && token_bool((const char *)data, &t[v], &m->kernel_components_present) < 0) goto fail;
    v = object_value((const char *)data, t, nt, root, "files");
    if (v < 0 || t[v].type != 2) goto fail;
    for (i = v + 1; i < nt; i++) {
        int p, z, item = i;
        PayloadFile *f;
        if (t[i].parent != v) continue;
        if (t[i].type != 1 || m->file_count >= UPDATE_MAX_FILES) goto fail;
        f = &m->files[m->file_count++];
        p = object_value((const char *)data, t, nt, item, "path");
        z = object_value((const char *)data, t, nt, item, "sha256");
        if (p < 0 || z < 0 || token_copy((const char *)data, &t[p], f->path, sizeof(f->path)) < 0 ||
            token_copy((const char *)data, &t[z], f->sha256, sizeof(f->sha256)) < 0 ||
            safe_relpath(f->path) == 0 || strlen(f->sha256) != 64) goto fail;
        p = object_value((const char *)data, t, nt, item, "size");
        z = object_value((const char *)data, t, nt, item, "mode");
        if (p < 0 || z < 0 || token_u64((const char *)data, &t[p], &f->size) < 0 ||
            token_u64((const char *)data, &t[z], &u) < 0 || u > 07777) goto fail;
        f->mode = (unsigned)u;
        p = object_value((const char *)data, t, nt, item, "component");
        if (p < 0 || token_copy((const char *)data, &t[p], f->component, sizeof(f->component)) < 0)
            goto fail;
        for (z = 0; z < 64; z++) {
            int x;
            char c = f->sha256[z];
            if (!c) break;
            x = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!x) goto fail;
        }
    }
    free(t);
    return m->file_count > 0 ? 0 : -1;
fail:
    free(t);
    return -1;
}

static int valid_id(const char *s)
{
    size_t i, n;
    if (!s) return 0;
    n = strlen(s);
    if (n < 8 || n >= 40) return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

typedef struct ParsedVersion {
    unsigned long long major;
    unsigned long long minor;
    unsigned long long patch;
    char prerelease[64];
} ParsedVersion;

static int parse_version(const char *s, ParsedVersion *out)
{
    const char *p = s;
    char *end;
    unsigned long long *parts[3];
    int i;
    if (!s || !out || !*s || strlen(s) >= 96) return -1;
    memset(out, 0, sizeof(*out));
    parts[0] = &out->major; parts[1] = &out->minor; parts[2] = &out->patch;
    for (i = 0; i < 3; i++) {
        const char *start = p;
        if (*p < '0' || *p > '9') return -1;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') return -1;
        errno = 0;
        *parts[i] = strtoull(p, &end, 10);
        if (errno || end == p || (unsigned long long)*parts[i] > UINT_MAX) return -1;
        p = end;
        if (i < 2) {
            if (*p != '.') return -1;
            p++;
        }
        if (p == start) return -1;
    }
    if (*p == '-') {
        size_t n;
        const char *start = ++p;
        while (*p && *p != '+') {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= 'a' && *p <= 'z') || *p == '.' || *p == '-')) return -1;
            p++;
        }
        n = (size_t)(p - start);
        if (!n || n >= sizeof(out->prerelease)) return -1;
        memcpy(out->prerelease, start, n); out->prerelease[n] = 0;
    }
    if (*p == '+') {
        p++;
        if (!*p) return -1;
        while (*p) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= 'a' && *p <= 'z') || *p == '.' || *p == '-')) return -1;
            p++;
        }
    }
    return *p == 0 ? 0 : -1;
}

static int valid_version(const char *s)
{
    ParsedVersion v;
    return parse_version(s, &v) == 0;
}

static int prerelease_cmp(const char *a, const char *b)
{
    char ac[64], bc[64], *as, *bs, *ae, *be;
    if (!a[0] && !b[0]) return 0;
    if (!a[0]) return 1;
    if (!b[0]) return -1;
    snprintf(ac, sizeof(ac), "%s", a); snprintf(bc, sizeof(bc), "%s", b);
    as = ac; bs = bc;
    for (;;) {
        unsigned long long av, bv;
        int anumeric = 1, bnumeric = 1;
        ae = strchr(as, '.'); if (ae) *ae++ = 0;
        be = strchr(bs, '.'); if (be) *be++ = 0;
        for (char *p = as; *p; p++) if (*p < '0' || *p > '9') anumeric = 0;
        for (char *p = bs; *p; p++) if (*p < '0' || *p > '9') bnumeric = 0;
        if (anumeric && bnumeric) {
            errno = 0; av = strtoull(as, NULL, 10); if (errno) return 0;
            errno = 0; bv = strtoull(bs, NULL, 10); if (errno) return 0;
            if (av != bv) return av > bv ? 1 : -1;
        } else if (anumeric != bnumeric) {
            return anumeric ? -1 : 1;
        } else {
            int c = strcmp(as, bs); if (c) return c > 0 ? 1 : -1;
        }
        if (!ae || !be) {
            if (!ae && !be) return 0;
            return !ae ? -1 : 1;
        }
        as = ae; bs = be;
    }
}

static int version_cmp(const char *a, const char *b)
{
    ParsedVersion av, bv;
    int c;
    if (parse_version(a, &av) < 0 || parse_version(b, &bv) < 0) return 0;
    if (av.major != bv.major) return av.major > bv.major ? 1 : -1;
    if (av.minor != bv.minor) return av.minor > bv.minor ? 1 : -1;
    if (av.patch != bv.patch) return av.patch > bv.patch ? 1 : -1;
    c = prerelease_cmp(av.prerelease, bv.prerelease);
    return c;
}

static int hex_decode(const char *s, unsigned char *out, size_t out_len)
{
    size_t i;
    for (i = 0; i < out_len; i++) {
        int hi, lo;
        if (!s[2 * i] || !s[2 * i + 1]) return -1;
        hi = s[2 * i] >= '0' && s[2 * i] <= '9' ? s[2 * i] - '0' :
             s[2 * i] >= 'a' && s[2 * i] <= 'f' ? s[2 * i] - 'a' + 10 :
             s[2 * i] >= 'A' && s[2 * i] <= 'F' ? s[2 * i] - 'A' + 10 : -1;
        lo = s[2 * i + 1] >= '0' && s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' :
             s[2 * i + 1] >= 'a' && s[2 * i + 1] <= 'f' ? s[2 * i + 1] - 'a' + 10 :
             s[2 * i + 1] >= 'A' && s[2 * i + 1] <= 'F' ? s[2 * i + 1] - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return s[2 * out_len] == '\0' ? 0 : -1;
}

static void hex_encode(const unsigned char *data, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 15];
    }
    out[len * 2] = '\0';
}

static int public_key_is_valid(unsigned char key[32])
{
    static const unsigned char zero[32] = {0};
    /* RFC 8032 Ed25519 test vector 1.  It is intentionally rejected even
     * when a caller bypasses the Makefile validation. */
    static const unsigned char known_test[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a
    };
    /* Do not accept an all-zero key or a known public test vector as a
     * release trust anchor.  The Makefile performs the exact text-level
     * validation; this is the runtime defense-in-depth check. */
    return memcmp(key, zero, sizeof(zero)) != 0 &&
           memcmp(key, known_test, sizeof(known_test)) != 0;
}

static int verify_signature(const unsigned char *manifest, size_t manifest_len,
                            const unsigned char *sig_data, size_t sig_len)
{
    unsigned char public_key[32], signature[64];
    EVP_PKEY *key = NULL;
    EVP_MD_CTX *ctx = NULL;
    char sig_hex[129];
    int ok = -1;
    if (strlen(ASB_UPDATE_PUBLIC_KEY_HEX) != 64 ||
        hex_decode(ASB_UPDATE_PUBLIC_KEY_HEX, public_key, sizeof(public_key)) < 0 ||
        !public_key_is_valid(public_key)) return -1;
    if (sig_len == sizeof(signature)) memcpy(signature, sig_data, sizeof(signature));
    else {
        size_t i = 0;
        while (i < sig_len && (sig_data[i] == ' ' || sig_data[i] == '\n' || sig_data[i] == '\r' || sig_data[i] == '\t')) i++;
        if (sig_len - i >= sizeof(sig_hex)) return -1;
        memcpy(sig_hex, sig_data + i, sig_len - i); sig_hex[sig_len - i] = 0;
        while (sig_hex[0] && (sig_hex[strlen(sig_hex) - 1] == ' ' || sig_hex[strlen(sig_hex) - 1] == '\n' ||
                              sig_hex[strlen(sig_hex) - 1] == '\r' || sig_hex[strlen(sig_hex) - 1] == '\t'))
            sig_hex[strlen(sig_hex) - 1] = 0;
        if (strlen(sig_hex) != 128 || hex_decode(sig_hex, signature, sizeof(signature)) < 0) return -1;
    }
    key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, sizeof(public_key));
    ctx = key ? EVP_MD_CTX_new() : NULL;
    if (ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
        EVP_DigestVerify(ctx, signature, sizeof(signature), manifest, manifest_len) == 1)
        ok = 0;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

static int sha256_fd(int fd, uint64_t expected_size, unsigned char out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char buf[1024 * 1024];
    uint64_t total = 0;
    int ok = -1;
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    while (total < expected_size) {
        size_t want = (size_t)((expected_size - total) > sizeof(buf) ? sizeof(buf) : (expected_size - total));
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) goto done;
        total += (uint64_t)n;
    }
    if (total != expected_size || EVP_DigestFinal_ex(ctx, out, NULL) != 1) goto done;
    ok = 0;
done:
    EVP_MD_CTX_free(ctx);
    return ok;
}

static int sha256_file(const char *path, unsigned char out[32], uint64_t *size_out)
{
    struct stat st;
    int fd, ok;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > UPDATE_MAX_BUNDLE) return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ok = sha256_fd(fd, (uint64_t)st.st_size, out);
    close(fd);
    if (ok == 0 && size_out) *size_out = (uint64_t)st.st_size;
    return ok;
}

static int spawn_zstd_reader(const char *bundle, int *out_fd, pid_t *pid_out)
{
    int p[2];
    pid_t pid;
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        int fd = open(bundle, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) _exit(126);
        dup2(p[1], STDOUT_FILENO);
        close(p[0]); close(p[1]);
        /* zstd has no fd argument on all supported versions; use /proc/self/fd/3
         * so the pathname is not shell-interpreted and the open handle remains
         * stable if the bundle is renamed after validation. */
        if (dup2(fd, 3) < 0) _exit(126);
        close(fd);
        execlp("zstd", "zstd", "--quiet", "--decompress", "--stdout", "/proc/self/fd/3", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    *out_fd = p[0]; *pid_out = pid;
    return 0;
}

static int tar_octal(const unsigned char *p, size_t n, uint64_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > UINT64_MAX / 8) return -1;
        v = v * 8 + (unsigned)(p[i] - '0');
    }
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    if (i != n) return -1;
    *out = v;
    return 0;
}

static int tar_path(const unsigned char block[512], char *out, size_t cap)
{
    char name[101], prefix[156];
    size_t n = 0;
    memcpy(name, block, 100); name[100] = 0;
    memcpy(prefix, block + 345, 155); prefix[155] = 0;
    while (n < 100 && name[n]) n++;
    while (n && name[n - 1] == ' ') name[--n] = 0;
    if (prefix[0]) {
        size_t p = strnlen(prefix, sizeof(prefix));
        if (p + 1 + n >= cap) return -1;
        memcpy(out, prefix, p); out[p] = '/'; memcpy(out + p + 1, name, n + 1);
    } else {
        if (n >= cap) return -1;
        memcpy(out, name, n + 1);
    }
    while (out[0] == '.' && out[1] == '/') memmove(out, out + 2, strlen(out) - 1);
    return out[0] ? 0 : -1;
}

static int extracted_has(const ExtractedFile *files, int count, const char *path)
{
    int i;
    for (i = 0; i < count; i++) if (!strcmp(files[i].path, path)) return 1;
    return 0;
}

/* Extract only the payload and the two signed metadata files.  We do not use
 * tar's pathname handling here: every entry is validated and each payload
 * file is created with O_NOFOLLOW|O_EXCL below the private staging root. */
static int extract_bundle(const char *bundle, const char *stage,
                          unsigned char **manifest, size_t *manifest_len,
                          unsigned char **signature, size_t *signature_len,
                          ExtractedFile *extracted, int *extracted_count)
{
    int zfd = -1, status, done_blocks = 0, count = 0, fd = -1;
    pid_t zpid = 0;
    uint64_t uncompressed_total = 0;
    unsigned char block[512];
    char payload_root[PATH_MAX];
    if (snprintf(payload_root, sizeof(payload_root), "%s/payload", stage) >= (int)sizeof(payload_root)) return -1;
    if (mkdir_p(stage, 0700) < 0 || spawn_zstd_reader(bundle, &zfd, &zpid) < 0) return -1;
    for (;;) {
        char path[PATH_MAX], rel[PATH_MAX];
        uint64_t size, left;
        unsigned char type;
        if (read_all(zfd, block, sizeof(block)) < 0) goto fail;
        {
            int zero = 1; size_t i;
            for (i = 0; i < sizeof(block); i++) if (block[i]) { zero = 0; break; }
            if (zero) { if (++done_blocks == 2) break; continue; }
        }
        done_blocks = 0;
        type = block[156] ? block[156] : '0';
        if (tar_path(block, path, sizeof(path)) < 0 || tar_octal(block + 124, 12, &size) < 0 ||
            size > UPDATE_MAX_FILE_SIZE ||
            uncompressed_total > UPDATE_MAX_UNCOMPRESSED - size) goto fail;
        uncompressed_total += size;
        /* GNU tar commonly writes directory names with a trailing slash;
         * normalize that one harmless representation before applying the
         * strict relative-path checks. */
        if (type == '5' && path[0] && path[strlen(path) - 1] == '/')
            path[strlen(path) - 1] = '\0';
        if (!safe_relpath(path)) goto fail;
        if (type == '5') {
            if (size != 0) goto fail;
            if (!strcmp(path, "payload") || !strcmp(path, "payload/")) {
                if (mkdir_p(payload_root, 0700) < 0) goto fail;
            } else if (strncmp(path, "payload/", 8) == 0 && safe_relpath(path + 8)) {
                char dir[PATH_MAX];
                if (join_path(dir, sizeof(dir), stage, path + 8) < 0) goto fail;
                if (mkdir_p(dir, 0700) < 0) goto fail;
            } else goto fail;
        } else if (type == '0') {
            const char *target = NULL;
            if (!strcmp(path, "manifest.json")) {
                if (*manifest) goto fail;
                target = "manifest";
            } else if (!strcmp(path, "manifest.sig")) {
                if (*signature) goto fail;
                target = "signature";
            }
            else if (!strncmp(path, "payload/", 8) && safe_relpath(path + 8) && path[8]) {
                snprintf(rel, sizeof(rel), "%s", path + 8);
                if (extracted_has(extracted, count, rel) || count >= UPDATE_MAX_FILES) goto fail;
                if (ensure_parent(payload_root, rel) < 0) goto fail;
                if (join_path(path, sizeof(path), payload_root, rel) < 0) goto fail;
                fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0700);
                if (fd < 0) goto fail;
                left = size;
                while (left) {
                    unsigned char buf[65536];
                    size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
                    if (read_all(zfd, buf, want) < 0 || write_all(fd, buf, want) < 0) { close(fd); goto fail; }
                    left -= want;
                }
                if (fchmod(fd, 0700) < 0 || fsync(fd) < 0 || close(fd) < 0) goto fail;
                fd = -1;
                if (skip_bytes(zfd, (512 - (size % 512)) % 512) < 0) goto fail;
                memcpy(extracted[count].path, rel, strlen(rel) + 1);
                count++;
                continue;
            } else goto fail;
            if (size > UPDATE_MAX_MANIFEST && !strcmp(target, "manifest")) goto fail;
            if (size > 4096 && !strcmp(target, "signature")) goto fail;
            {
                unsigned char *buf = malloc((size_t)size + 1);
                if (!buf || read_all(zfd, buf, (size_t)size) < 0) { free(buf); goto fail; }
                buf[size] = 0;
                if (!strcmp(target, "manifest")) { free(*manifest); *manifest = buf; *manifest_len = (size_t)size; }
                else { free(*signature); *signature = buf; *signature_len = (size_t)size; }
                if (skip_bytes(zfd, (512 - (size % 512)) % 512) < 0) goto fail;
            }
        } else {
            /* Reject symlinks, hard links, pax/global headers, devices, and
             * every other tar extension.  Bundles contain ordinary files. */
            goto fail;
        }
    }
    close(zfd);
    if (waitpid(zpid, &status, 0) != zpid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !*manifest || !*signature || count == 0) return -1;
    *extracted_count = count;
    return 0;
fail:
    if (fd >= 0) close(fd);
    close(zfd);
    kill(zpid, SIGTERM);
    waitpid(zpid, NULL, 0);
    return -1;
}

static int hash_matches(const char *path, const PayloadFile *f)
{
    unsigned char digest[32];
    char hex[65];
    uint64_t size;
    size_t i;
    int fd;
    if (sha256_file(path, digest, &size) < 0 || size != f->size) return 0;
    for (i = 0; i < sizeof(digest); i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = 0;
    if (strcasecmp(hex, f->sha256) != 0) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fchmod(fd, f->mode & 07777) < 0 || fsync(fd) < 0) {
        if (fd >= 0) close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static int read_os_tuple(char *out, size_t cap)
{
    unsigned char *data = NULL;
    size_t len = 0, i;
    char id[32] = {0}, version[32] = {0};
    if (file_read_limited("/etc/os-release", &data, &len, 65536) < 0) return -1;
    for (i = 0; i < len;) {
        size_t start = i, end;
        while (i < len && data[i] != '\n') i++;
        end = i;
        if (end > start && data[end - 1] == '\r') end--;
        if (end - start > 3 && !memcmp(data + start, "ID=", 3))
            snprintf(id, sizeof(id), "%.*s", (int)(end - start - 3), data + start + 3);
        if (end - start > 11 && !memcmp(data + start, "VERSION_ID=", 11))
            snprintf(version, sizeof(version), "%.*s", (int)(end - start - 11), data + start + 11);
        i++;
    }
    free(data);
    if (id[0] == '"') memmove(id, id + 1, strlen(id));
    if (version[0] == '"') memmove(version, version + 1, strlen(version));
    if (id[strlen(id) - (id[0] ? 1 : 0)] == '"') id[strlen(id) - 1] = 0;
    if (version[strlen(version) - (version[0] ? 1 : 0)] == '"') version[strlen(version) - 1] = 0;
    return snprintf(out, cap, "%s-%s", id, version) < (int)cap ? 0 : -1;
}

static int verify_payload(const Manifest *m, const char *stage,
                           const ExtractedFile *extracted, int extracted_count)
{
    int i, j;
    uint64_t declared_total = 0;
    char path[PATH_MAX], payload_root[PATH_MAX];
    if (snprintf(payload_root, sizeof(payload_root), "%s/payload", stage) >= (int)sizeof(payload_root)) return -1;
    if (!valid_version(m->version) || strcmp(m->arch, "amd64") != 0 ||
        strcmp(m->os, "ubuntu-26.04") != 0 || m->host_protocol_min > UPDATE_PROTOCOL ||
        m->host_protocol_max < UPDATE_PROTOCOL || version_cmp(m->updater_min_version, UPDATER_VERSION) > 0)
        return -1;
    for (i = 0; i < m->file_count; i++) {
        const PayloadFile *f = &m->files[i];
        if (f->size > UPDATE_MAX_FILE_SIZE || declared_total > UPDATE_MAX_UNCOMPRESSED - f->size)
            return -1;
        declared_total += f->size;
        if (strstr(f->path, "..") || !strcmp(f->component, "kernel") ||
            strstr(f->path, "dxgkrnl") || strstr(f->path, "asb_drm.ko") ||
            join_path(path, sizeof(path), payload_root, f->path) < 0 ||
            !hash_matches(path, f)) return -1;
        for (j = i + 1; j < m->file_count; j++) if (!strcmp(f->path, m->files[j].path)) return -1;
        if (!extracted_has(extracted, extracted_count, f->path)) return -1;
    }
    for (i = 0; i < extracted_count; i++) {
        int found = 0;
        for (j = 0; j < m->file_count; j++) if (!strcmp(extracted[i].path, m->files[j].path)) { found = 1; break; }
        if (!found) return -1;
    }
    return 0;
}

static int read_state(UpdateState *s)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char *p, *q;
    memset(s, 0, sizeof(*s));
    if (file_read_limited(STATE_FILE, &data, &len, 65536) < 0) return -1;
#define STATE_STR(key, field) do { \
        p = strstr((char *)data, "\"" key "\":\""); \
        if (p) { p += strlen("\"" key "\":\""); q = strchr(p, '\"'); if (q && (size_t)(q-p) < sizeof(field)) { memcpy(field,p,(size_t)(q-p)); field[q-p]=0; } } \
    } while (0)
    STATE_STR("state", s->state); STATE_STR("txid", s->txid); STATE_STR("version", s->version);
    STATE_STR("graphics_version", s->graphics_version); STATE_STR("sha256", s->sha256);
    STATE_STR("previous", s->previous); STATE_STR("target", s->target);
    STATE_STR("old_runtime_target", s->old_runtime_target);
    STATE_STR("new_runtime_target", s->new_runtime_target);
    STATE_STR("old_graphics_target", s->old_graphics_target);
    STATE_STR("new_graphics_target", s->new_graphics_target);
    STATE_STR("old_config_backup", s->old_config_backup);
    STATE_STR("error", s->error);
#undef STATE_STR
    p = strstr((char *)data, "\"bundle_size\":"); if (p) s->bundle_size = strtoull(p + 14, NULL, 10);
    p = strstr((char *)data, "\"deadline\":"); if (p) s->deadline = strtoull(p + 11, NULL, 10);
    p = strstr((char *)data, "\"reboot_required\":true"); s->reboot_required = p != NULL;
    p = strstr((char *)data, "\"runtime_changed\":true"); s->runtime_changed = p != NULL;
    p = strstr((char *)data, "\"graphics_changed\":true"); s->graphics_changed = p != NULL;
    free(data);
    return s->state[0] ? 0 : -1;
}

static int write_state(const UpdateState *s)
{
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "{\"schema\":1,\"state\":\"%s\",\"txid\":\"%s\",\"version\":\"%s\","
        "\"graphics_version\":\"%s\",\"sha256\":\"%s\",\"bundle_size\":%llu,"
        "\"previous\":\"%s\",\"target\":\"%s\","
        "\"old_runtime_target\":\"%s\",\"new_runtime_target\":\"%s\","
        "\"old_graphics_target\":\"%s\",\"new_graphics_target\":\"%s\","
        "\"old_config_backup\":\"%s\",\"deadline\":%llu,"
        "\"reboot_required\":%s,\"runtime_changed\":%s,\"graphics_changed\":%s,"
        "\"error\":\"%s\"}\n",
        s->state, s->txid, s->version, s->graphics_version, s->sha256,
        (unsigned long long)s->bundle_size, s->previous, s->target,
        s->old_runtime_target, s->new_runtime_target, s->old_graphics_target,
        s->new_graphics_target, s->old_config_backup,
        (unsigned long long)s->deadline, s->reboot_required ? "true" : "false",
        s->runtime_changed ? "true" : "false", s->graphics_changed ? "true" : "false",
        s->error);
    if (n < 0 || n >= (int)sizeof(buf)) return -1;
    return atomic_write_file(STATE_FILE, buf, (size_t)n, 0600);
}

static int update_lock(void)
{
    int fd;
    mkdir_p(STATE_ROOT, 0700); mkdir_p(BUNDLE_ROOT, 0700); mkdir_p(RELEASES_ROOT, 0755);
    fd = open(STATE_ROOT "/.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

static int replace_symlink(const char *link_path, const char *target)
{
    char tmp[PATH_MAX];
    if (!target || target[0] == '/' || strstr(target, "..") ||
        (strncmp(target, "releases/", 9) && strcmp(target, ".")) ||
        (!strcmp(target, ".") && strcmp(link_path, "/opt/wsl-mesa/current") &&
         strcmp(link_path, "/opt/wsl-mesa/previous"))) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.new.%ld", link_path, (long)getpid()) >= (int)sizeof(tmp)) return -1;
    unlink(tmp);
    if (symlink(target, tmp) < 0 || rename(tmp, link_path) < 0) { unlink(tmp); return -1; }
    return fsync_parent(link_path);
}

static int current_target(char *out, size_t cap)
{
    ssize_t n = readlink(UPDATE_ROOT "/current", out, cap - 1);
    if (n < 0 || n >= (ssize_t)cap) return -1;
    out[n] = 0;
    return !strncmp(out, "releases/", 9) && !strstr(out, "..") ? 0 : -1;
}

static int read_current_version(char *out, size_t cap)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char value[96] = {0};
    char *p, *e;
    if (file_read_limited(UPDATE_ROOT "/current/RELEASE", &data, &len, 4096) < 0)
        return -1;
    p = strstr((char *)data, "version=");
    if (!p) { free(data); return -1; }
    p += 8;
    e = strchr(p, '\n');
    if (e) *e = 0;
    if (snprintf(value, sizeof(value), "%s", p) >= (int)sizeof(value) ||
        !valid_version(value) || snprintf(out, cap, "%s", value) >= (int)cap) {
        free(data);
        return -1;
    }
    free(data);
    return 0;
}

static int absolute_symlink(const char *link_path, const char *target)
{
    char tmp[PATH_MAX];
    if (!target || target[0] != '/' || strstr(target, "..") ||
        strncmp(target, UPDATE_ROOT "/current/", strlen(UPDATE_ROOT "/current/"))) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.new.%ld", link_path, (long)getpid()) >= (int)sizeof(tmp)) return -1;
    unlink(tmp);
    if (symlink(target, tmp) < 0 || rename(tmp, link_path) < 0) { unlink(tmp); return -1; }
    return fsync_parent(link_path);
}

static int graphics_target(char *out, size_t cap)
{
    ssize_t n = readlink("/opt/wsl-mesa/current", out, cap - 1);
    if (n < 0 || n >= (ssize_t)cap) return -1;
    out[n] = 0;
    return ((!strcmp(out, ".")) || !strncmp(out, "releases/", 9)) &&
           !strstr(out, "..") ? 0 : -1;
}

static int extract_graphics(const char *archive, const char *version, const char *txid)
{
    int zfd = -1, status, zeros = 0, fd = -1;
    pid_t zpid = 0;
    uint64_t uncompressed_total = 0;
    unsigned char block[512];
    char stage[PATH_MAX], final[PATH_MAX];
    if (!valid_version(version) || snprintf(stage, sizeof(stage), "/opt/wsl-mesa/.staging-%s", txid) >= (int)sizeof(stage) ||
        snprintf(final, sizeof(final), "/opt/wsl-mesa/releases/%s-%s", version, txid) >= (int)sizeof(final) ||
        mkdir_p("/opt/wsl-mesa/releases", 0755) < 0 || mkdir_p(stage, 0755) < 0 ||
        spawn_zstd_reader(archive, &zfd, &zpid) < 0) return -1;
    for (;;) {
        char path[PATH_MAX], full[PATH_MAX];
        uint64_t size, mode, left;
        unsigned char type;
        const char *p;
        if (read_all(zfd, block, sizeof(block)) < 0) goto fail;
        { int zero = 1; size_t i; for (i = 0; i < sizeof(block); i++) if (block[i]) { zero = 0; break; }
          if (zero) { if (++zeros == 2) break; continue; } }
        zeros = 0;
        type = block[156] ? block[156] : '0';
        if (tar_path(block, path, sizeof(path)) < 0 ||
            tar_octal(block + 124, 12, &size) < 0 || tar_octal(block + 100, 8, &mode) < 0 ||
            size > UPDATE_MAX_FILE_SIZE ||
            uncompressed_total > UPDATE_MAX_UNCOMPRESSED - size) goto fail;
        uncompressed_total += size;
        if (type == '5' && path[0] && path[strlen(path) - 1] == '/')
            path[strlen(path) - 1] = '\0';
        if (!safe_relpath(path)) goto fail;
        if (!strncmp(path, "opt/wsl-mesa/", 13)) p = path + 13;
        else if (!strcmp(path, "opt/wsl-mesa")) p = "";
        else if (!strncmp(path, "wsl-mesa/", 9)) p = path + 9;
        else p = path;
        type = block[156] ? block[156] : '0';
        if (!p[0]) { if (type != '5') goto fail; continue; }
        if (!safe_relpath(p) || join_path(full, sizeof(full), stage, p) < 0) goto fail;
        if (type == '5') {
            if (size != 0) goto fail;
            if (mkdir_p(full, 0755) < 0) goto fail;
            continue;
        }
        if (type != '0' || ensure_parent(stage, p) < 0 ||
            (fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)) < 0) goto fail;
        left = size;
        while (left) {
            unsigned char buf[65536]; size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            if (read_all(zfd, buf, want) < 0 || write_all(fd, buf, want) < 0) goto fail;
            left -= want;
        }
        if (fchmod(fd, (mode_t)(mode & 0777)) < 0 || fsync(fd) < 0 || close(fd) < 0) { fd = -1; goto fail; }
        fd = -1;
        if (skip_bytes(zfd, (512 - (size % 512)) % 512) < 0) goto fail;
    }
    {
        char marker[PATH_MAX];
        if (append_path(marker, sizeof(marker), stage, "/GRAPHICS") < 0 ||
            atomic_write_file(marker, version, strlen(version) + 1, 0644) < 0)
            goto fail;
    }
    close(zfd);
    if (waitpid(zpid, &status, 0) != zpid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || rename(stage, final) < 0)
        return -1;
    return 0;
fail:
    if (fd >= 0) close(fd);
    close(zfd);
    kill(zpid, SIGTERM);
    waitpid(zpid, NULL, 0);
    return -1;
}

static int install_config_file(const char *source, const char *target)
{
    unsigned char *data = NULL; size_t len = 0; int ok;
    if (access(source, R_OK) != 0) return 0;
    if (file_read_limited(source, &data, &len, 65536) < 0) return -1;
    ok = atomic_write_file(target, data, len, 0644); free(data); return ok;
}

static int install_release_integrations(const char *release)
{
    static const char *const units[] = {
        "appsandbox-agent.service", "appsandbox-display.service",
        "appsandbox-display-d3d12.service", "appsandbox-input.service",
        "appsandbox-audio.service"
    };
    static const char *const gnome_files[] = {"metadata.json", "extension.js"};
    unsigned char *data = NULL;
    char source[PATH_MAX], dir[PATH_MAX], prefix[PATH_MAX], target[PATH_MAX];
    size_t len, i;
    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (join_rel_path(dir, sizeof(dir), release, "systemd") < 0 ||
            append_path(prefix, sizeof(prefix), dir, "/") < 0 ||
            append_path(source, sizeof(source), prefix, units[i]) < 0 ||
            append_path(target, sizeof(target), "/etc/systemd/system/", units[i]) < 0)
            return -1;
        if (access(source, R_OK) != 0) continue;
        if (file_read_limited(source, &data, &len, 65536) < 0) return -1;
        if (atomic_write_file(target, data, len, 0644) < 0) { free(data); return -1; }
        free(data); data = NULL;
    }
    for (i = 0; i < sizeof(gnome_files) / sizeof(gnome_files[0]); i++) {
        if (join_rel_path(dir, sizeof(dir), release,
                          "gnome/appsandbox-pointer@appsandbox") < 0 ||
            append_path(prefix, sizeof(prefix), dir, "/") < 0 ||
            append_path(source, sizeof(source), prefix, gnome_files[i]) < 0 ||
            append_path(target, sizeof(target),
                        "/usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/",
                        gnome_files[i]) < 0)
            return -1;
        if (access(source, R_OK) != 0) continue;
        if (file_read_limited(source, &data, &len, 65536) < 0) return -1;
        if (atomic_write_file(target, data, len, 0644) < 0) { free(data); return -1; }
        free(data); data = NULL;
    }
    return 0;
}

static int install_runtime_links(void)
{
    static const char *bins[] = {"appsandbox-agent", "appsandbox-display", "appsandbox-input",
                                 "appsandbox-audio", "appsandbox-clipboard"};
    size_t i;
    char link[PATH_MAX], target[PATH_MAX];
    for (i = 0; i < sizeof(bins) / sizeof(bins[0]); i++) {
        snprintf(link, sizeof(link), "/usr/local/bin/%s", bins[i]);
        snprintf(target, sizeof(target), UPDATE_ROOT "/current/bin/%s", bins[i]);
        if (access(target, X_OK) == 0 && absolute_symlink(link, target) < 0) return -1;
    }
    snprintf(link, sizeof(link), "/usr/local/libexec/appsandbox-display-d3d12");
    snprintf(target, sizeof(target), UPDATE_ROOT "/current/libexec/appsandbox-display-d3d12");
    if (access(target, X_OK) == 0 && absolute_symlink(link, target) < 0) return -1;
    return 0;
}

static int run_fixed(const char *const argv[])
{
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int spawn_systemctl(const char *verb)
{
    static const char *const reload[] = {"/usr/bin/systemctl", "daemon-reload", NULL};
    static const char *const restart[] = {"/usr/bin/systemctl", "restart",
        "appsandbox-agent.service", "appsandbox-display.service", "appsandbox-display-d3d12.service", NULL};
    static const char *const reboot[] = {"/usr/bin/systemctl", "reboot", NULL};
    if (!strcmp(verb, "reboot")) return run_fixed(reboot);
    if (run_fixed(reload) != 0) return -1;
    return run_fixed(restart);
}

static int service_active(const char *unit)
{
    const char *const argv[] = {"/usr/bin/systemctl", "is-active", "--quiet", unit, NULL};
    return run_fixed(argv) == 0;
}

typedef struct HealthResult {
    int agent_active;
    int display_active;
    int d3d12_helper_active;
    int dxg_present;
    int mutter_active;
    int shared_path_initialized;
    int encoder_initialized;
    int native_d3d12_shared;
    int gpu_copy;
    int cpu_copy;
    int cpu_conversion;
    int framebuffer_mmap;
    int cpu_memcpy_framebuffer;
    int gpu_cpu_gpu;
    int stale_frames;
    int mismatches;
    int encode_failures;
    char configured_resolution[64];
    char graphics_version[96];
} HealthResult;

static int health_probe_flag(const char *name, int fallback)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char needle[64];
    int result = fallback;
    if (file_read_limited("/run/appsandbox/display-d3d12.health", &data, &len, 65536) < 0)
        return fallback;
    snprintf(needle, sizeof(needle), "%s=1", name);
    if (strstr((char *)data, needle) || (snprintf(needle, sizeof(needle), "\"%s\":1", name),
                                        strstr((char *)data, needle))) result = 1;
    else {
        snprintf(needle, sizeof(needle), "%s=0", name);
        if (strstr((char *)data, needle)) result = 0;
    }
    free(data);
    return result;
}

static int any_wayland_session(void)
{
    DIR *d = opendir("/run/user");
    struct dirent *entry;
    if (!d) return 0;
    while ((entry = readdir(d)) != NULL) {
        char path[PATH_MAX];
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        if (snprintf(path, sizeof(path), "/run/user/%s/wayland-0", entry->d_name) < (int)sizeof(path) &&
            access(path, F_OK) == 0) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static void health_collect(HealthResult *h)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char *p;
    int width = 0, height = 0, refresh = 0;
    memset(h, 0, sizeof(*h));
    h->agent_active = service_active("appsandbox-agent.service");
    h->display_active = service_active("appsandbox-display.service");
    h->dxg_present = access("/dev/dxg", F_OK) == 0;
    h->d3d12_helper_active = access("/etc/systemd/system/appsandbox-display-d3d12.service", F_OK) == 0 ?
        service_active("appsandbox-display-d3d12.service") : 0;
    h->mutter_active = any_wayland_session();
    h->shared_path_initialized = access("/opt/wsl-mesa/current", F_OK) == 0;
    h->encoder_initialized = h->d3d12_helper_active || access("/run/appsandbox/display-d3d12.sock", F_OK) == 0;
    snprintf(h->configured_resolution, sizeof(h->configured_resolution), "unknown");
    if (file_read_limited("/etc/modprobe.d/asb_drm.conf", &data, &len, 65536) == 0) {
        p = strstr((char *)data, "width="); if (p) width = atoi(p + 6);
        p = strstr((char *)data, "height="); if (p) height = atoi(p + 7);
        p = strstr((char *)data, "refresh="); if (p) refresh = atoi(p + 8);
        if (width > 0 && height > 0)
            snprintf(h->configured_resolution, sizeof(h->configured_resolution),
                     refresh > 0 ? "%dx%d@%d" : "%dx%d", width, height, refresh);
        free(data);
    }
    snprintf(h->graphics_version, sizeof(h->graphics_version), "unknown");
    if (file_read_limited("/opt/wsl-mesa/current/GRAPHICS", &data, &len, 256) == 0) {
        sscanf((char *)data, "%95[0-9A-Za-z.+-]", h->graphics_version);
        free(data); data = NULL;
    } else if (file_read_limited(UPDATE_ROOT "/current/RELEASE", &data, &len, 4096) == 0) {
        p = strstr((char *)data, "graphics_version=");
        if (p) sscanf(p + 17, "%95[0-9A-Za-z.+-]", h->graphics_version);
        free(data);
    }
    h->native_d3d12_shared = health_probe_flag("native_d3d12_shared", 0);
    h->gpu_copy = health_probe_flag("gpu_copy", 0);
    h->cpu_copy = health_probe_flag("cpu_copy", 0);
    h->cpu_conversion = health_probe_flag("cpu_conversion", 0);
    h->framebuffer_mmap = health_probe_flag("framebuffer_mmap", 0);
    h->cpu_memcpy_framebuffer = health_probe_flag("cpu_memcpy_framebuffer", 0);
    h->gpu_cpu_gpu = health_probe_flag("gpu_cpu_gpu", 0);
    h->stale_frames = health_probe_flag("stale_frames", 0);
    h->mismatches = health_probe_flag("mismatches", 0);
    h->encode_failures = health_probe_flag("encode_failures", 0);
}

static int health_check(void)
{
    HealthResult h;
    health_collect(&h);
    if (!h.agent_active || !h.display_active) return -1;
    if (h.dxg_present && access("/etc/systemd/system/appsandbox-display-d3d12.service", F_OK) == 0 &&
        !h.d3d12_helper_active) return -1;
    return 0;
}

static int health_main(void)
{
    HealthResult h;
    health_collect(&h);
    printf("{\"agent_active\":%d,\"display_active\":%d,\"d3d12_helper_active\":%d,"
           "\"dxg_present\":%d,\"mutter_active\":%d,\"configured_resolution\":\"%s\","
           "\"graphics_version\":\"%s\",\"shared_path_initialized\":%d,"
           "\"encoder_initialized\":%d,\"native_d3d12_shared\":%d,\"gpu_copy\":%d,"
           "\"cpu_copy\":%d,\"cpu_conversion\":%d,\"framebuffer_mmap\":%d,"
           "\"cpu_memcpy_framebuffer\":%d,\"gpu_cpu_gpu\":%d,\"stale_frames\":%d,"
           "\"mismatches\":%d,\"encode_failures\":%d}\n",
           h.agent_active, h.display_active, h.d3d12_helper_active, h.dxg_present,
           h.mutter_active, h.configured_resolution, h.graphics_version,
           h.shared_path_initialized, h.encoder_initialized, h.native_d3d12_shared,
           h.gpu_copy, h.cpu_copy, h.cpu_conversion, h.framebuffer_mmap,
           h.cpu_memcpy_framebuffer, h.gpu_cpu_gpu, h.stale_frames, h.mismatches,
           h.encode_failures);
    return 0;
}

static int backup_live_config(UpdateState *s)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char path[PATH_MAX];
    if (access("/etc/modprobe.d/asb_drm.conf", F_OK) != 0) {
        s->old_config_backup[0] = 0;
        return 0;
    }
    if (file_read_limited("/etc/modprobe.d/asb_drm.conf", &data, &len, 65536) < 0 ||
        snprintf(path, sizeof(path), "%s/%s.asb_drm.conf", STATE_ROOT, s->txid) >= (int)sizeof(path) ||
        atomic_write_file(path, data, len, 0600) < 0) {
        free(data);
        return -1;
    }
    free(data);
    snprintf(s->old_config_backup, sizeof(s->old_config_backup), "%s", path);
    return 0;
}

static int restore_config(const UpdateState *s, const char *old_release)
{
    char config[PATH_MAX];
    if (s->old_config_backup[0] && access(s->old_config_backup, R_OK) == 0)
        return install_config_file(s->old_config_backup, "/etc/modprobe.d/asb_drm.conf");
    if (old_release && old_release[0] &&
        append_path(config, sizeof(config), old_release, "/config/asb_drm.conf") < 0)
        return -1;
    if (old_release && old_release[0] && access(config, R_OK) == 0)
        return install_config_file(config, "/etc/modprobe.d/asb_drm.conf");
    if (unlink("/etc/modprobe.d/asb_drm.conf") < 0 && errno != ENOENT) return -1;
    return fsync_parent("/etc/modprobe.d/asb_drm.conf");
}

static int restore_activation_locked(UpdateState *s, const char *reason)
{
    char current[PATH_MAX], old_release[PATH_MAX];
    int runtime_owned = 0, graphics_owned = 0;

    if (s->runtime_changed) {
        if (current_target(current, sizeof(current)) == 0) {
            if (s->new_runtime_target[0] && !strcmp(current, s->new_runtime_target)) {
                runtime_owned = 1;
            } else if (s->old_runtime_target[0] && !strcmp(current, s->old_runtime_target)) {
                runtime_owned = 0;
            } else {
                return -1; /* a later transaction owns the live pointer */
            }
        } else if (s->old_runtime_target[0]) {
            return -1;
        }
        if (runtime_owned && s->old_runtime_target[0] &&
            replace_symlink(UPDATE_ROOT "/current", s->old_runtime_target) < 0)
            return -1;
        if (s->old_runtime_target[0]) {
            if (snprintf(old_release, sizeof(old_release), "%s/%s", UPDATE_ROOT,
                         s->old_runtime_target) >= (int)sizeof(old_release) ||
                install_release_integrations(old_release) < 0 ||
                restore_config(s, old_release) < 0 || install_runtime_links() < 0)
                return -1;
        } else if (restore_config(s, NULL) < 0) {
            return -1;
        }
    }

    if (s->graphics_changed) {
        char graphics[PATH_MAX];
        if (graphics_target(graphics, sizeof(graphics)) == 0) {
            if (s->new_graphics_target[0] && !strcmp(graphics, s->new_graphics_target)) {
                graphics_owned = 1;
            } else if (s->old_graphics_target[0] && !strcmp(graphics, s->old_graphics_target)) {
                graphics_owned = 0;
            } else {
                return -1;
            }
        }
        if (graphics_owned && s->old_graphics_target[0] &&
            replace_symlink("/opt/wsl-mesa/current", s->old_graphics_target) < 0)
            return -1;
    }
    strcpy(s->state, "rollback");
    snprintf(s->error, sizeof(s->error), "%s", reason ? reason : "activation_failed");
    return write_state(s);
}

static int apply_update(const char *txid)
{
    UpdateState s;
    Manifest m;
    unsigned char *manifest = NULL, *signature = NULL;
    size_t manifest_len = 0, signature_len = 0;
    ExtractedFile *files = NULL;
    int file_count = 0, lock = -1, ok = -1;
    int runtime_payload_present = 0;
    char bundle[PATH_MAX], stage[PATH_MAX], stage_payload[PATH_MAX], release[PATH_MAX], release_payload[PATH_MAX];
    char old_target[PATH_MAX], old_graphics[PATH_MAX], os[64], release_file[PATH_MAX], config_file[PATH_MAX];
    char current_version[96] = {0};
    if (!valid_id(txid) || read_state(&s) < 0 || strcmp(s.txid, txid) ||
        (strcmp(s.state, "received") && strcmp(s.state, "receiving") &&
         strcmp(s.state, "apply_requested"))) return -1;
    lock = update_lock(); if (lock < 0) return -1;
    snprintf(bundle, sizeof(bundle), "%s/%s.bundle", BUNDLE_ROOT, txid);
    snprintf(stage, sizeof(stage), "%s/%s", STAGING_ROOT, txid);
    files = calloc(UPDATE_MAX_FILES, sizeof(*files));
    if (!files || extract_bundle(bundle, stage, &manifest, &manifest_len, &signature, &signature_len,
                                 files, &file_count) < 0 || verify_signature(manifest, manifest_len, signature, signature_len) < 0 ||
        parse_manifest(manifest, manifest_len, &m) < 0 || read_os_tuple(os, sizeof(os)) < 0 ||
        strcmp(os, m.os) != 0 || verify_payload(&m, stage, files, file_count) < 0) {
        snprintf(s.error, sizeof(s.error), "verification_failed"); strcpy(s.state, "failed"); write_state(&s); goto done;
    }
    for (int i = 0; i < m.file_count; i++)
        if (strncmp(m.files[i].path, "graphics/", 9) != 0) { runtime_payload_present = 1; break; }
    if (!m.allow_downgrade && current_target(old_target, sizeof(old_target)) == 0) {
        if (read_current_version(current_version, sizeof(current_version)) < 0 ||
            version_cmp(m.version, current_version) < 0) {
            snprintf(s.error, sizeof(s.error), "downgrade_rejected");
            strcpy(s.state, "failed"); write_state(&s); goto done;
        }
    }
    s.reboot_required = m.reboot_required || m.kernel_components_present;
    snprintf(s.version, sizeof(s.version), "%s", m.version);
    snprintf(s.graphics_version, sizeof(s.graphics_version), "%s", m.graphics_version);
    strcpy(s.state, "verified"); if (write_state(&s) < 0) goto done;
    if (current_target(old_target, sizeof(old_target)) == 0) {
        snprintf(s.previous, sizeof(s.previous), "%s", old_target);
        snprintf(s.old_runtime_target, sizeof(s.old_runtime_target), "%s", old_target);
    } else {
        if (!runtime_payload_present) {
            snprintf(s.error, sizeof(s.error), "graphics_update_requires_current_runtime");
            strcpy(s.state, "failed"); write_state(&s); goto done;
        }
        s.previous[0] = 0;
        s.old_runtime_target[0] = 0;
    }
    snprintf(release, sizeof(release), "%s/%s-%s", RELEASES_ROOT, m.version, txid);
    if (runtime_payload_present) {
        snprintf(s.target, sizeof(s.target), "releases/%s-%s", m.version, txid);
        snprintf(s.new_runtime_target, sizeof(s.new_runtime_target), "%s", s.target);
        s.runtime_changed = 1;
    } else {
        /* A graphics-only bundle still gets its own release directory for
         * the Mesa archive, but it must not move the runtime A/B pointer. */
        snprintf(s.target, sizeof(s.target), "%s", s.old_runtime_target);
        snprintf(s.new_runtime_target, sizeof(s.new_runtime_target), "%s", s.old_runtime_target);
        s.runtime_changed = 0;
    }
    if (graphics_target(old_graphics, sizeof(old_graphics)) == 0)
        snprintf(s.old_graphics_target, sizeof(s.old_graphics_target), "%s", old_graphics);
    else s.old_graphics_target[0] = 0;
    s.graphics_changed = m.graphics_version[0] != 0;
    if (s.graphics_changed)
        snprintf(s.new_graphics_target, sizeof(s.new_graphics_target), "releases/%s-%s",
                 m.graphics_version, txid);
    else s.new_graphics_target[0] = 0;
    if (snprintf(stage_payload, sizeof(stage_payload), "%s/payload", stage) >= (int)sizeof(stage_payload) ||
        snprintf(release_payload, sizeof(release_payload), "%s/payload", release) >= (int)sizeof(release_payload) ||
        mkdir_p(release, 0755) < 0 || rename(stage_payload, release_payload) < 0) goto done;
    /* Runtime files live directly below the version directory. */
    {
        char src[PATH_MAX], dst[PATH_MAX];
#define MOVE_PAYLOAD_DIR(name) do { \
            if (join_rel_path(src, sizeof(src), release, "payload/" name) < 0 || \
                join_rel_path(dst, sizeof(dst), release, name) < 0) goto done; \
            if (rename(src, dst) < 0 && errno != ENOENT) goto done; \
        } while (0)
        MOVE_PAYLOAD_DIR("bin"); MOVE_PAYLOAD_DIR("libexec"); MOVE_PAYLOAD_DIR("systemd");
        MOVE_PAYLOAD_DIR("gnome"); MOVE_PAYLOAD_DIR("config"); MOVE_PAYLOAD_DIR("graphics");
#undef MOVE_PAYLOAD_DIR
    }
    {
        char rel_info[256];
        int n = snprintf(rel_info, sizeof(rel_info), "version=%s\ngraphics_version=%s\ncommit=%s\n",
                         m.version, m.graphics_version, m.commit);
        if (append_path(release_file, sizeof(release_file), release, "/RELEASE") < 0) goto done;
        if (atomic_write_file(release_file, rel_info, (size_t)n, 0644) < 0) goto done;
    }
    if (m.graphics_version[0]) {
        char graphics_archive[PATH_MAX];
        if (append_path(graphics_archive, sizeof(graphics_archive), release,
                        "/graphics/wsl-mesa.tar.zst") < 0) goto done;
        if (extract_graphics(graphics_archive, m.graphics_version, txid) < 0) goto done;
    }
    /* No live path, service unit, or config is touched until this durable
     * record says activation has begun and contains both sides of every A/B
     * switch. */
    if (backup_live_config(&s) < 0) goto done;
    strcpy(s.state, "activating");
    s.deadline = (uint64_t)time(NULL) + UPDATE_HEALTH_SECONDS;
    if (write_state(&s) < 0) goto done;
    if (s.graphics_changed) {
        if (s.old_graphics_target[0] &&
            replace_symlink("/opt/wsl-mesa/previous", s.old_graphics_target) < 0) goto done;
        {
            if (replace_symlink("/opt/wsl-mesa/current", s.new_graphics_target) < 0) goto done;
        }
    }
    if (append_path(config_file, sizeof(config_file), release,
                    "/config/asb_drm.conf") < 0) goto done;
    if (install_release_integrations(release) < 0 ||
        install_config_file(config_file, "/etc/modprobe.d/asb_drm.conf") < 0) goto done;
    if (s.runtime_changed && s.previous[0] && replace_symlink(UPDATE_ROOT "/previous", s.previous) < 0) goto done;
    if (s.runtime_changed && replace_symlink(UPDATE_ROOT "/current", s.target) < 0) goto done;
    if (s.runtime_changed && install_runtime_links() < 0) goto done;
    strcpy(s.state, "health_check");
    s.error[0] = 0; if (write_state(&s) < 0) goto done;
    if (s.reboot_required) {
        strcpy(s.state, "reboot_pending"); write_state(&s);
        /* The state is durable before reboot.  The updater service, not the
         * newly-installed agent, owns post-boot health and rollback. */
        spawn_systemctl("reboot");
        ok = 0; goto done;
    }
    /* The watcher owns the durable health decision. Restarting the agent is
     * deliberately after state persistence and cannot invalidate the host's
     * already-returned update_apply acknowledgement. */
    spawn_systemctl("restart");
    ok = 0;
done:
    if (ok < 0 && s.state[0] && !strcmp(s.state, "activating")) {
        if (restore_activation_locked(&s, "activation_failed") < 0) {
            snprintf(s.error, sizeof(s.error), "activation_recovery_failed");
            strcpy(s.state, "failed"); write_state(&s);
        }
    } else if (ok < 0 && s.state[0] && strcmp(s.state, "failed") && strcmp(s.state, "rollback")) {
        snprintf(s.error, sizeof(s.error), "activation_failed"); strcpy(s.state, "failed"); write_state(&s);
    }
    free(manifest); free(signature); free(files); if (lock >= 0) close(lock);
    return ok;
}

static int rollback_update(void)
{
    UpdateState s;
    int lock = update_lock();
    if (lock < 0 || read_state(&s) < 0) {
        if (lock >= 0) close(lock);
        return -1;
    }
    /* States written by older updater builds only had previous/target. Treat
     * that metadata as a runtime-only transaction; never infer graphics
     * ownership from a global /opt/wsl-mesa/previous link. */
    if (!s.old_runtime_target[0] && s.previous[0]) {
        snprintf(s.old_runtime_target, sizeof(s.old_runtime_target), "%s", s.previous);
        s.runtime_changed = 1; /* compatibility with pre-metadata state */
    }
    if (!s.new_runtime_target[0] && s.target[0]) {
        snprintf(s.new_runtime_target, sizeof(s.new_runtime_target), "%s", s.target);
        s.runtime_changed = 1; /* compatibility with pre-metadata state */
    }
    if (restore_activation_locked(&s, "health_check_failed") < 0) {
        close(lock);
        return -1;
    }
    spawn_systemctl("restart");
    printf("rolled_back\n"); close(lock); return 0;
}

static int migrate_legacy_file(const char *source, const char *target, mode_t mode)
{
    unsigned char *data = NULL;
    size_t len = 0;
    int ok;
    if (access(source, R_OK) != 0) return 0;
    if (file_read_limited(source, &data, &len, UPDATE_MAX_FILE_SIZE) < 0) return -1;
    ok = atomic_write_file(target, data, len, mode);
    free(data);
    return ok;
}

static int migrate_legacy_main(void)
{
    static const char *const bins[] = {"appsandbox-agent", "appsandbox-display", "appsandbox-input",
                                       "appsandbox-audio", "appsandbox-clipboard"};
    static const char *const libexecs[] = {"appsandbox-display-d3d12"};
    static const char *const units[] = {"appsandbox-agent.service", "appsandbox-display.service",
                                        "appsandbox-display-d3d12.service", "appsandbox-input.service",
                                        "appsandbox-audio.service"};
    static const char *const gnome_files[] = {"metadata.json", "extension.js"};
    UpdateState s;
    char release[PATH_MAX], path[PATH_MAX], target[PATH_MAX];
    struct stat current_stat;
    size_t i;
    int lock = update_lock();
    if (lock < 0) return -1;
    if (current_target(target, sizeof(target)) == 0) {
        puts("ready"); close(lock); return 0;
    }
    if (lstat(UPDATE_ROOT "/current", &current_stat) == 0 || errno != ENOENT ||
        snprintf(release, sizeof(release), "%s/legacy", RELEASES_ROOT) >= (int)sizeof(release) ||
        mkdir_p(release "/bin", 0755) < 0 || mkdir_p(release "/libexec", 0755) < 0 ||
        mkdir_p(release "/systemd", 0755) < 0 || mkdir_p(release "/config", 0755) < 0 ||
        mkdir_p(release "/gnome/appsandbox-pointer@appsandbox", 0755) < 0)
        { close(lock); return -1; }
    if (access("/usr/local/bin/appsandbox-agent", X_OK) != 0) { close(lock); return -1; }
    for (i = 0; i < sizeof(bins) / sizeof(bins[0]); i++) {
        snprintf(path, sizeof(path), "/usr/local/bin/%s", bins[i]);
        snprintf(target, sizeof(target), "%s/bin/%s", release, bins[i]);
        if (migrate_legacy_file(path, target, 0755) < 0) { close(lock); return -1; }
    }
    for (i = 0; i < sizeof(libexecs) / sizeof(libexecs[0]); i++) {
        snprintf(path, sizeof(path), "/usr/local/libexec/%s", libexecs[i]);
        snprintf(target, sizeof(target), "%s/libexec/%s", release, libexecs[i]);
        if (migrate_legacy_file(path, target, 0755) < 0) { close(lock); return -1; }
    }
    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        snprintf(path, sizeof(path), "/etc/systemd/system/%s", units[i]);
        snprintf(target, sizeof(target), "%s/systemd/%s", release, units[i]);
        if (migrate_legacy_file(path, target, 0644) < 0) { close(lock); return -1; }
    }
    for (i = 0; i < sizeof(gnome_files) / sizeof(gnome_files[0]); i++) {
        snprintf(path, sizeof(path),
                 "/usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/%s",
                 gnome_files[i]);
        snprintf(target, sizeof(target),
                 "%s/gnome/appsandbox-pointer@appsandbox/%s", release, gnome_files[i]);
        if (migrate_legacy_file(path, target, 0644) < 0) { close(lock); return -1; }
    }
    snprintf(path, sizeof(path), "/etc/modprobe.d/asb_drm.conf");
    snprintf(target, sizeof(target), "%s/config/asb_drm.conf", release);
    if (migrate_legacy_file(path, target, 0644) < 0) { close(lock); return -1; }
    if (snprintf(path, sizeof(path), "%s/RELEASE", release) >= (int)sizeof(path) ||
        atomic_write_file(path, "version=0.0.0\ngraphics_version=legacy-provisioned\ncommit=migrated\n",
                          strlen("version=0.0.0\ngraphics_version=legacy-provisioned\ncommit=migrated\n"), 0644) < 0 ||
        replace_symlink(UPDATE_ROOT "/current", "releases/legacy") < 0 || install_runtime_links() < 0)
        { close(lock); return -1; }
    {
        struct stat graphics_current;
        if (lstat("/opt/wsl-mesa/current", &graphics_current) < 0) {
            if (errno != ENOENT || mkdir_p("/opt/wsl-mesa", 0755) < 0 ||
                replace_symlink("/opt/wsl-mesa/current", ".") < 0) {
                close(lock);
                return -1;
            }
        }
    }
    memset(&s, 0, sizeof(s)); strcpy(s.state, "idle");
    if (write_state(&s) < 0) { close(lock); return -1; }
    puts("migrated");
    close(lock);
    return 0;
}

static int begin_update(const char *txid, const char *size_text, const char *sha)
{
    UpdateState s;
    uint64_t size;
    char *end;
    int lock;
    if (!valid_id(txid) || !sha || strlen(sha) != 64) return -1;
    errno = 0; size = strtoull(size_text, &end, 10);
    if (errno || end == size_text || *end || size == 0 || size > UPDATE_MAX_BUNDLE) return -1;
    lock = update_lock(); if (lock < 0) return -1;
    if (read_state(&s) == 0 && s.txid[0] && strcmp(s.txid, txid) && strcmp(s.state, "committed") && strcmp(s.state, "failed")) { close(lock); return -1; }
    memset(&s, 0, sizeof(s)); strcpy(s.state, "receiving"); snprintf(s.txid, sizeof(s.txid), "%s", txid);
    snprintf(s.sha256, sizeof(s.sha256), "%s", sha); s.bundle_size = size;
    {
        char old_part[PATH_MAX];
        snprintf(old_part, sizeof(old_part), "%s/%s.bundle.part", BUNDLE_ROOT, txid);
        unlink(old_part);
    }
    if (write_state(&s) < 0) { close(lock); return -1; }
    printf("ready\n"); close(lock); return 0;
}

static int submit_apply(const char *txid)
{
    UpdateState s;
    int lock;
    if (!valid_id(txid)) return -1;
    lock = update_lock();
    if (lock < 0 || read_state(&s) < 0 || strcmp(s.txid, txid)) {
        if (lock >= 0) close(lock);
        return -1;
    }
    if (!strcmp(s.state, "received")) {
        strcpy(s.state, "apply_requested");
        s.error[0] = 0;
        if (write_state(&s) < 0) { close(lock); return -1; }
    } else if (strcmp(s.state, "apply_requested") && strcmp(s.state, "activating") &&
               strcmp(s.state, "health_check") && strcmp(s.state, "reboot_pending") &&
               strcmp(s.state, "committed")) {
        close(lock);
        return -1;
    }
    /* The caller receives this only after the request is atomically persisted;
     * the watcher service is free to outlive both the agent and this process. */
    puts("accepted");
    close(lock);
    return 0;
}

static int receive_stream(int fd)
{
    UpdateStreamHeader h;
    UpdateState s;
    char part[PATH_MAX], final[PATH_MAX];
    unsigned char digest[32];
    int out = -1, lock = -1, ok = -1;
    char header_sha[65];
    if (read_all(fd, &h, sizeof(h)) < 0 || memcmp(h.magic, "ASBUPD1", 7) ||
        h.protocol != UPDATE_PROTOCOL || h.header_size != sizeof(h) || !valid_id(h.txid) ||
        h.bundle_size == 0 || h.bundle_size > UPDATE_MAX_BUNDLE || read_state(&s) < 0 ||
        strcmp(s.state, "receiving") || strcmp(s.txid, h.txid) || s.bundle_size != h.bundle_size) return -1;
    hex_encode(h.sha256, sizeof(h.sha256), header_sha);
    if (strcmp(s.sha256, header_sha) != 0) return -1;
    snprintf(part, sizeof(part), "%s/%s.bundle.part", BUNDLE_ROOT, h.txid);
    snprintf(final, sizeof(final), "%s/%s.bundle", BUNDLE_ROOT, h.txid);
    lock = update_lock(); if (lock < 0) return -1;
    out = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) goto done;
    {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        uint64_t left = h.bundle_size;
        unsigned char buf[1024 * 1024];
        if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) { EVP_MD_CTX_free(ctx); goto done; }
        while (left) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            if (read_all(fd, buf, want) < 0 || write_all(out, buf, want) < 0 || EVP_DigestUpdate(ctx, buf, want) != 1) { EVP_MD_CTX_free(ctx); goto done; }
            left -= want;
        }
        if (EVP_DigestFinal_ex(ctx, digest, NULL) != 1) { EVP_MD_CTX_free(ctx); goto done; }
        EVP_MD_CTX_free(ctx);
    }
    if (memcmp(digest, h.sha256, sizeof(digest)) != 0 || fsync(out) < 0 || close(out) < 0) { out = -1; goto done; }
    out = -1;
    if (rename(part, final) < 0 || fsync_parent(final) < 0) goto done;
    strcpy(s.state, "received"); s.error[0] = 0; write_state(&s); ok = 0;
done:
    if (out >= 0) close(out);
    if (ok < 0) {
        unlink(part);
        snprintf(s.error, sizeof(s.error), "truncated_or_hash_mismatch");
        strcpy(s.state, "failed");
        write_state(&s);
    }
    if (lock >= 0) close(lock);
    return ok;
}

static int daemon_main(void)
{
    int ls, one = 1;
    struct sockaddr_vm addr;
    signal(SIGPIPE, SIG_IGN);
    ls = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0); if (ls < 0) return 1;
    memset(&addr, 0, sizeof(addr)); addr.svm_family = AF_VSOCK; addr.svm_cid = VMADDR_CID_ANY; addr.svm_port = UPDATE_PORT;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 1) < 0) { close(ls); return 1; }
    for (;;) { int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC); if (fd < 0) { if (errno == EINTR) continue; break; } receive_stream(fd); close(fd); }
    close(ls); return 1;
}

static int watch_main(void)
{
    for (;;) {
        UpdateState s;
        if (read_state(&s) == 0 && !strcmp(s.state, "apply_requested")) {
            apply_update(s.txid);
        } else if (read_state(&s) == 0 && !strcmp(s.state, "activating")) {
            /* Power loss or a watchdog restart during activation cannot leave
             * live pointers half-switched: restore only pointers still owned
             * by this transaction. */
            rollback_update();
        } else if (read_state(&s) == 0 && !strcmp(s.state, "verified")) {
            /* An unsubmitted verified transaction is inert. */
            strcpy(s.state, "failed");
            snprintf(s.error, sizeof(s.error), "activation_request_missing");
            write_state(&s);
        } else if (read_state(&s) == 0 && (!strcmp(s.state, "reboot_pending") || !strcmp(s.state, "health_check"))) {
            if (!strcmp(s.state, "reboot_pending")) { strcpy(s.state, "health_check"); s.deadline = (uint64_t)time(NULL) + UPDATE_HEALTH_SECONDS; write_state(&s); }
            if (health_check() == 0) { strcpy(s.state, "committed"); s.error[0] = 0; write_state(&s); }
            else if (s.deadline && (uint64_t)time(NULL) >= s.deadline) rollback_update();
        }
        sleep(2);
    }
    return 1;
}

static int query_main(void)
{
    unsigned char *data = NULL; size_t len = 0; char version[96] = "unknown", graphics[96] = "unknown";
    if (file_read_limited(UPDATE_ROOT "/current/RELEASE", &data, &len, 4096) == 0) {
        char *p = strstr((char *)data, "version="); if (p) sscanf(p + 8, "%95[^\n]", version);
        p = strstr((char *)data, "graphics_version="); if (p) sscanf(p + 17, "%95[^\n]", graphics);
        free(data); data = NULL;
    }
    if (file_read_limited("/opt/wsl-mesa/current/GRAPHICS", &data, &len, 256) == 0) {
        sscanf((char *)data, "%95[0-9A-Za-z.+-]", graphics);
        free(data);
    }
    printf("guest_version:%s\ngraphics_version:%s\nguest_caps:update-v1,graphics-v1,health-v1\n", version, graphics);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--daemon")) return daemon_main();
    if (argc >= 2 && !strcmp(argv[1], "--watch")) return watch_main();
    if (argc >= 2 && !strcmp(argv[1], "--query")) return query_main();
    if (argc >= 2 && !strcmp(argv[1], "--health")) return health_main();
    if (argc >= 2 && !strcmp(argv[1], "--migrate")) return migrate_legacy_main();
    if (argc >= 2 && !strcmp(argv[1], "--begin") && argc == 5) return begin_update(argv[2], argv[3], argv[4]);
    if (argc >= 2 && !strcmp(argv[1], "--submit") && argc == 3) return submit_apply(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--apply") && argc == 3) return apply_update(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--rollback") && argc == 2) return rollback_update();
    if (argc >= 2 && !strcmp(argv[1], "--status") && argc == 3) { UpdateState s; if (read_state(&s) < 0 || strcmp(s.txid, argv[2])) return 1; printf("%s\n", s.state); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "--cancel") && argc == 3) {
        UpdateState s; char bundle[PATH_MAX], part[PATH_MAX]; int l = update_lock();
        if (l < 0 || read_state(&s) < 0 || strcmp(s.txid, argv[2])) { if (l >= 0) close(l); return 1; }
        snprintf(bundle, sizeof(bundle), "%s/%s.bundle", BUNDLE_ROOT, argv[2]);
        snprintf(part, sizeof(part), "%s/%s.bundle.part", BUNDLE_ROOT, argv[2]);
        unlink(bundle); unlink(part); strcpy(s.state, "failed"); strcpy(s.error, "cancelled"); write_state(&s); close(l); puts("ok"); return 0;
    }
    fprintf(stderr, "usage: %s --daemon|--watch|--query|--begin TXID SIZE SHA256|--submit TXID|--apply TXID|--status TXID|--cancel TXID|--rollback\n", argv[0]);
    return 2;
}
