#pragma once

#include <atomic>
#include <string>
#include <thread>

class InfoServer {
public:
    ~InfoServer();

    bool start(const std::string &payload);
    void stop();
    bool running() const;
    void setPayload(const std::string &payload);

private:
    void run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::string payload_;
    int listen_fd_ = -1;
};

std::string buildCatalogJson();
