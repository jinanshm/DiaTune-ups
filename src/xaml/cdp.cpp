// cdp.cpp - the Chrome DevTools Protocol client (see cdp.h).
//
// The HTTP part talks to the browser's own REST endpoint on 127.0.0.1. The
// WebSocket part is a small RFC 6455 client: it does the HTTP upgrade, then
// sends and receives text frames. Only what the tweak needs is here.

#include "cdp.h"

// Winsock has to come before windows.h: windows.h pulls in the older winsock
// declarations, which then clash with the real ones.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <winhttp.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace Cdp {
namespace {

// What went wrong the last time a call did not work, so it can be said.
std::string g_lastError;

}  // namespace

const std::string& LastError() {
    return g_lastError;
}
namespace {

// ---- tiny JSON helpers ----------------------------------------------------

// Finds "key" in a JSON object and returns the text of its value.
std::string JsonValue(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return std::string();
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return std::string();
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' ||
                                json[at] == '\n' || json[at] == '\r'))
        ++at;
    if (at >= json.size()) return std::string();

    char quote = json[at];
    if (quote == '"') {
        ++at;
        std::string out;
        while (at < json.size()) {
            char c = json[at++];
            if (c == '\\' && at < json.size()) {
                char e = json[at++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // A \uXXXX escape: keep it as the character it names.
                        if (at + 4 <= json.size()) {
                            unsigned code = 0;
                            sscanf(json.substr(at, 4).c_str(), "%x", &code);
                            at += 4;
                            if (code < 0x80) {
                                out += static_cast<char>(code);
                            } else if (code < 0x800) {
                                out += static_cast<char>(0xC0 | (code >> 6));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (code >> 12));
                                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += e; break;
                }
                continue;
            }
            if (c == '"') break;
            out += c;
        }
        return out;
    }

    size_t end = at;
    int depth = 0;
    bool inString = false;
    while (end < json.size()) {
        char c = json[end];
        if (c == '"' && (end == 0 || json[end - 1] != '\\')) inString = !inString;
        if (!inString) {
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) break;
        }
        ++end;
    }
    return json.substr(at, end - at);
}

// Undoes the JSON escapes of a string value (JsonValue already did that for
// the values it returns, so this is only used for keys we compare against).
std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                   nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size,
                         nullptr, nullptr);
    return out;
}

// ---- HTTP -----------------------------------------------------------------

// A WinHTTP session, used for the REST calls.
struct Http {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;

    ~Http() {
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }

    bool Open(int port) {
        session = WinHttpOpen(L"DiaTweaks", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        // A request to a port nobody is listening on must fail quickly
        // rather than wait out the default timeouts.
        WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);

        connect = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
        return connect != nullptr;
    }

    // Runs a request and returns the body. verb is "GET" or "PUT".
    bool Run(const wchar_t* verb, const std::wstring& path, std::string& body) {
        HINTERNET request = WinHttpOpenRequest(connect, verb, path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_BYPASS_PROXY_CACHE);
        if (!request) return false;
        bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
        if (ok) ok = WinHttpReceiveResponse(request, nullptr) != FALSE;
        if (ok) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
                body.append(chunk.data(), read);
            }
        }
        WinHttpCloseHandle(request);
        return ok;
    }
};

// ---- WebSocket ------------------------------------------------------------

// A minimal RFC 6455 client: text frames only, no extensions, no permessage
// deflate (Chrome's DevTools endpoint does not ask for it).
struct Socket {
    SOCKET fd = INVALID_SOCKET;
    std::string incoming;  // bytes read but not yet consumed

    ~Socket() {
        if (fd != INVALID_SOCKET) closesocket(fd);
    }

    bool ConnectTo(const std::string& host, int port, const std::string& path,
                    const std::string& origin) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) return false;

        // A connection that never completes must not hold the caller up.
        DWORD timeout = 250;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, host.c_str(), &address.sin_addr);

        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            return false;
        }

        // The handshake. Chrome checks the Origin header, so one is sent for
        // a page. The browser's own endpoint wants the handshake plain.
        char key[32];
        for (int i = 0; i < 16; ++i)
            key[i] = static_cast<char>("0123456789ABCDEF"[rand() % 16]);
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + std::string(key, 16) + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        if (!origin.empty()) request += "Origin: " + origin + "\r\n";
        request += "\r\n";

        if (SendAll(request) != 0) return false;

        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos) {
            char buffer[1024];
            int got = recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) return false;
            response.append(buffer, got);
            if (response.size() > 16384) return false;
        }
        return response.find("101") != std::string::npos;
    }

    int SendAll(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int wrote = send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (wrote <= 0) return -1;
            sent += static_cast<size_t>(wrote);
        }
        return 0;
    }

    // Sends one text frame. Client frames must be masked.
    bool SendText(const std::string& payload) {
        std::string frame;
        frame += static_cast<char>(0x81);  // FIN + text
        size_t length = payload.size();
        unsigned char mask[4];
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(rand() & 0xFF);

        if (length < 126) {
            frame += static_cast<char>(0x80 | length);
        } else if (length <= 0xFFFF) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>((length >> 8) & 0xFF);
            frame += static_cast<char>(length & 0xFF);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                frame += static_cast<char>((static_cast<uint64_t>(length) >> (i * 8)) & 0xFF);
        }
        frame.append(reinterpret_cast<char*>(mask), 4);

        std::string masked = payload;
        for (size_t i = 0; i < masked.size(); ++i)
            masked[i] = static_cast<char>(masked[i] ^ mask[i % 4]);
        frame += masked;

        return SendAll(frame) == 0;
    }

    // Reads one text frame's payload.
    bool ReceiveText(std::string& payload) {
        while (true) {
            std::string frame;
            if (!ReadFrame(frame)) return false;
            unsigned char opcode = static_cast<unsigned char>(frame[0]) & 0x0F;
            if (opcode == 0x8) return false;  // close
            if (opcode == 0x9) continue;     // ping: ignore
            if (opcode == 0xA) continue;     // pong: ignore
            if (opcode == 0x1 || opcode == 0x0) {
                payload = frame.substr(1);
                return true;
            }
        }
    }

    // Reads one frame into out: the first byte is the opcode, the rest the
    // payload.
    bool ReadFrame(std::string& out) {
        unsigned char header[2];
        if (!ReadExact(reinterpret_cast<char*>(header), 2)) return false;
        out.assign(1, static_cast<char>(header[0]));

        bool masked = (header[1] & 0x80) != 0;
        uint64_t length = header[1] & 0x7F;
        if (length == 126) {
            unsigned char extra[2];
            if (!ReadExact(reinterpret_cast<char*>(extra), 2)) return false;
            length = (static_cast<uint64_t>(extra[0]) << 8) | extra[1];
        } else if (length == 127) {
            unsigned char extra[8];
            if (!ReadExact(reinterpret_cast<char*>(extra), 8)) return false;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | extra[i];
        }

        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && !ReadExact(reinterpret_cast<char*>(mask), 4)) return false;

        if (length > 64 * 1024 * 1024) return false;
        std::string payload(static_cast<size_t>(length), '\0');
        if (length && !ReadExact(&payload[0], static_cast<size_t>(length))) return false;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        out += payload;
        return true;
    }

    bool ReadExact(char* into, size_t count) {
        size_t got = 0;
        while (got < count) {
            int n = recv(fd, into + got, static_cast<int>(count - got), 0);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }
};

// Splits "ws://host:port/path" into its parts.
bool SplitWsUrl(const std::wstring& url, std::string& host, int& port, std::string& path) {
    const std::wstring prefix = L"ws://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    std::wstring rest = url.substr(prefix.size());
    size_t slash = rest.find('/');
    std::wstring authority = slash == std::wstring::npos ? rest : rest.substr(0, slash);
    path = Narrow(slash == std::wstring::npos ? std::wstring(L"/") : rest.substr(slash));
    size_t colon = authority.find(':');
    if (colon == std::wstring::npos) {
        host = Narrow(authority);
        port = 80;
    } else {
        host = Narrow(authority.substr(0, colon));
        port = _wtoi(authority.c_str() + colon + 1);
    }
    return !host.empty();
}

std::string Base64(const unsigned char* data, size_t length) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < length; i += 3) {
        unsigned value = data[i] << 16;
        if (i + 1 < length) value |= data[i + 1] << 8;
        if (i + 2 < length) value |= data[i + 2];
        out += table[(value >> 18) & 0x3F];
        out += table[(value >> 12) & 0x3F];
        out += i + 1 < length ? table[(value >> 6) & 0x3F] : '=';
        out += i + 2 < length ? table[value & 0x3F] : '=';
    }
    return out;
}

bool DecodeBase64(const std::string& text, std::vector<unsigned char>& out) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int values[4];
    int count = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const char* at = strchr(table, c);
        if (!at) return false;
        values[count++] = static_cast<int>(at - table);
        if (count == 4) {
            out.push_back(static_cast<unsigned char>((values[0] << 2) | (values[1] >> 4)));
            out.push_back(static_cast<unsigned char>((values[1] << 4) | (values[2] >> 2)));
            out.push_back(static_cast<unsigned char>((values[2] << 6) | values[3]));
            count = 0;
        }
    }
    return true;
}

}  // namespace

std::string JsonEscape(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) {
        switch (c) {
            case L'"': out += "\\\""; break;
            case L'\\': out += "\\\\"; break;
            case L'\n': out += "\\n"; break;
            case L'\r': out += "\\r"; break;
            case L'\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    sprintf_s(buffer, "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += Narrow(std::wstring(1, c));
                }
        }
    }
    return out;
}

Connection Connect(int port) {
    Connection result;
    if (port <= 0) port = 9222;
    result.port = port;

    Http http;
    if (!http.Open(port)) return result;

    std::string body;
    if (!http.Run(L"GET", L"/json/version", body)) return result;
    std::string ws = JsonValue(body, "webSocketDebuggerUrl");
    if (ws.empty()) return result;

    result.connected = true;
    result.webSocketDebuggerUrl = Widen(ws);
    return result;
}

bool ListTargets(int port, std::vector<Target>& targets) {
    Http http;
    if (!http.Open(port)) return false;
    std::string body;
    if (!http.Run(L"GET", L"/json/list", body)) return false;

    size_t at = 0;
    while (true) {
        size_t start = body.find('{', at);
        if (start == std::string::npos) break;
        // Find the end of this object.
        int depth = 0;
        bool inString = false;
        size_t end = start;
        for (; end < body.size(); ++end) {
            char c = body[end];
            if (c == '"' && (end == 0 || body[end - 1] != '\\')) inString = !inString;
            if (inString) continue;
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) break;
            }
        }
        if (end >= body.size()) break;

        std::string object = body.substr(start, end - start + 1);
        Target target;
        target.id = Widen(JsonValue(object, "id"));
        target.type = Widen(JsonValue(object, "type"));
        target.title = Widen(JsonValue(object, "title"));
        target.url = Widen(JsonValue(object, "url"));
        target.websocketDebuggerUrl = Widen(JsonValue(object, "webSocketDebuggerUrl"));
        if (!target.id.empty()) targets.push_back(target);
        at = end + 1;
    }
    return true;
}

std::wstring OpenTarget(int port, const std::wstring& url) {
    Http http;
    if (!http.Open(port)) return std::wstring();

    // The path is the URL, percent-encoded.
    std::string encoded;
    for (wchar_t c : url) {
        if (iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            encoded += Narrow(std::wstring(1, c));
        } else {
            char buffer[8];
            sprintf_s(buffer, "%%%02X", static_cast<unsigned>(c) & 0xFF);
            encoded += buffer;
        }
    }
    std::wstring path = L"/json/new?" + Widen(encoded);
    std::string body;
    if (!http.Run(L"PUT", path, body)) return std::wstring();
    return Widen(JsonValue(body, "id"));
}

bool CreateTab(int port, const std::wstring& url) {
    // The browser's own endpoint makes the tab, so the browser gives it a
    // place in a window instead of a window of its own.
    //
    // The endpoint's own way of making a tab is used, rather than the
    // command over the browser's socket, because the browser answers this
    // one with a tab of its own.
    std::wstring id = OpenTarget(port, url);
    return !id.empty();
}

namespace {

// Sends one command on a socket that is already open, and returns the
// "result" member of the reply as raw JSON.
std::string SendOn(Socket& socket, const std::string& method, const std::string& params,
                   const std::string& sessionId) {
    std::string message = "{\"id\":1,\"method\":\"" + method + "\"";
    if (!params.empty()) message += ",\"params\":" + params;
    if (!sessionId.empty()) message += ",\"sessionId\":\"" + sessionId + "\"";
    message += "}";
    if (!socket.SendText(message)) return std::string();

    // Read until the reply for our id arrives.
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::string payload;
        if (!socket.ReceiveText(payload)) break;
        if (payload.find("\"id\":1") == std::string::npos) continue;
        std::string error = JsonValue(payload, "error");
        if (!error.empty()) {
            g_lastError = error;
            return std::string();
        }
        return JsonValue(payload, "result");
    }
    g_lastError = "no answer";
    return std::string();
}

}  // namespace

std::string SendCommand(const std::wstring& webSocketDebuggerUrl, const std::string& method,
                        const std::string& params, const std::string& sessionId) {
    std::string host;
    int port = 0;
    std::string path;
    if (!SplitWsUrl(webSocketDebuggerUrl, host, port, path)) return std::string();

    Socket socket;
    std::string origin = "http://" + host + ":" + std::to_string(port);
    if (!socket.ConnectTo(host, port, path, origin)) return std::string();

    return SendOn(socket, method, params, sessionId);
}

bool FocusedPageUrl(int port, const std::wstring& preferredTitle, std::wstring& url, std::wstring& targetId) {
    std::vector<Target> targets;
    if (!ListTargets(port, targets)) return false;

    auto isCandidatePage = [](const Target& target) {
        if (target.type != L"page") return false;
        if (target.url.find(L"devtools://") != std::wstring::npos) return false;
        return true;
    };

    // If preferredTitle is provided, try to match it against candidate pages
    if (!preferredTitle.empty()) {
        std::wstring prefLower = preferredTitle;
        for (auto& c : prefLower) c = static_cast<wchar_t>(::towlower(c));

        for (const Target& target : targets) {
            if (!isCandidatePage(target)) continue;

            std::wstring titleLower = target.title;
            for (auto& c : titleLower) c = static_cast<wchar_t>(::towlower(c));

            std::wstring urlLower = target.url;
            for (auto& c : urlLower) c = static_cast<wchar_t>(::towlower(c));

            if (!titleLower.empty() &&
                (titleLower.find(prefLower) != std::wstring::npos ||
                 prefLower.find(titleLower) != std::wstring::npos)) {
                url = target.url;
                targetId = target.id;
                return true;
            }

            if (!urlLower.empty() &&
                (urlLower.find(prefLower) != std::wstring::npos ||
                 prefLower.find(urlLower) != std::wstring::npos)) {
                url = target.url;
                targetId = target.id;
                return true;
            }

            size_t p = urlLower.find(L"://");
            if (p != std::wstring::npos) {
                std::wstring host = urlLower.substr(p + 3);
                size_t sl = host.find(L'/');
                if (sl != std::wstring::npos) host = host.substr(0, sl);
                if (!host.empty() && (prefLower.find(host) != std::wstring::npos || host.find(prefLower) != std::wstring::npos)) {
                    url = target.url;
                    targetId = target.id;
                    return true;
                }
            }
        }
    }

    std::vector<const Target*> candidates;
    for (const Target& target : targets) {
        if (isCandidatePage(target)) candidates.push_back(&target);
    }
    if (candidates.size() == 1) {
        url = candidates.front()->url;
        targetId = candidates.front()->id;
        return true;
    }

    std::vector<const Target*> onScreen;
    const Target* focused = nullptr;

    for (const Target* tPtr : candidates) {
        const Target& target = *tPtr;
        if (target.websocketDebuggerUrl.empty()) continue;

        std::string result = SendCommand(
            target.websocketDebuggerUrl, "Runtime.evaluate",
            "{\"expression\":\"document.visibilityState === 'visible'\"}", "");
        bool visible = result.find("\"value\":true") != std::string::npos;
        if (!visible) continue;

        onScreen.push_back(&target);

        result = SendCommand(target.websocketDebuggerUrl, "Runtime.evaluate",
                             "{\"expression\":\"document.hasFocus()\"}", "");
        if (result.find("\"value\":true") != std::string::npos && !focused) {
            focused = &target;
            break;
        }
    }

    if (focused) {
        url = focused->url;
        targetId = focused->id;
        return true;
    }

    if (!onScreen.empty()) {
        url = onScreen.front()->url;
        targetId = onScreen.front()->id;
        return true;
    }

    for (const Target& target : targets) {
        if (isCandidatePage(target)) {
            url = target.url;
            targetId = target.id;
            return true;
        }
    }

    return false;
}

bool FocusedPageUrl(int port, std::wstring& url, std::wstring& targetId) {
    return FocusedPageUrl(port, std::wstring(), url, targetId);
}

std::string SendToPage(int port, const std::wstring& webSocketDebuggerUrl,
                       const std::wstring& targetId, const std::string& method,
                       const std::string& params) {
    // The page's own socket answers first.
    std::string result = SendCommand(webSocketDebuggerUrl, method, params, "");
    if (!result.empty()) return result;
    if (targetId.empty()) return result;

    // The developer tools may be holding on to the page. The browser's own
    // connection can still talk to it, through a session.
    Connection connection = Connect(port);
    if (!connection.connected) {
        g_lastError = "no browser connection";
        return result;
    }

    std::string host;
    int browserPort = 0;
    std::string path;
    if (!SplitWsUrl(connection.webSocketDebuggerUrl, host, browserPort, path)) {
        g_lastError = "the browser's address is not right";
        return result;
    }

    // One connection is used for both the session and the command: a
    // session belongs to the connection that made it.
    Socket socket;
    if (!socket.ConnectTo(host, browserPort, path, "")) {
        g_lastError = "the browser would not talk";
        return result;
    }

    std::string attach = "{\"targetId\":\"" + JsonEscape(targetId) +
                         "\",\"flatten\":true}";
    std::string reply = SendOn(socket, "Target.attachToTarget", attach, "");
    std::string sessionId = JsonValue(reply, "sessionId");
    if (sessionId.empty()) {
        g_lastError = "no session for the page";
        return result;
    }

    return SendOn(socket, method, params, sessionId);
}

bool PageSizeOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                    const std::wstring& targetId, int& width, int& height) {
    std::string result = SendToPage(port, webSocketDebuggerUrl, targetId,
                                     "Page.getLayoutMetrics", "{}");
    std::string content = JsonValue(result, "contentSize");
    if (content.empty()) return false;
    std::string w = JsonValue(content, "width");
    std::string h = JsonValue(content, "height");
    if (w.empty() || h.empty()) return false;
    width = atoi(w.c_str());
    height = atoi(h.c_str());
    return width > 0;
}

bool ViewportSizeOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                        const std::wstring& targetId, int& width, int& height) {
    std::string result = SendToPage(port, webSocketDebuggerUrl, targetId,
                                     "Page.getLayoutMetrics", "{}");
    // Read cssVisualViewport for viewport dimensions instead of contentSize
    std::string viewport = JsonValue(result, "cssVisualViewport");
    if (viewport.empty()) {
        // Fallback to visualViewport
        viewport = JsonValue(result, "visualViewport");
    }
    if (viewport.empty()) return false;
    std::string w = JsonValue(viewport, "clientWidth");
    std::string h = JsonValue(viewport, "clientHeight");
    if (w.empty() || h.empty()) return false;
    width = atoi(w.c_str());
    height = atoi(h.c_str());
    return width > 0;
}

bool CaptureRegionOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                          const std::wstring& targetId, double x, double y,
                          double width, double height, double scale,
                          std::vector<unsigned char>& png) {
    // Only the part of the page that was chosen.
    char params[256];
    sprintf_s(params, sizeof(params),
              "{\"format\":\"png\",\"captureBeyondViewport\":true,"
              "\"clip\":{\"x\":%.0f,\"y\":%.0f,"
              "\"width\":%.0f,\"height\":%.0f,\"scale\":%.4f}}",
              x, y, width, height, scale);
    std::string result =
        SendToPage(port, webSocketDebuggerUrl, targetId, "Page.captureScreenshot",
                   params);
    std::string data = JsonValue(result, "data");
    if (data.empty()) return false;
    return DecodeBase64(data, png);
}

bool CaptureViewportOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                            const std::wstring& targetId,
                            std::vector<unsigned char>& png) {
    // Capture only the visible viewport — no scrolled content
    std::string result =
        SendToPage(port, webSocketDebuggerUrl, targetId, "Page.captureScreenshot",
                   "{\"format\":\"png\",\"captureBeyondViewport\":false}");
    std::string data = JsonValue(result, "data");
    if (data.empty()) return false;
    return DecodeBase64(data, png);
}

std::string EvaluateOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                           const std::wstring& targetId,
                           const std::string& expression) {
    std::string params = "{\"expression\":\"" + expression + "\",\"returnByValue\":true}";
    std::string result =
        SendToPage(port, webSocketDebuggerUrl, targetId, "Runtime.evaluate", params);
    std::string value = JsonValue(result, "value");
    if (!value.empty()) return value;
    std::string type = JsonValue(result, "type");
    if (type == "undefined" || type.empty()) return std::string();
    return result;
}

std::string Evaluate(const std::wstring& webSocketDebuggerUrl, const std::string& expression) {
    std::string params = "{\"expression\":\"" + expression + "\",\"returnByValue\":true}";
    std::string result = SendCommand(webSocketDebuggerUrl, "Runtime.evaluate", params, "");
    std::string value = JsonValue(result, "value");
    if (!value.empty()) return value;
    // A non-string result is returned as its JSON text.
    std::string type = JsonValue(result, "type");
    if (type == "undefined" || type.empty()) return std::string();
    return result;
}

bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>& data) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = data.empty() ||
              ::ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (ok) data.resize(read);
    return ok;
}

}  // namespace Cdp



