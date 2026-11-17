// The Linux updater keeps the existing AppImage verification and atomic replacement.
struct LinuxUpdate {
    std::thread worker;
    std::atomic<bool> busy{false}, auto_install{false}, ready{false}, cancel{false};
    bool restart_sent = false;
    std::mutex mutex;
    std::string release, message;
} g_linux_update;
bool UpdateBusy() { return g_linux_update.busy; }
void StartUpdateInstall() {
    auto& u = g_linux_update;
    if (u.busy || g_app.game_running || Env("APPIMAGE").empty()) return;
    if (u.worker.joinable()) u.worker.join();
    const std::string release = u.release;
    if (release.empty()) return;
    u.busy = true; u.auto_install = false; u.cancel = false; u.ready = false; u.restart_sent = false;
    u.worker = std::thread([release] {
        const auto answer = RunService("{\"op\":\"update_install\",\"release\":" + release + "}", 30 * 60 * 1000, &g_linux_update.cancel);
        std::scoped_lock lock(g_linux_update.mutex);
        g_linux_update.ready = JsonBool(answer, "ok");
        g_linux_update.message = g_linux_update.ready ? "Update installed. Restart the launcher to use it." : JsonText(answer, "error");
        g_linux_update.busy = false;
    });
}
void StartUpdateCheck(bool auto_install) {
    auto& u = g_linux_update;
    if (u.busy) return;
    if (u.worker.joinable()) u.worker.join();
    const auto version = g_app.version;
    u.busy = true; u.auto_install = false; u.ready = false;
    u.worker = std::thread([version, auto_install] {
        const auto answer = RunService("{\"op\":\"update_check\",\"version\":\"" + JsonEscape(version) + "\"}");
        std::scoped_lock lock(g_linux_update.mutex);
        const auto tag = JsonText(answer, "tag");
        g_linux_update.release = tag.empty() ? "" : answer;
        g_linux_update.message = !JsonText(answer, "error").empty() ? JsonText(answer, "error") :
            tag.empty() ? "Already up to date." : "Update available: " + tag;
        g_linux_update.auto_install = auto_install && !tag.empty();
        g_linux_update.busy = false;
    });
}
void PollUpdate() {
    auto& u = g_linux_update;
    if (!u.busy && u.worker.joinable()) {
        u.worker.join();
        if (u.auto_install.exchange(false) && !u.ready) StartUpdateInstall();
    }
    if (u.ready && !u.busy && !g_app.game_running && !u.restart_sent) {
        u.restart_sent = true;
        SaveLauncher();
        if (!RestartLauncher(L"--updated"))
            g_app.notice = T("Actualizado. Cierra y vuelve a abrir el launcher.", "Updated. Close and open the launcher again.");
    }
}
void UpdateBanner() {
    auto& u = g_linux_update;
    std::scoped_lock lock(u.mutex);
    if (u.release.empty()) return;
    ImGui::TextColored(kYellow, "%s", u.message.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(u.busy || g_app.game_running);
    if (u.ready) { if (ImGui::Button(T("Reiniciar", "Restart"))) RestartLauncher(L"--updated"); }
    else if (!Env("APPIMAGE").empty()) { if (ImGui::Button(T("Actualizar", "Update"))) StartUpdateInstall(); }
    else if (ImGui::Button("GitHub")) OpenUrl("https://github.com/Ryu7w7/OCTR-NitroFueled-PC/releases/latest");
    ImGui::EndDisabled();
}
void UpdateSection() {
    ImGui::SeparatorText(T("Actualizaciones", "Updates"));
    LauncherFlag("update_check", T("Buscar al abrir", "Check on startup"), true);
    if (!Env("APPIMAGE").empty()) LauncherFlag("update_auto", T("Descargar automáticamente", "Download automatically"), false);
    ImGui::BeginDisabled(UpdateBusy());
    if (ImGui::Button(T("Comprobar actualizaciones", "Check for updates"))) StartUpdateCheck(false);
    ImGui::EndDisabled();
    std::scoped_lock lock(g_linux_update.mutex);
    ImGui::TextWrapped("%s", g_linux_update.busy ? "Working..." : g_linux_update.message.c_str());
    if (Env("APPIMAGE").empty()) ImGui::TextDisabled("Automatic updates require the AppImage package.");
}
