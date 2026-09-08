#include <switch.h>

#include <stdio.h>
#include <string.h>
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
static char g_pc_ip[64] = "192.168.1.100";
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
    io.Fonts->AddFontDefault();
    if (R_SUCCEEDED(plInitialize(PlServiceType_System))) {
        PlFontData std_font, ext_font;
        if (R_SUCCEEDED(plGetSharedFontByType(&std_font, PlSharedFontType_Standard)) &&
            R_SUCCEEDED(plGetSharedFontByType(&ext_font, PlSharedFontType_NintendoExt))) {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            strncpy(config.Name, "Nintendo Standard", sizeof(config.Name) - 1);
            io.Fonts->AddFontFromMemoryTTF(std_font.address, std_font.size, 24.0f, &config,
                                           io.Fonts->GetGlyphRangesCyrillic());
        }
        plExit();
    }
    io.Fonts->Build();
}

static void drawHeader()
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.12f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.13f, 0.24f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.18f, 0.30f, 1.0f));

    static const char *names[] = {"Overview", "Games", "System", "Settings"};
    for (int i = 0; i < Screen_Count; i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(names[i], ImVec2(180, 42))) {
            g_app.screen = (ScreenId)i;
        }
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    const char *server_status = g_info_server.running() ? "Info server: ON" : "Info server: OFF";
    ImGui::TextUnformatted(server_status);

    ImGui::PopStyleColor(3);
}

static void drawOverview()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::BeginChild("overview", ImVec2(0, 0), true);
    ImGui::TextWrapped("Connect this homebrew to the PC Hub by WiFi. "
                       "The PC app can discover installed games, save sizes and system information.");
    ImGui::Spacing();
    ImGui::Text("Users: %d", (int)g_users.size());
    ImGui::Text("Installed saves: %d", (int)g_titles.size());
    ImGui::Text("Firmware: %s", g_system.firmware[0] ? g_system.firmware : "unknown");
    ImGui::Text("Hardware: %s", g_system.hardware[0] ? g_system.hardware : "unknown");
    ImGui::Text("CFW: %s", g_system.atmosphere[0] ? g_system.atmosphere : "unknown");
    ImGui::Spacing();
    if (ImGui::Button("Scan installed games", ImVec2(260, 48))) {
        g_app.scan_requested = true;
        refreshData();
        g_app.screen = Screen_Games;
    }
    ImGui::SameLine();
    if (ImGui::Button("Start WiFi info server", ImVec2(280, 48))) {
        if (g_info_server.running()) {
            g_info_server.stop();
            g_app.server_running = false;
        } else {
            g_app.server_running = g_info_server.start(buildCatalogJson());
            if (g_app.server_running) {
                snprintf(g_app.status, sizeof(g_app.status),
                         "Info server listening on port 8080. Open PC Hub and press WiFi scan.");
            } else {
                snprintf(g_app.status, sizeof(g_app.status),
                         "Failed to start info server. Check network or port 8080.");
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
        ImGui::TextWrapped("No save data found. Run games once so they create saves, then rescan.");
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
        ImGui::Text("Selected: %s | %s", title.name, title.title_id);
        if (!g_info_server.running() && !g_transfer_server.active()) {
            if (ImGui::Button("Export selected to PC", ImVec2(300, 42))) {
                std::string error;
                if (exportTitleToZip(g_users[0], title, error)) {
                    if (g_transfer_server.start("sdmc:/temp.zip")) {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 "Waiting for PC to download %s...", title.name);
                    } else {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 "Failed to start transfer server");
                    }
                } else {
                    snprintf(g_transfer_status, sizeof(g_transfer_status),
                             "Export failed: %s", error.c_str());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Receive from PC", ImVec2(300, 42))) {
                std::string error;
                if (downloadZipFromPc(g_pc_ip, error)) {
                    if (restoreTitleFromZip(g_users[0], title, error)) {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 "Restored %s from PC", title.name);
                    } else {
                        snprintf(g_transfer_status, sizeof(g_transfer_status),
                                 "Restore failed: %s", error.c_str());
                    }
                } else {
                    snprintf(g_transfer_status, sizeof(g_transfer_status),
                             "Download failed: %s", error.c_str());
                }
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Status: %s", g_transfer_status);
    ImGui::EndChild();
}

static void drawSystem()
{
    ImGui::BeginChild("system", ImVec2(0, 0), true);
    ImGui::TextWrapped("Firmware: %s", g_system.firmware[0] ? g_system.firmware : "unknown");
    ImGui::TextWrapped("Hardware: %s", g_system.hardware[0] ? g_system.hardware : "unknown");
    ImGui::TextWrapped("CFW: %s", g_system.atmosphere[0] ? g_system.atmosphere : "unknown");
    ImGui::Separator();
    for (size_t i = 0; i < g_users.size(); i++) {
        ImGui::Text("User %d: %s", (int)i + 1, g_users[i].nickname);
    }
    ImGui::EndChild();
}

static void drawSettings()
{
    ImGui::BeginChild("settings", ImVec2(0, 0), true);
    ImGui::InputText("PC IP", g_pc_ip, sizeof(g_pc_ip));
    ImGui::TextWrapped("Use this IP on the PC Hub when syncing PC to Switch.");
    ImGui::EndChild();
}

static void drawFrame()
{
    ImGui::Begin("Switch Save Sync Hub", NULL,
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
    if (!initSdl()) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    loadSystemFont(io);

    refreshData();

    ImGui_ImplSDL2_InitForOpenGL(g_window, g_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    bool exit_app = false;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
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
    return 0;
}
