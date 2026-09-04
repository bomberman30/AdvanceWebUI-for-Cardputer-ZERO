/*
 * Cardputer Zero - WiFi AP + Web File Manager
 *
 * Target: CardputerZero Template, CM0/aarch64 Linux, LVGL 9.5
 * C++17
 */

#include <lvgl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#include "linux_input.h"
#endif

namespace fs = std::filesystem;

// ============================================================
// CONFIGURATION
// ============================================================

static constexpr const char *AP_SSID = "CardputerZero";
static constexpr const char *AP_PASSWORD = "12345678";

static constexpr const char *WEB_USERNAME = "admin";
static constexpr const char *WEB_PASSWORD = "cardputer";

static constexpr int WEB_PORT = 8080;
static constexpr size_t MAX_UPLOAD_SIZE = 128ULL * 1024ULL * 1024ULL; // 128 MB
struct UploadedFile
{
    std::string filename;
    std::string data;
};
static uint32_t g_choiceScreenReadyAt = 0;
static constexpr uint32_t CHOICE_SCREEN_COOLDOWN_MS = 500;
static std::string shellQuote(const std::string &s);
static std::atomic<bool> g_choiceReady{false};
static std::atomic<bool> g_useExistingWifi{false};
static std::atomic<bool> g_apMode{false}; // האם בפועל יצרנו AP (לצורך teardown ביציאה)
static std::string getInterfaceIP(const std::string &iface);
static void on_choice_use_wifi(lv_event_t *e)
{
    if (lv_tick_get() - g_choiceScreenReadyAt < CHOICE_SCREEN_COOLDOWN_MS)
        return; // מתעלמים מלחיצה שהגיעה מוקדם מדי - כנראה שארית מהסיסמה
    g_useExistingWifi = true;
    g_choiceReady = true;
}

static void on_choice_use_ap(lv_event_t *e)
{
    if (lv_tick_get() - g_choiceScreenReadyAt < CHOICE_SCREEN_COOLDOWN_MS)
        return;
    g_useExistingWifi = false;
    g_choiceReady = true;
}
static std::string g_sudoPassword;
static std::atomic<bool> g_passwordReady{false};
static std::atomic<bool> g_running{true};
static class WebServer *g_webServerPtr = nullptr;
static std::string g_wifiInterface;
static void on_password_ready(lv_event_t *e)
{
    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    g_sudoPassword = lv_textarea_get_text(ta);
    g_passwordReady = true;
}

// Helper to run sudo commands with the provided password
static int runSudo(const std::string &pass, const std::string &cmd, const std::string &redirect = ">/dev/null 2>&1")
{
    std::string fullCmd = "echo " + shellQuote(pass) + " | sudo -S " + cmd + " " + redirect;
    return std::system(fullCmd.c_str());
}

// Helper to run a sudo command and capture its combined stdout/stderr output,
// along with its exit code. Used for actions whose result we want to show
// the user (e.g. installing a .deb package).
static std::string runSudoCapture(const std::string &pass, const std::string &cmd, int &exitCode)
{
    std::string fullCmd = "echo " + shellQuote(pass) + " | sudo -S " + cmd + " 2>&1";
    FILE *pipe = ::popen(fullCmd.c_str(), "r");
    if (!pipe)
    {
        exitCode = -1;
        return "Failed to launch command";
    }
    char buffer[512];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe))
        result += buffer;
    int status = ::pclose(pipe);
    exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return result;
}

// ============================================================
// HTTP HELPERS
// ============================================================

struct HttpRequest
{
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
};

static std::string toLower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool sendAll(int fd, const void *data, size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0)
    {
        ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

static bool sendText(int fd, const std::string &text,
                     const std::string &contentType = "text/html; charset=utf-8",
                     int status = 200,
                     const std::string &extra = "")
{
    std::ostringstream h;
    h << "HTTP/1.1 " << status << " "
      << (status == 200 ? "OK" : status == 201 ? "Created"
                             : status == 302   ? "Found"
                             : status == 400   ? "Bad Request"
                             : status == 401   ? "Unauthorized"
                             : status == 403   ? "Forbidden"
                             : status == 404   ? "Not Found"
                             : status == 413   ? "Payload Too Large"
                                               : "Internal Server Error")
      << "\r\n";
    h << "Content-Type: " << contentType << "\r\n";
    h << "Content-Length: " << text.size() << "\r\n";
    h << "Connection: close\r\n";
    if (!extra.empty())
        h << extra;
    h << "\r\n";
    if (!sendAll(fd, h.str().data(), h.str().size()))
        return false;
    return sendAll(fd, text.data(), text.size());
}

static std::string urlDecode(const std::string &input)
{
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '%')
        {
            if (i + 2 < input.size())
            {
                char *end = nullptr;
                const std::string hex = input.substr(i + 1, 2);
                long v = std::strtol(hex.c_str(), &end, 16);
                if (end && *end == '\0')
                {
                    out.push_back(static_cast<char>(v));
                    i += 2;
                    continue;
                }
            }
        }
        if (input[i] == '+')
            out.push_back(' ');
        else
            out.push_back(input[i]);
    }
    return out;
}

static std::string urlEncode(const std::string &input)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string htmlEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

static std::map<std::string, std::string> parseForm(const std::string &body)
{
    std::map<std::string, std::string> form;
    size_t pos = 0;
    while (pos <= body.size())
    {
        size_t amp = body.find('&', pos);
        std::string part = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos)
        {
            form[urlDecode(part.substr(0, eq))] = urlDecode(part.substr(eq + 1));
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return form;
}

static std::map<std::string, std::string> parseQuery(const std::string &query)
{
    return parseForm(query);
}

// ============================================================
// SAFE FILE PATHS
// ============================================================

// rootPath() is the default landing folder shown right after login, not a
// jail: safePath() now anchors browsing at the real filesystem root ("/"),
// so the user can navigate up out of CardputerFiles and reach any directory
// the process has permission to read.
static fs::path rootPath()
{
    const char *home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path("/tmp");
    return fs::absolute(base / "CardputerFiles").lexically_normal();
}

// Resolves a user-supplied path (absolute, or relative to the default
// landing folder) to a normalized absolute filesystem path. lexically_normal()
// collapses any ".." components, and since the result is anchored at "/" it
// can never resolve to anything outside the real filesystem root.
static bool safePath(const std::string &userPath, fs::path &output)
{
    std::string decoded = urlDecode(userPath);
    fs::path candidate = (!decoded.empty() && decoded.front() == '/')
                              ? fs::path(decoded)
                              : (rootPath() / decoded);
    candidate = candidate.lexically_normal();
    if (!candidate.is_absolute())
        return false;
    output = candidate;
    return true;
}

// Absolute, URL-friendly representation of a filesystem path, used for the
// "dir"/"path" query and form parameters (replaces the old root-relative
// scheme now that browsing isn't confined to CardputerFiles).
static std::string pathParam(const fs::path &p)
{
    std::string s = p.generic_string();
    return s.empty() ? "/" : s;
}

static std::string formatSize(uintmax_t n)
{
    std::ostringstream ss;
    if (n >= 1024ULL * 1024ULL)
        ss << (n / (1024.0 * 1024.0)) << " MB";
    else if (n >= 1024ULL)
        ss << (n / 1024.0) << " KB";
    else
        ss << n << " B";
    return ss.str();
}

// Returns true if the given path looks like a Debian package (.deb),
// based on its file extension.
static bool isDebFile(const fs::path &p)
{
    return toLower(p.extension().string()) == ".deb";
}

// ============================================================
// AP CONTROL
// ============================================================

static std::string shellQuote(const std::string &s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out += "'";
    return out;
}

static std::string commandOutput(const std::string &command)
{
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe)
        return {};
    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe))
        result += buffer;
    ::pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}
struct WifiStatus
{
    bool connected = false;
    std::string iface;
    std::string ssid;
    std::string ip;
};

// מוצא ממשק WiFi שכבר מחובר בפועל לרשת (לא ה-AP שלנו)
static WifiStatus getCurrentWifiStatus()
{
    WifiStatus status;
    std::string out = commandOutput(
        "nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line))
    {
        std::vector<std::string> f;
        std::string cur;
        for (char c : line)
        {
            if (c == ':') { f.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        f.push_back(cur);
        if (f.size() < 4)
            continue;

        const std::string &device = f[0];
        const std::string &type = f[1];
        const std::string &state = f[2];
        const std::string &conn = f[3];

        if (type == "wifi" && state == "connected" &&
            conn != "CardputerZeroAP" && !device.empty())
        {
            status.connected = true;
            status.iface = device;
            status.ssid = conn;
            status.ip = getInterfaceIP(device);
            return status;
        }
    }
    return status;
}
static std::string findWifiInterface()
{
    std::string out = commandOutput("nmcli -t -f DEVICE,TYPE device status 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line))
    {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string device = line.substr(0, colon);
        const std::string type = line.substr(colon + 1);
        if (type == "wifi" && !device.empty())
            return device;
    }
    return {};
}

static std::string getInterfaceIP(const std::string &iface)
{
    std::string cmd = "nmcli -g IP4.ADDRESS device show " + shellQuote(iface) + " 2>/dev/null";
    std::string out = commandOutput(cmd);
    size_t newline = out.find('\n');
    if (newline != std::string::npos)
        out.resize(newline);
    size_t slash = out.find('/');
    if (slash != std::string::npos)
        out.resize(slash);
    return out.empty() ? "10.42.0.1" : out;
}
static bool startAccessPoint(std::string &iface, std::string &ip, const std::string &sudoPass)
{
    runSudo(sudoPass, "nmcli radio wifi on");

    iface = findWifiInterface();
    if (iface.empty())
    {
        std::cerr << "No WiFi interface found.\n";
        return false;
    }

    std::cerr << "Using WiFi interface: " << iface << "\n";

    runSudo(sudoPass, "nmcli device set " + shellQuote(iface) + " managed yes");
    runSudo(sudoPass, "nmcli device disconnect " + shellQuote(iface));
    runSudo(sudoPass, "nmcli connection delete " + shellQuote("CardputerZeroAP"));

    const std::string addCmd =
        "nmcli connection add type wifi ifname " + shellQuote(iface) +
        " con-name " + shellQuote("CardputerZeroAP") +
        " autoconnect no ssid " + shellQuote(AP_SSID);

    if (runSudo(sudoPass, addCmd, "2>/tmp/cardputerzero-ap.log") != 0)
    {
        std::cerr << "Failed to create WiFi AP profile. See /tmp/cardputerzero-ap.log\n";
        return false;
    }

    const char *commands[] = {
        "nmcli connection modify CardputerZeroAP 802-11-wireless.mode ap",
        "nmcli connection modify CardputerZeroAP 802-11-wireless.band bg",
        "nmcli connection modify CardputerZeroAP 802-11-wireless.channel 6",
        "nmcli connection modify CardputerZeroAP wifi-sec.key-mgmt wpa-psk",
        "nmcli connection modify CardputerZeroAP wifi-sec.psk '12345678'",
        "nmcli connection modify CardputerZeroAP ipv4.method shared",
        "nmcli connection modify CardputerZeroAP ipv6.method disabled",
        "nmcli connection modify CardputerZeroAP connection.autoconnect no"};

    for (const char *cmd : commands)
    {
        if (runSudo(sudoPass, cmd, ">>/tmp/cardputerzero-ap.log 2>&1") != 0)
        {
            std::cerr << "Failed to configure AP. See /tmp/cardputerzero-ap.log\n";
            return false;
        }
    }

    if (runSudo(sudoPass, "nmcli connection up CardputerZeroAP", ">>/tmp/cardputerzero-ap.log 2>&1") != 0)
    {
        std::cerr << "Failed to activate AP. See /tmp/cardputerzero-ap.log\n";
        return false;
    }

    for (int i = 0; i < 20; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ip = getInterfaceIP(iface);
        if (ip != "10.42.0.1" || commandOutput(
                                     "nmcli -g IP4.ADDRESS device show " + shellQuote(iface) +
                                     " 2>/dev/null")
                                         .find("10.42.0.1") != std::string::npos)
        {
            break;
        }
    }

    ip = getInterfaceIP(iface);

    const std::string mode = commandOutput(
        "nmcli -g 802-11-wireless.mode connection show CardputerZeroAP 2>/dev/null");
    if (mode != "ap")
    {
        std::cerr << "AP profile exists but is not in AP mode.\n";
        return false;
    }

    return true;
}

// ============================================================
// LOGIN
// ============================================================

static bool authorized(const HttpRequest &req)
{
    auto it = req.headers.find("cookie");
    if (it == req.headers.end())
        return false;
    return it->second.find("CZSESSION=1") != std::string::npos;
}

static std::string loginPage(const std::string &error = {})
{
    std::ostringstream h;
    h << R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cardputer Zero - Login</title><style>
body{margin:0;background:#0f1115;color:#eee;font:16px Arial;display:flex;justify-content:center;align-items:center;min-height:100vh}
.box{width:min(360px,90vw);background:#181c23;padding:28px;border-radius:16px;box-shadow:0 10px 40px #0008}
h1{font-size:24px}input,button{width:100%;box-sizing:border-box;padding:12px;margin:7px 0;border-radius:8px;border:1px solid #38404d;background:#0e1218;color:#fff}
button{background:#2e7dff;border:0;font-weight:bold;cursor:pointer}.err{color:#ff7070}
</style></head><body><div class="box"><h1>Cardputer Zero</h1><p>Web File Manager</p>)HTML";
    if (!error.empty())
        h << "<p class='err'>" << htmlEscape(error) << "</p>";
    h << R"HTML(<form method="post" action="/login"><input name="username" placeholder="Username" autocomplete="username">
<input name="password" type="password" placeholder="Password" autocomplete="current-password"><button>Login</button></form></div></body></html>)HTML";
    return h.str();
}

// Simple result page shown after attempting to install a .deb package,
// with the captured command output and a link back to the folder it lived in.
static std::string installResultPage(const std::string &packageName, bool success,
                                     const std::string &output, const std::string &backDir)
{
    std::ostringstream h;
    h << R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cardputer Zero - Install</title><style>
body{margin:0;background:#0f1115;color:#eee;font:15px Arial}.wrap{max-width:950px;margin:auto;padding:18px}
.card{background:#181c23;border:1px solid #272d38;border-radius:14px;padding:16px;margin:12px 0}
h1{margin:0;font-size:22px}pre{white-space:pre-wrap;word-break:break-word;background:#10141a;padding:12px;border-radius:8px;max-height:60vh;overflow:auto}
a.back{display:inline-block;margin-top:10px;padding:10px 14px;border-radius:8px;background:#2e7dff;color:white;text-decoration:none;font-weight:bold}
.ok{color:#4dff9a}.fail{color:#ff6b6b}
</style></head><body><div class="wrap"><div class="card">)HTML";
    h << "<h1 class='" << (success ? "ok" : "fail") << "'>"
      << (success ? "Installed: " : "Install failed: ") << htmlEscape(packageName) << "</h1>";
    h << "<pre>" << htmlEscape(output) << "</pre>";
    h << "<a class='back' href='/?dir=" << urlEncode(backDir) << "'>Back to files</a>";
    h << "</div></div></body></html>";
    return h.str();
}

// ============================================================
// WEB UI
// ============================================================

static std::string fileManagerPage(const std::string &currentDir)
{
    fs::path current;
    if (!safePath(currentDir, current) || !fs::is_directory(current))
        current = rootPath();

    std::ostringstream h;
    h << R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cardputer Zero Files</title><style>
body{margin:0;background:#0f1115;color:#eee;font:15px Arial}.wrap{max-width:950px;margin:auto;padding:18px}
.header{display:flex;justify-content:space-between;gap:10px;align-items:center;flex-wrap:wrap}.card{background:#181c23;border:1px solid #272d38;border-radius:14px;padding:16px;margin:12px 0}
h1{margin:0;font-size:24px}.muted{color:#9da5b2}.path{font-family:monospace;background:#10141a;padding:8px;border-radius:8px;overflow:auto}
table{width:100%;border-collapse:collapse}td{padding:10px 6px;border-bottom:1px solid #282e39}a{color:#71a9ff;text-decoration:none}button{padding:8px 11px;border:0;border-radius:8px;background:#2e7dff;color:white;cursor:pointer;margin:2px}button.red{background:#c43b4b}button.gray{background:#39414d}button.green{background:#2ea043}.actions{white-space:nowrap}
input{padding:9px;border-radius:8px;border:1px solid #39414b;background:#0e1218;color:white}input[type=file]{width:100%;box-sizing:border-box}
@media(max-width:650px){td:nth-child(2){display:none}.actions button{display:block;width:100%}}
#installOverlay{display:none;position:fixed;inset:0;background:#0f1115ee;z-index:999;
  flex-direction:column;justify-content:center;align-items:center;color:#eee;font:16px Arial;text-align:center;padding:20px}
#installOverlay .spinner{width:40px;height:40px;border:4px solid #272d38;border-top-color:#2ea043;
  border-radius:50%;animation:spin 0.8s linear infinite;margin-bottom:16px}
@keyframes spin{to{transform:rotate(360deg)}}
</style></head><body>
<div id="installOverlay">
  <div class="spinner"></div>
  <div>Installing package...</div>
  <div class="muted">This can take a few minutes. Please don't close this page.</div>
</div>
<div class="wrap">
<div class="header">
    <h1>
        Advance WebUI By Bomberman30 
        <a href="https://github.com/bomberman30/AdvanceWebUI-for-Cardputer-ZERO" target="_blank">Github</a>
    </h1>

    <form method="post" action="/logout">
        <button class="gray">Logout</button>
    </form>
</div>
<div class="card"><div class="muted">Current path</div><div class="path">)HTML";
    h << htmlEscape(pathParam(current)) << R"HTML(</div></div>
<div class="card"><h3>Upload</h3><form onsubmit="return false;">
<input type="file" id="fileInput" name="file" multiple style="display:none">
<input type="file" id="folderInput" name="file" multiple webkitdirectory directory style="display:none">
<button type="button" onclick="document.getElementById('fileInput').click()">Select files</button>
<button type="button" onclick="document.getElementById('folderInput').click()">Select folder</button>
<button type="button" onclick="doUpload()">Upload</button>
<div id="uploadStatus" class="muted"></div>
</form></div>
<script>
document.getElementById('fileInput').addEventListener('change', function(){ document.getElementById('folderInput').value=''; updateStatus(); });
document.getElementById('folderInput').addEventListener('change', function(){ document.getElementById('fileInput').value=''; updateStatus(); });
function updateStatus(){
  var f = document.getElementById('fileInput').files.length ? document.getElementById('fileInput').files : document.getElementById('folderInput').files;
  document.getElementById('uploadStatus').textContent = f.length ? (f.length + ' file(s) selected') : '';
}
function doUpload(){
  var fileInput = document.getElementById('fileInput');
  var folderInput = document.getElementById('folderInput');
  var files = folderInput.files.length ? folderInput.files : fileInput.files;
  if (!files.length) { alert('Select one or more files, or a folder'); return; }
  var fd = new FormData();
  var names = [];
  for (var i = 0; i < files.length; i++) {
    var f = files[i];
    fd.append('file', f, f.webkitRelativePath || f.name);
    names.push(f.webkitRelativePath || f.name);
  }
  var status = document.getElementById('uploadStatus');
  status.textContent = names.length === 1
    ? ('Uploading ' + names[0] + '...')
    : ('Uploading ' + names.length + ' files: ' + names.join(', '));
  fetch(')HTML"
      << "/upload?dir=" << urlEncode(pathParam(current)) << R"HTML(', { method: 'POST', body: fd })
    .then(function(r){ return r.text().then(function(t){ return {ok:r.ok, text:t}; }); })
    .then(function(res){
      if (res.ok) {
        status.textContent = names.length === 1 ? ('Uploaded ' + names[0]) : ('Uploaded ' + names.length + ' files');
        setTimeout(function(){ window.location.reload(); }, 600);
      } else {
        status.textContent = 'Error: ' + res.text;
      }
    })
    .catch(function(e){ status.textContent = 'Error: ' + e; });
}
function submitInstall(form, name) {
  if (!confirm('Install ' + name + ' on this device?')) return false;
  document.getElementById('installOverlay').style.display = 'flex';
  return true;
}
</script>
<div class="card"><h3>New folder</h3><form method="post")HTML";
    h << " action='/mkdir?dir=" << urlEncode(pathParam(current)) << "'>";
    h << R"HTML(<input name="name" placeholder="Folder name" required><button>Create</button></form></div>
<div class="card"><table><tr><td><b>Name</b></td><td><b>Size</b></td><td><b>Actions</b></td></tr>)HTML";

    if (current != fs::path("/"))
    {
        fs::path parent = current.parent_path();
        h << "<tr><td colspan='3'><a href='/?dir=" << urlEncode(pathParam(parent)) << "'>📁 ..</a></td></tr>";
    }

    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto &entry : fs::directory_iterator(current, ec))
        entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
              {
        if (a.is_directory() != b.is_directory()) return a.is_directory() > b.is_directory();
        return a.path().filename().string() < b.path().filename().string(); });

    for (const auto &entry : entries)
    {
        const std::string name = entry.path().filename().string();
        const std::string rel = pathParam(entry.path());
        h << "<tr><td>";
        if (entry.is_directory())
        {
            h << "📁 <a href='/?dir=" << urlEncode(rel) << "'>" << htmlEscape(name) << "</a>";
        }
        else
        {
            h << "📄 " << htmlEscape(name);
        }
        h << "</td><td>";
        if (entry.is_regular_file())
        {
            std::error_code sec;
            h << htmlEscape(formatSize(entry.file_size(sec)));
        }
        else
        {
            h << "-";
        }
        h << "</td><td class='actions'>";
        if (entry.is_regular_file())
        {
            h << "<a href='/download?path=" << urlEncode(rel) << "'><button>Download</button></a>";
        }
        if (entry.is_regular_file() && isDebFile(entry.path()))
        {
            h << "<form style='display:inline' method='post' action='/install' onsubmit='return submitInstall(this, "
              << "\"" << htmlEscape(name) << "\")'>"
              << "<input type='hidden' name='path' value='" << htmlEscape(rel) << "'>"
              << "<button class='green'>Install</button></form>";
        }
        h << "<form style='display:inline' method='post' action='/rename'>"
          << "<input type='hidden' name='path' value='" << htmlEscape(rel) << "'>"
          << "<input name='newname' placeholder='New name'>"
          << "<button class='gray'>Rename</button></form>";
        h << "<form style='display:inline' method='post' action='/delete' onsubmit='return confirm(\"Delete this item?\")'>"
          << "<input type='hidden' name='path' value='" << htmlEscape(rel) << "'>"
          << "<button class='red'>Delete</button></form>";
        h << "</td></tr>";
    }

    h << R"HTML(</table></div></div></body></html>)HTML";
    return h.str();
}
// ============================================================
// HTTP SERVER
// ============================================================

class WebServer
{
public:
    WebServer() = default;
    ~WebServer() { stop(); }

    bool start()
    {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0)
            return false;

        int yes = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(WEB_PORT);

        if (::bind(serverFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            ::close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        if (::listen(serverFd_, 8) < 0)
        {
            ::close(serverFd_);
            serverFd_ = -1;
            return false;
        }

        running_ = true;
        thread_ = std::thread([this]
                              { loop(); });
        return true;
    }

    void stop()
    {
        running_ = false;
        if (serverFd_ >= 0)
        {
            ::shutdown(serverFd_, SHUT_RDWR);
            ::close(serverFd_);
            serverFd_ = -1;
        }
        if (thread_.joinable())
            thread_.join();
    }

private:
    bool readRequest(int fd, HttpRequest &req)
    {
        constexpr size_t MAX_HEADER = 64 * 1024;
        std::vector<char> buffer;
        buffer.reserve(8192);
        size_t headerEnd = std::string::npos;

        char temp[8192];
        while (buffer.size() < MAX_HEADER)
        {
            ssize_t n = ::recv(fd, temp, sizeof(temp), 0);
            if (n <= 0)
                return false;
            buffer.insert(buffer.end(), temp, temp + n);
            auto it = std::search(buffer.begin(), buffer.end(), "\r\n\r\n", "\r\n\r\n" + 4);
            if (it != buffer.end())
            {
                headerEnd = static_cast<size_t>(it - buffer.begin()) + 4;
                break;
            }
        }
        if (headerEnd == std::string::npos)
            return false;

        const std::string header(buffer.begin(), buffer.begin() + headerEnd);
        std::istringstream hs(header);
        std::string requestLine;
        if (!std::getline(hs, requestLine))
            return false;
        if (!requestLine.empty() && requestLine.back() == '\r')
            requestLine.pop_back();

        std::istringstream rl(requestLine);
        rl >> req.method >> req.target;
        if (req.method.empty() || req.target.empty())
            return false;

        const size_t q = req.target.find('?');
        req.path = q == std::string::npos ? req.target : req.target.substr(0, q);
        req.query = q == std::string::npos ? "" : req.target.substr(q + 1);

        std::string line;
        while (std::getline(hs, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = toLower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.erase(value.begin());
            req.headers[key] = value;
        }

        size_t contentLength = 0;
        if (auto it = req.headers.find("content-length"); it != req.headers.end())
        {
            try
            {
                contentLength = std::stoull(it->second);
            }
            catch (...)
            {
                return false;
            }
        }
        if (contentLength > MAX_UPLOAD_SIZE + 8 * 1024 * 1024)
            return false;

        req.body.reserve(contentLength);
        if (buffer.size() > headerEnd)
        {
            size_t already = buffer.size() - headerEnd;
            req.body.insert(req.body.end(), buffer.begin() + headerEnd, buffer.end());
            if (already > contentLength)
                req.body.resize(contentLength);
        }
        while (req.body.size() < contentLength)
        {
            size_t remaining = contentLength - req.body.size();
            size_t want = std::min<size_t>(remaining, sizeof(temp));
            ssize_t n = ::recv(fd, temp, want, 0);
            if (n <= 0)
                return false;
            req.body.insert(req.body.end(), temp, temp + n);
        }
        return true;
    }
    static std::vector<UploadedFile> multipartFields(const std::string &body, const std::string &boundary,
                                                     const std::string &fieldName)
    {
        std::vector<UploadedFile> result;
        const std::string marker = "--" + boundary;
        size_t pos = 0;
        while (true)
        {
            size_t start = body.find(marker, pos);
            if (start == std::string::npos)
                break;
            size_t headersStart = start + marker.size();
            if (headersStart + 2 > body.size())
                break;
            if (body.compare(headersStart, 2, "--") == 0)
                break;
            if (body.compare(headersStart, 2, "\r\n") == 0)
                headersStart += 2;

            size_t headersEnd = body.find("\r\n\r\n", headersStart);
            if (headersEnd == std::string::npos)
                break;
            std::string partHeaders = body.substr(headersStart, headersEnd - headersStart);

            size_t dataStart = headersEnd + 4;
            size_t nextBoundary = body.find("\r\n" + marker, dataStart);
            if (nextBoundary == std::string::npos)
                break;

            if (partHeaders.find("name=\"" + fieldName + "\"") != std::string::npos)
            {
                std::string filename;
                size_t fp = partHeaders.find("filename=\"");
                if (fp != std::string::npos)
                {
                    fp += 10;
                    size_t fe = partHeaders.find('"', fp);
                    if (fe != std::string::npos)
                        filename = partHeaders.substr(fp, fe - fp);
                }
                if (!filename.empty())
                    result.push_back({filename, body.substr(dataStart, nextBoundary - dataStart)});
            }

            pos = nextBoundary + 2;
        }
        return result;
    }

    static bool hasPathTraversal(const std::string &p)
    {
        std::istringstream ss(p);
        std::string seg;
        while (std::getline(ss, seg, '/'))
            if (seg == "..")
                return true;
        return false;
    }

    bool uploadFile(const HttpRequest &req, std::string &error)
{
    std::lock_guard<std::mutex> lock(fsMutex_);
    if (req.body.size() > MAX_UPLOAD_SIZE)
    {
        error = "Upload is too large";
        return false;
    }

    auto ct = req.headers.find("content-type");
    if (ct == req.headers.end())
    {
        error = "Missing multipart content type";
        return false;
    }

    const std::string prefix = "multipart/form-data; boundary=";
    size_t bp = toLower(ct->second).find(prefix);
    if (bp == std::string::npos)
    {
        error = "Expected multipart/form-data";
        return false;
    }
    std::string boundary = ct->second.substr(bp + prefix.size());
    if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"')
        boundary = boundary.substr(1, boundary.size() - 2);

    const std::string body(req.body.begin(), req.body.end());
    auto files = multipartFields(body, boundary, "file");
    if (files.empty())
    {
        error = "No file selected";
        return false;
    }

    auto q = parseQuery(req.query);
    const std::string dir = q.count("dir") ? q["dir"] : pathParam(rootPath());
    fs::path directory;
    if (!safePath(dir, directory) || !fs::is_directory(directory))
    {
        error = "Invalid directory";
        return false;
    }

    for (auto &f : files)
    {
        // Windows clients may send backslashes for folder uploads.
        std::string relName = f.filename;
        std::replace(relName.begin(), relName.end(), '\\', '/');
        while (!relName.empty() && relName.front() == '/')
            relName.erase(relName.begin());

        if (relName.empty() || relName == "." || hasPathTraversal(relName))
        {
            error = "Invalid file name: " + relName;
            return false;
        }

        fs::path destination = (directory / fs::path(relName)).lexically_normal();
        if (!safePath(pathParam(destination), destination))
        {
            error = "Invalid destination: " + relName;
            return false;
        }

        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);

        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "Cannot open destination: " + relName;
            return false;
        }
        out.write(f.data.data(), static_cast<std::streamsize>(f.data.size()));
        if (!out.good())
        {
            error = "Write failed: " + relName;
            return false;
        }
    }
    return true;
}

    void redirect(int fd, const std::string &path)
    {
        const std::string response =
            "HTTP/1.1 302 Found\r\nLocation: " + path +
            "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, response.data(), response.size());
    }

    void handleDownload(int fd, const HttpRequest &req)
    {
        auto q = parseQuery(req.query);
        if (!q.count("path"))
        {
            sendText(fd, "Missing path", "text/plain", 400);
            return;
        }
        fs::path file;
        if (!safePath(q["path"], file) || !fs::is_regular_file(file))
        {
            sendText(fd, "Not found", "text/plain", 404);
            return;
        }

        std::error_code ec;
        const uintmax_t size = fs::file_size(file, ec);
        if (ec)
        {
            sendText(fd, "Cannot read file", "text/plain", 500);
            return;
        }

        const std::string name = file.filename().string();
        std::ostringstream h;
        h << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
          << "Content-Length: " << size << "\r\n"
          << "Content-Disposition: attachment; filename=\"" << htmlEscape(name) << "\"\r\n"
          << "Connection: close\r\n\r\n";
        const std::string hs = h.str();
        if (!sendAll(fd, hs.data(), hs.size()))
            return;

        std::ifstream in(file, std::ios::binary);
        std::vector<char> buffer(64 * 1024);
        while (in)
        {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize n = in.gcount();
            if (n > 0 && !sendAll(fd, buffer.data(), static_cast<size_t>(n)))
                break;
        }
    }

    // Installs a .deb package on the device using apt-get (which resolves
    // dependencies for a local package file, unlike plain dpkg -i).
    void handleInstall(int fd, const HttpRequest &req)
    {
        std::lock_guard<std::mutex> lock(fsMutex_);
        const std::string body(req.body.begin(), req.body.end());
        auto form = parseForm(body);

        fs::path file;
        if (!form.count("path") || !safePath(form["path"], file) ||
            !fs::is_regular_file(file) || !isDebFile(file))
        {
            sendText(fd, "Invalid package path", "text/plain", 400);
            return;
        }

        const std::string backDir = pathParam(file.parent_path());
        const std::string packageName = file.filename().string();

        // apt-get accepts a local .deb path directly and will pull in any
        // missing dependencies from configured repositories.
        const std::string cmd = "apt-get install -y " + shellQuote(file.string());
        int exitCode = 0;
        const std::string output = runSudoCapture(g_sudoPassword, cmd, exitCode);

        sendText(fd, installResultPage(packageName, exitCode == 0, output, backDir),
                 "text/html; charset=utf-8", exitCode == 0 ? 200 : 500);
    }

    void handleRequest(int fd, const HttpRequest &req)
    {
        if (req.path == "/login" && req.method == "GET")
        {
            sendText(fd, loginPage());
            return;
        }
        if (req.path == "/login" && req.method == "POST")
        {
            const std::string body(req.body.begin(), req.body.end());
            auto form = parseForm(body);
            if (form["username"] == WEB_USERNAME && form["password"] == WEB_PASSWORD)
            {
                const std::string r =
                    "HTTP/1.1 302 Found\r\nLocation: /?dir=" + urlEncode(pathParam(rootPath())) + "\r\n"
                    "Set-Cookie: CZSESSION=1; Path=/; HttpOnly; SameSite=Strict\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                sendAll(fd, r.data(), r.size());
            }
            else
            {
                sendText(fd, loginPage("Wrong username or password"), "text/html; charset=utf-8", 401);
            }
            return;
        }

        if (req.path == "/logout" && req.method == "POST")
        {
            const std::string r =
                "HTTP/1.1 302 Found\r\nLocation: /login\r\n"
                "Set-Cookie: CZSESSION=; Max-Age=0; Path=/\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(fd, r.data(), r.size());
            return;
        }

        if (!authorized(req))
        {
            const std::string r =
                "HTTP/1.1 302 Found\r\nLocation: /login\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(fd, r.data(), r.size());
            return;
        }

        if (req.path == "/" && req.method == "GET")
        {
            auto q = parseQuery(req.query);
            const std::string dir = q.count("dir") ? q["dir"] : pathParam(rootPath());
            sendText(fd, fileManagerPage(dir));
            return;
        }

        if (req.path == "/download" && req.method == "GET")
        {
            handleDownload(fd, req);
            return;
        }

        if (req.path == "/upload" && req.method == "POST")
        {
            std::string error;
            if (uploadFile(req, error))
            {
                auto q = parseQuery(req.query);
                const std::string dir = q.count("dir") ? q["dir"] : pathParam(rootPath());
                redirect(fd, "/?dir=" + urlEncode(dir));
            }
            else
            {
                sendText(fd, error, "text/plain; charset=utf-8", error == "Upload is too large" ? 413 : 400);
            }
            return;
        }

        if (req.path == "/install" && req.method == "POST")
        {
            handleInstall(fd, req);
            return;
        }

        if (req.path == "/delete" && req.method == "POST")
        {
            const std::string body(req.body.begin(), req.body.end());
            auto form = parseForm(body);
            fs::path p;
            if (!form.count("path") || !safePath(form["path"], p) || p == fs::path("/"))
            {
                sendText(fd, "Invalid path", "text/plain", 400);
                return;
            }
            std::lock_guard<std::mutex> lock(fsMutex_);
            std::error_code ec;
            fs::remove_all(p, ec);
            if (ec)
            {
                sendText(fd, "Delete failed", "text/plain", 500);
                return;
            }
            redirect(fd, "/?dir=" + urlEncode(pathParam(p.parent_path())));
            return;
        }

        if (req.path == "/rename" && req.method == "POST")
        {
            const std::string body(req.body.begin(), req.body.end());
            auto form = parseForm(body);
            fs::path oldPath;
            if (!form.count("path") || !form.count("newname") ||
                !safePath(form["path"], oldPath) || oldPath == fs::path("/"))
            {
                sendText(fd, "Invalid input", "text/plain", 400);
                return;
            }
            const std::string newName = fs::path(form["newname"]).filename().string();
            if (newName.empty() || newName == "." || newName == "..")
            {
                sendText(fd, "Invalid new name", "text/plain", 400);
                return;
            }
            fs::path newPath = oldPath.parent_path() / newName;
            if (!safePath(pathParam(newPath), newPath))
            {
                sendText(fd, "Invalid destination", "text/plain", 400);
                return;
            }
            std::lock_guard<std::mutex> lock(fsMutex_);
            std::error_code ec;
            fs::rename(oldPath, newPath, ec);
            if (ec)
            {
                sendText(fd, "Rename failed", "text/plain", 500);
                return;
            }
            redirect(fd, "/?dir=" + urlEncode(pathParam(newPath.parent_path())));
            return;
        }

        if (req.path == "/mkdir" && req.method == "POST")
        {
            auto q = parseQuery(req.query);
            const std::string dir = q.count("dir") ? q["dir"] : pathParam(rootPath());
            const std::string body(req.body.begin(), req.body.end());
            auto form = parseForm(body);
            if (!form.count("name"))
            {
                sendText(fd, "Missing name", "text/plain", 400);
                return;
            }
            fs::path parent;
            if (!safePath(dir, parent) || !fs::is_directory(parent))
            {
                sendText(fd, "Invalid directory", "text/plain", 400);
                return;
            }
            const std::string name = fs::path(form["name"]).filename().string();
            if (name.empty() || name == "." || name == "..")
            {
                sendText(fd, "Invalid folder name", "text/plain", 400);
                return;
            }
            fs::path folder = parent / name;
            if (!safePath(pathParam(folder), folder))
            {
                sendText(fd, "Invalid destination", "text/plain", 400);
                return;
            }
            std::lock_guard<std::mutex> lock(fsMutex_);
            std::error_code ec;
            fs::create_directory(folder, ec);
            if (ec)
            {
                sendText(fd, "Create folder failed", "text/plain", 500);
                return;
            }
            redirect(fd, "/?dir=" + urlEncode(pathParam(parent)));
            return;
        }

        sendText(fd, "Not found", "text/plain", 404);
    }

    // Handles a single already-accepted connection. Runs on its own thread so
    // that one slow/idle client (e.g. a browser's speculative "preconnect"
    // socket that never sends a request) can't stall every other request.
    void serveConnection(int clientFd)
    {
        HttpRequest req;
        if (readRequest(clientFd, req))
        {
            handleRequest(clientFd, req);
        }
        else
        {
            sendText(clientFd, "Bad Request", "text/plain", 400);
        }
        ::shutdown(clientFd, SHUT_RDWR);
        ::close(clientFd);
    }

    void loop()
    {
        while (running_)
        {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int clientFd = ::accept(serverFd_, reinterpret_cast<sockaddr *>(&client), &len);
            if (clientFd < 0)
            {
                if (!running_)
                    break;
                if (errno == EINTR)
                    continue;
                continue;
            }
            struct timeval tv{15, 0};
            setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // One thread per connection: an idle/slow connection only blocks
            // itself (up to the 15s recv timeout), not the whole server.
            std::thread([this, clientFd]()
                        { serveConnection(clientFd); })
                .detach();
        }
    }

    int serverFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex fsMutex_; // serializes filesystem-mutating operations across connection threads
};

// ============================================================
// LVGL UI
// ============================================================

static void addLabel(lv_obj_t *parent, const char *text, int x, int y,
                     lv_color_t color = lv_color_hex(0xFFFFFF))
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
}
static void teardownAccessPoint(const std::string &pass, const std::string &iface)
{
    if (iface.empty())
        return;
    runSudo(pass, "nmcli connection down CardputerZeroAP");
    runSudo(pass, "nmcli connection delete CardputerZeroAP");
}

static void shutdownApp()
{
    static std::atomic<bool> shuttingDown{false};
    bool expected = false;
    if (!shuttingDown.compare_exchange_strong(expected, true))
        return;

    std::cerr << "Shutting down...\n";

    if (g_webServerPtr)
        g_webServerPtr->stop();

    if (g_apMode) // רק אם באמת יצרנו AP
        teardownAccessPoint(g_sudoPassword, g_wifiInterface);

    g_running = false;
}

static void on_global_key(lv_event_t *e)
{
    const uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC)
    {
        shutdownApp();
    }
}
static lv_obj_t *createChoiceScreen(const WifiStatus &wifi)
{
    lv_obj_t *screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1117), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Network mode");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4DFF9A), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, -10); // 

    char info[160];
    if (wifi.connected)
        std::snprintf(info, sizeof(info), "Connected to: %s\nIP: %s",
                      wifi.ssid.c_str(), wifi.ip.c_str());
    else
        std::snprintf(info, sizeof(info), "Not connected to any WiFi");

    lv_obj_t *infoLabel = lv_label_create(screen);
    lv_label_set_text(infoLabel, info);
    lv_obj_set_style_text_color(infoLabel, lv_color_hex(0x8B949E), 0);
    lv_obj_align(infoLabel, LV_ALIGN_TOP_MID, 0, 10); 

    // הסבר על שימוש ב-TAB, מותנה בזמינות
    lv_obj_t *hintLabel = lv_label_create(screen);
    if (wifi.connected)
        lv_label_set_text(hintLabel, "Press TAB to switch, ENTER to select");
    else
        lv_label_set_text(hintLabel, "WiFi unavailable Press OK For AP mode");
    lv_obj_set_style_text_color(hintLabel, lv_color_hex(0x8B949E), 0);
    lv_obj_align(hintLabel, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *btnWifi = lv_btn_create(screen);
    lv_obj_set_size(btnWifi, LV_PCT(85), 40);
    lv_obj_align(btnWifi, LV_ALIGN_CENTER, 0, 15);
    lv_obj_t *btnWifiLabel = lv_label_create(btnWifi);
    lv_label_set_text(btnWifiLabel, "Use current WiFi");
    lv_obj_center(btnWifiLabel);

    if (wifi.connected)
    {
        lv_obj_add_event_cb(btnWifi, on_choice_use_wifi, LV_EVENT_CLICKED, nullptr);
    }
    else
    {
        // אין חיבור קיים - משביתים את הכפתור
        lv_obj_add_state(btnWifi, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btnWifi, lv_color_hex(0x21262D), 0);
        lv_label_set_text(btnWifiLabel, "Use current WiFi (not connected)");
    }

    lv_obj_t *btnAp = lv_btn_create(screen);
    lv_obj_set_size(btnAp, LV_PCT(85), 40);
    lv_obj_align(btnAp, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_color(btnAp, lv_color_hex(0x39414D), 0);
    lv_obj_add_event_cb(btnAp, on_choice_use_ap, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btnApLabel = lv_label_create(btnAp);
    lv_label_set_text(btnApLabel, "Start Access Point");
    lv_obj_center(btnApLabel);

    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    if (wifi.connected)
        lv_group_add_obj(group, btnWifi);
    lv_group_add_obj(group, btnAp);
    lv_group_focus_obj(wifi.connected ? btnWifi : btnAp);

    lv_indev_t *indev = lv_indev_get_next(nullptr);
    while (indev != nullptr)
    {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
            lv_indev_set_group(indev, group);
        indev = lv_indev_get_next(indev);
    }

    return screen;
}
static void createUI(const std::string &ip, const std::string &iface,
                      bool netOk, bool webOk, bool apMode, const std::string &ssid)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1117), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Cardputer Zero");
    lv_obj_set_pos(title, 8, 5);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4DFF9A), 0);

    addLabel(screen, "FILE SERVER", 8, 27, lv_color_hex(0x8B949E));

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 5, 48);
    lv_obj_set_size(panel, 310, 112);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char line[160];

    if (apMode)
    {
        std::snprintf(line, sizeof(line), "WiFi: %s", AP_SSID);
        addLabel(panel, line, 8, 7);
        std::snprintf(line, sizeof(line), "AP pass: %s", AP_PASSWORD);
        addLabel(panel, line, 8, 26);
    }
    else
    {
        std::snprintf(line, sizeof(line), "WiFi: %s (existing)", ssid.c_str());
        addLabel(panel, line, 8, 7);
        addLabel(panel, "Using existing connection", 8, 26, lv_color_hex(0x8B949E));
    }

    std::snprintf(line, sizeof(line), "Web user: %s", WEB_USERNAME);
    addLabel(panel, line, 8, 45);
    std::snprintf(line, sizeof(line), "Web pass: %s", WEB_PASSWORD);
    addLabel(panel, line, 137, 45);
    std::snprintf(line, sizeof(line), "Web: http://%s:%d", ip.c_str(), WEB_PORT);
    addLabel(panel, line, 8, 64, lv_color_hex(0x71A7FF));
    std::snprintf(line, sizeof(line), "%s: %s  |  Web: %s",
                  apMode ? "WiFi" : "Net",
                  netOk ? "OK" : "FAIL", webOk ? "OK" : "FAIL");
    addLabel(panel, line, 8, 84, (netOk && webOk) ? lv_color_hex(0x4DFF9A) : lv_color_hex(0xFF6B6B));

    if (!netOk)
    {
        std::snprintf(line, sizeof(line), "Interface: %s", iface.empty() ? "none" : iface.c_str());
        addLabel(screen, line, 8, 163, lv_color_hex(0xFF6B6B));
    }
    addLabel(screen, "ESC: Exit", 8, 195, lv_color_hex(0x8B949E));

    lv_obj_t *keyCatcher = lv_obj_create(screen);
    lv_obj_set_size(keyCatcher, 1, 1);
    lv_obj_set_style_opa(keyCatcher, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyCatcher, 0, 0);
    lv_obj_clear_flag(keyCatcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(keyCatcher, on_global_key, LV_EVENT_KEY, nullptr);

    lv_group_t *mainGroup = lv_group_create();
    lv_group_set_default(mainGroup);
    lv_group_add_obj(mainGroup, keyCatcher);
    lv_group_focus_obj(keyCatcher);

    lv_indev_t *indev = lv_indev_get_next(nullptr);
    while (indev != nullptr)
    {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
        {
            lv_indev_set_group(indev, mainGroup);
        }
        indev = lv_indev_get_next(indev);
    }
}

// ============================================================
// DISPLAY
// ============================================================

#if !USE_DESKTOP
static lv_display_t *initDeviceDisplay()
{
#if APP_USE_DRM
    lv_display_t *display = lv_linux_drm_create();
    if (!display)
        return nullptr;
    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK)
    {
        lv_display_delete(display);
        return nullptr;
    }
    platform::init_key_input(display);
    return display;
#else
    lv_display_t *display = lv_linux_fbdev_create();
    if (!display)
        return nullptr;
    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK)
    {
        lv_display_delete(display);
        return nullptr;
    }
    platform::init_key_input(display);
    return display;
#endif
}
#endif

// ============================================================
// MAIN
// ============================================================
int main()
{
    std::error_code ec;
    fs::create_directories(rootPath(), ec);
    if (ec)
    {
        std::cerr << "Cannot create file root: " << ec.message() << "\n";
        return 1;
    }

    // Initialize LVGL early to render the password prompt
    lv_init();

#if USE_DESKTOP
    std::cerr << "Build this application with USE_DESKTOP=OFF for Cardputer Zero.\n";
    return 1;
#else
    lv_display_t *display = initDeviceDisplay();
    if (!display)
    {
        std::cerr << "Failed to initialize Cardputer display.\n";
        return 1;
    }
#endif

    // Create password UI screen
    lv_obj_t *pwd_screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(pwd_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(pwd_screen, lv_color_hex(0x0D1117), 0);
    lv_obj_clear_flag(pwd_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(pwd_screen);
    lv_label_set_text(label, "Enter sudo password for WIFI permission:");
    lv_obj_set_style_text_color(label, lv_color_hex(0x4DFF9A), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *ta = lv_textarea_create(pwd_screen);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, LV_PCT(80));
    lv_obj_align(ta, LV_ALIGN_CENTER, 0, 0);

    // Event that triggers when the user presses Enter on the keyboard
    lv_obj_add_event_cb(ta, on_password_ready, LV_EVENT_READY, nullptr);

    // Create a new group for input navigation and set it as default
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);

    // Add the text area to the group and focus it
    lv_group_add_obj(group, ta);
    lv_group_focus_obj(ta);

    // Iterate through all input devices and assign KEYPADs to the group
    lv_indev_t *indev = lv_indev_get_next(nullptr);
    while (indev != nullptr)
    {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
        {
            lv_indev_set_group(indev, group);
        }
        indev = lv_indev_get_next(indev);
    }

    // Wait for the user to type the password and hit Enter
    while (!g_passwordReady)
    {
        lv_timer_handler();
        lv_delay_ms(5);
    }

    // Remove the password UI before loading the main UI
    lv_obj_delete(pwd_screen);
    // Small delay to let the ENTER keypress from the password field
    // fully drain, so it doesn't immediately trigger a button on the
    // next screen.
    {
        const int delayMs = 400;
        const int stepMs = 5;
        for (int elapsed = 0; elapsed < delayMs; elapsed += stepMs)
        {
            lv_timer_handler();
            lv_delay_ms(stepMs);
        }
    }
    // Check if the device is already connected to an existing WiFi network
    // תמיד מציגים למשתמש מסך בחירה בהפעלה
    WifiStatus currentWifi = getCurrentWifiStatus();

    lv_obj_t *choiceScreen = createChoiceScreen(currentWifi);
    g_choiceScreenReadyAt = lv_tick_get();
    while (!g_choiceReady)
    {
        lv_timer_handler();
        lv_delay_ms(5);
    }
    lv_obj_delete(choiceScreen);

    bool useExisting = currentWifi.connected && g_useExistingWifi;

    std::string wifiInterface;
    std::string ip = "10.42.0.1";
    bool netOk = false;

    if (useExisting)
    {
        wifiInterface = currentWifi.iface;
        ip = currentWifi.ip;
        netOk = !ip.empty();
        g_apMode = false;
    }
    else
    {
        netOk = startAccessPoint(wifiInterface, ip, g_sudoPassword);
        g_apMode = netOk; // מפרקים AP ביציאה רק אם באמת יצרנו אחד
    }

    g_wifiInterface = wifiInterface;
    WebServer webServer;
    const bool webOk = webServer.start();
    g_webServerPtr = &webServer;

    std::cout << "==============================\n";
    std::cout << " Cardputer Zero File Server\n";
    std::cout << "==============================\n";
    std::cout << "Mode    : " << (useExisting ? "Existing WiFi" : "Access Point") << "\n";
    if (useExisting)
        std::cout << "SSID    : " << currentWifi.ssid << "\n";
    else
    {
        std::cout << "AP SSID : " << AP_SSID << "\n";
        std::cout << "AP PASS : " << AP_PASSWORD << "\n";
    }
    std::cout << "WEB USER: " << WEB_USERNAME << "\n";
    std::cout << "WEB PASS: " << WEB_PASSWORD << "\n";
    std::cout << "IP      : " << ip << "\n";
    std::cout << "WEB     : http://" << ip << ":" << WEB_PORT << "\n";
    std::cout << "FILES   : " << rootPath().string() << "\n";
    std::cout << "NET     : " << (netOk ? "OK" : "FAILED") << "\n";
    std::cout << "WEB     : " << (webOk ? "OK" : "FAILED") << "\n";

    createUI(ip, wifiInterface, netOk, webOk, !useExisting,
             useExisting ? currentWifi.ssid : std::string(AP_SSID));

    while (g_running)
    {
        lv_timer_handler();
        lv_delay_ms(5);
    }

    return 0;
}