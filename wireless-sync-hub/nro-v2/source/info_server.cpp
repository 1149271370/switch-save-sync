#include "info_server.h"

#include "discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>
#include <vector>

extern std::vector<AppUser> g_users;
extern std::vector<AppTitle> g_titles;
extern SystemSnapshot g_system;
extern char g_pc_ip[64];

static void savePcIp(const char *ip)
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchSaveSyncHub", 0777);
    FILE *file = fopen("sdmc:/switch/SwitchSaveSyncHub/config.json", "w");
    if (!file) return;
    fprintf(file, "{\"pc\":\"%s\"}", ip);
    fclose(file);
}

static void setPcIp(const char *query)
{
    const char *pc = strstr(query, "pc=");
    if (!pc) return;
    pc += 3;
    char ip[64];
    size_t n = 0;
    while (pc[n] && pc[n] != '&' && pc[n] != ' ' && n < sizeof(ip) - 1) {
        ip[n] = pc[n];
        n++;
    }
    ip[n] = 0;
    struct in_addr parsed;
    if (inet_pton(AF_INET, ip, &parsed) == 1) {
        strncpy(g_pc_ip, ip, sizeof(g_pc_ip) - 1);
        savePcIp(g_pc_ip);
    }
}

static std::string escapeJson(const char *text)
{
    std::string out;
    if (!text) return out;
    for (const char *p = text; *p; p++) {
        switch (*p) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)*p);
                out += tmp;
            } else {
                out += *p;
            }
        }
    }
    return out;
}

std::string buildCatalogJson()
{
    std::ostringstream out;
    out << "{\"system\":{";
    out << "\"firmware\":\"" << escapeJson(g_system.firmware) << "\",";
    out << "\"hardware\":\"" << escapeJson(g_system.hardware) << "\",";
    out << "\"atmosphere\":\"" << escapeJson(g_system.atmosphere) << "\"},";
    out << "\"users\":[";
    for (size_t i = 0; i < g_users.size(); i++) {
        if (i) out << ",";
        out << "{\"name\":\"" << escapeJson(g_users[i].nickname) << "\"}";
    }
    out << "],\"titles\":[";
    for (size_t i = 0; i < g_titles.size(); i++) {
        if (i) out << ",";
        out << "{\"title_id\":\"" << g_titles[i].title_id
            << "\",\"name\":\"" << escapeJson(g_titles[i].name)
            << "\",\"size_bytes\":" << g_titles[i].size_bytes
            << ",\"modified\":" << g_titles[i].modified << "}";
    }
    out << "]}";
    return out.str();
}

InfoServer::~InfoServer()
{
    stop();
}

bool InfoServer::start(const std::string &payload)
{
    if (running_) return true;
    payload_ = payload;

    socketInitializeDefault();
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        socketExit();
        return false;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        nifmExit();
        socketExit();
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd_, 4) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        nifmExit();
        socketExit();
        return false;
    }

    stop_ = false;
    thread_ = std::thread([this] { run(); });
    running_ = true;
    return true;
}

void InfoServer::run()
{
    while (!stop_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);
        struct timeval tv = {1, 0};
        if (select(listen_fd_ + 1, &read_fds, NULL, NULL, &tv) <= 0) continue;

        int client = accept(listen_fd_, NULL, NULL);
        if (client < 0) continue;

        char buffer[1024];
        int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) {
            buffer[received] = 0;
            setPcIp(buffer);
            std::string response = payload_;
            std::string header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: " + std::to_string(response.size()) +
                "\r\nConnection: close\r\n\r\n";
            send(client, header.data(), header.size(), 0);
            send(client, response.data(), response.size(), 0);
        }
        close(client);
    }
}

void InfoServer::stop()
{
    stop_ = true;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    if (running_) {
        running_ = false;
        nifmExit();
        socketExit();
    }
}

bool InfoServer::running() const
{
    return running_;
}

void InfoServer::setPayload(const std::string &payload)
{
    payload_ = payload;
}
