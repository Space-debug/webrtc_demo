#include "webrtc/signaling/client.h"
#include "webrtc/utils/json_utils.h"
#include "webrtc/utils/net_io.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdlib>

namespace webrtc_demo {
namespace {

bool SignalingTimingTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("WEBRTC_DEMO_SIGNALING_TIMING_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void TraceSig(const std::string& msg) {
    if (!SignalingTimingTraceEnabled()) {
        return;
    }
    std::cout << "[SIG_TIMING] t_us=" << NowUs() << " " << msg << std::endl;
}

std::string ExtractTypeForTrace(const std::string& line) {
    return utils::ExtractJsonString(line, "type");
}

}  // namespace

SignalingClient::SignalingClient(const std::string& server_addr, const std::string& role,
                                const std::string& stream_id)
    : server_addr_(server_addr), role_(role) {
    utils::ParseHostPort(server_addr, host_, port_);
    stream_id_ = stream_id.empty() ? "livestream" : stream_id;
}

SignalingClient::~SignalingClient() {
    Stop();
}

bool SignalingClient::Connect() {
    std::cout << "[Signaling] Connecting to " << host_ << ":" << port_ << " (role=" << role_ << ")..."
              << std::endl;
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        if (on_error_) on_error_(std::string("socket: ") + strerror(errno));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        if (on_error_) on_error_("invalid host: " + host_);
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    if (connect(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (on_error_) on_error_(std::string("connect: ") + strerror(errno));
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }
    std::cout << "[Signaling] TCP connected" << std::endl;

    std::ostringstream reg;
    reg << "{\"type\":\"register\",\"role\":\"" << role_ << "\",\"stream_id\":\""
        << utils::EscapeJsonString(stream_id_) << "\"}\n";
    std::string msg = reg.str();
    if (send(sock_fd_, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size())) {
        if (on_error_) on_error_("send register failed");
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }
    std::cout << "[Signaling] Registered (role=" << role_ << ")" << std::endl;
    return true;
}

bool SignalingClient::Start() {
    if (running_) return true;
    if (!Connect()) return false;
    running_ = true;
    reader_thread_ = std::make_unique<std::thread>(&SignalingClient::ReaderLoop, this);
    return true;
}

void SignalingClient::Stop() {
    running_ = false;
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
        close(sock_fd_);
        sock_fd_ = -1;
    }
    if (reader_thread_ && reader_thread_->joinable()) {
        reader_thread_->join();
        reader_thread_.reset();
    }
}

void SignalingClient::SendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (sock_fd_ < 0) {
        return;
    }
    const std::string msg = line + "\n";
    const std::string type = ExtractTypeForTrace(line);
    TraceSig("send begin type=" + type + " bytes=" + std::to_string(msg.size()));
    size_t off = 0;
    while (off < msg.size()) {
        const ssize_t sent = send(sock_fd_, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
        if (sent > 0) {
            off += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (on_error_) {
            on_error_(std::string("send signaling failed: ") + strerror(errno));
        }
        break;
    }
    TraceSig("send done type=" + type + " sent=" + std::to_string(off));
}

std::string SignalingClient::ResolveTargetPeer(const std::string& to_peer_id) const {
    if (!to_peer_id.empty()) return to_peer_id;
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return last_remote_peer_id_;
}

void SignalingClient::SendOffer(const std::string& sdp, const std::string& to_peer_id) {
    std::string target = ResolveTargetPeer(to_peer_id);
    if (role_ == "publisher" && target.empty()) {
        std::cerr << "[Signaling] Ignoring offer without subscriber target" << std::endl;
        return;
    }
    std::ostringstream oss;
    oss << "{\"type\":\"offer\"";
    if (!target.empty()) {
        oss << ",\"to\":\"" << utils::EscapeJsonString(target) << "\"";
    }
    oss << ",\"sdp\":\"" << utils::EscapeJsonString(sdp) << "\"";
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::SendAnswer(const std::string& sdp, const std::string& to_peer_id) {
    std::string target = ResolveTargetPeer(to_peer_id);
    std::ostringstream oss;
    oss << "{\"type\":\"answer\"";
    if (!target.empty()) {
        oss << ",\"to\":\"" << utils::EscapeJsonString(target) << "\"";
    }
    oss << ",\"sdp\":\"" << utils::EscapeJsonString(sdp) << "\"";
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::SendIceCandidate(const std::string& mid, int mline_index,
                                       const std::string& candidate,
                                       const std::string& to_peer_id) {
    std::string target = ResolveTargetPeer(to_peer_id);
    std::ostringstream oss;
    oss << "{\"type\":\"ice\"";
    if (!target.empty()) {
        oss << ",\"to\":\"" << utils::EscapeJsonString(target) << "\"";
    }
    oss << ",\"mid\":\"" << utils::EscapeJsonString(mid) << "\",\"mlineIndex\":" << mline_index
        << ",\"candidate\":\"" << utils::EscapeJsonString(candidate);
    oss << "\"}";
    SendLine(oss.str());
}

void SignalingClient::ReaderLoop() {
    std::string buf;
    char tmp[65536];
    while (running_ && sock_fd_ >= 0) {
        ssize_t n = recv(sock_fd_, tmp, sizeof(tmp) - 1, 0);
        if (n > 0) {
            tmp[n] = '\0';
            buf += tmp;
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                TraceSig("recv line bytes=" + std::to_string(line.size()));
                ParseAndDispatch(line);
            }
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
}

void SignalingClient::ParseAndDispatch(const std::string& line) {
    if (line.empty()) return;
    if (line.find("\"type\":\"register\"") != std::string::npos) return;

    std::string type = utils::ExtractJsonString(line, "type");
    std::string from = utils::ExtractJsonString(line, "from");
    TraceSig("dispatch type=" + type + " from=" + (from.empty() ? std::string("-") : from));
    if (!from.empty()) {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        last_remote_peer_id_ = from;
    }

    if (type == "welcome") {
        std::string id = utils::ExtractJsonString(line, "id");
        std::lock_guard<std::mutex> lock(peer_mutex_);
        self_peer_id_ = id;
        return;
    }

    if (type == "subscriber_join") {
        TraceSig("callback subscriber_join");
        if (on_subscriber_join_) on_subscriber_join_(from);
        return;
    }
    if (type == "subscriber_leave") {
        TraceSig("callback subscriber_leave");
        if (on_subscriber_leave_) on_subscriber_leave_(from);
        return;
    }

    if (type == "answer") {
        std::string sdp = utils::ExtractJsonString(line, "sdp");
        TraceSig("callback answer sdp_len=" + std::to_string(sdp.size()));
        if (on_answer_) on_answer_(from, "answer", sdp);
        return;
    }
    if (type == "offer") {
        std::string sdp = utils::ExtractJsonString(line, "sdp");
        TraceSig("callback offer sdp_len=" + std::to_string(sdp.size()));
        if (on_offer_) on_offer_(from, "offer", sdp);
        return;
    }
    if (type == "ice") {
        std::string mid = utils::ExtractJsonString(line, "mid");
        std::string candidate = utils::ExtractJsonString(line, "candidate");
        int mline = utils::ExtractJsonInt(line, "mlineIndex", 0);
        TraceSig("callback ice mid=" + mid + " cand_len=" + std::to_string(candidate.size()));
        if (on_ice_ && !candidate.empty()) on_ice_(from, mid, mline, candidate);
    }
}

}  // namespace webrtc_demo
