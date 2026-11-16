// cdp.h - a small Chrome DevTools Protocol client.
//
// Dia on Windows is a full Chromium browser (ArcCore.dll), so Chromium's own
// DevTools are in it. Two ways to reach them:
//
//   * chrome://inspect lists the open pages and opens Chromium's DevTools
//     frontend for one of them. The frontend is a normal page, so it is
//     opened in Dia like any other tab.
//   * The CDP endpoint (127.0.0.1:9222) talks to those pages directly: it
//     reads the URL of the focused page, takes a screenshot of it, and opens
//     the DevTools frontend on a chosen panel.
//
// The endpoint only exists when Dia was started with --remote-debugging-port,
// so every call here is best effort: the caller gets "no connection" and the
// tweak simply does nothing rather than failing.
//
// The transport is plain HTTP for the endpoint's own REST calls (/json/list,
// /json/new) and a WebSocket for the CDP commands. The WebSocket part is
// written out here rather than pulled in as a dependency: only the few
// commands the tweak needs are implemented, and a Chrome DevTools handshake
// needs no authentication.

#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace Cdp {

// What went wrong the last time a call did not work.
const std::string& LastError();

// A page (or other target) the browser knows about.
struct Target {
    std::wstring id;
    std::wstring type;
    std::wstring title;
    std::wstring url;
    std::wstring websocketDebuggerUrl;
};

// The state of the browser's CDP endpoint.
struct Connection {
    bool connected = false;
    int port = 0;
    std::wstring webSocketDebuggerUrl;  // the browser-level endpoint
};

// Tries the given port (0 tries 9222). Returns a connection that is not
// connected when the endpoint is not there.
Connection Connect(int port);

// Asks the endpoint for its targets. Returns false when there is no endpoint.
bool ListTargets(int port, std::vector<Target>& targets);

// Opens a new target with the given URL (the DevTools frontend lives there).
// Returns the new target's id, or an empty string on failure.
std::wstring OpenTarget(int port, const std::wstring& url);

// Opens a new tab in the window of the browser, with the given URL. This is
// how Chromium's own developer tools are opened: the browser gives the tab a
// place in the window, rather than a window of its own.
bool CreateTab(int port, const std::wstring& url);

// Sends one CDP command to a target and returns the "result" member of the
// reply as raw JSON (empty when the call failed). sessionId may be empty.
std::string SendCommand(const std::wstring& webSocketDebuggerUrl,
                        const std::string& method,
                        const std::string& params,
                        const std::string& sessionId);

// Sends a command to a page.
//
// The page's own socket is used first. When the developer tools are open on
// that page they hold on to it and the page will not answer anyone else,
// so the browser's own connection is used instead, with a session that
// talks to the page.
std::string SendToPage(int port, const std::wstring& webSocketDebuggerUrl,
                       const std::wstring& targetId, const std::string& method,
                       const std::string& params);

// Reads the URL of the page the user is looking at. When preferredTitle is provided,
// it matches against the open tabs first to reliably find the active tab without lag.
bool FocusedPageUrl(int port, const std::wstring& preferredTitle, std::wstring& url, std::wstring& targetId);
bool FocusedPageUrl(int port, std::wstring& url, std::wstring& targetId);

// The size of the page in CSS pixels, using the browser's connection when
// the page's own socket is held by the developer tools.
bool PageSizeOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                    const std::wstring& targetId, int& width, int& height);

// The viewport (visible) size of the page in CSS pixels.
bool ViewportSizeOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                        const std::wstring& targetId, int& width, int& height);

// Takes a screenshot of one part of the page, given in CSS pixels, using
// the browser's connection when the page's own socket is held by the
// developer tools.
bool CaptureRegionOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                          const std::wstring& targetId, double x, double y,
                          double width, double height, double scale,
                          std::vector<unsigned char>& png);

// Takes a screenshot of just the visible viewport (no scroll content).
bool CaptureViewportOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                            const std::wstring& targetId,
                            std::vector<unsigned char>& png);

// Asks a page to run a piece of JavaScript, using the browser's connection
// when the page's own socket is held by the developer tools.
std::string EvaluateOnPage(int port, const std::wstring& webSocketDebuggerUrl,
                           const std::wstring& targetId,
                           const std::string& expression);

// Asks a page to run a piece of JavaScript and returns the result as a string.
std::string Evaluate(const std::wstring& webSocketDebuggerUrl, const std::string& expression);

// Escapes a string for use inside a JSON document.
std::string JsonEscape(const std::wstring& text);

// Reads a whole file into memory.
bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>& data);

}  // namespace Cdp


