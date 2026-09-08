#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL.h>
#include <glad/glad.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"
#include "imgui/imgui_impl_opengl3.h"
#include "discovery.h"
#include "info_server.h"
#include "transfer.h"

const GLuint SCREEN_WIDTH = 1280, SCREEN_HEIGHT = 720;

SDL_Window *g_window = NULL;
SDL_GLContext g_context;

enum Language {
    Lang_English,
    Lang_Chinese,
};

static Language g_language = Lang_English;
static ImFont *g_font_standard = NULL;
static ImFont *g_font_chinese = NULL;

static const char *T(const char *english, const char *chinese)
{
    return g_language == Lang_Chinese ? chinese : english;
}

static void logApp(const char *message)
{
    FILE *file = fopen("sdmc:/SwitchSaveSyncHub.log", "a");
    if (!file) return;
    fprintf(file, "%s\n", message);
    fclose(file);
}

enum ScreenId {
    Screen_Overview,
    Screen_Games,
    Screen_System,
    Screen_Settings,
    Screen_Count,
};

struct AppState {
    ScreenId screen = Screen_Overview;
    bool server_running = false;
    bool scan_requested = false;
    char status[512] = "Ready";
    int selected_game = -1;
};

static AppState g_app;
std::vector<AppUser> g_users;
std::vector<AppTitle> g_titles;
SystemSnapshot g_system;
static InfoServer g_info_server;
static TransferServer g_transfer_server;
char g_pc_ip[64] = "192.168.1.100";
static char g_transfer_status[1024] = "No transfer running";

static void refreshData()
{
    scanSystem(g_system);
    if (R_SUCCEEDED(scanUsers(g_users)) && !g_users.empty()) {
        scanTitles(g_users[0].uid, g_titles);
    }
    if (g_info_server.running()) {
        g_info_server.setPayload(buildCatalogJson());
    }
}

static void loadPcIpFromSd()
{
    FILE *file = fopen("sdmc:/switch/SwitchSaveSyncHub/config.json", "r");
    if (!file) return;
    char text[256] = {0};
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    const char *marker = strstr(text, "\"pc\":\"");
    if (!marker) return;
    marker += 6;
    char ip[64] = {0};
    size_t i = 0;
    while (marker[i] && marker[i] != '"' && i < sizeof(ip) - 1) {
        ip[i] = marker[i];
        i++;
    }
    ip[i] = 0;
    if (ip[0]) {
        snprintf(g_pc_ip, sizeof(g_pc_ip), "%s", ip);
    }
}

static bool initSdl()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL Error: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    g_window = SDL_CreateWindow(
        "Switch Save Sync Hub",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_window) {
        fprintf(stderr, "Window could not be created! SDL Error: %s\n", SDL_GetError());
        return false;
    }

    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) {
        fprintf(stderr, "OpenGL context could not be created! SDL Error: %s\n", SDL_GetError());
        return false;
    }

    gladLoadGL();
    return true;
}

static void loadSystemFont(ImGuiIO &io)
{
    if (R_SUCCEEDED(plInitialize(PlServiceType_System))) {
        PlFontData std_font;
        PlFontData chinese_font;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;

        if (R_SUCCEEDED(plGetSharedFontByType(&std_font, PlSharedFontType_Standard))) {
            strncpy(config.Name, "Nintendo Standard", sizeof(config.Name) - 1);
            g_font_standard = io.Fonts->AddFontFromMemoryTTF(
                std_font.address, std_font.size, 24.0f, &config,
                io.Fonts->GetGlyphRangesCyrillic());
        }
        if (R_SUCCEEDED(plGetSharedFontByType(&chinese_font, PlSharedFontType_ChineseSimplified))) {
            strncpy(config.Name, "Chinese Simplified", sizeof(config.Name) - 1);
            g_font_chinese = io.Fonts->AddFontFromMemoryTTF(
                chinese_font.address, chinese_font.size, 24.0f, &config,
                io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        io.Fonts->Build();
        plExit();
    }
}

static bool g_touch_down = false;

static void updateImGuiInput(ImGuiIO &io, const PadState &pad)
{
    u64 buttons = padGetButtons(&pad);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.NavInputs[ImGuiNavInput_Activate] = (buttons & HidNpadButton_A) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_Cancel] = (buttons & HidNpadButton_B) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_Input] = (buttons & HidNpadButton_X) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_Menu] = (buttons & HidNpadButton_Y) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadUp] = (buttons & HidNpadButton_AnyUp) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadDown] = (buttons & HidNpadButton_AnyDown) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadLeft] = (buttons & HidNpadButton_AnyLeft) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadRight] = (buttons & HidNpadButton_AnyRight) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_FocusPrev] = (buttons & HidNpadButton_L) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_FocusNext] = (buttons & HidNpadButton_R) ? 1.0f : 0.0f;

    HidTouchScreenState touch_state;
    memset(&touch_state, 0, sizeof(touch_state));
    hidGetTouchScreenStates(&touch_state, 1);
    if (touch_state.count > 0) {
        float x = (float)touch_state.touches[0].x;
        float y = (float)touch_state.touches[0].y;
        io.MousePos = ImVec2(x, y);
        io.MouseDown[0] = true;
        g_touch_down = true;
    } else {
        io.MouseDown[0] = false;
        g_touch_down = false;
    }
}

static void drawHeader()
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.12f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.13f, 0.24f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.18f, 0.30f, 1.0f));

    static const char *names[] = {
        T("Overview", "概览"),
        T("Games", "游戏"),
        T("System", "系统"),
        T("Settings", "设置"),
    };
    for (int i = 0; i < Screen_Count; i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(names[i], ImVec2(180, 42))) {
            g_app.screen = (ScreenId)i;
        }
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    const char *server_status = g_info_server.running()
        ? T("Info server: ON", "信息服务器：开")
        : T("Info server: OFF", "信息服务器：关");
    ImGui::TextUnformatted(server_status);

    ImGui::PopStyleColor(3);
}

static void drawOverview()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::BeginChild("overview", ImVec2(0, 0), true);
    ImGui::TextWrapped(T(
        "Connect this homebrew to the PC Hub by WiFi. "
        "The PC app can discover installed games, save sizes and system information.",
        "通过 WiFi 连接 PC Hub。PC 端可以自动发现游戏、存档大小和系统信息。"));
    ImGui::Spacing();
    ImGui::Text(T("Users: %d", "用户：%d"), (int)g_users.size());
    ImGui::Text(T("Installed saves: %d", "已安装存档：%d"), (int)g_titles.size());
    ImGui::Text(T("Firmware: %s", "固件：%s"),
                g_system.firmware[0] ? g_system.firmware : T("unknown", "未知"));
    ImGui::Text(T("Hardware: %s", "硬件：%s"),
                g_system.hardware[0] ? g_system.hardware : T("unknown", "未知"));
    ImGui::Text(T("CFW: %s", "自制系统：%s"),
                g_system.atmosphere[0] ? g_system.atmosphere : T("unknown", "未知"));
    ImGui::Spacing();
    if (ImGui::Button(T("Scan installed games", "扫描已安装游戏"), ImVec2(300, 48))) {
        g_app.scan_requested = true;
        refreshData();
        g_app.screen = Screen_Games;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Start WiFi info server", "启动 WiFi 信息服务器"), ImVec2(340, 48))) {
        if (g_info_server.running()) {
            g_info_server.stop();
            g_app.server_running = false;
        } else {
            g_app.server_running = g_info_server.start(buildCatalogJson());
            if (g_app.server_running) {
                snprintf(g_app.status, sizeof(g_app.status),
                         T("Info server listening on port 8080. Open PC Hub and press WiFi scan.",
                           "信息服务器已在 8080 端口监听，请在 PC Hub 中点击 WiFi 自动扫描。"));
            } else {
                snprintf(g_app.status, sizeof(g_app.status),
                         T("Failed to start info server. Check network or port 8080.",
                           "信息服务器启动失败，请检查网络或 8080 端口。"));
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(g_app.status);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void drawGames()
{
    ImGui::BeginChild("games", ImVec2(0, 0), true);
    if (g_titles.empty()) {
        ImGui::TextWrapped(T(
            "No save data found. Run games once so they create saves, then rescan.",
            "没有找到存档。请先运行一次游戏生成存档，然后重新扫描。"));
    }
    for (size_t i = 0; i < g_titles.size(); i++) {
        char label[1400];
        double mb = (double)g_titles[i].size_bytes / (1024.0 * 1024.0);
        snprintf(label, sizeof(label), "[%zu] %s | %s | %.2f MB",
                 i + 1, g_titles[i].name, g_titles[i].title_id, mb);
        if (ImGui::Selectable(label, (int)i == g_app.selected_game)) {
            g_app.selected_game = (int)i;
        }
    }
    ImGui::Separator();
    if (g_app.selected_game >= 0 && g_app.selected_game < (int)g_titles.size()) {
        AppTitle &title = g_titles[g_app.selected_game];
        ImGui::Text(T("Selected: %s | %s", "已选择：%s | %s"), title.name, title.title_id);
        if (!g_info_server.running() && !g_transfer_server.active()) {
            if (ImGui::Button(T("Export selected to PC", "发送所选存档到 PC"), ImVec2(360, 42))) {
                std::string error;
                if (exportTitleToZip(g_users[0], title, error)) {
                    if (g_transfer_server.start("sdmc:/temp.zip")) {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 T("Waiting for PC to download %s...",
                                   "等待 PC 下载 %s..."), title.name);
                    } else {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 T("Failed to start transfer server",
                                   "传输服务器启动失败"));
                    }
                } else {
                    snprintf(g_transfer_status, sizeof(g_transfer_status),
                             T("Export failed: %s", "导出失败：%s"), error.c_str());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Receive from PC", "从 PC 接收"), ImVec2(300, 42))) {
                std::string error;
                if (downloadZipFromPc(g_pc_ip, error)) {
                    if (restoreTitleFromZip(g_users[0], title, error)) {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 T("Restored %s from PC", "已从 PC 恢复 %s"), title.name);
                    } else {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 T("Restore failed: %s", "恢复失败：%s"), error.c_str());
                    }
                } else {
                    snprintf(g_transfer_status, sizeof(g_transfer_status),
                             T("Download failed: %s", "下载失败：%s"), error.c_str());
                }
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped(T("Status: %s", "状态：%s"), g_transfer_status);
    ImGui::EndChild();
}

static void drawSystem()
{
    ImGui::BeginChild("system", ImVec2(0, 0), true);
    ImGui::TextWrapped(T("Firmware: %s", "固件：%s"),
                       g_system.firmware[0] ? g_system.firmware : T("unknown", "未知"));
    ImGui::TextWrapped(T("Hardware: %s", "硬件：%s"),
                       g_system.hardware[0] ? g_system.hardware : T("unknown", "未知"));
    ImGui::TextWrapped(T("CFW: %s", "自制系统：%s"),
                       g_system.atmosphere[0] ? g_system.atmosphere : T("unknown", "未知"));
    ImGui::Separator();
    for (size_t i = 0; i < g_users.size(); i++) {
        ImGui::Text(T("User %d: %s", "用户 %d：%s"), (int)i + 1, g_users[i].nickname);
    }
    ImGui::EndChild();
}

static void drawSettings()
{
    ImGui::BeginChild("settings", ImVec2(0, 0), true);
    ImGui::TextWrapped(T("Language", "语言"));
    if (ImGui::Button(g_language == Lang_English ? "Chinese" : "English",
                      ImVec2(200, 42))) {
        g_language = g_language == Lang_English ? Lang_Chinese : Lang_English;
    }
    ImGui::Spacing();
    ImGui::InputText(T("PC IP", "PC IP"), g_pc_ip, sizeof(g_pc_ip));
    ImGui::TextWrapped(T(
        "Use this IP on the PC Hub when syncing PC to Switch.",
        "PC Hub 向 Switch 发送存档时使用此 IP。"));
    ImGui::EndChild();
}

static void drawFrame()
{
    ImGui::Begin(T("Switch Save Sync Hub", "Switch 存档同步中心"), NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::SetWindowPos(ImVec2(0, 0));
    ImGui::SetWindowSize(ImVec2((float)SCREEN_WIDTH, (float)SCREEN_HEIGHT));

    drawHeader();
    ImGui::Separator();

    if (g_app.screen == Screen_Overview) drawOverview();
    else if (g_app.screen == Screen_Games) drawGames();
    else if (g_app.screen == Screen_System) drawSystem();
    else drawSettings();

    ImGui::End();
}

int main()
{
    logApp("start");
    if (!initSdl()) return 1;
    logApp("sdl init ok");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    loadSystemFont(io);
    logApp("font load ok");

    loadPcIpFromSd();
    logApp("config load ok");

    ImGui_ImplSDL2_InitForOpenGL(g_window, g_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    logApp("imgui init ok");

    bool exit_app = false;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    logApp("main loop start");
    while (!exit_app && appletMainLoop()) {
        if (g_transfer_server.active() && g_transfer_server.done()) {
            g_transfer_server.stop();
            snprintf(g_transfer_status, sizeof(g_transfer_status),
                     "Export completed by PC");
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) exit_app = true;
        }
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) exit_app = true;
        io.FontDefault = (g_language == Lang_Chinese && g_font_chinese)
            ? g_font_chinese : g_font_standard;
        updateImGuiInput(io, pad);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame(g_window);
        ImGui::NewFrame();

        drawFrame();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g_window);
        SDL_Delay(1);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(g_context);
    SDL_DestroyWindow(g_window);
    g_info_server.stop();
    g_transfer_server.stop();
    SDL_Quit();
    logApp("exit");
    return 0;
}
