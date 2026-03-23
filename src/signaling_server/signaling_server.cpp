/**
 * WebRTC P2P 纯信令服务器 (C++ TCP)
 * 仅转发 SDP 和 ICE，不传输媒体。协议：每行一个 JSON
 */
#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

static int g_listen_fd = -1;
static std::atomic<bool> g_running{true};
static std::mutex g_mutex;
// 多流支持：stream_id -> (fd, pub_id)
static std::unordered_map<std::string, std::pair<int, std::string>> g_publishers;
// stream_id -> (sub_id -> fd)
static std::unordered_map<std::string, std::unordered_map<std::string, int>> g_subscribers;
// fd -> (stream_id, peer_id, role) 用于清理和路由
static std::unordered_map<int, std::tuple<std::string, std::string, std::string>> g_fd_to_info;
static std::atomic<unsigned long> g_peer_seq{1};

static std::string NextPeerId(const char* role) {
    unsigned long seq = g_peer_seq.fetch_add(1);
    std::string prefix = (std::strcmp(role, "publisher") == 0) ? "pub" : "sub";
    return prefix + "-" + std::to_string(seq);
}

static std::string JsonGetString(const std::string& line, const std::string& key) {
    std::string token = "\"" + key + "\":\"";
    size_t p = line.find(token);
    if (p == std::string::npos) return "";
    p += token.size();
    std::string out;
    for (size_t i = p; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            out += line[i + 1];
            ++i;
            continue;
        }
        if (line[i] == '"') break;
        out += line[i];
    }
    return out;
}

static void SendJsonLine(int fd, const std::string& line) {
    if (fd < 0) return;
    std::string msg = line + "\n";
    send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);
}

void SignalHandler(int) {
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void NotifyPublisherSubscriberEvent(const std::string& stream_id, const char* type,
                                           const std::string& sub_id) {
    int pub = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_publishers.find(stream_id);
        if (it != g_publishers.end()) pub = it->second.first;
    }
    if (pub >= 0) {
        SendJsonLine(pub, std::string("{\"type\":\"") + type + "\",\"from\":\"" + sub_id + "\"}");
    }
}

static void RoutePublisherMessage(const std::string& stream_id, const std::string& line,
                                  const std::string& from_id) {
    std::string target_id = JsonGetString(line, "to");
    if (target_id.empty()) return;

    int target_fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_subscribers.find(stream_id);
        if (it != g_subscribers.end()) {
            auto sub_it = it->second.find(target_id);
            if (sub_it != it->second.end()) target_fd = sub_it->second;
        }
    }
    if (target_fd < 0) return;

    std::string forwarded = line;
    if (forwarded.find("\"from\":\"") == std::string::npos) {
        size_t pos = forwarded.rfind('}');
        if (pos != std::string::npos) {
            forwarded.insert(pos, ",\"from\":\"" + from_id + "\"");
        }
    }
    SendJsonLine(target_fd, forwarded);
}

static void RouteSubscriberMessage(const std::string& stream_id, const std::string& line,
                                   const std::string& from_id) {
    int pub = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_publishers.find(stream_id);
        if (it != g_publishers.end()) pub = it->second.first;
    }
    if (pub < 0) return;

    std::string forwarded = line;
    if (forwarded.find("\"from\":\"") == std::string::npos) {
        size_t pos = forwarded.rfind('}');
        if (pos != std::string::npos) {
            forwarded.insert(pos, ",\"from\":\"" + from_id + "\"");
        }
    }
    SendJsonLine(pub, forwarded);
}

static void PeerLoop(int my_fd, const char* my_role, const std::string& my_id,
                    const std::string& stream_id) {
    std::string buf;
    char tmp[65536];
    while (g_running && my_fd >= 0) {
        ssize_t n = recv(my_fd, tmp, sizeof(tmp) - 1, 0);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;
            if (line.find("\"type\":\"register\"") != std::string::npos) continue;

            if (std::strcmp(my_role, "publisher") == 0) {
                RoutePublisherMessage(stream_id, line, my_id);
            } else {
                RouteSubscriberMessage(stream_id, line, my_id);
            }
        }
    }
    if (my_fd >= 0) {
        close(my_fd);
        std::string removed_stream_id;
        std::string removed_sub_id;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_fd_to_info.find(my_fd);
            if (it != g_fd_to_info.end()) {
                removed_stream_id = std::get<0>(it->second);
                removed_sub_id = std::get<1>(it->second);
                std::string role = std::get<2>(it->second);
                g_fd_to_info.erase(it);

                if (role == "publisher") {
                    auto pub_it = g_publishers.find(removed_stream_id);
                    if (pub_it != g_publishers.end() && pub_it->second.first == my_fd) {
                        g_publishers.erase(pub_it);
                        std::cout << "[信令] 推流端离线 stream=" << removed_stream_id << std::endl;
                    }
                } else if (role == "subscriber") {
                    auto sub_it = g_subscribers.find(removed_stream_id);
                    if (sub_it != g_subscribers.end()) {
                        sub_it->second.erase(removed_sub_id);
                        if (sub_it->second.empty()) g_subscribers.erase(sub_it);
                        std::cout << "[信令] 拉流端离线 stream=" << removed_stream_id
                                  << " sub=" << removed_sub_id << std::endl;
                    }
                }
            }
        }
        if (!removed_stream_id.empty() && !removed_sub_id.empty()) {
            NotifyPublisherSubscriberEvent(removed_stream_id, "subscriber_leave", removed_sub_id);
        }
    }
}

int main(int argc, char* argv[]) {
    int port = 8765;
    if (argc >= 2) port = std::atoi(argv[1]);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(g_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind() failed" << std::endl;
        close(g_listen_fd);
        return 1;
    }
    if (listen(g_listen_fd, 128) != 0) {
        std::cerr << "listen() failed" << std::endl;
        close(g_listen_fd);
        return 1;
    }

    std::signal(SIGINT, SignalHandler);

    std::cout << "WebRTC P2P 信令服务器: 0.0.0.0:" << port << " (TCP)" << std::endl;
    std::cout << "支持多流：推流端按 stream_id 注册，拉流端指定 stream_id 订阅" << std::endl;

    while (g_running) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = accept(g_listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) break;

        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            close(fd);
            continue;
        }
        buf[n] = '\0';
        std::string msg(buf);
        std::string stream_id = JsonGetString(msg, "stream_id");
        if (stream_id.empty()) stream_id = "livestream";

        if (msg.find("\"role\":\"publisher\"") != std::string::npos) {
            std::string pub_id = NextPeerId("publisher");
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_publishers.find(stream_id);
                if (it != g_publishers.end() && it->second.first >= 0) {
                    int old_fd = it->second.first;
                    g_fd_to_info.erase(old_fd);
                    shutdown(old_fd, SHUT_RDWR);
                    std::cout << "[信令] 替换旧推流端 stream=" << stream_id << std::endl;
                }
                g_publishers[stream_id] = {fd, pub_id};
                g_fd_to_info[fd] = {stream_id, pub_id, "publisher"};
                std::cout << "[信令] 推流端已连接 stream=" << stream_id << " (fd=" << fd
                          << ", id=" << pub_id << ")" << std::endl;
            }
            SendJsonLine(fd, "{\"type\":\"welcome\",\"id\":\"" + pub_id + "\"}");
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_subscribers.find(stream_id);
                if (it != g_subscribers.end()) {
                    for (const auto& kv : it->second) {
                        SendJsonLine(fd, "{\"type\":\"subscriber_join\",\"from\":\"" + kv.first + "\"}");
                    }
                }
            }
            std::thread(PeerLoop, fd, "publisher", pub_id, stream_id).detach();
        } else if (msg.find("\"role\":\"subscriber\"") != std::string::npos) {
            std::string sub_id = NextPeerId("subscriber");
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_subscribers[stream_id][sub_id] = fd;
                g_fd_to_info[fd] = {stream_id, sub_id, "subscriber"};
                std::cout << "[信令] 拉流端已连接 stream=" << stream_id << " (fd=" << fd
                          << ", id=" << sub_id << "), 该流订阅者: "
                          << g_subscribers[stream_id].size() << std::endl;
            }
            SendJsonLine(fd, "{\"type\":\"welcome\",\"id\":\"" + sub_id + "\"}");
            NotifyPublisherSubscriberEvent(stream_id, "subscriber_join", sub_id);
            std::thread(PeerLoop, fd, "subscriber", sub_id, stream_id).detach();
        } else {
            close(fd);
        }
    }

    if (g_listen_fd >= 0) close(g_listen_fd);
    std::cout << "信令服务器已退出" << std::endl;
    return 0;
}
