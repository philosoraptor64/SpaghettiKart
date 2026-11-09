/*
 * Online screen: host or join an online session from inside the game.
 *
 * Works with a finger and with a controller: every control is a big button
 * (taps arrive as mouse clicks), and while the screen is open ImGui's gamepad
 * navigation is on (D-pad moves, A presses, B backs out) and the game itself
 * gets no input. Open it with the "Online" button in the corner of the menus,
 * the controller's View/Back button, or F2.
 *
 * The room code is picked with letter tiles (+ above and - below each letter;
 * with a controller: D-pad onto them, A to step), so no keyboard is needed; the server and password (Settings)
 * are text fields, filled once (or later by an invite link).
 *
 * Settings live in cvars: gNet.Server ("host:port"), gNet.Password,
 * gNet.Voice (0 off, 1 open mic, 2 push-to-talk), gNet.LastRoom.
 * The connection itself is KartNet's (KartNet_Ui*).
 */
#include "OnlineWindow.h"

#include <imgui.h>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include "port/KartNet.h"
#include "main.h"
#include "defines.h"
}

namespace {

const char kCodeChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const int kNumCodeChars = (int) sizeof(kCodeChars) - 1;

bool sOpen = false;
bool sWasOpen = false;
bool sFocusNext = false;
bool sShowSettings = false;
int sCode[4] = { 0, 0, 0, 0 };
char sServer[128] = "";
char sPassword[128] = "";
int sVoice = 1;
bool sLoaded = false;
ImGuiConfigFlags sSavedFlags = 0;

int CodeIndex(char c) {
    const char* p = strchr(kCodeChars, c);
    return (p && c) ? (int) (p - kCodeChars) : 0;
}

void LoadSettings() {
    if (sLoaded) {
        return;
    }
    sLoaded = true;
    snprintf(sServer, sizeof(sServer), "%s", CVarGetString("gNet.Server", ""));
    snprintf(sPassword, sizeof(sPassword), "%s", CVarGetString("gNet.Password", ""));
    sVoice = CVarGetInteger("gNet.Voice", 1);
    const char* last = CVarGetString("gNet.LastRoom", "");
    for (int i = 0; i < 4; i++) {
        sCode[i] = (last && (int) strlen(last) > i) ? CodeIndex(last[i]) : 0;
    }
}

void SaveSettings() {
    CVarSetString("gNet.Server", sServer);
    CVarSetString("gNet.Password", sPassword);
    CVarSetInteger("gNet.Voice", sVoice);
    CVarSave();
}

std::string CodeString() {
    std::string s;
    for (int i = 0; i < 4; i++) {
        s += kCodeChars[sCode[i]];
    }
    return s;
}

/* A full-width button. */
bool WideButton(const char* label, float height) {
    return ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, height));
}

void SetOpen(bool open) {
    if (open == sOpen) {
        return;
    }
    sOpen = open;
    if (open) {
        LoadSettings();
        sFocusNext = true;
    }
}

void DrawOpener(float scale) {
    /* only in the menus (not in races) and not while the screen is open */
    if (sOpen || gGamestate == RACING) {
        return;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const char* label = KartNet_UiPhase() == KARTNET_UI_IDLE ? "Online" : "Online...";
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 8.0f * scale, vp->Pos.y + 8.0f * scale), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##OnlineOpener", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale);
        if (ImGui::Button(label)) {
            SetOpen(true);
        }
    }
    ImGui::End();
}

void DrawCodePicker(float scale) {
    const float tile = 54.0f * scale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 12.0f * scale;
    const float startX = ImGui::GetCursorPosX() + (avail - (4 * tile + 3 * gap)) / 2.0f;
    const float y0 = ImGui::GetCursorPosY();
    for (int i = 0; i < 4; i++) {
        ImGui::PushID(i);
        const float x = startX + (float) i * (tile + gap);
        ImGui::SetCursorPos(ImVec2(x, y0));
        if (ImGui::Button("+", ImVec2(tile, tile * 0.7f))) {
            sCode[i] = (sCode[i] + 1) % kNumCodeChars;
        }
        ImGui::SetCursorPos(ImVec2(x, y0 + tile * 0.75f));
        char letter[2] = { kCodeChars[sCode[i]], 0 };
        /* the letter itself: tapping it (or A on it) also steps it forward */
        if (ImGui::Button(letter, ImVec2(tile, tile))) {
            sCode[i] = (sCode[i] + 1) % kNumCodeChars;
        }
        ImGui::SetCursorPos(ImVec2(x, y0 + tile * 1.8f));
        if (ImGui::Button("-", ImVec2(tile, tile * 0.7f))) {
            sCode[i] = (sCode[i] + kNumCodeChars - 1) % kNumCodeChars;
        }
        ImGui::PopID();
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), y0 + tile * 2.6f));
    ImGui::Dummy(ImVec2(1, 1));
}

void DrawSettings(float scale) {
    ImGui::TextUnformatted("Room server (address:port)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##server", sServer, sizeof(sServer));
    ImGui::TextUnformatted("Server password");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##password", sPassword, sizeof(sPassword), ImGuiInputTextFlags_Password);
    ImGui::TextUnformatted("Voice chat");
    const char* voices[] = { "Off", "Open mic", "Push to talk" };
    for (int i = 0; i < 3; i++) {
        if (i) {
            ImGui::SameLine();
        }
        if (ImGui::RadioButton(voices[i], sVoice == i)) {
            sVoice = i;
        }
    }
    if (WideButton("Save settings", 40.0f * scale)) {
        SaveSettings();
        sShowSettings = false;
    }
}

void DrawScreen(float scale) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(vp->Size.x * 0.9f, 520.0f * scale), std::min(vp->Size.y * 0.92f, 600.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (sFocusNext) {
        ImGui::SetNextWindowFocus();
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    bool open = true;
    if (ImGui::Begin("Play online", &open, flags)) {
        ImGui::SetWindowFontScale(scale);
        const float buttonH = 46.0f * scale;
        const int phase = KartNet_UiPhase();
        char status[200];
        KartNet_UiStatus(status, sizeof(status));
        char room[8];
        KartNet_UiRoom(room, sizeof(room));

        if (phase == KARTNET_UI_IDLE) {
            if (sShowSettings) {
                DrawSettings(scale);
            } else {
                if (sFocusNext) {
                    ImGui::SetKeyboardFocusHere();
                }
                if (WideButton("Host a room", buttonH)) {
                    SaveSettings();
                    KartNet_UiSetVoice(sVoice);
                    KartNet_UiStart(1, sServer, "", sPassword);
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextUnformatted("Join a friend's room:");
                DrawCodePicker(scale);
                if (WideButton("Join", buttonH)) {
                    std::string code = CodeString();
                    CVarSetString("gNet.LastRoom", code.c_str());
                    SaveSettings();
                    KartNet_UiSetVoice(sVoice);
                    KartNet_UiStart(0, sServer, code.c_str(), sPassword);
                }
                ImGui::Spacing();
                ImGui::Separator();
                if (ImGui::Button("Settings", ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, buttonH * 0.8f))) {
                    sShowSettings = true;
                }
                ImGui::SameLine();
                if (WideButton("Close", buttonH * 0.8f)) {
                    open = false;
                }
            }
        } else {
            if (room[0]) {
                ImGui::TextUnformatted("Room code");
                ImGui::SetWindowFontScale(scale * 2.5f);
                ImGui::TextUnformatted(room);
                ImGui::SetWindowFontScale(scale);
                ImGui::TextUnformatted("Tell your friend this code.");
                ImGui::Spacing();
            }
            const char* stopLabel = phase == KARTNET_UI_SESSION ? "Leave the session" : "Cancel";
            if (WideButton(stopLabel, buttonH)) {
                KartNet_UiStop();
            }
            if (WideButton("Close this screen", buttonH * 0.8f)) {
                open = false;
            }
        }
        if (status[0]) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextWrapped("%s", status);
        }
        /* B (or Escape) backs out of the settings, then closes the screen */
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (sShowSettings) {
                sShowSettings = false;
            } else {
                open = false;
            }
        }
    }
    ImGui::End();
    sFocusNext = false;
    if (!open) {
        SetOpen(false);
    }
}

} // namespace

void OnlineWindow::Draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float scale = vp->Size.y / 480.0f;
    if (scale < 1.0f) {
        scale = 1.0f;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        SetOpen(!sOpen);
    }
    /* a session that just started closes the screen */
    static int lastPhase = KARTNET_UI_IDLE;
    const int phase = KartNet_UiPhase();
    if (phase == KARTNET_UI_SESSION && lastPhase != KARTNET_UI_SESSION) {
        SetOpen(false);
    }
    lastPhase = phase;

    ImGuiIO& io = ImGui::GetIO();
    if (sOpen && !sWasOpen) {
        sSavedFlags = io.ConfigFlags;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    } else if (!sOpen && sWasOpen) {
        io.ConfigFlags = (io.ConfigFlags & ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard)) |
                         (sSavedFlags & (ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard));
    }
    sWasOpen = sOpen;
    KartNet_UiSetBlocking(sOpen ? 1 : 0);

    DrawOpener(scale);
    if (sOpen) {
        DrawScreen(scale);
    }
}
