#ifndef WEBRTC_DEMO_UTILS_NET_IO_H_
#define WEBRTC_DEMO_UTILS_NET_IO_H_

#include <cstdint>
#include <cstddef>
#include <string>

namespace webrtc_demo::utils {

// Internal utility: set socket/file descriptor non-blocking mode.
void SetNonBlocking(int fd);
// Internal utility: send full buffer on non-blocking socket with poll-based wait.
bool WriteAllWithPoll(int fd, const char* data, size_t len, int timeout_ms);
// Internal utility: receive until first newline with timeout budget.
bool RecvUntilNewline(int fd, std::string* first_line, std::string* rest, int timeout_ms);
// Internal utility: parse "host:port"/"ws://host:port"/"tcp://host:port".
void ParseHostPort(const std::string& addr, std::string& host, uint16_t& port);

}  // namespace webrtc_demo::utils

#endif  // WEBRTC_DEMO_UTILS_NET_IO_H_

