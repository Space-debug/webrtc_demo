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

SignalingClient::SignalingClient(const std::string& server_addr, const std::string& role)
    : server_addr_(server_addr), role_(role) {
    ParseHostPort(server_addr, host_, port_);
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
    reg << "{\"type\":\"register\",\"role\":\"" << role_ << "\"}\n";
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

void SignalingClient::SendOffer(const std::string& sdp) {
    std::ostringstream oss;
    oss << "{\"type\":\"offer\",\"sdp\":\"";
    for (char c : sdp) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else if (c == '\r') oss << "\\r";
        else oss << c;
    }
    oss << "\"}";
    SendLine(oss.str());
}

void SignalingClient::SendAnswer(const std::string& sdp) {
    std::ostringstream oss;
    oss << "{\"type\":\"answer\",\"sdp\":\"";
    for (char c : sdp) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else if (c == '\r') oss << "\\r";
        else oss << c;
    }
    oss << "\"}";
    SendLine(oss.str());
}

void SignalingClient::SendIceCandidate(const std::string& mid, int mline_index,
                                       const std::string& candidate) {
    std::ostringstream oss;
    oss << "{\"type\":\"ice\",\"mid\":\"" << mid << "\",\"mlineIndex\":" << mline_index
       << ",\"candidate\":\"";
    for (char c : candidate) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else oss << c;
    }
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

    if (line.find("\"type\":\"answer\"") != std::string::npos ||
        line.find("\"type\": \"answer\"") != std::string::npos) {
        size_t sdp_start = line.find("\"sdp\":\"");
        if (sdp_start != std::string::npos) {
            sdp_start += 7;
            std::string sdp;
            for (size_t i = sdp_start; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    if (line[i + 1] == 'n') sdp += '\n';
                    else if (line[i + 1] == 'r') sdp += '\r';
                    else if (line[i + 1] == '"') sdp += '"';
                    else sdp += line[i + 1];
                    ++i;
                } else if (line[i] == '"') break;
                else sdp += line[i];
            }
            if (on_answer_) on_answer_("answer", sdp);
        }
        return;
    }
    if (line.find("\"type\":\"offer\"") != std::string::npos ||
        line.find("\"type\": \"offer\"") != std::string::npos) {
        size_t sdp_start = line.find("\"sdp\":\"");
        if (sdp_start != std::string::npos) {
            sdp_start += 7;
            std::string sdp;
            for (size_t i = sdp_start; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    if (line[i + 1] == 'n') sdp += '\n';
                    else if (line[i + 1] == 'r') sdp += '\r';
                    else if (line[i + 1] == '"') sdp += '"';
                    else sdp += line[i + 1];
                    ++i;
                } else if (line[i] == '"') break;
                else sdp += line[i];
            }
            if (on_offer_) on_offer_("offer", sdp);
        }
        return;
    }
    if (line.find("\"type\":\"ice\"") != std::string::npos ||
        line.find("\"type\": \"ice\"") != std::string::npos) {
        std::string mid, candidate;
        int mline = 0;
        size_t p = line.find("\"mid\":\"");
        if (p != std::string::npos) {
            p += 6;
            size_t end = line.find('"', p);
            if (end != std::string::npos) mid = line.substr(p, end - p);
        }
        p = line.find("\"mlineIndex\":");
        if (p != std::string::npos) mline = std::atoi(line.c_str() + p + 12);
        p = line.find("\"candidate\":\"");
        if (p != std::string::npos) {
            p += 13;
            for (size_t i = p; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    candidate += line[i + 1];
                    ++i;
                } else if (line[i] == '"') break;
                else candidate += line[i];
            }
        }
        if (on_ice_ && !candidate.empty()) on_ice_(mid, mline, candidate);
    }
}

}  // namespace webrtc_demo
