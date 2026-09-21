/* prefetch_repo.c - see header.
 *
 * Pipeline:
 *   1. WinHTTP-download
 *      https://codeload.github.com/<owner>/<repo>/tar.gz/<ref>
 *      → <tmp>/repo.tar.gz
 *   2. Shell out to tar.exe (libarchive handles .tar.gz natively):
 *      `tar -xzf repo.tar.gz -C <tmp>` → <tmp>/<repo>-<ref>/...
 *   3. Copy each named subdir / file from the extracted tree to <out_dir>
 *      in the final layout. Renames where needed.
 *   4. Clean up <tmp>.
 *
 * No staging dance / atomic rename: <out_dir> is the per-VM staging extras
 * folder which is fresh for this VM-create.
 *
 * Uses HTTPS via WinHTTP — github.com requires TLS.
 */

#include "prefetch_repo.h"
#include "iso_patch_log.h"
#include "target_arch.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

#define PREFETCH_PATH_CAP 2048

/* ---- shared helpers (small; not extracted to a common header
       because the three prefetch modules each only need 2-3 of them) */

static int u_run_cmd(const wchar_t *cmdline)
{
    wchar_t mut[2048];
    wcsncpy_s(mut, 2048, cmdline, _TRUNCATE);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_err(L"prefetch-repo: CreateProcess failed: %lu (%s)",
                GetLastError(), cmdline);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

static BOOL u_mkdir_p(const wchar_t *path)
{
    wchar_t buf[PREFETCH_PATH_CAP];
    DWORD attrs;
    if (!path || !path[0] || wcslen(path) >= _countof(buf)) return FALSE;
    wcscpy_s(buf, _countof(buf), path);
    wchar_t *p = buf;
    if (wcslen(buf) >= 3 && buf[1] == L':' && buf[2] == L'\\') p = buf + 3;
    for (; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            if (buf[0] && !CreateDirectoryW(buf, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
            *p = L'\\';
        }
    }
    if (!CreateDirectoryW(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    attrs = GetFileAttributesW(buf);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static BOOL u_is_safe_repo_char(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
}

static BOOL u_validate_repo(const wchar_t *repo, wchar_t *repo_name,
                            size_t repo_name_cap)
{
    const wchar_t *slash;
    const wchar_t *p;
    size_t name_len;

    if (!repo || !repo[0] || wcslen(repo) >= PREFETCH_PATH_CAP) return FALSE;
    slash = wcschr(repo, L'/');
    if (!slash || slash == repo || !slash[1] || wcschr(slash + 1, L'/')) return FALSE;
    for (p = repo; *p; p++) {
        if (p == slash) continue;
        if (!u_is_safe_repo_char(*p)) return FALSE;
    }
    name_len = wcslen(slash + 1);
    if (name_len + 1 > repo_name_cap) return FALSE;
    wcscpy_s(repo_name, repo_name_cap, slash + 1);
    return TRUE;
}

static BOOL u_validate_ref(const wchar_t *ref)
{
    const wchar_t *p;
    size_t len;

    if (!ref || !ref[0] || wcslen(ref) >= PREFETCH_PATH_CAP) return FALSE;
    len = wcslen(ref);
    if (ref[0] == L'/' || ref[len - 1] == L'/' || wcsstr(ref, L"..")) return FALSE;
    for (p = ref; *p; p++) {
        if ((p[0] >= L'a' && p[0] <= L'z') ||
            (p[0] >= L'A' && p[0] <= L'Z') ||
            (p[0] >= L'0' && p[0] <= L'9') ||
            p[0] == L'.' || p[0] == L'_' || p[0] == L'-' || p[0] == L'/') continue;
        return FALSE;
    }
    return TRUE;
}

static BOOL u_is_commit_sha(const wchar_t *ref)
{
    const wchar_t *p;
    if (!ref || wcslen(ref) != 40) return FALSE;
    for (p = ref; *p; p++) {
        if (!((p[0] >= L'a' && p[0] <= L'f') ||
              (p[0] >= L'A' && p[0] <= L'F') ||
              (p[0] >= L'0' && p[0] <= L'9'))) return FALSE;
    }
    return TRUE;
}

static int u_write_source_version(const wchar_t *out_dir, const wchar_t *repo,
                                  const wchar_t *ref)
{
    wchar_t path[PREFETCH_PATH_CAP];
    wchar_t content[PREFETCH_PATH_CAP];
    char utf8[PREFETCH_PATH_CAP];
    HANDLE h;
    DWORD bytes, written;
    int n;

    swprintf_s(path, _countof(path), L"%s\\source-version", out_dir);
    swprintf_s(content, _countof(content),
               L"repo=%s\nref=%s\ncommit_sha=%s\n",
               repo, ref, u_is_commit_sha(ref) ? ref : L"unresolved");
    n = WideCharToMultiByte(CP_UTF8, 0, content, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 1) return -1;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_err(L"prefetch-repo: cannot write %s: %lu", path, GetLastError());
        return -1;
    }
    bytes = (DWORD)(n - 1);
    if (!WriteFile(h, utf8, bytes, &written, NULL) || written != bytes) {
        log_err(L"prefetch-repo: cannot write %s: %lu", path, GetLastError());
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

static int http_download_secure(const wchar_t *host, INTERNET_PORT port,
                                const wchar_t *path, const wchar_t *out_path)
{
    HINTERNET hSession = WinHttpOpen(L"AppSandbox-iso-patch/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { log_err(L"prefetch-repo: WinHttpOpen failed: %lu", GetLastError()); return -1; }
    int rc = -1;
    HINTERNET hConn = WinHttpConnect(hSession, host, port, 0);
    if (!hConn) {
        log_err(L"prefetch-repo: WinHttpConnect %s:%u failed: %lu", host, port, GetLastError());
        goto cleanup_sess;
    }
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        log_err(L"prefetch-repo: WinHttpOpenRequest failed: %lu", GetLastError());
        goto cleanup_conn;
    }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log_err(L"prefetch-repo: SendRequest failed: %lu", GetLastError());
        goto cleanup_req;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        log_err(L"prefetch-repo: ReceiveResponse failed: %lu", GetLastError());
        goto cleanup_req;
    }
    /* codeload.github.com redirects (302) onto an objects.githubusercontent.com
     * CDN URL; without WINHTTP_OPTION_DISABLE_FEATURE the auto-follow happens
     * silently, but for completeness handle a non-2xx ourselves. */
    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status / 100 != 2) {
        log_err(L"prefetch-repo: HTTP %lu", status);
        goto cleanup_req;
    }

    HANDLE hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_err(L"prefetch-repo: CreateFileW(%s) failed: %lu", out_path, GetLastError());
        goto cleanup_req;
    }
    BYTE buf[65536];
    DWORD total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { CloseHandle(hFile); goto cleanup_req; }
        if (avail == 0) break;
        DWORD n = avail > sizeof(buf) ? sizeof(buf) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, n, &read)) { CloseHandle(hFile); goto cleanup_req; }
        if (read == 0) break;
        DWORD wr = 0;
        if (!WriteFile(hFile, buf, read, &wr, NULL) || wr != read) {
            CloseHandle(hFile); goto cleanup_req;
        }
        total += read;
    }
    CloseHandle(hFile);
    log_msg(L"prefetch-repo: downloaded %lu bytes", total);
    rc = 0;

cleanup_req:  WinHttpCloseHandle(hReq);
cleanup_conn: WinHttpCloseHandle(hConn);
cleanup_sess: WinHttpCloseHandle(hSession);
    return rc;
}

/* Copy one regular file. Creates intermediate dirs in dst path. */
static int u_cp_file(const wchar_t *src, const wchar_t *dst)
{
    wchar_t parent[PREFETCH_PATH_CAP];
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"prefetch-repo: required file missing: %s", src);
        return -1;
    }
    if (wcslen(dst) >= _countof(parent)) return -1;
    wcscpy_s(parent, _countof(parent), dst);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = 0;
        if (!u_mkdir_p(parent)) {
            log_err(L"prefetch-repo: cannot create destination directory: %s", parent);
            return -1;
        }
    }
    if (!CopyFileW(src, dst, FALSE)) {
        log_err(L"prefetch-repo: copy %s -> %s failed: %lu",
                src, dst, GetLastError());
        return -1;
    }
    return 0;
}

/* Recursive copy: src_dir/* -> dst_dir/. Creates dst_dir. */
static int u_cp_tree(const wchar_t *src_dir, const wchar_t *dst_dir)
{
    wchar_t spec[PREFETCH_PATH_CAP];
    DWORD src_attrs = GetFileAttributesW(src_dir);
    if (src_attrs == INVALID_FILE_ATTRIBUTES ||
        (src_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        !u_mkdir_p(dst_dir)) {
        log_err(L"prefetch-repo: required directory missing: %s", src_dir);
        return -1;
    }
    swprintf_s(spec, _countof(spec), L"%s\\*", src_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int count = 0;
    int failed = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        swprintf_s(s, _countof(s), L"%s\\%s", src_dir, fd.cFileName);
        swprintf_s(d, _countof(d), L"%s\\%s", dst_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int sub = u_cp_tree(s, d);
            if (sub < 0) failed = 1;
            else count += sub;
        } else {
            if (u_cp_file(s, d) == 0) count++;
            else failed = 1;
        }
    } while (FindNextFileW(h, &fd));
    if (GetLastError() != ERROR_NO_MORE_FILES) failed = 1;
    FindClose(h);
    return failed ? -1 : count;
}

/* ====================================================================
 * Main flow
 * ==================================================================== */

int do_prefetch_repo(const wchar_t *repo, const wchar_t *ref,
                     const wchar_t *out_dir)
{
    wchar_t repo_name[MAX_PATH], tmp[MAX_PATH] = L"";
    int rc = -1;

    if (!u_validate_repo(repo, repo_name, _countof(repo_name)) ||
        !u_validate_ref(ref) || !out_dir || !out_dir[0]) {
        log_err(L"prefetch-repo: invalid repo/ref/out-dir");
        return -1;
    }
    log_msg(L"guest_source repo=%s ref=%s commit_sha=%s",
            repo, ref, u_is_commit_sha(ref) ? ref : L"unresolved");
    log_msg(L"prefetch-repo: repo=%s ref=%s out=%s", repo, ref, out_dir);

    /* Temp dir for download + extract. Wiped on completion. */
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return -1;
    swprintf_s(tmp + wcslen(tmp), MAX_PATH - wcslen(tmp),
               L"isopatch-repo-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
        if (!CreateDirectoryW(tmp, NULL)) {
            log_err(L"prefetch-repo: cannot create temp directory %s: %lu",
                    tmp, GetLastError());
            goto cleanup;
        }
    }

    /* ---- 1. Download tarball via codeload.github.com (HTTPS). ---- */
    wchar_t tgz[MAX_PATH];
    swprintf_s(tgz, MAX_PATH, L"%s\\repo.tar.gz", tmp);
    {
        wchar_t path[PREFETCH_PATH_CAP];
        swprintf_s(path, 1024,
            L"/%s/tar.gz/%s", repo, ref);
        log_msg(L"prefetch-repo: GET https://codeload.github.com%s", path);
        if (http_download_secure(L"codeload.github.com", 443, path, tgz) != 0) {
            log_err(L"prefetch-repo: download failed");
            goto cleanup;
        }
    }

    /* ---- 2. Extract via tar.exe. ---- */
    {
        wchar_t sys_dir[MAX_PATH], cmd[2048];
        GetSystemDirectoryW(sys_dir, MAX_PATH);
        swprintf_s(cmd, 2048,
            L"\"%s\\tar.exe\" -xzf \"%s\" -C \"%s\"",
            sys_dir, tgz, tmp);
        if (u_run_cmd(cmd) != 0) {
            log_err(L"prefetch-repo: tar -xzf failed");
            goto cleanup;
        }
    }

    /* GitHub names the top-level dir "<repo>-<ref>". Find it
     * by repo name, in case '/' in a branch ref became '-'. */
    wchar_t extracted_root[PREFETCH_PATH_CAP] = L"";
    {
        wchar_t spec[PREFETCH_PATH_CAP];
        swprintf_s(spec, _countof(spec), L"%s\\%s-*", tmp, repo_name);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(spec, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;
                swprintf_s(extracted_root, _countof(extracted_root), L"%s\\%s", tmp, fd.cFileName);
                break;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!extracted_root[0]) {
            log_err(L"prefetch-repo: no %s-* dir under %s", repo_name, tmp);
            goto cleanup;
        }
    }
    log_msg(L"prefetch-repo: extracted to %s", extracted_root);

    /* ---- 3. Copy each piece into the final layout under <out_dir>. ----
     *
     * The layout matches what generate_vhdx_manifest_ubuntu walks and
     * what firstboot.sh expects at /opt/appsandbox/<...> in the guest.
     */
    if (!u_mkdir_p(out_dir)) {
        log_err(L"prefetch-repo: cannot create output directory %s", out_dir);
        goto cleanup;
    }

    /* agent-src/ */
    {
        wchar_t agent_src[PREFETCH_PATH_CAP], dst[PREFETCH_PATH_CAP];
        swprintf_s(agent_src, _countof(agent_src),
                   L"%s\\tools\\linux\\agent", extracted_root);
        const wchar_t *files[] = {
            L"appsandbox-agent.c", L"appsandbox-audio.c",
            L"appsandbox-clipboard.c", L"appsandbox-display.c",
            L"appsandbox-input.c", L"mutter-displayconfig-helper.py",
            L"Makefile"
        };
        wchar_t out_agent[PREFETCH_PATH_CAP];
        swprintf_s(out_agent, _countof(out_agent), L"%s\\agent-src", out_dir);
        if (!u_mkdir_p(out_agent)) goto cleanup;
        for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
            wchar_t s[PREFETCH_PATH_CAP];
            swprintf_s(s, _countof(s), L"%s\\%s", agent_src, files[i]);
            swprintf_s(dst, _countof(dst), L"%s\\%s", out_agent, files[i]);
            if (u_cp_file(s, dst) != 0) goto cleanup;
        }
        wchar_t core_src[PREFETCH_PATH_CAP], out_core[PREFETCH_PATH_CAP];
        swprintf_s(core_src, _countof(core_src), L"%s\\src\\core", extracted_root);
        swprintf_s(out_core, _countof(out_core), L"%s\\core", out_agent);
        if (!u_mkdir_p(out_core)) goto cleanup;
        {
            const wchar_t *core_files[] = {
                L"protocol.h", L"display_protocol.h",
                L"display_snapshot.h", L"display_fb_state.h"
            };
            for (int i = 0; i < (int)(sizeof(core_files) / sizeof(core_files[0])); i++) {
                wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
                swprintf_s(s, _countof(s), L"%s\\%s", core_src, core_files[i]);
                swprintf_s(d, _countof(d), L"%s\\%s", out_core, core_files[i]);
                if (u_cp_file(s, d) != 0) goto cleanup;
            }
        }
        wchar_t gnome_src[PREFETCH_PATH_CAP];
        swprintf_s(gnome_src, _countof(gnome_src),
                   L"%s\\gnome\\appsandbox-pointer@appsandbox", agent_src);
        {
            const wchar_t *gnome_files[] = { L"metadata.json", L"extension.js" };
            for (int i = 0; i < 2; i++) {
                wchar_t s[PREFETCH_PATH_CAP];
                swprintf_s(s, _countof(s), L"%s\\%s", gnome_src, gnome_files[i]);
                swprintf_s(dst, _countof(dst),
                           L"%s\\gnome\\appsandbox-pointer@appsandbox\\%s", out_agent, gnome_files[i]);
                if (u_cp_file(s, dst) != 0) goto cleanup;
            }
        }
        log_msg(L"prefetch-repo: staged agent-src/");
    }

    /* asb_drm-src/: full asb_drm tree */
    {
        wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        swprintf_s(s, _countof(s), L"%s\\tools\\linux\\asb_drm", extracted_root);
        swprintf_s(d, _countof(d), L"%s\\asb_drm-src", out_dir);
        int c = u_cp_tree(s, d);
        if (c <= 0) goto cleanup;
        log_msg(L"prefetch-repo: staged asb_drm-src/ (%d files)", c);
    }

    /* dxgkrnl-src/: contents of tools/linux/dxgkrnl/src/ */
    {
        wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        swprintf_s(s, _countof(s), L"%s\\tools\\linux\\dxgkrnl\\src", extracted_root);
        swprintf_s(d, _countof(d), L"%s\\dxgkrnl-src", out_dir);
        int c = u_cp_tree(s, d);
        if (c <= 0) goto cleanup;
        log_msg(L"prefetch-repo: staged dxgkrnl-src/ (%d files)", c);
    }

    /* systemd/: service files + asb-evict-simpledrm + 2 modules-load.d */
    {
        wchar_t systemd_dst[PREFETCH_PATH_CAP], s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        swprintf_s(systemd_dst, _countof(systemd_dst), L"%s\\systemd", out_dir);
        if (!u_mkdir_p(systemd_dst)) goto cleanup;

        const wchar_t *units[] = {
            L"appsandbox-agent.service", L"appsandbox-audio.service",
            L"appsandbox-clipboard.service", L"appsandbox-display.service",
            L"appsandbox-input.service", L"appsandbox-display-helper.service",
            L"appsandbox-firstboot.service"
        };
        for (int i = 0; i < (int)(sizeof(units) / sizeof(units[0])); i++) {
            swprintf_s(s, _countof(s),
                L"%s\\tools\\linux\\agent\\systemd\\%s", extracted_root, units[i]);
            swprintf_s(d, _countof(d), L"%s\\%s", systemd_dst, units[i]);
            if (u_cp_file(s, d) != 0) goto cleanup;
        }
        /* asb-evict-simpledrm: rename from systemd-asb-evict-simpledrm.service */
        swprintf_s(s, _countof(s),
            L"%s\\tools\\linux\\asb_drm\\systemd-asb-evict-simpledrm.service",
            extracted_root);
        swprintf_s(d, _countof(d), L"%s\\asb-evict-simpledrm.service", systemd_dst);
        if (u_cp_file(s, d) != 0) goto cleanup;

        /* modules-load.d configs (renamed to match what setup expects). */
        swprintf_s(s, _countof(s),
            L"%s\\tools\\linux\\agent\\modules-load.d-snd-aloop.conf", extracted_root);
        swprintf_s(d, _countof(d), L"%s\\modules-load.d-snd-aloop.conf", systemd_dst);
        if (u_cp_file(s, d) != 0) goto cleanup;

        swprintf_s(s, _countof(s),
            L"%s\\tools\\linux\\asb_drm\\modules-load.d-asb_drm.conf", extracted_root);
        swprintf_s(d, _countof(d), L"%s\\modules-load.d-asb_drm.conf", systemd_dst);
        if (u_cp_file(s, d) != 0) goto cleanup;

        log_msg(L"prefetch-repo: staged systemd/");
    }

    /* modprobe.d-asb_drm.conf at extras root */
    {
        wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        swprintf_s(s, _countof(s),
            L"%s\\tools\\linux\\asb_drm\\modprobe.d-asb_drm.conf", extracted_root);
        swprintf_s(d, _countof(d), L"%s\\modprobe.d-asb_drm.conf", out_dir);
        if (u_cp_file(s, d) != 0) goto cleanup;
    }

    /* wsl-mesa pieces */
    {
        const wchar_t *files[] = {
            L"50-appsandbox-gpu",
            L"org.gnome.Shell-no-gpu.conf",
            L"appsandbox-gpu"
        };
        wchar_t s[PREFETCH_PATH_CAP], d[PREFETCH_PATH_CAP];
        for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
            swprintf_s(s, _countof(s),
                L"%s\\tools\\linux\\wsl-mesa\\%s", extracted_root, files[i]);
            swprintf_s(d, _countof(d), L"%s\\%s", out_dir, files[i]);
            if (u_cp_file(s, d) != 0) goto cleanup;
        }
        /* wsl-mesa.tar.zst from the prebuilt dir (large, ~21 MB). */
        swprintf_s(s, _countof(s),
            L"%s\\tools\\linux\\wsl-mesa\\prebuilt\\ubuntu-26.04-" IP_DEB_ARCH L"\\wsl-mesa.tar.zst",
            extracted_root);
        swprintf_s(d, _countof(d), L"%s\\wsl-mesa.tar.zst", out_dir);
        if (u_cp_file(s, d) != 0) goto cleanup;
        log_msg(L"prefetch-repo: staged wsl-mesa pieces");
    }

    if (u_write_source_version(out_dir, repo, ref) != 0) goto cleanup;

    /* Clean up temp. */
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
    }

    log_msg(L"prefetch-repo: OK -> %s", out_dir);
    return 0;

cleanup:
    if (tmp[0]) {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
    }
    return rc;
}
