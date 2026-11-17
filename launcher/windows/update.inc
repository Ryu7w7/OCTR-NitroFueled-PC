// Included by launcher.cpp inside its application namespace.
struct UpdateState {
    // Dismissed: a newer release the player chose not to install now (Tools offers it again).
    enum Stage { Idle, Checking, UpToDate, Available, Dismissed, Downloading, Unpacking, Installing, Done, Failed };
    std::mutex mutex;
    std::atomic<int> stage{Idle};
    PortUpdate::Release release;  // under mutex
    std::string error;            // under mutex
    PortUpdate::Progress progress;
    std::thread worker;
    std::atomic<bool> auto_install{false};  // install as soon as the check finds a newer release
    bool restart_sent = false;
};
UpdateState g_update;

bool UpdateBusy() {
    const int s = g_update.stage;
    return s == UpdateState::Checking || s == UpdateState::Downloading || s == UpdateState::Unpacking ||
           s == UpdateState::Installing || s == UpdateState::Done;
}

fs::path UpdateDir() { return g_app.root / L"update"; }

void UpdateFail(const std::string& error) {
    {
        std::scoped_lock lock{g_update.mutex};
        g_update.error = error;
    }
    g_update.stage = UpdateState::Failed;
}

void StartUpdateInstall();

/// Asks GitHub for the latest release; with auto_install a newer one is installed at once.
void StartUpdateCheck(bool auto_install) {
    if (UpdateBusy()) return;
    if (g_update.worker.joinable()) g_update.worker.join();
    g_update.stage = UpdateState::Checking;
    g_update.auto_install = auto_install;
    const std::string current = g_app.version;
    g_update.worker = std::thread([current, auto_install] {
        std::string error;
        const std::string url = std::string("https://api.github.com/repos/") + PortUpdate::kRepo + "/releases/latest";
        const auto body = PortUpdate::FetchText(Widen(url), &error);
        if (!body) {
            UpdateFail(std::string(T("No se pudo consultar GitHub: ", "Could not ask GitHub: ")) + error);
            return;
        }
        const auto release = PortUpdate::ParseRelease(*body);
        if (!release) {
            UpdateFail(T("GitHub no devolvió una versión para Windows.", "GitHub returned no Windows release."));
            return;
        }
        {
            std::scoped_lock lock{g_update.mutex};
            g_update.release = *release;
            g_update.error.clear();
        }
        const auto have = PortUpdate::ParseVersion(current);
        const auto latest = PortUpdate::ParseVersion(release->tag);
        if (!have.valid || !PortUpdate::IsNewer(have, latest)) {
            g_update.stage = UpdateState::UpToDate;
            return;
        }
        g_update.stage = UpdateState::Available;  // PollUpdate starts an automatic install
    });
}

void StartUpdateInstall() {
    App& a = g_app;
    if (g_update.stage != UpdateState::Available && g_update.stage != UpdateState::Dismissed &&
        g_update.stage != UpdateState::Failed)
        return;
    if (a.game_running) {
        a.notice = T("Cierra el juego para actualizar.", "Close the game to update.");
        return;
    }
    if (!a.python) {
        UpdateFail(T("Falta Python para descomprimir la actualización.", "Python is missing to unpack the update."));
        return;
    }
    PortUpdate::Release release;
    {
        std::scoped_lock lock{g_update.mutex};
        release = g_update.release;
        g_update.error.clear();
    }
    if (release.asset_url.empty()) return;
    if (g_update.worker.joinable()) g_update.worker.join();
    g_update.progress.cancel = false;
    g_update.stage = UpdateState::Downloading;
    const fs::path root = a.root;
    const Python python = *a.python;
    g_update.worker = std::thread([release, root, python] {
        const fs::path dir = root / L"update";
        const fs::path zip = dir / Widen(release.asset_name);
        const fs::path staging = dir / L"staging";
        std::error_code ec;
        fs::remove_all(staging, ec);
        std::string error = PortUpdate::Download(Widen(release.asset_url), zip, release.size, release.sha256,
                                                 g_update.progress);
        if (!error.empty()) {
            UpdateFail(std::string(T("La descarga falló: ", "The download failed: ")) + error);
            return;
        }
        g_update.stage = UpdateState::Unpacking;
        const std::wstring cmd = Quote(python.exe) + L" " + python.args + L"-I -X utf8 -c " +
                                 Quote(L"import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])") +
                                 L" " + Quote(zip.wstring()) + L" " + Quote(staging.wstring());
        const Captured r = RunCaptured(python.exe, cmd, dir, 600000);
        const fs::path package = staging / PortUpdate::kPackageFolder;
        const std::string unpacked_version = Trim(ReadText(package / L"bin" / L"VERSION"));
        if (!r.started || r.code != 0 || !fs::is_regular_file(package / L"bin" / L"sacredfire.exe", ec) ||
            !PortUpdate::ParseVersion(unpacked_version).valid) {
            fs::remove_all(staging, ec);
            UpdateFail(std::string(T("No se pudo descomprimir la actualización. ", "Could not unpack the update. ")) +
                       Trim(r.output).substr(0, 300));
            return;
        }
        if (g_app.game_running) {
            UpdateFail(T("Cierra el juego para actualizar.", "Close the game to update."));
            return;
        }
        g_update.stage = UpdateState::Installing;
        error = PortUpdate::Install(package, root, dir / L"rollback");
        fs::remove_all(staging, ec);
        if (!error.empty()) {
            UpdateFail(std::string(T("No se pudo instalar (todo quedó como estaba): ",
                                     "Could not install (everything was put back): ")) + error);
            return;
        }
        fs::remove(zip, ec);
        g_update.stage = UpdateState::Done;
    });
}

/// Every frame: an automatic update starts once the check found one; a finished install
/// restarts the launcher in its new version.
void PollUpdate() {
    App& a = g_app;
    if (g_update.auto_install && g_update.stage != UpdateState::Checking) {
        g_update.auto_install = false;
        if (g_update.stage == UpdateState::Available && !a.game_running) StartUpdateInstall();
    }
    if (g_update.stage == UpdateState::Done && !g_update.restart_sent) {
        g_update.restart_sent = true;
        SaveLauncher();
        if (!RestartLauncher(L"--updated"))
            a.notice = T("Actualizado. Cierra y vuelve a abrir el launcher.", "Updated. Close and open the launcher again.");
    }
}

/// At start: the previous update's leftovers (the old launcher in rollback\ may still be running
/// for a moment, so this retries in the background).
void CleanupUpdate() {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::thread([dir] {
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::error_code e;
            fs::remove_all(dir / L"rollback", e);
            fs::remove_all(dir / L"staging", e);
            if (!fs::exists(dir / L"rollback", e) && !fs::exists(dir / L"staging", e)) break;
            Sleep(500);
        }
        std::error_code e;
        fs::remove(dir, e);  // only when empty (a download in progress keeps it)
    }).detach();
}

/// The update line under the header: a newer version, the progress, or what went wrong.
void UpdateBanner() {
    const int stage = g_update.stage;
    if (stage == UpdateState::Idle || stage == UpdateState::Checking || stage == UpdateState::UpToDate ||
        stage == UpdateState::Dismissed)
        return;
    PortUpdate::Release release;
    std::string error;
    {
        std::scoped_lock lock{g_update.mutex};
        release = g_update.release;
        error = g_update.error;
    }
    if (stage == UpdateState::Available) {
        ImGui::TextColored(kGreen, "%s", Format(T("Hay una versión nueva: %s (tienes %s).", "A new version is out: %s (you have %s)."),
                                                release.tag, g_app.version).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(g_app.game_running);
        if (ImGui::SmallButton(T("Actualizar ahora", "Update now"))) StartUpdateInstall();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Novedades", "What's new"))) OpenInShell(Widen(release.page_url));
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Ahora no", "Not now"))) g_update.stage = UpdateState::Dismissed;
    } else if (stage == UpdateState::Downloading) {
        const double total = double(std::max<std::uint64_t>(1, g_update.progress.total));
        const float fraction = float(double(g_update.progress.done) / total);
        ImGui::TextColored(kGrey, "%s", Format(T("Descargando %s...", "Downloading %s..."), release.tag).c_str());
        ImGui::SameLine();
        char text[64];
        std::snprintf(text, sizeof text, "%.0f / %.0f MB", double(g_update.progress.done) / 1e6, total / 1e6);
        ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), text);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Cancelar", "Cancel"))) g_update.progress.cancel = true;
    } else if (stage == UpdateState::Unpacking || stage == UpdateState::Installing) {
        ImGui::TextColored(kGrey, "%s", stage == UpdateState::Unpacking ? T("Descomprimiendo la actualización...", "Unpacking the update...")
                                                                       : T("Instalando la actualización...", "Installing the update..."));
    } else if (stage == UpdateState::Done) {
        ImGui::TextColored(kGreen, "%s", T("Actualizado: reiniciando el launcher...", "Updated: restarting the launcher..."));
    } else if (stage == UpdateState::Failed) {
        ImGui::TextColored(kYellow, "%s", error.c_str());
        ImGui::SameLine();
        if (!release.asset_url.empty() && ImGui::SmallButton(T("Reintentar", "Retry"))) StartUpdateInstall();
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) g_update.stage = UpdateState::Idle;
    }
}

void UpdateSection() {
    App& a = g_app;
    ImGui::SeparatorText(T("Actualizaciones", "Updates"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s",
                       T("Descarga la última versión publicada en GitHub y la instala encima. Tus ajustes, amigos, apodo, "
                         "parches propios, partidas, copias, DLC y registros se mantienen: la actualización solo cambia "
                         "los archivos del port. Si algo falla, todo vuelve a como estaba.",
                         "Downloads the latest release published on GitHub and installs it over this one. Your settings, "
                         "friends, nickname, own patches, saves, backups, DLC and logs are kept: the update only changes "
                         "the port's files. If anything fails, everything is put back."));
    ImGui::PopTextWrapPos();
    ImGui::Text("%s %s", T("Versión instalada:", "Installed version:"), a.version.c_str());
    const int stage = g_update.stage;
    ImGui::BeginDisabled(UpdateBusy());
    if (ImGui::Button(T("Comprobar actualizaciones", "Check for updates"))) StartUpdateCheck(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (stage == UpdateState::Available || stage == UpdateState::Dismissed) {
        PortUpdate::Release release;
        {
            std::scoped_lock lock{g_update.mutex};
            release = g_update.release;
        }
        ImGui::TextColored(kGreen, "%s", Format(T("Versión nueva: %s", "New version: %s"), release.tag).c_str());
        ImGui::TextUnformatted(T("¿Quieres actualizar ahora?", "Do you want to update now?"));
        ImGui::BeginDisabled(a.game_running);
        if (ImGui::Button(T("Actualizar ahora", "Update now"))) StartUpdateInstall();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (stage == UpdateState::Available && ImGui::Button(T("Ahora no", "Not now")))
            g_update.stage = UpdateState::Dismissed;
        ImGui::SameLine();
        if (ImGui::Button(T("Novedades", "What's new"))) OpenInShell(Widen(release.page_url));
        if (a.game_running)
            ImGui::TextColored(kGrey, "%s", T("Cierra el juego para actualizar.", "Close the game to update."));
    } else if (stage == UpdateState::Checking) {
        ImGui::TextColored(kGrey, "%s", T("Consultando GitHub...", "Asking GitHub..."));
    } else if (stage == UpdateState::UpToDate) {
        std::string tag;
        {
            std::scoped_lock lock{g_update.mutex};
            tag = g_update.release.tag;
        }
        ImGui::TextColored(kGreen, "%s", Format(T("Estás al día (última versión: %s).", "You are up to date (latest: %s)."), tag).c_str());
    } else if (stage != UpdateState::Idle) {
        ImGui::TextColored(kGrey, "%s", T("Mira el aviso de arriba.", "See the notice at the top."));
    }
    LauncherFlag("update_check", T("Buscar actualizaciones al abrir el launcher", "Check for updates when the launcher opens"), true,
                 T("Una consulta pequeña a GitHub al abrir. No descarga nada sin tu permiso, salvo que actives la opción de abajo.",
                   "A small request to GitHub at start. Nothing is downloaded without your say, unless the option below is on."));
    LauncherFlag("update_auto", T("Instalar las actualizaciones automáticamente", "Install updates automatically"), false,
                 T("Al abrir el launcher, si hay una versión nueva se descarga e instala sola y el launcher se reinicia. "
                   "Nunca mientras juegas. Si abres el juego directo (acceso directo o Steam), no se actualiza.",
                   "When the launcher opens and a new version is out, it is downloaded and installed and the launcher "
                   "restarts. Never while you play. Starting straight into the game (shortcut, Steam) does not update."));
}
