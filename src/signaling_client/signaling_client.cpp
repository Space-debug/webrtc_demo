#include "signaling_client.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <sstream>

namespace webrtc_demo {

namespace {

static std::string EscapeJsonString(const std::string& input) {
    std::ostringstream oss;
    for (char c : input) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else if (c == '\r') oss << "\\r";
        else oss << c;
    }
    return oss.str();
}

static std::string ExtractJsonString(const std::string& line, const std::string& key) {
    std::string token = "\"" + key + "\":\"";
    size_t p = line.find(token);
    if (p == std::string::npos) return "";
    p += token.size();
    std::string out;
    for (size_t i = p; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char n = line[i + 1];
            if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else out += n;
            ++i;
            continue;
        }
        if (line[i] == '"') break;
        out += line[i];
    }
    return out;
}

static int ExtractJsonInt(const std::string& line, const std::string& key, int fallback = 0) {
    std::string token = "\"" + key + "\":";
    size_t p = line.find(token);
    if (p == std::string::npos) return fallback;
    p += token.size();
    return std::atoi(line.c_str() + p);
}

}  // namespace

static void ParseHostPort(const std::string& addr, std::string& host, uint16_t& port) {
    std::string s = addr;
    if (s.find("ws://") == 0) s = s.substr(5);
    else if (s.find("tcp://") == 0) s = s.substr(6);
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
    } else {
        host = s;
        port = 8765;
    }
}

SignalingClient::SignalingClient(const std::string& server_addr, const std::string& role,
                                const std::string& stream_id)
    : server_addr_(server_addr), role_(role) {
    ParseHostPort(server_addr, host_, port_);
    stream_id_ = stream_id.empty() ? "livestream" : stream_id;
}

SignalingClient::~SignalingClient() {
    Stop();
}

bool SignalingClient::Connect() {
    std::cout << "[Signaling] 连接 " << host_ << ":" << port_ << " (role=" << role_ << ")..." << std::endl;
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
    std::cout << "[Signaling] TCP 连接成功" << std::endl;

    std::ostringstream reg;
    reg << "{\"type\":\"register\",\"role\":\"" << role_ << "\",\"stream_id\":\""
        << EscapeJsonString(stream_id_) << "\"}\n";
    std::string msg = reg.str();
    if (send(sock_fd_, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size())) {
        if (on_error_) on_error_("send register failed");
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }
    std::cout << "[Signaling] 注册成功 (role=" << role_ << ")" << std::endl;
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
    if (sock_fd_ < 0) return;
    std::string msg = line + "\n";
    ssize_t sent = send(sock_fd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    (void)sent;
}

std::string SignalingClient::ResolveTargetPeer(const std::string& to_peer_id) const {
    if (!to_peer_id.empty()) return to_peer_id;
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return last_remote_peer_id_;
}

void SignalingClient::SendOffer(const std::string& sdp, const std::string& to_peer_id) {
    std::string target = ResolveTargetPeer(to_peer_id);
    if (role_ == "publisher" && target.empty()) {
        std::cerr << "[Signaling] 忽略未指定目标订阅者的 offer" << std::endl;
        return;
    }
    std::ostringstream oss;
    oss << "{\"type\":\"offer\"";
    if (!target.empty()) {
        oss << ",\"to\":\"" << EscapeJsonString(target) << "\"";
    }
    oss << ",\"sdp\":\"" << EscapeJsonString(sdp) << "\"";
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::SendAnswer(const std::string& sdp, const std::string& to_peer_id) {
    std::string target = ResolveTargetPeer(to_peer_id);
    std::ostringstream oss;
    oss << "{\"type\":\"answer\"";
    if (!target.empty()) {
        oss << ",\"to\":\"" << EscapeJsonString(target) << "\"";
    }
    oss << ",\"sdp\":\"" << EscapeJsonString(sdp) << "\"";
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
        oss << ",\"to\":\"" << EscapeJsonString(target) << "\"";
    }
    oss << ",\"mid\":\"" << EscapeJsonString(mid) << "\",\"mlineIndex\":" << mline_index
        << ",\"candidate\":\"" << EscapeJsonString(candidate);
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

    std::string type = ExtractJsonString(line, "type");
    std::string from = ExtractJsonString(line, "from");
    if (!from.empty()) {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        last_remote_peer_id_ = from;
    }

    if (type == "welcome") {
        std::string id = ExtractJsonString(line, "id");
        std::lock_guard<std::mutex> lock(peer_mutex_);
        self_peer_id_ = id;
        return;
    }

    if (type == "subscriber_join") {
        if (on_subscriber_join_) on_subscriber_join_(from);
        return;
    }
    if (type == "subscriber_leave") {
        if (on_subscriber_leave_) on_subscriber_leave_(from);
        return;
    }

    if (type == "answer") {
        std::string sdp = ExtractJsonString(line, "sdp");
        if (on_answer_) on_answer_(from, "answer", sdp);
        return;
    }
    if (type == "offer") {
        std::string sdp = ExtractJsonString(line, "sdp");
        if (on_offer_) on_offer_(from, "offer", sdp);
        return;
    }
    if (type == "ice") {
        std::string mid = ExtractJsonString(line, "mid");
        std::string candidate = ExtractJsonString(line, "candidate");
        int mline = ExtractJsonInt(line, "mlineIndex", 0);
        if (on_ice_ && !candidate.empty()) on_ice_(from, mid, mline, candidate);
    }
}

}  // namespace webrtc_demo
