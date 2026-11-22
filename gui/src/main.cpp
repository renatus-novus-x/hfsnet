// main.cpp - hfsnet ホスト GUI 版 (プロトタイプ)
// 使い方:
//   hfsnet-host [--lang ja|en] [folder]      GUI
//   hfsnet-host --headless <folder>          エンジンのみ (テスト・CI 用)
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "ui_strings.h"

namespace {

// ---- 設定 (言語とフォルダを小さな ini に残す) ----
std::string g_settings_path = "hfsnet-host.ini";

struct Settings {
    Lang lang = Lang::Ja;
    std::string folder;
};

Settings load_settings()
{
    Settings st;
    FILE *f = std::fopen(g_settings_path.c_str(), "r");
    if (!f)
        return st;
    char line[600];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "lang=", 5) == 0)
            st.lang = std::strstr(line, "en") ? Lang::En : Lang::Ja;
        else if (std::strncmp(line, "folder=", 7) == 0) {
            st.folder = line + 7;
            while (!st.folder.empty() &&
                   (st.folder.back() == '\n' || st.folder.back() == '\r'))
                st.folder.pop_back();
        }
    }
    std::fclose(f);
    return st;
}

void save_settings(const Settings &st)
{
    FILE *f = std::fopen(g_settings_path.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "lang=%s\nfolder=%s\n",
                 st.lang == Lang::En ? "en" : "ja", st.folder.c_str());
    std::fclose(f);
}

// ---- ログ受け口 ----
struct LogSink {
    std::mutex m;
    std::deque<std::string> lines;
    void add(const std::string &s)
    {
        std::lock_guard<std::mutex> lk(m);
        lines.push_back(s);
        while (lines.size() > 500)
            lines.pop_front();
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(m);
        lines.clear();
    }
};

int run_headless(const std::string &folder)
{
    Engine engine;
    engine.set_log_callback([](const std::string &s) {
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    });
    engine.start(folder);
    engine.wait();
    return 0;
}

} // namespace

// ---- GUI ----
#ifndef HFSNET_NO_GUI
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "nfd.h"

#include "icon_rgba.h"

namespace {

const char *STATE_NAMES[] = {"CLOSED",   "LISTEN",      "SYN_SENT",
                             "SYN_RCVD", "ESTABLISHED", "CLOSE_WAIT"};
const char *KIND_NAMES[] = {"TCP", "UDP", "RAW"};

std::string ip_str(uint32_t ip, uint16_t port)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                  (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255,
                  ip & 255, port);
    return buf;
}

std::string byte_str(uint64_t n)
{
    char buf[64];
    if (n >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%llu (%.1f MiB)",
                      (unsigned long long)n, n / 1048576.0);
    else if (n >= 1024)
        std::snprintf(buf, sizeof(buf), "%llu (%.1f KiB)",
                      (unsigned long long)n, n / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    return buf;
}

bool load_japanese_font()
{
    const char *candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/PingFang.ttc",
#else
        "fonts/ipaexg.ttf",
        "ipaexg.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/ipaexfont-gothic/ipaexg.ttf",
#endif
    };
    ImGuiIO &io = ImGui::GetIO();
    for (const char *path : candidates) {
        // 存在しないファイルで AddFontFromFileTTF を呼ぶと ImGui が
        // エラーを記録する (デバッグビルドではその経路で落ちる) ので、
        // 先に存在を確認する
        FILE *fp = std::fopen(path, "rb");
        if (!fp)
            continue;
        std::fclose(fp);
        if (io.Fonts->AddFontFromFileTTF(
                path, 16.0f, nullptr, io.Fonts->GetGlyphRangesJapanese()))
            return true;
    }
    io.Fonts->AddFontDefault();
    return false;
}

void draw_graph(const Snapshot &sn)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 80.0f;
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                ImGui::GetColorU32(ImGuiCol_Border));
    if (sn.history.empty())
        return;
    uint64_t mx = 1;
    for (auto &p : sn.history)
        mx = std::max(mx, std::max(p.first, p.second));
    auto plot = [&](bool tx, ImU32 color) {
        size_t n = sn.history.size();
        for (size_t i = 1; i < n; i++) {
            float x0 = pos.x + w * (i - 1) / 59.0f;
            float x1 = pos.x + w * i / 59.0f;
            float v0 = (float)(tx ? sn.history[i - 1].first
                                  : sn.history[i - 1].second);
            float v1 = (float)(tx ? sn.history[i].first
                                  : sn.history[i].second);
            float y0 = pos.y + h - 4 - (h - 8) * v0 / (float)mx;
            float y1 = pos.y + h - 4 - (h - 8) * v1 / (float)mx;
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 1.5f);
        }
    };
    plot(true, IM_COL32(80, 160, 255, 255));
    plot(false, IM_COL32(90, 220, 130, 255));
}

// ---- フォルダ欄へのドラッグ&ドロップ ----
// ドロップ位置がフォルダ入力欄の矩形の中だったときだけ、そのパスを
// 共有フォルダとして受け付ける。ファイルが落とされた場合は親フォルダを
// 使う。実際の設定はメインループ側で g_drop_pending を見て反映する。
std::string g_drop_pending;
ImVec2 g_folder_rect_min(0, 0), g_folder_rect_max(0, 0);

void drop_callback(GLFWwindow *window, int count, const char **paths)
{
    if (count <= 0 || !paths || !paths[0])
        return;
    double cx = 0, cy = 0;
    glfwGetCursorPos(window, &cx, &cy);
    if (cx < g_folder_rect_min.x || cx > g_folder_rect_max.x ||
        cy < g_folder_rect_min.y || cy > g_folder_rect_max.y)
        return;
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(paths[0]);
    if (std::filesystem::is_directory(p, ec)) {
        g_drop_pending = p.u8string();
    } else {
        std::filesystem::path parent = p.parent_path();
        if (!parent.empty())
            g_drop_pending = parent.u8string();
    }
}

int run_gui(Settings settings)
{
    if (!glfwInit())
        return 1;
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    const char *glsl = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char *glsl = "#version 130";
#endif
    GLFWwindow *window = glfwCreateWindow(760, 640, "hfsnet Host",
                                          nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    {
        // ウィンドウアイコン (タスクバー/タイトルバー用)。
        // Windows では exe リソースのアイコンが優先されるが、
        // 他 OS と実行時表示の統一のためこちらでも設定する
        GLFWimage icons[3];
        icons[0].width = 32;
        icons[0].height = 32;
        icons[0].pixels = const_cast<unsigned char *>(icon_rgba_32);
        icons[1].width = 48;
        icons[1].height = 48;
        icons[1].pixels = const_cast<unsigned char *>(icon_rgba_48);
        icons[2].width = 64;
        icons[2].height = 64;
        icons[2].pixels = const_cast<unsigned char *>(icon_rgba_64);
        glfwSetWindowIcon(window, 3, icons);
    }
    glfwSetDropCallback(window, drop_callback);
    glfwSwapInterval(1);
    glewInit();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    bool jp_font_ok = load_japanese_font();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);
    NFD_Init();

    Engine engine;
    LogSink sink;
    engine.set_log_callback([&sink](const std::string &s) { sink.add(s); });
    if (!jp_font_ok) {
        // 日本語フォントが無いと日本語表示は読めないので、このセッション
        // の表示だけ English に倒し、理由をログに残す (設定は変えない)
        if (i18n::lang() == Lang::Ja)
            i18n::set_lang(Lang::En);
        sink.add(i18n::s(Str::FontMissing));
    }

    char folder_buf[512];
    std::snprintf(folder_buf, sizeof(folder_buf), "%s",
                  settings.folder.c_str());
    bool show_about = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        Snapshot sn = engine.snapshot();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        int wpx, hpx;
        glfwGetWindowSize(window, &wpx, &hpx);
        ImGui::SetNextWindowSize(ImVec2((float)wpx, (float)hpx));
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu(i18n::s(Str::MenuFile))) {
                if (ImGui::MenuItem(i18n::s(Str::MenuQuit)))
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(i18n::s(Str::MenuLanguage))) {
                if (ImGui::MenuItem("日本語", nullptr,
                                    i18n::lang() == Lang::Ja)) {
                    i18n::set_lang(Lang::Ja);
                    settings.lang = Lang::Ja;
                    save_settings(settings);
                }
                if (ImGui::MenuItem("English", nullptr,
                                    i18n::lang() == Lang::En)) {
                    i18n::set_lang(Lang::En);
                    settings.lang = Lang::En;
                    save_settings(settings);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(i18n::s(Str::MenuHelp))) {
                if (ImGui::MenuItem(i18n::s(Str::MenuAbout)))
                    show_about = true;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (show_about) {
            ImGui::OpenPopup("about");
            show_about = false;
        }
        if (ImGui::BeginPopupModal("about", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(i18n::s(Str::AboutText));
            if (ImGui::Button("OK", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (!g_drop_pending.empty()) {
            std::snprintf(folder_buf, sizeof(folder_buf), "%s",
                          g_drop_pending.c_str());
            g_drop_pending.clear();
        }
        ImGui::TextUnformatted(i18n::s(Str::Folder));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190);
        ImGui::InputText("##folder", folder_buf, sizeof(folder_buf));
        g_folder_rect_min = ImGui::GetItemRectMin();
        g_folder_rect_max = ImGui::GetItemRectMax();
        ImGui::SameLine();
        if (ImGui::Button(i18n::s(Str::Browse), ImVec2(80, 0))) {
            nfdchar_t *out = nullptr;
            nfdresult_t res = NFD_PickFolder(&out, nullptr);
            if (res == NFD_OKAY && out) {
                std::snprintf(folder_buf, sizeof(folder_buf), "%s", out);
                NFD_FreePath(out);
            } else if (res == NFD_ERROR) {
                // ダイアログ基盤が無い環境 (WSL 等) では無反応に見える
                // ため、失敗したことをログで知らせる (キャンセルは無言)
                sink.add(i18n::s(Str::DialogFailed));
            }
        }
        ImGui::SameLine();
        if (!sn.running) {
            if (ImGui::Button(i18n::s(Str::Start), ImVec2(90, 0))) {
                engine.start(folder_buf);
                settings.folder = folder_buf;
                save_settings(settings);
            }
        } else {
            if (ImGui::Button(i18n::s(Str::Stop), ImVec2(90, 0)))
                engine.stop();
        }

        ImGui::Separator();
        ImGui::Text("%s: %s", i18n::s(Str::Title),
                    sn.running ? i18n::s(Str::StatusRunning)
                               : i18n::s(Str::StatusStopped));
        ImGui::SameLine();
        ImGui::Text("  %s: %llu", i18n::s(Str::StatePolls),
                    (unsigned long long)sn.state_polls);
        ImGui::Text("%s: %s    %s: %s", i18n::s(Str::TxTotal),
                    byte_str(sn.tx_total).c_str(), i18n::s(Str::RxTotal),
                    byte_str(sn.rx_total).c_str());
        ImGui::Text("%s: TX %llu %s / RX %llu %s", i18n::s(Str::Rate),
                    (unsigned long long)sn.tx_rate, i18n::s(Str::PerSec),
                    (unsigned long long)sn.rx_rate, i18n::s(Str::PerSec));

        ImGui::Separator();
        ImGui::TextUnformatted(i18n::s(Str::HistoryTitle));
        draw_graph(sn);

        ImGui::Separator();
        ImGui::TextUnformatted(i18n::s(Str::SlotsTitle));
        if (ImGui::BeginTable("slots", 6,
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(i18n::s(Str::ColSlot));
            ImGui::TableSetupColumn(i18n::s(Str::ColKind));
            ImGui::TableSetupColumn(i18n::s(Str::ColState));
            ImGui::TableSetupColumn(i18n::s(Str::ColPeer));
            ImGui::TableSetupColumn(i18n::s(Str::ColTx));
            ImGui::TableSetupColumn(i18n::s(Str::ColRx));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < sn.slots.size(); i++) {
                const SlotInfo &in = sn.slots[i];
                if (!in.used)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", i);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(KIND_NAMES[in.kind]);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(
                    (in.state >= 0 && in.state <= 5) ? STATE_NAMES[in.state]
                                                     : "?");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    ip_str(in.peer_ip, in.peer_port).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", (unsigned long long)in.tx);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu", (unsigned long long)in.rx);
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::TextUnformatted(i18n::s(Str::LogTitle));
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::s(Str::ClearLog)))
            sink.clear();
        ImGui::BeginChild("logpane", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> lk(sink.m);
            for (auto &line : sink.lines)
                ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::End();
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    engine.stop();
    settings.folder = folder_buf;
    save_settings(settings);
    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace
#endif // HFSNET_NO_GUI

int main(int argc, char **argv)
{
    Settings settings = load_settings();
    bool headless = false;
    std::string folder;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--headless")
            headless = true;
        else if (a == "--lang" && i + 1 < argc)
            settings.lang = std::string(argv[++i]) == "en" ? Lang::En
                                                           : Lang::Ja;
        else
            folder = a;
    }
    i18n::set_lang(settings.lang);
    if (headless) {
        if (folder.empty() && !settings.folder.empty())
            folder = settings.folder;
        if (folder.empty()) {
            std::fprintf(stderr, "usage: hfsnet-host --headless <folder>\n");
            return 1;
        }
        return run_headless(folder);
    }
    if (!folder.empty())
        settings.folder = folder;
#ifdef HFSNET_NO_GUI
    std::fprintf(stderr, "this build has no GUI; use --headless\n");
    return 1;
#else
    return run_gui(settings);
#endif
}
