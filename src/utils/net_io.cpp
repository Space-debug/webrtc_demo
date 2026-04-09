#include "webrtc/utils/net_io.h"

#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace webrtc_demo::utils {

void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

bool WriteAllWithPoll(int fd, const char* data, size_t len, int timeout_ms) {
    size_t off = 0;
    while (off < len) {
        const ssize_t w = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool RecvUntilNewline(int fd, std::string* first_line, std::string* rest, int timeout_ms) {
    std::string acc;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const size_t nl = acc.find('\n');
        if (nl != std::string::npos) {
            *first_line = acc.substr(0, nl);
            *rest = acc.substr(nl + 1);
            return true;
        }
        if (acc.size() > 65536) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const int left_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int slice = left_ms > 500 ? 500 : left_ms;
        if (slice < 1) {
            slice = 1;
        }
        const int pr = poll(&pfd, 1, slice);
        if (pr <= 0) {
            continue;
        }
        char buf[8192];
        const ssize_t rn = recv(fd, buf, sizeof(buf), 0);
        if (rn > 0) {
            acc.append(buf, static_cast<size_t>(rn));
        } else if (rn == 0) {
            return false;
        } else {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
    }
}

void ParseHostPort(const std::string& addr, std::string& host, uint16_t& port) {
    std::string s = addr;
    if (s.find("ws://") == 0) {
        s = s.substr(5);
    } else if (s.find("tcp://") == 0) {
        s = s.substr(6);
    }
    const size_t colon = s.find(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
    } else {
        host = s;
        port = 8765;
    }
}

}  // namespace webrtc_demo::utils

