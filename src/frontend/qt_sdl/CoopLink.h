// Host side of the PMD co-op LAN mode.
#pragma once

#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <cstdint>
#include "types.h"
#include "../../coop/CoopProtocol.h"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace melonDS { class NDS; }

class CoopHost
{
public:
    CoopHost();
    ~CoopHost();

    // thread-safe requests from the UI thread, executed on the emu thread in preFrame()
    void requestStart(uint16_t port) { pending = port; }
    void requestStop() { pending = 0; }
    bool wantsActive() const { int p = pending; return p > 0 || (p < 0 && host); }
    // message about the last start/stop (emu thread -> OSD), empty if none
    const char* takeStatus() { const char* s = status; status = nullptr; return s; }

    // name announced to LAN discovery (host player's firmware name)
    void setHostName(const std::string& n) { std::lock_guard<std::mutex> lk(evLock); hostName = n; }
    // OSD messages about players joining/leaving (emu thread)
    bool takeEvent(std::string& out);

    bool start(uint16_t port);
    void stop();
    bool isActive() const { return host != nullptr; }
    int numClients() const;

    // Called on the emu thread right before SetKeyMask/TouchScreen.
    // Takes the host's local input and returns the input that should be fed to the DS.
    void preFrame(melonDS::NDS* nds, melonDS::u32& keyMask, bool& touching, melonDS::u16& touchX, melonDS::u16& touchY);
    // Called on the emu thread after RunFrame.
    void postFrame(melonDS::NDS* nds);
    // Current frame as two 256x192 RGB32 buffers (software or OpenGL renderer). Emu thread only.
    bool grabFrame(melonDS::NDS* nds, const melonDS::u32*& top, const melonDS::u32*& bottom);
    // Called from the SDL audio callback (any thread).
    void pushAudio(const melonDS::s16* stereo, int frames, int freq);

    // local (host player) hotkey press, any thread: 1-4 = move slot, 5-10 = item slot 1-6
    void pressHotkey(int n) { localHotkey = n; }
    int takeLocalHotkey() { return localHotkey.exchange(0); }

    // whose turn it is according to the patched ROM (0 = ROM not patched)
    int activePlayer() const { return curActive; }
    // returns true once when the active player changed (for OSD)
    bool takeActiveChanged() { bool r = activeChanged; activeChanged = false; return r; }

private:
    struct Client
    {
        ENetPeer* peer = nullptr;
        int player = 0;              // 2..4
        uint16_t keys = 0xFFF;
        bool touching = false;
        uint8_t tx = 0, ty = 0;
        uint8_t hotkey = 0, hotkeySeq = 0;
        bool hotkeyPending = false;
        uint32_t clientId = 0;
        char name[17] = {};
    };

    void poll();
    void pollDiscovery();
    void pushEvent(const std::string& e);
    int assignPlayer(uint32_t clientId, bool& reconnect);
    Client* clientForPlayer(int player);
    void readGLFramebuffers(void* texHandle, std::vector<melonDS::u32>& top, std::vector<melonDS::u32>& bottom);

    // scripted input (hotkey macros), applied on top of the active player's pad
    struct Step { uint16_t pressed; bool touch; uint8_t x, y; int frames; };
    void startHotkey(melonDS::u8* cfg, int n);
    std::vector<Step> script;
    size_t scriptPos = 0;
    int scriptFrames = 0;
    bool scriptIsMove = false;
    std::atomic<int> localHotkey{0};

    std::atomic<int> pending{-1};
    const char* status = nullptr;
    uint16_t hostPort = 0;
    uint32_t hostId = 0;
    uintptr_t discoverySock = ~(uintptr_t)0;
    struct Reservation { uint32_t clientId; int player; };
    std::vector<Reservation> reserved;     // remembered slots of known clients (while hosting)
    std::mutex evLock;
    std::string hostName;
    std::vector<std::string> events;
    std::vector<melonDS::u32> glTop, glBottom, glScratch;
    unsigned int glFbo = 0;
    ENetHost* host = nullptr;
    Client clients[Coop::MaxPlayers - 1];
    int curActive = 0;
    bool activeChanged = false;
    uint32_t frameNum = 0;

    std::mutex audioLock;
    std::vector<melonDS::s16> audioBuf;
    int audioFreq = 48000;

    std::vector<uint8_t> raw, comp;
};


// Client side inside melonDS: a second melonDS instance (no ROM loaded) joins a host,
// shows its screens in the normal screen panel, plays its audio and sends the local pad.
class CoopClient
{
public:
    CoopClient();
    ~CoopClient();

    // UI thread
    void requestConnect(const std::string& hostName, uint16_t port, const std::string& name, uint32_t clientId);
    void requestDisconnect() { wantDisconnect = true; }

    // true while connecting or connected (emu thread drives frame() in that case)
    bool isActive() const { return active; }
    // true once the host accepted us; the screen panels then show our buffers
    bool isConnected() const { return connected; }

    // emu thread; returns true when a new video frame arrived
    bool frame(melonDS::u32 keyMask, bool touching, melonDS::u16 touchX, melonDS::u16 touchY, int hotkey);
    // audio thread; returns number of stereo frames written
    int popAudio(melonDS::s16* out, int frames);

    const melonDS::u32* topBuffer() const { return top.data(); }
    const melonDS::u32* bottomBuffer() const { return bottom.data(); }

    int myPlayer() const { return player; }
    int activePlayer() const { return curActive; }
    int numPlayers() const { return curPlayers; }
    // one-shot status text for the window title / message box ("" if none)
    std::string takeMessage();

private:
    void shutdown(const char* msg);

    std::mutex reqLock;
    std::string reqHost, reqName;
    uint32_t reqClientId = 0, myClientId = 0;
    uint16_t reqPort = 0;
    bool reqPending = false;
    std::atomic<bool> wantDisconnect{false};

    std::atomic<bool> active{false}, connected{false};
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    std::string myName;
    int player = 0, curActive = 0, curPlayers = 0;
    uint32_t connectStartMs = 0;

    std::vector<melonDS::u32> top = std::vector<melonDS::u32>(Coop::ScreenPixels);
    std::vector<melonDS::u32> bottom = std::vector<melonDS::u32>(Coop::ScreenPixels);
    std::vector<uint8_t> raw;

    Coop::InputMsg lastSent{};
    uint32_t lastSendMs = 0;
    uint8_t hotkeyVal = 0, hotkeySeq = 0;

    std::mutex audioLock;
    std::vector<melonDS::s16> audioBuf;

    std::mutex msgLock;
    std::string message;
};
