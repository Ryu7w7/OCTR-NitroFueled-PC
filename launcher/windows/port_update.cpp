// SPDX-License-Identifier: GPL-2.0-or-later
// Updating the portable package from GitHub releases (port_update.h).
#include "port_update.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <variant>

namespace fs = std::filesystem;

namespace PortUpdate {

// --- versions ------------------------------------------------------------------------------

Version ParseVersion(const std::string& text) {
    Version v;
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    int parts[3] = {0, 0, 0};
    int count = 0;
    while (count < 3 && i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int n = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            n = n * 10 + (text[i] - '0');
            if (n > 100000) return {};
            ++i;
        }
        parts[count++] = n;
        if (i < text.size() && text[i] == '.' && i + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            ++i;
        } else {
            break;
        }
    }
    if (count < 2) return {};
    std::string suffix = text.substr(i);
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.back()))) suffix.pop_back();
    for (bool dropped = true; dropped;) {  // in any order: -dirty-windows, -windows
        dropped = false;
        for (const char* drop : {"-dirty", "-windows", "-linux"}) {
            const size_t n = std::strlen(drop);
            if (suffix.size() >= n && suffix.compare(suffix.size() - n, n, drop) == 0) {
                suffix.resize(suffix.size() - n);
                dropped = true;
            }
        }
    }
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.valid = true;
    if (suffix.empty()) {
        v.stage = 1;
    } else {
        // git describe: -<commits>-g<hash>
        size_t j = 0;
        bool describe = suffix[j] == '-';
        ++j;
        const size_t digits = j;
        while (j < suffix.size() && std::isdigit(static_cast<unsigned char>(suffix[j]))) ++j;
        describe = describe && j > digits && j + 1 < suffix.size() && suffix[j] == '-' && suffix[j + 1] == 'g';
        if (describe) {
            j += 2;
            const size_t hex = j;
            while (j < suffix.size() && std::isxdigit(static_cast<unsigned char>(suffix[j]))) ++j;
            describe = j > hex && j == suffix.size();
        }
        v.stage = describe ? 2 : 0;
    }
    return v;
}

bool IsNewer(const Version& a, const Version& b) {
    if (!a.valid || !b.valid) return false;
    if (b.major != a.major) return b.major > a.major;
    if (b.minor != a.minor) return b.minor > a.minor;
    if (b.patch != a.patch) return b.patch > a.patch;
    return b.stage > a.stage;
}

// --- a small JSON reader (the release answer) ----------------------------------------------

namespace {

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;
struct Json {
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>>
        value;
    const Json* Get(const std::string& key) const {
        const auto* object = std::get_if<std::shared_ptr<JsonObject>>(&value);
        if (!object || !*object) return nullptr;
        const auto it = (*object)->find(key);
        return it == (*object)->end() ? nullptr : &it->second;
    }
    std::string Text(const std::string& key) const {
        const Json* j = Get(key);
        const auto* s = j ? std::get_if<std::string>(&j->value) : nullptr;
        return s ? *s : std::string();
    }
    bool Flag(const std::string& key) const {
        const Json* j = Get(key);
        const auto* b = j ? std::get_if<bool>(&j->value) : nullptr;
        return b && *b;
    }
    double Number(const std::string& key) const {
        const Json* j = Get(key);
        const auto* d = j ? std::get_if<double>(&j->value) : nullptr;
        return d ? *d : 0.0;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s(text) {}
    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        Space();
        return i == s.size();
    }

private:
    const std::string& s;
    size_t i = 0;
    void Space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Literal(const char* word) {
        const size_t n = std::strlen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(unsigned& cp) {
        if (i + 4 > s.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned low = 0;
                    if (!Hex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 64) return false;
        Space();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') {
            ++i;
            auto object = std::make_shared<JsonObject>();
            Space();
            if (i < s.size() && s[i] == '}') {
                ++i;
                out.value = object;
                return true;
            }
            while (true) {
                Space();
                std::string key;
                if (!String(key)) return false;
                Space();
                if (i >= s.size() || s[i++] != ':') return false;
                Json item;
                if (!Value(item, depth + 1)) return false;
                (*object)[key] = std::move(item);
                Space();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    out.value = object;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            auto array = std::make_shared<JsonArray>();
            Space();
            if (i < s.size() && s[i] == ']') {
                ++i;
                out.value = array;
                return true;
            }
            while (true) {
                Json item;
                if (!Value(item, depth + 1)) return false;
                array->push_back(std::move(item));
                Space();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    out.value = array;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string text;
            if (!String(text)) return false;
            out.value = std::move(text);
            return true;
        }
        if (Literal("true")) {
            out.value = true;
            return true;
        }
        if (Literal("false")) {
            out.value = false;
            return true;
        }
        if (Literal("null")) {
            out.value = std::monostate{};
            return true;
        }
        const size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (i == start) return false;
        out.value = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }
};

bool EndsWith(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

std::optional<Release> ParseRelease(const std::string& json) {
    Json root;
    if (!JsonParser(json).Parse(root)) return std::nullopt;
    if (root.Flag("draft") || root.Flag("prerelease")) return std::nullopt;
    Release r;
    r.tag = root.Text("tag_name");
    r.page_url = root.Text("html_url");
    if (!ParseVersion(r.tag).valid) return std::nullopt;
    const Json* assets = root.Get("assets");
    const auto* list = assets ? std::get_if<std::shared_ptr<JsonArray>>(&assets->value) : nullptr;
    if (!list || !*list) return std::nullopt;
    for (const Json& asset : **list) {
        const std::string name = asset.Text("name");
        if (!EndsWith(name, "-windows.zip")) continue;
        r.asset_name = name;
        r.asset_url = asset.Text("browser_download_url");
        r.size = static_cast<std::uint64_t>(asset.Number("size"));
        std::string digest = asset.Text("digest");
        if (digest.rfind("sha256:", 0) == 0) {
            digest = digest.substr(7);
            std::transform(digest.begin(), digest.end(), digest.begin(),
                           [](unsigned char ch) { return char(std::tolower(ch)); });
            if (digest.size() == 64 && std::all_of(digest.begin(), digest.end(),
                                                   [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
                r.sha256 = digest;
        }
        if (r.asset_url.rfind("https://", 0) != 0 || r.size == 0) return std::nullopt;
        return r;
    }
    return std::nullopt;
}

// --- which files an update writes ------------------------------------------------------------

namespace {
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

bool IsProtected(const fs::path& relative) {
    const fs::path normal = relative.lexically_normal();
    if (normal.empty() || normal.is_absolute() || normal.has_root_name()) return true;
    std::vector<std::string> parts;
    for (const fs::path& part : normal) parts.push_back(Lower(reinterpret_cast<const char*>(part.u8string().c_str())));
    if (parts.empty() || parts[0].empty() || parts[0] == "..") return true;
    const std::string& first = parts[0];
    if (parts.size() > 1) {
        // out/aerolib.inl is the package's (the symbol names it ships); the rest of out/ is generated.
        if (first == "out") return !(parts.size() == 2 && parts[1] == "aerolib.inl");
        // The player's data, the update's own work folder and game dumps kept next to the port.
        static const char* const kFolders[] = {"user", "backups", "dlc", "logs", "update"};
        for (const char* folder : kFolders)
            if (first == folder) return true;
        return first.rfind("cusa", 0) == 0;
    }
    // Files at the top: settings, online identity, patch choices, logs, Discord hand-offs.
    static const char* const kFiles[] = {"bbport.ini", "launcher.ini", "ryunet_online.key", "patches.json",
                                         "ryunet_join.txt", "ryunet_announce.txt", "selftest.txt"};
    for (const char* file : kFiles)
        if (first == file) return true;
    return first.rfind("last_run", 0) == 0 && EndsWith(first, ".log");
}

std::vector<fs::path> PackageFiles(const fs::path& package) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(package, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const fs::path relative = it->path().lexically_relative(package);
        if (!IsProtected(relative)) files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string Install(const fs::path& package, const fs::path& port, const fs::path& rollback) {
    const std::vector<fs::path> files = PackageFiles(package);
    if (files.empty()) return "the package has no files";
    std::error_code ec;
    fs::remove_all(rollback, ec);
    fs::create_directories(rollback, ec);
    struct Step {
        fs::path target, saved;  // saved: where the old file went ("" when there was none)
    };
    std::vector<Step> done;
    std::string error;
    for (const fs::path& relative : files) {
        const fs::path source = package / relative;
        const fs::path target = port / relative;
        Step step{target, {}};
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            step.saved = rollback / relative;
            fs::create_directories(step.saved.parent_path(), ec);
            // A rename works on the running launcher too; copying over it would not.
            if (!MoveFileExW(target.c_str(), step.saved.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (error %lu)", GetLastError());
                error = "could not move aside " + std::string(reinterpret_cast<const char*>(relative.u8string().c_str())) + buf;
                break;
            }
        }
        done.push_back(step);
        if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " (error %lu)", GetLastError());
            error = "could not write " + std::string(reinterpret_cast<const char*>(relative.u8string().c_str())) + buf;
            break;
        }
    }
    if (error.empty()) return {};
    // Put everything back as it was.
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        DeleteFileW(it->target.c_str());
        if (!it->saved.empty()) MoveFileExW(it->saved.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return error;
}

// --- network ---------------------------------------------------------------------------------

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(Handle&& other) noexcept : h(other.h) { other.h = nullptr; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string LastError(const char* what) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s (error %lu)", what, GetLastError());
    return buf;
}

/// Opens a GET request for an https URL and receives its headers; the status code in *status.
bool OpenGet(const std::wstring& url, Handle& session, std::unique_ptr<Handle>& connection,
             std::unique_ptr<Handle>& request, DWORD* status, std::string* error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256]{}, path[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    wchar_t extra[4096]{};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *error = "not an https address";
        return false;
    }
    connection = std::make_unique<Handle>(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connection->h) {
        *error = LastError("could not connect");
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    request = std::make_unique<Handle>(WinHttpOpenRequest(connection->h, L"GET", object.c_str(), nullptr,
                                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                          WINHTTP_FLAG_SECURE));
    if (!request->h) {
        *error = LastError("could not open the request");
        return false;
    }
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(request->h, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request->h, nullptr)) {
        *error = LastError("no answer");
        return false;
    }
    DWORD size = sizeof *status;
    if (!WinHttpQueryHeaders(request->h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX)) {
        *error = LastError("no status");
        return false;
    }
    return true;
}

Handle OpenSession(int timeout_ms) {
    Handle session(WinHttpOpen(L"CTR-NitroFueled-PC-launcher-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.h) {
        WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
    }
    return session;
}

}  // namespace

std::optional<std::string> FetchText(const std::wstring& url, std::string* error) {
    Handle session = OpenSession(15000);
    if (!session.h) {
        *error = LastError("WinHTTP unavailable");
        return std::nullopt;
    }
    std::unique_ptr<Handle> connection, request;
    DWORD status = 0;
    if (!OpenGet(url, session, connection, request, &status, error)) return std::nullopt;
    std::string body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request->h, &avail) && avail) {
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request->h, chunk.data(), avail, &got) || !got) break;
        body.append(chunk.data(), got);
        if (body.size() > (8u << 20)) break;  // a release answer is a few KB
    }
    if (status != 200) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "HTTP %lu", status);
        *error = buf;
        return std::nullopt;
    }
    return body;
}

std::string Download(const std::wstring& url, const fs::path& file, std::uint64_t size, const std::string& sha256,
                     Progress& progress) {
    progress.done = 0;
    progress.total = size;
    Handle session = OpenSession(30000);
    if (!session.h) return LastError("WinHTTP unavailable");
    std::unique_ptr<Handle> connection, request;
    DWORD status = 0;
    std::string error;
    if (!OpenGet(url, session, connection, request, &status, &error)) return error;
    if (status != 200) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "HTTP %lu", status);
        return buf;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return "SHA-256 unavailable";
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::string result;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) result = "could not create the download file";
        std::vector<char> buffer(1 << 16);
        while (result.empty()) {
            if (progress.cancel) {
                result = "cancelled";
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(request->h, buffer.data(), DWORD(buffer.size()), &got)) {
                result = LastError("the download stopped");
                break;
            }
            if (got == 0) break;
            out.write(buffer.data(), got);
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), got, 0);
            progress.done += got;
            if (progress.done > size + (1u << 20)) {
                result = "the download is larger than the release says";
                break;
            }
        }
        out.close();
        if (result.empty() && !out) result = "could not write the download (disk full?)";
    }
    unsigned char digest[32]{};
    BCryptFinishHash(hash, digest, sizeof digest, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (result.empty() && progress.done != size) result = "the download is incomplete";
    if (result.empty() && !sha256.empty()) {
        static const char kHex[] = "0123456789abcdef";
        std::string hex;
        for (const unsigned char b : digest) {
            hex += kHex[b >> 4];
            hex += kHex[b & 15];
        }
        if (hex != sha256) result = "the download's SHA-256 does not match the release";
    }
    if (!result.empty()) fs::remove(file, ec);
    return result;
}

}  // namespace PortUpdate
