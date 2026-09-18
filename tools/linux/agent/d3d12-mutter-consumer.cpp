/* SPDX-License-Identifier: MIT
 *
 * Socket-facing encoder consumer for the isolated Mutter probe.
 *
 * The actual D3D12 Video Encode implementation deliberately comes from the
 * already validated d3d12-video-encode-probe.cpp.  This process only adds a
 * Unix-domain SOCK_SEQPACKET listener so the patched Mutter/Mesa producer can
 * be the producer side of the same SCM_RIGHTS/eventfd protocol.
 */

#define ASB_D3D12_VIDEO_NO_MAIN
#include "d3d12-video-encode-probe.cpp"
#undef ASB_D3D12_VIDEO_NO_MAIN

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

static int listen_socket(const char *path)
{
    if (std::strlen(path) >= sizeof(((sockaddr_un *)nullptr)->sun_path)) {
        std::fprintf(stderr, "FAIL stage=mutter-socket-path-too-long\n");
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::fprintf(stderr, "FAIL stage=mutter-socket errno=%d (%s)\n",
                     errno, std::strerror(errno));
        return -1;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    unlink(path);
    if (bind(fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 || listen(fd, 1) != 0) {
        std::fprintf(stderr, "FAIL stage=mutter-socket-bind errno=%d (%s)\n",
                     errno, std::strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s SOCKET_PATH\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const int listener = listen_socket(path);
    if (listener < 0)
        return 1;
    std::printf("LISTENING stage=mutter-consumer socket=%s\n", path);
    const int control = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    close(listener);
    if (control < 0) {
        std::fprintf(stderr, "FAIL stage=mutter-socket-accept errno=%d (%s)\n",
                     errno, std::strerror(errno));
        unlink(path);
        return 1;
    }
    const bool ok = encode_consumer_main(control);
    close(control);
    unlink(path);
    return ok ? 0 : 1;
}
