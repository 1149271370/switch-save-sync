#pragma once

#include <switch.h>

#include <vector>

struct AppUser {
    AccountUid uid;
    char nickname[0x21];
};

struct AppTitle {
    char title_id[17];
    char name[0x201];
    u64 size_bytes;
    u64 modified;
};

struct SystemSnapshot {
    char firmware[64];
    char hardware[64];
    char atmosphere[64];
    char region[32];
};

Result scanUsers(std::vector<AppUser> &users);
Result scanTitles(const AccountUid &uid, std::vector<AppTitle> &titles);
Result scanSystem(SystemSnapshot &snapshot);
