#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <nlohmann/json.hpp>

struct Stream {
    std::string name;
    std::string url;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Stream, name, url);
};

struct Streams {
    std::vector<Stream> streams;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Streams, streams);
};

struct StreamInfo {
    std::string ip;
    int port;
};

struct MulticastStream {
    StreamInfo info;
    std::atomic<bool> running{true};
    int sock{-1};
};

int readStreamsFromJson(const std::string &filename, Streams &streams) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return -1;
    }

    nlohmann::json j;
    try {
        file >> j;
        streams = j;
    } catch (const nlohmann::json::parse_error &e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return -1;
    } catch (const nlohmann::json::exception &e) {
        std::cerr << "JSON error: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}

void cleanup_socket(MulticastStream *stream) {
    if (stream->sock >= 0) {
        shutdown(stream->sock, SHUT_RDWR);
        close(stream->sock);
        stream->sock = -1;
    }
}

void *recvThread(void *arg) {
    auto *stream = static_cast<MulticastStream *>(arg);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    sockaddr_in address{};
    stream->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (stream->sock < 0) {
        perror("socket creation failed");
        return nullptr;
    }

    constexpr int reuse = 1;
    if (setsockopt(stream->sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        cleanup_socket(stream);
        return nullptr;
    }

    address.sin_family = AF_INET;
    address.sin_port = htons(stream->info.port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(stream->sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        perror("bind failed");
        cleanup_socket(stream);
        return nullptr;
    }

    ip_mreq mreq{};
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    mreq.imr_multiaddr.s_addr = inet_addr(stream->info.ip.c_str());

    if (setsockopt(stream->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP failed");
        cleanup_socket(stream);
        return nullptr;
    }

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(stream->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO failed");
        cleanup_socket(stream);
        return nullptr;
    }

    socklen_t addrLen = sizeof(address);

    while (stream->running) {
        char buffer[65535];
        const ssize_t received = recvfrom(stream->sock, buffer, sizeof(buffer), 0,
                                          reinterpret_cast<sockaddr *>(&address), &addrLen);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recvfrom failed");
            break;
        }
        // std::cout << "** " << received << " from: " << stream->info.ip << " **" << std::endl;
    }

    cleanup_socket(stream);
    return nullptr;
}

StreamInfo parseRtpUrl(const std::string &url) {
    StreamInfo info;
    const size_t protocolEnd = url.find("://");
    if (protocolEnd == std::string::npos) {
        return {};
    }

    std::string address = url.substr(protocolEnd + 3);
    const size_t colonPos = address.find(':');
    if (colonPos == std::string::npos) {
        return {};
    }

    info.ip = address.substr(0, colonPos);
    info.port = std::stoi(address.substr(colonPos + 1));
    return info;
}

void signal_handler(const int signum) {
    std::cout << "\nReceived signal " << signum << ", exiting ..." << std::endl;
    exit(0);
}

int main(int argc, const char *argv[]) {
    signal(SIGINT, signal_handler);

    auto streams = Streams{};
    int n = 0;


    if (argc == 3 && std::string_view(argv[1]) == "-s") {
        if (readStreamsFromJson(argv[2], streams) < 0) {
            return -1;
        }
    } else if (argc == 5 && std::string_view(argv[1]) == "-n" && std::string_view(argv[3]) == "-s") {
        if (readStreamsFromJson(argv[4], streams) < 0) {
            return -1;
        }
        n = atoi(argv[2]);
    } else {
        std::cerr << "Usage: " << argv[0] << " [-n count] -s streams.json" << std::endl;
        return -1;
    }

    if (streams.streams.empty()) {
        std::cerr << "No streams found" << std::endl;
        return -1;
    }

    if (n == 0 || n > streams.streams.size()) {
        n = streams.streams.size();
    }

    std::vector<MulticastStream *> multicastStreams;
    std::vector<pthread_t> threads(n);

    for (int i = 0; i < n; i++) {
        const StreamInfo info = parseRtpUrl(streams.streams[i].url);
        if (info.ip.empty()) {
            std::cerr << "Failed to parse url: " << streams.streams[i].url << std::endl;
            continue;
        }
        auto *stream = new MulticastStream{info};
        multicastStreams.push_back(stream);

        if (pthread_create(&threads[i], nullptr, recvThread, stream) != 0) {
            delete stream;
        }
    }

    std::cout << "Receiving from " << n << " streams. Press Ctrl+C to stop." << std::endl;

    for (int i = 0; i < n; i++) {
        pthread_join(threads[i], nullptr);
    }


    for (auto *stream: multicastStreams) {
        stream->running = false;
        delete stream;
    }

    return 0;
}
