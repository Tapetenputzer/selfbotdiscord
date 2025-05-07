#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <random>
#include <thread>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <ctime>

// Liefert den Ordner, in dem die EXE liegt
std::filesystem::path GetExeFolder() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
}

// Absoluter Pfad zu Text.txt neben der EXE
const std::filesystem::path TEXT_PATH = GetExeFolder() / "Text.txt";

// Führt einen HTTP-Request aus und liefert den HTTP-Statuscode zurück
DWORD HttpRequest(
    HINTERNET session,
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& verb,
    const std::wstring& headers,
    const std::string& body
) {
    HINTERNET connect = WinHttpConnect(session, host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) return 0;
    HINTERNET request = WinHttpOpenRequest(
        connect, verb.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!request) {
        WinHttpCloseHandle(connect);
        return 0;
    }
    WinHttpSendRequest(request,
        headers.c_str(), (ULONG)-1,
        (LPVOID)body.data(), (ULONG)body.size(),
        (ULONG)body.size(), 0);
    WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &len, nullptr);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    return status;
}

// Thread­sicherer Logger mit Zeitstempel HH:MM:SS
std::mutex cout_mtx;
void log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &tt);
    std::lock_guard<std::mutex> lock(cout_mtx);
    std::cout << '[' << std::put_time(&tm, "%H:%M:%S") << "] " << msg << "\n";
}

int wmain() {
    // --- 1) Sicherstellen, dass Text.txt existiert ---
    if (!std::filesystem::exists(TEXT_PATH)) {
        // Datei anlegen mit Beispieltext
        std::ofstream out(TEXT_PATH);
        out << "Type your messages here.\n";

        // Meldung ausgeben
        std::cout << "No Text.txt found – created a new one at:\n"
            << "  " << TEXT_PATH.string() << "\n\n"
            << "Please fill it with your messages and press ENTER to exit."
            << std::endl;
        // Warten, bis der Benutzer die Konsole schließt
        std::cin.get();
        return 1;
    }

    // Textdatei in Vektor laden
    std::ifstream fin(TEXT_PATH);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(fin, line))
        if (!line.empty())
            lines.push_back(line);
    fin.close();
    if (lines.empty()) {
        std::cerr << TEXT_PATH.filename().string()
            << " is empty. Please add at least one line.\n";
        return 1;
    }

    // --- 2) Token einlesen ---
    std::cout << "Enter your Discord user token: ";
    std::string token;
    std::getline(std::cin, token);
    if (token.empty()) return 1;

    // --- 3) WinHTTP-Session öffnen & Token validieren ---
    HINTERNET session = WinHttpOpen(
        L"SelfBot/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!session) return 1;

    std::wstring authHdr = L"Authorization: " +
        std::wstring(token.begin(), token.end()) + L"\r\n";
    if (HttpRequest(session, L"discord.com", L"/api/v10/users/@me",
        L"GET", authHdr, "") != 200) {
        std::cerr << "Invalid token, exiting.\n";
        return 1;
    }
    std::cout << "Token validated. Starting...\n\n";

    // --- 4) Channel-ID und Intervalle abfragen ---
    std::cout << "Channel ID: ";
    std::string channel_id;
    std::getline(std::cin, channel_id);
    std::cout << "Min interval (s): ";
    int minI, maxI;
    std::cin >> minI;
    std::cout << "Max interval (s): ";
    std::cin >> maxI;
    if (minI >= maxI) return 1;

    // --- 5) Zufalls- oder Sequenziell-Modus abfragen ---
    std::cout << "Random order? (y/n): ";
    char mode = 'n';
    std::cin >> mode;
    bool random_mode = (mode == 'y' || mode == 'Y');
    std::cout << "\n";

    // --- 6) Nachrichten-Loop im Hintergrundthread ---
    std::thread([&]() {
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(minI, maxI);
        size_t index = 0;
        std::string previous;
        while (true) {
            // Nachricht auswählen
            std::string msg;
            if (random_mode) {
                std::vector<std::string> opts;
                for (auto& m : lines)
                    if (m != previous)
                        opts.push_back(m);
                if (opts.empty()) opts = lines;
                msg = opts[rng() % opts.size()];
            }
            else {
                msg = lines[index++];
                if (index >= lines.size()) index = 0;
            }
            previous = msg;

            // JSON-Body bauen
            std::string body = "{\"content\":\"";
            for (char c : msg)
                body += (c == '"') ? "\\\"" : std::string(1, c);
            body += "\"}";

            // Absenden
            std::wstring path = L"/api/v10/channels/" +
                std::wstring(channel_id.begin(), channel_id.end()) +
                L"/messages";
            long code = HttpRequest(
                session, L"discord.com", path, L"POST",
                authHdr + L"Content-Type: application/json\r\n",
                body
            );
            if (code == 200 || code == 201)
                log("Sent: \"" + msg + '"');
            else
                log("Error sending (HTTP " + std::to_string(code) + ")");

            // Countdown
            for (int i = dist(rng); i > 0; --i) {
                std::lock_guard<std::mutex> lock(cout_mtx);
                std::cout << "\rNext in " << i << "s...   " << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "\r                     \r";
        }
        }).detach();

    // Hauptthread schläft, damit das Programm weiterläuft
    std::this_thread::sleep_for(std::chrono::hours(24 * 365));
    return 0;
}
