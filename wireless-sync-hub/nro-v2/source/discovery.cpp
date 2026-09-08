#include "discovery.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

static const char *hardwareName(u64 type)
{
    switch (type) {
    case 0: return "Switch (Erista)";
    case 1: return "Switch dev (Erista)";
    case 2: return "Switch Lite";
    case 3: return "Switch v2 (Mariko)";
    case 4: return "Switch dev (Mariko)";
    case 5: return "Switch OLED";
    default: return "Unknown";
    }
}

static void directoryStats(const char *path, u64 *size, u64 *modified)
{
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            directoryStats(full, size, modified);
        } else if (S_ISREG(st.st_mode)) {
            *size += (u64)st.st_size;
            if ((u64)st.st_mtime > *modified) *modified = (u64)st.st_mtime;
        }
    }
    closedir(dir);
}

static void titleName(u64 app_id, char *out, size_t out_size)
{
    out[0] = 0;
    NsApplicationControlData *data = (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));
    if (!data) return;
    memset(data, 0, sizeof(*data));

    u64 out_size_raw = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id,
                                             data, sizeof(*data), &out_size_raw))) {
        free(data);
        return;
    }

    NacpLanguageEntry *entry = NULL;
    if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry)) && entry && entry->name) {
        strncpy(out, entry->name, out_size - 1);
        out[out_size - 1] = 0;
    }
    free(data);
}

Result scanUsers(std::vector<AppUser> &users)
{
    users.clear();
    Result rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) return rc;

    s32 count = 0;
    rc = accountGetUserCount(&count);
    if (R_FAILED(rc)) {
        accountExit();
        return rc;
    }
    if (count <= 0) {
        accountExit();
        return 0;
    }

    std::vector<AccountUid> ids(count);
    s32 actual = 0;
    rc = accountListAllUsers(ids.data(), count, &actual);
    if (R_FAILED(rc)) {
        accountExit();
        return rc;
    }

    for (s32 i = 0; i < actual; i++) {
        AccountProfile profile;
        AccountProfileBase base;
        if (R_FAILED(accountGetProfile(&profile, ids[i]))) continue;
        if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
            AppUser user;
            user.uid = ids[i];
            memset(user.nickname, 0, sizeof(user.nickname));
            strncpy(user.nickname, base.nickname, sizeof(user.nickname) - 1);
            users.push_back(user);
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return 0;
}

Result scanTitles(const AccountUid &uid, std::vector<AppTitle> &titles)
{
    titles.clear();
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return rc;

    NsApplicationRecord records[512];
    s32 record_count = 0;
    rc = nsListApplicationRecord(records, 512, 0, &record_count);
    if (R_FAILED(rc)) {
        nsExit();
        return rc;
    }

    for (s32 i = 0; i < record_count; i++) {
        u64 app_id = records[i].application_id;
        rc = fsdevMountSaveData("save", app_id, uid);
        if (R_FAILED(rc)) continue;

        AppTitle title;
        memset(&title, 0, sizeof(title));
        snprintf(title.title_id, sizeof(title.title_id), "%016lX", app_id);
        titleName(app_id, title.name, sizeof(title.name));
        title.size_bytes = 0;
        title.modified = 0;
        directoryStats("save:/", &title.size_bytes, &title.modified);
        fsdevUnmountDevice("save");

        if (title.name[0]) {
            titles.push_back(title);
        }
    }

    nsExit();
    return 0;
}

Result scanSystem(SystemSnapshot &snapshot)
{
    memset(&snapshot, 0, sizeof(snapshot));

    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            snprintf(snapshot.firmware, sizeof(snapshot.firmware), "%u.%u.%u %s",
                     fw.major, fw.minor, fw.micro, fw.display_version);
        }
        setsysExit();
    }

    rc = splInitialize();
    if (R_SUCCEEDED(rc)) {
        u64 hardware = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem_HardwareType, &hardware))) {
            snprintf(snapshot.hardware, sizeof(snapshot.hardware), "%llu - %s",
                     (unsigned long long)hardware, hardwareName(hardware));
        }
        u64 exo = 0;
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &exo))) {
            u32 major = (u32)((exo >> 56) & 0xFF);
            u32 minor = (u32)((exo >> 48) & 0xFF);
            u32 micro = (u32)((exo >> 40) & 0xFF);
            snprintf(snapshot.atmosphere, sizeof(snapshot.atmosphere),
                     "Atmosphere %u.%u.%u", major, minor, micro);
        } else {
            strncpy(snapshot.atmosphere, "Not detected", sizeof(snapshot.atmosphere) - 1);
        }
        splExit();
    }
    return 0;
}
