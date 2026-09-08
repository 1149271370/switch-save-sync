#include "transfer.h"

#include "miniz.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

static void makeDir(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void removeTree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) removeTree(full);
            else remove(full);
        }
    }
    closedir(dir);
    rmdir(path);
}

static bool copyTree(const char *src, const char *dest)
{
    makeDir(dest);
    DIR *dir = opendir(src);
    if (!dir) return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dest, entry->d_name);
        struct stat st;
        if (stat(src_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            copyTree(src_path, dst_path);
        } else {
            FILE *in = fopen(src_path, "rb");
            FILE *out = fopen(dst_path, "wb");
            if (in && out) {
                char buf[131072];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
            }
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    closedir(dir);
    return true;
}

static bool zipDirToArchive(mz_zip_archive *zip, const char *dir, const char *prefix)
{
    DIR *dp = opendir(dir);
    if (!dp) return false;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full[1024], zip_name[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        snprintf(zip_name, sizeof(zip_name), "%s/%s", prefix, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!zipDirToArchive(zip, full, zip_name)) {
                closedir(dp);
                return false;
            }
        } else {
            FILE *file = fopen(full, "rb");
            if (!file) continue;
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            void *mem = malloc((size_t)size);
            if (mem && fread(mem, 1, (size_t)size, file) == (size_t)size) {
                mz_zip_writer_add_mem(zip, zip_name, mem, (size_t)size, MZ_BEST_SPEED);
            }
            free(mem);
            fclose(file);
        }
    }
    closedir(dp);
    return true;
}

static bool zipTempFolder()
{
    remove("sdmc:/temp.zip");
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, "sdmc:/temp.zip", 0)) return false;
    bool ok = zipDirToArchive(&zip, "sdmc:/temp", "temp");
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

static bool unzipTempFolder()
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, "sdmc:/temp.zip", 0)) return false;
    int count = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char out[1024];
        snprintf(out, sizeof(out), "sdmc:/%s", stat.m_filename);
        char *slash = strrchr(out, '/');
        if (slash) {
            *slash = 0;
            makeDir(out);
            *slash = '/';
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, out, 0)) {
            mz_zip_reader_end(&zip);
            return false;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool exportTitleToZip(const AppUser &user, const AppTitle &title, std::string &error)
{
    u64 app_id = strtoull(title.title_id, NULL, 16);
    removeTree("sdmc:/temp");
    makeDir("sdmc:/temp");
    char title_dir[64];
    snprintf(title_dir, sizeof(title_dir), "sdmc:/temp/%s", title.title_id);
    makeDir(title_dir);

    Result rc = fsdevMountSaveData("save", app_id, user.uid);
    if (R_FAILED(rc)) {
        error = "Failed to mount save data";
        return false;
    }
    bool copied = copyTree("save:/", title_dir);
    rc = fsdevCommitDevice("save");
    fsdevUnmountDevice("save");
    if (!copied || R_FAILED(rc)) {
        error = "Failed to export save files";
        return false;
    }
    if (!zipTempFolder()) {
        error = "Failed to create save archive";
        return false;
    }
    return true;
}

TransferServer::~TransferServer()
{
    stop();
}

bool TransferServer::start(const std::string &zip_path)
{
    if (active_) return true;
    zip_path_ = zip_path;
    done_ = false;
    stop_ = false;

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

    thread_ = std::thread([this] { run(); });
    active_ = true;
    return true;
}

void TransferServer::run()
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
            if (strstr(buffer, "SHUTDOWN")) {
                send(client, "HTTP/1.1 200 OK\r\n\r\n", 21, 0);
                done_ = true;
                stop_ = true;
            } else {
                FILE *file = fopen(zip_path_.c_str(), "rb");
                if (file) {
                    fseek(file, 0, SEEK_END);
                    long size = ftell(file);
                    fseek(file, 0, SEEK_SET);
                    char header[256];
                    int header_len = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                        size);
                    send(client, header, header_len, 0);
                    char buf[65536];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), file)) > 0) send(client, buf, n, 0);
                    fclose(file);
                } else {
                    send(client, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
                }
            }
        }
        close(client);
    }
}

void TransferServer::stop()
{
    stop_ = true;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    if (active_) {
        active_ = false;
        nifmExit();
        socketExit();
    }
}

bool TransferServer::active() const
{
    return active_;
}

bool TransferServer::done() const
{
    return done_;
}

bool downloadZipFromPc(const char *host, std::string &error)
{
    socketInitializeDefault();
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        socketExit();
        error = "Network service unavailable";
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        nifmExit();
        socketExit();
        error = "Socket creation failed";
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        nifmExit();
        socketExit();
        error = "Could not connect to PC";
        return false;
    }
    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(sock, request, strlen(request), 0);

    FILE *file = fopen("sdmc:/temp.zip", "wb");
    if (!file) {
        close(sock);
        nifmExit();
        socketExit();
        error = "Could not create temp file";
        return false;
    }
    char buf[65536];
    ssize_t n;
    bool body = false;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if (!body) {
            char *marker = NULL;
            for (int i = 0; i + 4 <= (int)n; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    marker = &buf[i];
                    break;
                }
            }
            if (marker) {
                size_t offset = (size_t)(marker - buf) + 4;
                fwrite(buf + offset, 1, (size_t)n - offset, file);
                body = true;
            }
        } else {
            fwrite(buf, 1, (size_t)n, file);
        }
    }
    fclose(file);
    close(sock);

    int wake = socket(AF_INET, SOCK_STREAM, 0);
    if (wake >= 0) {
        if (connect(wake, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            send(wake, "SHUTDOWN", 8, 0);
        }
        close(wake);
    }

    nifmExit();
    socketExit();
    return body;
}

bool restoreTitleFromZip(const AppUser &user, const AppTitle &title, std::string &error)
{
    removeTree("sdmc:/temp");
    if (!unzipTempFolder()) {
        error = "Failed to unzip save archive";
        return false;
    }

    u64 app_id = strtoull(title.title_id, NULL, 16);
    Result rc = fsdevMountSaveData("save", app_id, user.uid);
    if (R_FAILED(rc)) {
        error = "Failed to mount save data";
        return false;
    }
    removeTree("save:/");
    char source[128];
    snprintf(source, sizeof(source), "sdmc:/temp/%s", title.title_id);
    bool copied = copyTree(source, "save:/");
    rc = fsdevCommitDevice("save");
    fsdevUnmountDevice("save");
    if (!copied || R_FAILED(rc)) {
        error = "Failed to restore save files";
        return false;
    }
    return true;
}
