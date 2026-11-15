#include "CoopLink.h"

#include <cstring>
#include <cstdlib>
#include <enet/enet.h>
#include <SDL2/SDL.h>
#include <zstd.h>

#include "NDS.h"
#include "GPU.h"
#include "MemConstants.h"
#ifdef OGLRENDERER_ENABLED
#include "OpenGLSupport.h"
#endif

using namespace melonDS;
using namespace Coop;

static u8* cfgPtr(NDS* nds)
{
    return &nds->ARM9.ITCM[CfgAddr & (ITCMPhysicalSize - 1)];
}

CoopHost::CoopHost()
{
    // PMDCOOP_HOST=1 (or a port number) starts the LAN host automatically
    if (const char* env = getenv("PMDCOOP_HOST"))
    {
        int port = atoi(env);
        requestStart(port > 1 ? (uint16_t)port : DefaultPort);
    }
}

CoopHost::~CoopHost()
{
    stop();
}

bool CoopHost::start(uint16_t port)
{
    if (host) return true;
    if (enet_initialize() != 0) return false;

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    // extra peers: a dropped connection keeps its ENet peer until it times out, which must not
    // block a player who reconnects right away (player slots are limited separately)
    host = enet_host_create(&addr, (MaxPlayers - 1) * 3, NumChannels, 0, 0);
    if (!host)
    {
        enet_deinitialize();
        return false;
    }
    for (auto& c : clients) c = Client();
    reserved.clear();
    hostPort = port;
    hostId = (uint32_t)SDL_GetPerformanceCounter() * 2654435761u ^ (uint32_t)(uintptr_t)this;

    // LAN discovery responder (optional: hosting works without it)
    ENetSocket ds = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (ds != ENET_SOCKET_NULL)
    {
        enet_socket_set_option(ds, ENET_SOCKOPT_REUSEADDR, 1);
        enet_socket_set_option(ds, ENET_SOCKOPT_NONBLOCK, 1);
        enet_socket_set_option(ds, ENET_SOCKOPT_BROADCAST, 1);
        ENetAddress da;
        da.host = ENET_HOST_ANY;
        da.port = DiscoveryPort;
        if (enet_socket_bind(ds, &da) == 0) discoverySock = (uintptr_t)ds;
        else enet_socket_destroy(ds);
    }
    return true;
}

bool CoopHost::takeEvent(std::string& out)
{
    std::lock_guard<std::mutex> lk(evLock);
    if (events.empty()) return false;
    out = events.front();
    events.erase(events.begin());
    return true;
}

void CoopHost::pushEvent(const std::string& e)
{
    std::lock_guard<std::mutex> lk(evLock);
    events.push_back(e);
}

void CoopHost::pollDiscovery()
{
    if (discoverySock == ~(uintptr_t)0) return;
    ENetSocket ds = (ENetSocket)discoverySock;
    for (int i = 0; i < 16; i++)
    {
        char buf[64];
        ENetBuffer b{};
        b.data = buf;
        b.dataLength = sizeof(buf);
        ENetAddress from;
        int n = enet_socket_receive(ds, &from, &b, 1);
        if (n <= 0) break;
        if (n < (int)strlen(DiscoveryRequest) || memcmp(buf, DiscoveryRequest, strlen(DiscoveryRequest)) != 0) continue;

        DiscoveryReply r{};
        memcpy(r.magic, DiscoveryMagic, sizeof(r.magic));
        r.version = ProtocolVersion;
        r.players = (uint8_t)(1 + numClients());
        r.maxPlayers = MaxPlayers;
        r.coopRom = curActive != 0 ? 1 : 0;
        r.port = hostPort;
        r.hostId = hostId;
        {
            std::lock_guard<std::mutex> lk(evLock);
            strncpy(r.name, hostName.c_str(), sizeof(r.name));
        }
        ENetBuffer out{};
        out.data = &r;
        out.dataLength = sizeof(r);
        enet_socket_send(ds, &from, &out, 1);
    }
}

int CoopHost::assignPlayer(uint32_t clientId, bool& reconnect)
{
    reconnect = false;
    auto reservedFor = [&](int p) -> uint32_t {
        for (auto& r : reserved) if (r.player == p) return r.clientId;
        return 0;
    };
    int player = 0;
    // 1. a returning client gets its old slot back if it is free
    if (clientId)
        for (auto& r : reserved)
            if (r.clientId == clientId && !clientForPlayer(r.player)) { player = r.player; reconnect = true; break; }
    // 2. lowest free slot nobody else has reserved
    for (int p = 2; p <= MaxPlayers && !player; p++)
        if (!clientForPlayer(p) && reservedFor(p) == 0) player = p;
    // 3. lowest free slot, taking over an absent player's reservation
    for (int p = 2; p <= MaxPlayers && !player; p++)
        if (!clientForPlayer(p)) player = p;
    if (!player) return 0;

    if (clientId)
    {
        for (size_t i = 0; i < reserved.size();)
        {
            if (reserved[i].player == player || reserved[i].clientId == clientId) reserved.erase(reserved.begin() + i);
            else i++;
        }
        reserved.push_back({clientId, player});
    }
    return player;
}

void CoopHost::stop()
{
    if (discoverySock != ~(uintptr_t)0)
    {
        enet_socket_destroy((ENetSocket)discoverySock);
        discoverySock = ~(uintptr_t)0;
    }
    if (!host) return;
    for (auto& c : clients)
        if (c.peer) enet_peer_disconnect_now(c.peer, 0);
    enet_host_flush(host);
    enet_host_destroy(host);
    host = nullptr;
    enet_deinitialize();
    for (auto& c : clients) c = Client();
}

int CoopHost::numClients() const
{
    int n = 0;
    for (auto& c : clients) if (c.peer) n++;
    return n;
}

CoopHost::Client* CoopHost::clientForPlayer(int player)
{
    for (auto& c : clients)
        if (c.peer && c.player == player) return &c;
    return nullptr;
}

void CoopHost::poll()
{
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0)
    {
        switch (ev.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            ev.peer->data = nullptr; // assigned on Hello
            break;

        case ENET_EVENT_TYPE_RECEIVE:
        {
            const u8* d = ev.packet->data;
            size_t len = ev.packet->dataLength;
            Client* cl = (Client*)ev.peer->data;

            if (len >= sizeof(HelloMsg) && d[0] == MsgHello && !cl)
            {
                HelloMsg hello; memcpy(&hello, d, sizeof(hello));
                if (hello.version != ProtocolVersion)
                {
                    RejectMsg rej{MsgReject, 2};
                    enet_peer_send(ev.peer, ChanControl, enet_packet_create(&rej, sizeof(rej), ENET_PACKET_FLAG_RELIABLE));
                    enet_peer_disconnect_later(ev.peer, 0);
                    break;
                }
                // same client id still connected = it dropped and came back before the old
                // connection timed out: throw the stale connection out and reuse its slot
                if (hello.clientId)
                    for (auto& c : clients)
                        if (c.peer && c.clientId == hello.clientId && c.peer != ev.peer)
                        {
                            c.peer->data = nullptr;
                            enet_peer_reset(c.peer);
                            c = Client();
                        }
                bool reconnect = false;
                int player = assignPlayer(hello.clientId, reconnect);
                Client* slot = nullptr;
                for (auto& c : clients) if (!c.peer) { slot = &c; break; }
                if (!player || !slot)
                {
                    RejectMsg rej{MsgReject, 1};
                    enet_peer_send(ev.peer, ChanControl, enet_packet_create(&rej, sizeof(rej), ENET_PACKET_FLAG_RELIABLE));
                    enet_peer_disconnect_later(ev.peer, 0);
                    break;
                }
                *slot = Client();
                slot->peer = ev.peer;
                slot->player = player;
                slot->clientId = hello.clientId;
                memcpy(slot->name, hello.name, 16);
                ev.peer->data = slot;
                {
                    char msg[96];
                    snprintf(msg, sizeof(msg), reconnect ? "Coop: Spieler %d (%s) ist wieder da" : "Coop: Spieler %d (%s) ist beigetreten",
                             player, slot->name[0] ? slot->name : "?");
                    pushEvent(msg);
                }
                WelcomeMsg w{MsgWelcome, ProtocolVersion, (uint8_t)player};
                enet_peer_send(ev.peer, ChanControl, enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE));
            }
            else if (len >= sizeof(InputMsg) && d[0] == MsgInput && cl)
            {
                InputMsg in; memcpy(&in, d, sizeof(in));
                cl->keys = in.keys & 0xFFF;
                cl->touching = in.touching != 0;
                cl->tx = in.touchX;
                cl->ty = in.touchY;
                if (in.hotkeySeq != cl->hotkeySeq)
                {
                    cl->hotkeySeq = in.hotkeySeq;
                    cl->hotkey = in.hotkey;
                    cl->hotkeyPending = true;
                }
            }
            enet_packet_destroy(ev.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        {
            Client* cl = (Client*)ev.peer->data;
            if (cl)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "Coop: Spieler %d (%s) hat die Verbindung verloren", cl->player, cl->name[0] ? cl->name : "?");
                pushEvent(msg);
                *cl = Client();
            }
            ev.peer->data = nullptr;
            break;
        }

        default: break;
        }
    }
}

void CoopHost::preFrame(NDS* nds, u32& keyMask, bool& touching, u16& touchX, u16& touchY)
{
    int req = pending.exchange(-1);
    if (req == 0 && host) { stop(); status = "Coop: LAN-Host gestoppt"; }
    else if (req > 0 && !host)
        status = start((uint16_t)req) ? "Coop: LAN-Host aktiv (Port 7064)" : "Coop: LAN-Host konnte nicht starten";
    if (!host) return;
    poll();
    pollDiscovery();

    u8* cfg = cfgPtr(nds);
    u32 magic; memcpy(&magic, cfg, 4);
    if (magic != CfgMagic)
    {
        if (curActive != 0) { curActive = 0; activeChanged = true; }
        return;
    }

    // tell the game how many humans are playing (host + connected clients)
    int highest = 1;
    for (auto& c : clients) if (c.peer && c.player > highest) highest = c.player;
    cfg[4] = (u8)highest;

    int active = cfg[5];
    if (active < 1 || active > MaxPlayers) active = 1;
    if (active != curActive) { curActive = active; activeChanged = true; }

    // hotkeys: only the player whose turn it is may trigger one; others are dropped
    int hk = localHotkey.exchange(0);
    Client* cl = active > 1 ? clientForPlayer(active) : nullptr;
    if (cl) hk = cl->hotkeyPending ? cl->hotkey : 0;
    for (auto& c : clients) c.hotkeyPending = false;

    if (cl)
    {
        keyMask = cl->keys;
        touching = cl->touching;
        touchX = cl->tx;
        touchY = cl->ty;
    }
    // (active > 1 without client: player left mid-turn, host takes over)

    if (script.empty() && hk >= 1 && hk <= 10) startHotkey(cfg, hk);

    if (!script.empty())
    {
        const Step& st = script[scriptPos];
        keyMask = 0xFFF & ~(u32)st.pressed;
        touching = st.touch;
        touchX = st.x;
        touchY = st.y;
        if (++scriptFrames >= st.frames)
        {
            scriptFrames = 0;
            if (++scriptPos >= script.size())
            {
                script.clear();
                scriptPos = 0;
                if (scriptIsMove) cfg[8] = 0; // not consumed (e.g. a menu was open): drop it
            }
        }
    }
}

void CoopHost::startHotkey(u8* cfg, int n)
{
    const uint16_t kA = 1 << 0, kDown = 1 << 7, kL = 1 << 9;
    script.clear();
    scriptPos = 0;
    scriptFrames = 0;
    if (n <= 4)
    {
        // the game's own "L+A = use set move" shortcut; the ROM hook swaps in move slot n
        cfg[8] = (u8)n;
        scriptIsMove = true;
        script.push_back({kL, false, 0, 0, 2});
        script.push_back({(uint16_t)(kL | kA), false, 0, 0, 6});
        script.push_back({0, false, 0, 0, 2});
    }
    else
    {
        // touch the "Items" button, move the cursor to the slot and open that item's action menu
        scriptIsMove = false;
        int slot = n - 5;
        script.push_back({0, true, 129, 183, 4});
        script.push_back({0, false, 0, 0, 30});
        for (int i = 0; i < slot; i++)
        {
            script.push_back({kDown, false, 0, 0, 4});
            script.push_back({0, false, 0, 0, 6});
        }
        script.push_back({kA, false, 0, 0, 4});
        script.push_back({0, false, 0, 0, 2});
    }
}

bool CoopHost::grabFrame(NDS* nds, const u32*& top, const u32*& bottom)
{
    void* t = nullptr; void* b = nullptr;
    if (nds->GPU.GetFramebuffers(&t, &b) && t && b)
    {
        top = (const u32*)t;
        bottom = (const u32*)b;
        return true;
    }
    if (!t) return false;
    readGLFramebuffers(t, glTop, glBottom);
    if (glTop.empty()) return false;
    top = glTop.data();
    bottom = glBottom.data();
    return true;
}

void CoopHost::readGLFramebuffers(void* texHandle, std::vector<u32>& top, std::vector<u32>& bottom)
{
#ifdef OGLRENDERER_ENABLED
    GLuint tex = *(GLuint*)texHandle;
    GLint prevRead = 0, prevTex = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex);

    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex);
    if (w < ScreenW || h < ScreenH) { top.clear(); return; }

    if (!glFbo) glGenFramebuffers(1, &glFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, glFbo);
    glScratch.resize((size_t)w * h);
    top.resize(ScreenPixels);
    bottom.resize(ScreenPixels);
    int sx = w / ScreenW, sy = h / ScreenH;

    for (int layer = 0; layer < 2; layer++)
    {
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, glScratch.data());
        std::vector<u32>& dst = layer ? bottom : top;
        // nearest-neighbour downscale to native resolution (rows are already top-down: melonDS
        // renders its output textures in DS scanline order)
        for (int y = 0; y < ScreenH; y++)
        {
            const u32* srow = glScratch.data() + (size_t)(y * sy) * w;
            u32* drow = dst.data() + (size_t)y * ScreenW;
            for (int x = 0; x < ScreenW; x++) drow[x] = srow[x * sx];
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
#else
    (void)texHandle;
    top.clear();
#endif
}

void CoopHost::pushAudio(const s16* stereo, int frames, int freq)
{
    if (!host) return;
    std::lock_guard<std::mutex> lk(audioLock);
    audioFreq = freq;
    // cap at ~0.25s to avoid unbounded growth when the emu thread stalls
    size_t maxSamples = (size_t)freq / 2;
    if (audioBuf.size() + frames * 2 > maxSamples) audioBuf.clear();
    audioBuf.insert(audioBuf.end(), stereo, stereo + frames * 2);
}

void CoopHost::postFrame(NDS* nds)
{
    if (!host) return;
    frameNum++;
    if (numClients() == 0)
    {
        std::lock_guard<std::mutex> lk(audioLock);
        audioBuf.clear();
        return;
    }

    void* top = nullptr; void* bottom = nullptr;
    bool haveVideo = nds->GPU.GetFramebuffers(&top, &bottom) && top && bottom;
    if (!haveVideo && top)
    {
        // OpenGL renderer: the frame lives in a texture, read it back (GL context is current on the emu thread)
        readGLFramebuffers(top, glTop, glBottom);
        if (!glTop.empty())
        {
            top = glTop.data();
            bottom = glBottom.data();
            haveVideo = true;
        }
    }

    std::vector<s16> audio;
    int freq;
    {
        std::lock_guard<std::mutex> lk(audioLock);
        audio.swap(audioBuf);
        freq = audioFreq;
    }
    size_t audioFrames = audio.size() / 2;
    if (audioFrames > 0xFFFF) audioFrames = 0xFFFF;

    size_t videoBytes = haveVideo ? ScreenPixels * 4 * 2 : 0;
    size_t rawSize = videoBytes + audioFrames * 4;
    raw.resize(rawSize);
    if (haveVideo)
    {
        memcpy(raw.data(), top, ScreenPixels * 4);
        memcpy(raw.data() + ScreenPixels * 4, bottom, ScreenPixels * 4);
    }
    if (audioFrames) memcpy(raw.data() + videoBytes, audio.data(), audioFrames * 4);

    size_t bound = ZSTD_compressBound(rawSize);
    comp.resize(sizeof(FrameHeader) + bound);
    size_t csize = ZSTD_compress(comp.data() + sizeof(FrameHeader), bound, raw.data(), rawSize, 1);
    if (ZSTD_isError(csize)) return;

    FrameHeader h{};
    h.type = MsgFrame;
    h.activePlayer = (uint8_t)curActive;
    h.numPlayers = (uint8_t)(1 + numClients());
    h.flags = haveVideo ? 1 : 0;
    h.frameNum = frameNum;
    h.audioFreq = (uint32_t)freq;
    h.audioFrames = (uint16_t)audioFrames;
    h.rawSize = (uint32_t)rawSize;
    memcpy(comp.data(), &h, sizeof(h));

    // unreliable: a lost frame is simply replaced by the next one (no stalls)
    ENetPacket* pkt = enet_packet_create(comp.data(), sizeof(FrameHeader) + csize, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    for (auto& c : clients)
        if (c.peer) enet_peer_send(c.peer, ChanFrame, pkt);
    if (pkt->referenceCount == 0) enet_packet_destroy(pkt);
    enet_host_flush(host);
}


// ---------------------------------------------------------------------------------------------
// CoopClient
// ---------------------------------------------------------------------------------------------

CoopClient::CoopClient()
{
    // PMDCOOP_JOIN=<host>[:port] joins a host right after startup (same as the menu entry)
    if (const char* env = getenv("PMDCOOP_JOIN"))
    {
        std::string h = env;
        int port = DefaultPort;
        size_t colon = h.rfind(':');
        if (colon != std::string::npos && atoi(h.c_str() + colon + 1) > 0)
        {
            port = atoi(h.c_str() + colon + 1);
            h = h.substr(0, colon);
        }
        if (!h.empty()) requestConnect(h, (uint16_t)port, "Spieler", 0);
    }
}

CoopClient::~CoopClient()
{
    shutdown(nullptr);
}

void CoopClient::requestConnect(const std::string& hostName, uint16_t port, const std::string& name, uint32_t clientId)
{
    std::lock_guard<std::mutex> lk(reqLock);
    reqHost = hostName;
    reqPort = port;
    reqName = name;
    reqClientId = clientId;
    reqPending = true;
    wantDisconnect = false;
    active = true;
}

std::string CoopClient::takeMessage()
{
    std::lock_guard<std::mutex> lk(msgLock);
    std::string m;
    m.swap(message);
    return m;
}

void CoopClient::shutdown(const char* msg)
{
    if (host)
    {
        if (peer && connected) { enet_peer_disconnect(peer, 0); enet_host_flush(host); }
        enet_host_destroy(host);
        enet_deinitialize();
    }
    host = nullptr;
    peer = nullptr;
    connected = false;
    active = false;
    player = curActive = curPlayers = 0;
    {
        std::lock_guard<std::mutex> lk(audioLock);
        audioBuf.clear();
    }
    if (msg)
    {
        std::lock_guard<std::mutex> lk(msgLock);
        message = msg;
    }
}

bool CoopClient::frame(u32 keyMask, bool touching, u16 touchX, u16 touchY, int hotkey)
{
    // new connect request?
    {
        std::lock_guard<std::mutex> lk(reqLock);
        if (reqPending)
        {
            reqPending = false;
            shutdown(nullptr);
            active = true;
            if (enet_initialize() != 0) { shutdown("Coop: Netzwerk konnte nicht gestartet werden"); return false; }
            host = enet_host_create(nullptr, 1, NumChannels, 0, 0);
            ENetAddress addr;
            if (!host || enet_address_set_host(&addr, reqHost.c_str()) != 0)
            {
                if (!host) enet_deinitialize();
                shutdown("Coop: Host-Adresse ist ungültig");
                return false;
            }
            addr.port = reqPort;
            peer = enet_host_connect(host, &addr, NumChannels, 0);
            myName = reqName;
            myClientId = reqClientId;
            connectStartMs = SDL_GetTicks();
            std::fill(top.begin(), top.end(), 0);
            std::fill(bottom.begin(), bottom.end(), 0);
            lastSent = InputMsg{};
            if (!peer) { shutdown("Coop: Verbindung konnte nicht aufgebaut werden"); return false; }
        }
    }
    if (!host) return false;
    if (wantDisconnect.exchange(false)) { shutdown("Coop: Verbindung getrennt"); return false; }

    if (!connected && SDL_GetTicks() - connectStartMs > 6000)
    {
        shutdown("Coop: Host nicht erreichbar (IP, Firewall und \"LAN-Host\" beim Host prüfen)");
        return false;
    }

    bool newFrame = false;
    ENetEvent ev;
    while (host && enet_host_service(host, &ev, 0) > 0)
    {
        if (ev.type == ENET_EVENT_TYPE_CONNECT)
        {
            HelloMsg h{};
            h.type = MsgHello;
            h.version = ProtocolVersion;
            strncpy(h.name, myName.c_str(), sizeof(h.name) - 1);
            h.clientId = myClientId;
            enet_peer_send(peer, ChanControl, enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE));
        }
        else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            shutdown(connected ? "Coop: Der Host hat die Verbindung beendet" : "Coop: Host nicht erreichbar");
            return false;
        }
        else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
        {
            const u8* d = ev.packet->data;
            size_t len = ev.packet->dataLength;
            if (len >= sizeof(WelcomeMsg) && d[0] == MsgWelcome)
            {
                player = d[2];
                connected = true;
            }
            else if (len >= sizeof(RejectMsg) && d[0] == MsgReject)
            {
                enet_packet_destroy(ev.packet);
                shutdown(d[1] == 1 ? "Coop: Der Host ist voll" : "Coop: Host und Client haben unterschiedliche Versionen");
                return false;
            }
            else if (len >= sizeof(FrameHeader) && d[0] == MsgFrame)
            {
                FrameHeader h;
                memcpy(&h, d, sizeof(h));
                size_t vbytes = (h.flags & 1) ? ScreenPixels * 4 * 2 : 0;
                if (h.rawSize >= vbytes + (size_t)h.audioFrames * 4 && h.rawSize <= vbytes + 0x10000 * 4)
                {
                    raw.resize(h.rawSize);
                    size_t r = ZSTD_decompress(raw.data(), raw.size(), d + sizeof(h), len - sizeof(h));
                    if (!ZSTD_isError(r) && r == h.rawSize)
                    {
                        curActive = h.activePlayer;
                        curPlayers = h.numPlayers;
                        if (vbytes)
                        {
                            memcpy(top.data(), raw.data(), ScreenPixels * 4);
                            memcpy(bottom.data(), raw.data() + ScreenPixels * 4, ScreenPixels * 4);
                            newFrame = true;
                        }
                        if (h.audioFrames)
                        {
                            std::lock_guard<std::mutex> lk(audioLock);
                            // keep latency low: never queue more than ~100 ms
                            size_t maxSamples = (size_t)h.audioFreq / 10 * 2;
                            if (audioBuf.size() > maxSamples) audioBuf.clear();
                            const s16* a = (const s16*)(raw.data() + vbytes);
                            audioBuf.insert(audioBuf.end(), a, a + (size_t)h.audioFrames * 2);
                        }
                    }
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    if (!host) return false;

    if (connected)
    {
        if (hotkey >= 1 && hotkey <= 10) { hotkeyVal = (uint8_t)hotkey; hotkeySeq++; }
        InputMsg in{};
        in.type = MsgInput;
        in.keys = (uint16_t)(keyMask & 0xFFF);
        in.touching = touching ? 1 : 0;
        in.touchX = (uint8_t)touchX;
        in.touchY = (uint8_t)touchY;
        in.hotkey = hotkeyVal;
        in.hotkeySeq = hotkeySeq;
        uint32_t now = SDL_GetTicks();
        if (memcmp(&in, &lastSent, sizeof(in)) != 0 || now - lastSendMs > 100)
        {
            enet_peer_send(peer, ChanInput, enet_packet_create(&in, sizeof(in), ENET_PACKET_FLAG_RELIABLE));
            enet_host_flush(host);
            lastSent = in;
            lastSendMs = now;
        }
    }
    return newFrame;
}

int CoopClient::popAudio(s16* out, int frames)
{
    std::lock_guard<std::mutex> lk(audioLock);
    int avail = (int)(audioBuf.size() / 2);
    int n = avail < frames ? avail : frames;
    if (n > 0)
    {
        memcpy(out, audioBuf.data(), (size_t)n * 4);
        audioBuf.erase(audioBuf.begin(), audioBuf.begin() + (size_t)n * 2);
    }
    return n;
}
