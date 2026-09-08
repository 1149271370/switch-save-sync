#pragma once

#include "discovery.h"

#include <atomic>
#include <string>
#include <thread>

class TransferServer {
public:
    ~TransferServer();

    bool start(const std::string &zip_path);
    void stop();
    bool active() const;
    bool done() const;

private:
    void run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> done_{false};
    std::string zip_path_;
    int listen_fd_ = -1;
};

bool exportTitleToZip(const AppUser &user, const AppTitle &title, std::string &error);
bool downloadZipFromPc(const char *host, std::string &error);
bool restoreTitleFromZip(const AppUser &user, const AppTitle &title, std::string &error);
