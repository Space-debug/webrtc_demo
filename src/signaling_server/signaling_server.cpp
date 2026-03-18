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
#include <vector>

static int g_listen_fd = -1;
static std::atomic<bool> g_running{true};
static int g_pub_fd = -1;
static int g_sub_fd = -1;
static std::mutex g_mutex;
static std::vector<std::string> g_pending_to_sub;  // 推流端先发 offer 时，缓存待转发给拉流端

void SignalHandler(int) {
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void PeerLoop(int my_fd, int* other_fd, const char* my_name) {
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

            int target = -1;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                target = *other_fd;
            }
            if (target >= 0) {
                std::string msg = line + "\n";
                send(target, msg.data(), msg.size(), MSG_NOSIGNAL);
            } else if (other_fd == &g_sub_fd) {
                // 推流端发往拉流端，但拉流端未连接，缓存
                std::lock_guard<std::mutex> lock2(g_mutex);
                g_pending_to_sub.push_back(line + "\n");
            }
        }
    }
    if (my_fd >= 0) {
        close(my_fd);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pub_fd == my_fd) g_pub_fd = -1;
        if (g_sub_fd == my_fd) g_sub_fd = -1;
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
    if (listen(g_listen_fd, 2) != 0) {
        std::cerr << "listen() failed" << std::endl;
        close(g_listen_fd);
        return 1;
    }

    std::signal(SIGINT, SignalHandler);

    std::cout << "WebRTC P2P 信令服务器: 0.0.0.0:" << port << " (TCP)" << std::endl;
    std::cout << "推流端先启动，拉流端后启动" << std::endl;

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

        if (msg.find("\"role\":\"publisher\"") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pub_fd >= 0) {
                close(g_pub_fd);
                std::cout << "[信令] 替换旧推流端连接" << std::endl;
            }
            g_pub_fd = fd;
            std::cout << "[信令] 推流端已连接 (fd=" << fd << ")" << std::endl;
            std::thread(PeerLoop, fd, &g_sub_fd, "publisher").detach();
        } else if (msg.find("\"role\":\"subscriber\"") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_sub_fd >= 0) {
                close(g_sub_fd);
                std::cout << "[信令] 替换旧拉流端连接" << std::endl;
            }
            g_sub_fd = fd;
            std::cout << "[信令] 拉流端已连接 (fd=" << fd << ")，转发 " << g_pending_to_sub.size() << " 条缓存消息" << std::endl;
            for (const auto& pending : g_pending_to_sub) {
                send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
            }
            g_pending_to_sub.clear();
            std::thread(PeerLoop, fd, &g_pub_fd, "subscriber").detach();
        } else {
            close(fd);
        }
    }

    if (g_listen_fd >= 0) close(g_listen_fd);
    std::cout << "信令服务器已退出" << std::endl;
    return 0;
}
