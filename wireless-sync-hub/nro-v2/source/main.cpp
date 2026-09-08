#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <glad/glad.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"
#include "imgui/imgui_impl_opengl3.h"

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

static u64 hidKeysAllDown()
{
    u8 controller;
    u64 keysDown = 0;
    for (controller = 0; controller < (u8)CONTROLLER_P1_AUTO; controller++) {
        keysDown |= hidKeysDown((HidControllerID)controller);
    }
    return keysDown;
}

void consoleErrorScreen(const char *fmt, ...)
{
    consoleInit(NULL);
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    printf("\nPress any button to exit.\n");
    while (appletMainLoop()) {
        hidScanInput();
        u64 keysDown = hidKeysAllDown();
        if (keysDown && !((keysDown & KEY_TOUCH) || (keysDown & KEY_LSTICK_LEFT) ||
                          (keysDown & KEY_LSTICK_RIGHT) || (keysDown & KEY_LSTICK_UP) ||
                          (keysDown & KEY_LSTICK_DOWN) || (keysDown & KEY_RSTICK_LEFT) ||
                          (keysDown & KEY_RSTICK_RIGHT) || (keysDown & KEY_RSTICK_UP) ||
                          (keysDown & KEY_RSTICK_DOWN))) {
            break;
        }
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

static bool initSdl()
{
    bool success = true;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        consoleErrorScreen("SDL could not initialize! SDL Error: %s", SDL_GetError());
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
        consoleErrorScreen("Window could not be created! SDL Error: %s", SDL_GetError());
        return false;
    }

    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) {
        consoleErrorScreen("OpenGL context could not be created! SDL Error: %s", SDL_GetError());
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
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    const char *server_status = g_app.server_running ? "Info server: ON" : "Info server: OFF";
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
    if (ImGui::Button("Scan installed games", ImVec2(260, 48))) {
        g_app.scan_requested = true;
        g_app.screen = Screen_Games;
    }
    ImGui::SameLine();
    if (ImGui::Button("Start WiFi info server", ImVec2(280, 48))) {
        g_app.server_running = !g_app.server_running;
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(g_app.status);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void drawGames()
{
    ImGui::BeginChild("games", ImVec2(0, 0), true);
    ImGui::TextWrapped("Game discovery backend is being wired up. "
                       "This page will show every installed game, Title ID, save size and user.");
    ImGui::Spacing();
    if (g_app.scan_requested) {
        ImGui::TextUnformatted("Scan requested. Pull the latest NRO build for the full catalog.");
    }
    ImGui::EndChild();
}

static void drawSystem()
{
    ImGui::BeginChild("system", ImVec2(0, 0), true);
    ImGui::TextUnformatted("System information backend is being wired up.");
    ImGui::Separator();
    ImGui::TextWrapped("Planned fields: firmware version, hardware model, Atmosphere version, "
                       "user profiles, installed title count, free system memory.");
    ImGui::EndChild();
}

static void drawSettings()
{
    ImGui::BeginChild("settings", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Transfer settings backend is being wired up.");
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

    ImGui_ImplSDL2_InitForOpenGL(g_window, g_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    bool exit_app = false;
    while (!exit_app && appletMainLoop()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) exit_app = true;
        }

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
    SDL_Quit();
    return 0;
}
