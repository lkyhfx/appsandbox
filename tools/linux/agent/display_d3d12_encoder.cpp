/* SPDX-License-Identifier: MIT
 * Production service wrapper around the validated D3D12 encode data plane.
 * The encoder owns two private SOCK_SEQPACKET listeners: Mesa publishes
 * shared resources on the publisher socket and appsandbox-display consumes
 * ASVC/ASVE packets on the output socket.
 */
#define ASB_D3D12_VIDEO_NO_MAIN
#include "d3d12-video-encode-probe.cpp"
#undef ASB_D3D12_VIDEO_NO_MAIN

#include "display_d3d12_encoder.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

int make_listener(const char *path)
{
    if (std::strlen(path) >= sizeof(((sockaddr_un *)nullptr)->sun_path))
        return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    unlink(path);
    mode_t old_mask = umask(0007);
    int rc = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    umask(old_mask);
    if (rc != 0 || chmod(path, 0660) != 0 || listen(fd, 4) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

bool trusted_peer(int fd)
{
    ucred peer = {};
    socklen_t size = sizeof(peer);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
           (peer.uid == 0 || peer.uid == geteuid());
}

void output_pump(int listener, int encoded_fd)
{
    int client = -1;
    std::vector<unsigned char> packet(
        sizeof(AsbEncodedVideoFrame) + ASB_DISPLAY_MAX_VIDEO_FRAME);
    std::vector<unsigned char> config;
    for (;;) {
        pollfd fds[2] = {{encoded_fd, POLLIN, 0}, {listener, POLLIN, 0}};
        int ready;
        do { ready = poll(fds, 2, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (fds[1].revents & POLLIN) {
            int next = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (next >= 0) {
                if (!trusted_peer(next)) close(next);
                else {
                    if (client >= 0) close(client);
                    client = next;
                    std::fprintf(stderr,
                                 "display_protocol=v2 output_client_connected=1\n");
                    if (!config.empty() &&
                        send(client, config.data(), config.size(), MSG_NOSIGNAL) !=
                            static_cast<ssize_t>(config.size())) {
                        close(client); client = -1;
                    }
                }
            }
        }
        if (fds[0].revents & POLLIN) {
            ssize_t bytes = recv(encoded_fd, packet.data(), packet.size(), 0);
            if (bytes <= 0) break;
            uint32_t magic = 0;
            if (bytes >= static_cast<ssize_t>(sizeof(magic)))
                std::memcpy(&magic, packet.data(), sizeof(magic));
            if (magic == ASB_DISPLAY_VIDEO_CONFIG_MAGIC)
                config.assign(packet.begin(), packet.begin() + bytes);
            if (magic == ASB_DISPLAY_VIDEO_CONFIG_MAGIC &&
                bytes >= static_cast<ssize_t>(sizeof(AsbEncodedVideoConfig))) {
                AsbEncodedVideoConfig video_config = {};
                std::memcpy(&video_config, packet.data(), sizeof(video_config));
                std::fprintf(stderr,
                             "display_protocol=v2 display_backend=hevc-d3d12 fallback=0 generation=%llu mode=%ux%u fps=%u/%u\n",
                             static_cast<unsigned long long>(video_config.generation),
                             video_config.width, video_config.height,
                             video_config.fps_num, video_config.fps_den);
            }
            if (client >= 0 &&
                send(client, packet.data(), bytes, MSG_NOSIGNAL) != bytes) {
                close(client); client = -1;
            }
            /* With no host, packets are intentionally drained and dropped so
             * encoder backpressure can never stall Mutter. */
        }
    }
    if (client >= 0) close(client);
    close(encoded_fd);
}

} // namespace

int main(int argc, char **argv)
{
    const char *publisher_path = argc > 1 ? argv[1] : ASB_D3D12_PUBLISHER_SOCKET;
    const char *output_path = argc > 2 ? argv[2] : ASB_D3D12_OUTPUT_SOCKET;
    int publisher_listener = make_listener(publisher_path);
    int output_listener = make_listener(output_path);
    if (publisher_listener < 0 || output_listener < 0) {
        std::fprintf(stderr, "display-d3d12: listener setup failed: %s\n",
                     std::strerror(errno));
        if (publisher_listener >= 0) close(publisher_listener);
        if (output_listener >= 0) close(output_listener);
        unlink(publisher_path);
        unlink(output_path);
        return 1;
    }
    std::fprintf(stderr, "display-d3d12: ready publisher=%s output=%s\n",
                 publisher_path, output_path);

    for (;;) {
        int publisher = accept4(publisher_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (publisher < 0) { if (errno == EINTR) continue; break; }
        if (!trusted_peer(publisher)) { close(publisher); continue; }

        int output_pair[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                       output_pair) != 0) {
            close(publisher);
            continue;
        }
        std::thread pump(output_pump, output_listener, output_pair[1]);

        char output_text[32];
        std::snprintf(output_text, sizeof(output_text), "%d", output_pair[0]);
        setenv("ASB_D3D12_ENCODED_FD", output_text, 1);
        const char *mode_text = std::getenv("APPSANDBOX_DISPLAY_CODEC_MODE");
        EncodeProbeMode mode = EncodeProbeMode::Hevc420;
        if (mode_text && !std::strcmp(mode_text, "hevc444"))
            mode = EncodeProbeMode::Hevc444;
        else if (mode_text && std::strcmp(mode_text, "hevc420")) {
            std::fprintf(stderr,
                         "display_protocol=v2 fallback=1 fallback_reason=unknown-codec-mode\n");
            close(publisher);
            shutdown(output_pair[0], SHUT_RDWR);
            close(output_pair[0]);
            pump.join();
            continue;
        }
        bool ok = encode_consumer_main(publisher, mode);
        unsetenv("ASB_D3D12_ENCODED_FD");
        close(publisher);
        shutdown(output_pair[0], SHUT_RDWR);
        close(output_pair[0]);
        pump.join();
        std::fprintf(stderr,
                     "display_protocol=v2 display_backend=%s session_ended=1 fallback_required=%u status=%s\n",
                     mode == EncodeProbeMode::Hevc444 ? "hevc444-d3d12" : "hevc420-d3d12",
                     ok ? 0U : 1U, ok ? "complete" : "failed");
    }

    close(publisher_listener);
    close(output_listener);
    unlink(publisher_path);
    unlink(output_path);
    return 1;
}
