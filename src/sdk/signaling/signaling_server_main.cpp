/**
 * WebRTC P2P 信令服务器 (C++ TCP + epoll)
 * 仅转发 SDP/ICE，不传输媒体。协议：每行一个 JSON。
 *
 * - 单 epoll 线程负责 accept、读缓冲与拆行；注册（首条 register）在同线程完成以保证表一致。
 * - 固定大小线程池：按 fd % pool_size 分片，同连接信令始终进入同一工作线程，保序。
 * - 默认少打日志；设置环境变量 SIGNALING_VERBOSE=1 输出连接/离线详情。
 */
#include "signaling/signaling_server_main.h"
#include "webrtc/utils/json_utils.h"
#include "webrtc/utils/net_io.h"
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
std::atomic<bool> g_verbose{false};
int g_listen_fd = -1;
int g_epoll_fd = -1;

std::mutex g_mutex;
std::unordered_map<std::string, std::pair<int, std::string>> g_publishers;
std::unordered_map<std::string, std::unordered_map<std::string, int>> g_subscribers;
std::unordered_map<int, std::tuple<std::string, std::string, std::string>> g_fd_to_info;

std::atomic<unsigned long> g_peer_seq{1};

void LogVerbose(const std::string& msg) {
    if (g_verbose.load(std::memory_order_relaxed)) {
        std::cout << msg << std::endl;
    }
}

std::string NextPeerId(const char* role) {
    unsigned long seq = g_peer_seq.fetch_add(1, std::memory_order_relaxed);
    const char* prefix = (std::strcmp(role, "publisher") == 0) ? "pub" : "sub";
    return std::string(prefix) + "-" + std::to_string(seq);
}

static void SendJsonLine(int fd, const std::string& line) {
    if (fd < 0) {
        return;
    }
    const int kIoTimeoutMs = 8000;
    const char nl = '\n';
    if (!webrtc_demo::utils::WriteAllWithPoll(fd, line.data(), line.size(), kIoTimeoutMs)) {
        return;
    }
    (void)webrtc_demo::utils::WriteAllWithPoll(fd, &nl, 1, kIoTimeoutMs);
}

/// 在 JSON 单行末尾注入 "from"，减少 insert 整块搬移：只分配一次
static void ForwardLineWithFrom(int target_fd, const std::string& line, const std::string& from_id) {
    if (target_fd < 0) {
        return;
    }
    if (line.find("\"from\":\"") != std::string::npos) {
        SendJsonLine(target_fd, line);
        return;
    }
    const size_t pos = line.rfind('}');
    if (pos == std::string::npos) {
        return;
    }
    std::string out;
    out.reserve(line.size() + from_id.size() + 16);
    out.append(line, 0, pos);
    out.append(",\"from\":\"");
    out.append(from_id);
    out.push_back('"');
    out.push_back('}');
    SendJsonLine(target_fd, out);
}

struct Client : std::enable_shared_from_this<Client> {
    int fd{-1};
    std::string read_buf;
    bool registered{false};
    std::string stream_id{"livestream"};
    std::string peer_id;
    std::string role;
    std::atomic<bool> dead{false};
};

std::mutex g_clients_mutex;
std::unordered_map<int, std::shared_ptr<Client>> g_clients;

void EpollCtl(int op, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(g_epoll_fd, op, fd, &ev) != 0) {
        std::cerr << "[Signaling] epoll_ctl failed fd=" << fd << std::endl;
    }
}

void CloseClientFd(int fd) {
    if (fd < 0) {
        return;
    }
    EpollCtl(EPOLL_CTL_DEL, fd, 0);
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

void NotifyPublisherSubscriberEvent(const std::string& stream_id, const char* type, const std::string& sub_id) {
    int pub = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_publishers.find(stream_id);
        if (it != g_publishers.end()) {
            pub = it->second.first;
        }
    }
    if (pub >= 0) {
        std::string msg;
        msg.reserve(48 + stream_id.size() + sub_id.size());
        msg = "{\"type\":\"";
        msg += type;
        msg += "\",\"from\":\"";
        msg += sub_id;
        msg += "\"}";
        SendJsonLine(pub, msg);
    }
}

void RoutePublisherMessage(const std::string& stream_id, const std::string& line, const std::string& from_id) {
    const std::string target_id = webrtc_demo::utils::ExtractJsonString(line, "to");
    if (target_id.empty()) {
        return;
    }
    int target_fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_subscribers.find(stream_id);
        if (it != g_subscribers.end()) {
            auto sub_it = it->second.find(target_id);
            if (sub_it != it->second.end()) {
                target_fd = sub_it->second;
            }
        }
    }
    if (target_fd < 0) {
        return;
    }
    ForwardLineWithFrom(target_fd, line, from_id);
}

void RouteSubscriberMessage(const std::string& stream_id, const std::string& line, const std::string& from_id) {
    int pub = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_publishers.find(stream_id);
        if (it != g_publishers.end()) {
            pub = it->second.first;
        }
    }
    if (pub < 0) {
        return;
    }
    ForwardLineWithFrom(pub, line, from_id);
}

struct LineTask {
    std::weak_ptr<Client> client;
    std::string line;
};

class ShardedLineWorkers {
public:
    explicit ShardedLineWorkers(size_t num_threads) : n_(num_threads > 0 ? num_threads : 1) {
        queues_.resize(n_);
        mutexes_.reserve(n_);
        cvs_.reserve(n_);
        for (size_t i = 0; i < n_; ++i) {
            mutexes_.push_back(std::make_unique<std::mutex>());
            cvs_.push_back(std::make_unique<std::condition_variable>());
        }
        stop_ = false;
        threads_.reserve(n_);
        for (size_t i = 0; i < n_; ++i) {
            threads_.emplace_back([this, i] { RunShard(i); });
        }
    }

    ~ShardedLineWorkers() {
        {
            std::vector<std::unique_lock<std::mutex>> locks;
            locks.reserve(n_);
            for (size_t i = 0; i < n_; ++i) {
                locks.emplace_back(*mutexes_[i]);
            }
            stop_ = true;
            for (size_t i = 0; i < n_; ++i) {
                cvs_[i]->notify_all();
            }
        }
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void Enqueue(int source_fd, LineTask task) {
        const size_t shard = static_cast<size_t>(source_fd >= 0 ? source_fd : 0) % n_;
        {
            std::lock_guard<std::mutex> lock(*mutexes_[shard]);
            queues_[shard].push_back(std::move(task));
        }
        cvs_[shard]->notify_one();
    }

    ShardedLineWorkers(const ShardedLineWorkers&) = delete;
    ShardedLineWorkers& operator=(const ShardedLineWorkers&) = delete;

private:
    void RunShard(size_t shard) {
        while (true) {
            LineTask task;
            {
                std::unique_lock<std::mutex> lock(*mutexes_[shard]);
                cvs_[shard]->wait(lock, [this, shard] { return stop_ || !queues_[shard].empty(); });
                if (stop_ && queues_[shard].empty()) {
                    return;
                }
                task = std::move(queues_[shard].front());
                queues_[shard].pop_front();
            }
            auto c = task.client.lock();
            if (!c || c->dead.load(std::memory_order_acquire)) {
                continue;
            }
            if (c->role == "publisher") {
                RoutePublisherMessage(c->stream_id, task.line, c->peer_id);
            } else if (c->role == "subscriber") {
                RouteSubscriberMessage(c->stream_id, task.line, c->peer_id);
            }
        }
    }

    size_t n_;
    std::vector<std::deque<LineTask>> queues_;
    std::vector<std::unique_ptr<std::mutex>> mutexes_;
    std::vector<std::unique_ptr<std::condition_variable>> cvs_;
    std::vector<std::thread> threads_;
    bool stop_{false};
};

std::unique_ptr<ShardedLineWorkers> g_workers;

void RemoveClientResources(int fd, const std::shared_ptr<Client>&) {
    std::string removed_stream_id;
    std::string removed_sub_id;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_fd_to_info.find(fd);
        if (it != g_fd_to_info.end()) {
            removed_stream_id = std::get<0>(it->second);
            removed_sub_id = std::get<1>(it->second);
            const std::string& role = std::get<2>(it->second);
            g_fd_to_info.erase(it);

            if (role == "publisher") {
                auto pub_it = g_publishers.find(removed_stream_id);
                if (pub_it != g_publishers.end() && pub_it->second.first == fd) {
                    g_publishers.erase(pub_it);
                    LogVerbose(std::string("[Signaling] publisher disconnected stream=") + removed_stream_id);
                }
            } else if (role == "subscriber") {
                auto sub_it = g_subscribers.find(removed_stream_id);
                if (sub_it != g_subscribers.end()) {
                    sub_it->second.erase(removed_sub_id);
                    if (sub_it->second.empty()) {
                        g_subscribers.erase(sub_it);
                    }
                    LogVerbose(std::string("[Signaling] subscriber disconnected stream=") + removed_stream_id +
                               " sub=" + removed_sub_id);
                }
            }
        }
    }
    if (!removed_stream_id.empty() && !removed_sub_id.empty()) {
        NotifyPublisherSubscriberEvent(removed_stream_id, "subscriber_leave", removed_sub_id);
    }
}

static void DrainCompleteLines(const std::shared_ptr<Client>& client, int fd) {
    while (true) {
        const size_t pos = client->read_buf.find('\n');
        if (pos == std::string::npos) {
            break;
        }
        std::string line = client->read_buf.substr(0, pos);
        client->read_buf.erase(0, pos + 1);
        if (line.empty()) {
            continue;
        }
        if (line.find("\"type\":\"register\"") != std::string::npos) {
            continue;
        }
        if (!client->registered) {
            continue;
        }
        LineTask task;
        task.client = client->weak_from_this();
        task.line = std::move(line);
        g_workers->Enqueue(fd, std::move(task));
    }
}

void ProcessClientRead(int fd) {
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        auto it = g_clients.find(fd);
        if (it == g_clients.end()) {
            return;
        }
        client = it->second;
    }

    DrainCompleteLines(client, fd);

    char tmp[65536];
    for (;;) {
        const ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            client->read_buf.append(tmp, static_cast<size_t>(n));
            DrainCompleteLines(client, fd);
        } else if (n == 0) {
            client->dead.store(true, std::memory_order_release);
            CloseClientFd(fd);
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                g_clients.erase(fd);
            }
            RemoveClientResources(fd, client);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            client->dead.store(true, std::memory_order_release);
            CloseClientFd(fd);
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                g_clients.erase(fd);
            }
            RemoveClientResources(fd, client);
            return;
        }
    }
}

/// msg 为单行 register JSON（不含换行）
bool HandleInitialRegister(int fd, const std::string& msg) {
    std::string stream_id = webrtc_demo::utils::ExtractJsonString(msg, "stream_id");
    if (stream_id.empty()) {
        stream_id = "livestream";
    }

    auto client = std::make_shared<Client>();
    client->fd = fd;

    if (msg.find("\"role\":\"publisher\"") != std::string::npos) {
        const std::string pub_id = NextPeerId("publisher");
        client->registered = true;
        client->stream_id = std::move(stream_id);
        client->peer_id = pub_id;
        client->role = "publisher";

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_publishers.find(client->stream_id);
            if (it != g_publishers.end() && it->second.first >= 0) {
                const int old_fd = it->second.first;
                g_fd_to_info.erase(old_fd);
                {
                    std::lock_guard<std::mutex> cl(g_clients_mutex);
                    if (auto oit = g_clients.find(old_fd); oit != g_clients.end()) {
                        oit->second->dead.store(true, std::memory_order_release);
                        g_clients.erase(oit);
                    }
                }
                CloseClientFd(old_fd);
                LogVerbose(std::string("[Signaling] replaced publisher stream=") + client->stream_id);
            }
            g_publishers[client->stream_id] = {fd, pub_id};
            g_fd_to_info[fd] = {client->stream_id, pub_id, "publisher"};
        }
        {
            std::lock_guard<std::mutex> cl(g_clients_mutex);
            g_clients[fd] = client;
        }

        LogVerbose(std::string("[Signaling] publisher connected stream=") + client->stream_id +
                   " (fd=" + std::to_string(fd) + ", id=" + pub_id + ")");

        SendJsonLine(fd, std::string("{\"type\":\"welcome\",\"id\":\"") + pub_id + "\"}");
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_subscribers.find(client->stream_id);
            if (it != g_subscribers.end()) {
                for (const auto& kv : it->second) {
                    std::string j = "{\"type\":\"subscriber_join\",\"from\":\"";
                    j += kv.first;
                    j += "\"}";
                    SendJsonLine(fd, j);
                }
            }
        }
        return true;
    }

    if (msg.find("\"role\":\"subscriber\"") != std::string::npos) {
        const std::string sub_id = NextPeerId("subscriber");
        client->registered = true;
        client->stream_id = std::move(stream_id);
        client->peer_id = sub_id;
        client->role = "subscriber";

        size_t sub_count = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_subscribers[client->stream_id][sub_id] = fd;
            g_fd_to_info[fd] = {client->stream_id, sub_id, "subscriber"};
            sub_count = g_subscribers[client->stream_id].size();
        }
        {
            std::lock_guard<std::mutex> cl(g_clients_mutex);
            g_clients[fd] = client;
        }

        LogVerbose(std::string("[Signaling] subscriber connected stream=") + client->stream_id +
                   " (fd=" + std::to_string(fd) + ", id=" + sub_id + "), subscribers_on_stream=" +
                   std::to_string(sub_count));

        SendJsonLine(fd, std::string("{\"type\":\"welcome\",\"id\":\"") + sub_id + "\"}");
        NotifyPublisherSubscriberEvent(client->stream_id, "subscriber_join", sub_id);
        return true;
    }

    return false;
}

void SignalHandler(int) {
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
}

}  // namespace

int webrtc_demo::RunSignalingServerMain(int argc, char* argv[]) {
    int port = 8765;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    size_t pool_threads = 4;
    if (argc >= 3) {
        const int p = std::atoi(argv[2]);
        if (p > 0 && p <= 256) {
            pool_threads = static_cast<size_t>(p);
        }
    }
    if (const char* ev = std::getenv("SIGNALING_POOL_THREADS")) {
        const int p = std::atoi(ev);
        if (p > 0 && p <= 256) {
            pool_threads = static_cast<size_t>(p);
        }
    }
    if (const char* v = std::getenv("SIGNALING_VERBOSE")) {
        if (v[0] == '1' || (v[0] == 't' || v[0] == 'T') || (v[0] == 'y' || v[0] == 'Y')) {
            g_verbose.store(true, std::memory_order_relaxed);
        }
    }

    g_workers = std::make_unique<ShardedLineWorkers>(pool_threads);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    webrtc_demo::utils::SetNonBlocking(g_listen_fd);

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

    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        std::cerr << "epoll_create1 failed" << std::endl;
        close(g_listen_fd);
        return 1;
    }

    epoll_event lev{};
    lev.events = EPOLLIN;
    lev.data.fd = g_listen_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &lev) != 0) {
        std::cerr << "epoll_ctl listen failed" << std::endl;
        close(g_epoll_fd);
        close(g_listen_fd);
        return 1;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::cout << "WebRTC P2P signaling server: 0.0.0.0:" << port << " (TCP epoll, pool=" << pool_threads << ")"
              << std::endl;
    std::cout << "Multi-stream: publisher register + stream_id; subscriber register + stream_id" << std::endl;
    std::cout << "Verbose: SIGNALING_VERBOSE=1; threads: argv[2] or SIGNALING_POOL_THREADS" << std::endl;

    std::vector<epoll_event> events(256);

    while (g_running.load(std::memory_order_relaxed)) {
        const int n = epoll_wait(g_epoll_fd, events.data(), static_cast<int>(events.size()), 1000);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == g_listen_fd) {
                while (g_running.load(std::memory_order_relaxed)) {
                    sockaddr_in peer{};
                    socklen_t plen = sizeof(peer);
                    int cfd = accept(g_listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }
                    webrtc_demo::utils::SetNonBlocking(cfd);

                    std::string reg_line;
                    std::string leftover;
                    if (!webrtc_demo::utils::RecvUntilNewline(cfd, &reg_line, &leftover, 30000)) {
                        close(cfd);
                        continue;
                    }

                    if (!HandleInitialRegister(cfd, reg_line)) {
                        close(cfd);
                        continue;
                    }

                    {
                        std::lock_guard<std::mutex> lock(g_clients_mutex);
                        auto it = g_clients.find(cfd);
                        if (it != g_clients.end()) {
                            it->second->read_buf = std::move(leftover);
                        }
                    }

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &cev) != 0) {
                        close(cfd);
                        continue;
                    }
                    ProcessClientRead(cfd);
                }
            } else {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    std::shared_ptr<Client> dead_client;
                    {
                        std::lock_guard<std::mutex> lock(g_clients_mutex);
                        auto it = g_clients.find(fd);
                        if (it != g_clients.end()) {
                            dead_client = it->second;
                            dead_client->dead.store(true, std::memory_order_release);
                            g_clients.erase(it);
                        }
                    }
                    CloseClientFd(fd);
                    if (dead_client) {
                        RemoveClientResources(fd, dead_client);
                    }
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    ProcessClientRead(fd);
                }
            }
        }
    }

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    g_workers.reset();

    std::cout << "Signaling server exited" << std::endl;
    return 0;
}
