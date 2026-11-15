#include "CoopDebug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
static void setNonBlocking(SOCKET s) { u_long m = 1; ioctlsocket(s, FIONBIO, &m); }
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
static void setNonBlocking(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
#define CLOSESOCK close
#endif

#include "EmuInstance.h"
#include "NDS.h"
#include "GPU.h"

using namespace melonDS;

CoopDebug::CoopDebug()
{
    const char* env = getenv("PMDCOOP_DEBUG");
    if (!env) return;
    int port = atoi(env);
    if (port <= 0) return;
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0)
    {
        CLOSESOCK(fd);
        return;
    }
    setNonBlocking(fd);
    listenFd = (intptr_t)fd;
    printf("[coop-debug] listening on 127.0.0.1:%d\n", port);
}

CoopDebug::~CoopDebug()
{
    if (connFd >= 0) CLOSESOCK(connFd);
    if (listenFd >= 0) CLOSESOCK(listenFd);
}

void CoopDebug::reply(const std::string& s)
{
    if (connFd < 0) return;
    std::string l = s + "\n";
    send(connFd, l.data(), (int)l.size(), 0);
}

void CoopDebug::handleLine(EmuInstance* inst, NDS* nds, const std::string& line)
{
    std::istringstream is(line);
    std::string cmd; is >> cmd;
    if (cmd == "status")
    {
        char b[96]; snprintf(b, sizeof(b), "ok frame=%u active=%d clients=%d", frame, lastActive, lastClients);
        reply(b);
    }
    else if (cmd == "hold")
    {
        std::string m; is >> m; holdMask = (uint16_t)strtoul(m.c_str(), nullptr, 16);
        reply("ok");
    }
    else if (cmd == "tap")
    {
        std::string m; int n = 6; is >> m >> n;
        tapMask = (uint16_t)strtoul(m.c_str(), nullptr, 16);
        tapFrames = n > 0 ? n : 6;
        waitFrames = -2; // reply when tap finished
    }
    else if (cmd == "wait")
    {
        int n = 1; is >> n; waitFrames = n > 0 ? n : 1;
    }
    else if (cmd == "shot")
    {
        std::string path; std::getline(is >> std::ws, path);
        const u32* top = nullptr; const u32* bot = nullptr;
        if (!inst->coopHost.grabFrame(nds, top, bot)) { reply("err no framebuffer"); return; }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { reply("err open"); return; }
        fprintf(f, "P6\n256 384\n255\n");
        for (int s = 0; s < 2; s++)
        {
            const u32* px = (const u32*)(s ? bot : top);
            for (int i = 0; i < 256 * 192; i++)
            {
                u8 rgb[3] = {(u8)(px[i] >> 16), (u8)(px[i] >> 8), (u8)px[i]};
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        reply("ok");
    }
    else if (cmd == "save" || cmd == "load")
    {
        std::string path; std::getline(is >> std::ws, path);
        bool ok = cmd == "save" ? inst->saveState(path) : inst->loadState(path);
        reply(ok ? "ok" : "err state");
    }
    else if (cmd == "hotkey")
    {
        int n = 0; is >> n;
        inst->coopHost.pressHotkey(n);
        reply("ok");
    }
    else if (cmd == "peek")
    {
        std::string as; int n = 4; is >> as >> n;
        u32 addr = strtoul(as.c_str(), nullptr, 16);
        std::string out = "ok ";
        char h[3];
        for (int i = 0; i < n && i < 4096; i++)
        {
            snprintf(h, sizeof(h), "%02x", nds->ARM9Read8(addr + i));
            out += h;
        }
        reply(out);
    }
    else if (cmd == "poke")
    {
        std::string as, hex; is >> as >> hex;
        u32 addr = strtoul(as.c_str(), nullptr, 16);
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            nds->ARM9Write8(addr + (u32)(i / 2), (u8)strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        reply("ok");
    }
    else reply("err unknown command");
}

void CoopDebug::preFrame(EmuInstance* inst, NDS* nds, u32& keyMask)
{
    if (listenFd < 0) return;

    if (connFd < 0)
    {
        auto c = accept(listenFd, nullptr, nullptr);
        if ((intptr_t)c >= 0 && c != (decltype(c))~0)
        {
            setNonBlocking(c);
            connFd = (intptr_t)c;
            inbuf.clear();
        }
    }
    // only read new commands when nothing is pending
    if (connFd >= 0 && waitFrames == -1 && tapFrames == 0)
    {
        char buf[1024];
        int n = recv(connFd, buf, sizeof(buf), 0);
        if (n == 0) { CLOSESOCK(connFd); connFd = -1; holdMask = 0; }
        else if (n > 0) inbuf.append(buf, n);
        size_t nl;
        while (waitFrames == -1 && tapFrames == 0 && (nl = inbuf.find('\n')) != std::string::npos)
        {
            std::string line = inbuf.substr(0, nl);
            inbuf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handleLine(inst, nds, line);
        }
    }

    u16 pressed = holdMask | (tapFrames > 0 ? tapMask : 0);
    keyMask &= ~(u32)pressed;
}

void CoopDebug::postFrame(NDS* nds, int activePlayer, int clients)
{
    frame++;
    lastActive = activePlayer;
    lastClients = clients;
    if (listenFd < 0) return;
    if (tapFrames > 0)
    {
        if (--tapFrames == 0) waitFrames = 2; // let the release register before replying
    }
    else if (waitFrames > 0)
    {
        if (--waitFrames == 0) { waitFrames = -1; reply("ok"); }
    }
}
