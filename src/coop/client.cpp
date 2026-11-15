// pmdcoop_client: thin LAN client for the PMD co-op host.
// usage: pmdcoop_client <host-ip> [port] [name] [--pad N]
//   --pad N  use gamepad number N (0 = first). Two players on one PC: melonDS uses pad 0, client --pad 1
//
// Shows the host's two screens, plays its audio and sends the local pad.
// Keyboard: WASD (or arrows) = move, E = A (interact/confirm), Q = B (cancel), R = X (menu), F = Y,
//           Left Shift = R (hold for diagonals), C = L, Enter = Start, Backspace = Select
//           1-4 = use move 1-4, 5-9 and 0 = open item slot 1-6
// Gamepad: positional Nintendo layout (east=A, south=B, north=X, west=Y), shoulders=L/R
// Mouse on the bottom screen = touch. F11 = fullscreen.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <enet/enet.h>
#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <ctime>

#include "CoopProtocol.h"

using namespace Coop;

enum DSKey { kA, kB, kSelect, kStart, kRight, kLeft, kUp, kDown, kR, kL, kX, kY };

static int keyForScancode(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_E: return kA;            // interact / confirm
    case SDL_SCANCODE_Q: return kB;            // cancel / decline
    case SDL_SCANCODE_R: return kX;            // menu
    case SDL_SCANCODE_F: return kY;
    case SDL_SCANCODE_C: return kL;
    case SDL_SCANCODE_LSHIFT: return kR;       // hold: diagonal movement
    case SDL_SCANCODE_RETURN: return kStart;
    case SDL_SCANCODE_BACKSPACE: return kSelect;
    case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: return kRight;
    case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: return kLeft;
    case SDL_SCANCODE_W: case SDL_SCANCODE_UP: return kUp;
    case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: return kDown;
    default: return -1;
    }
}

static int keyForButton(int b)
{
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_B: return kA;
    case SDL_CONTROLLER_BUTTON_A: return kB;
    case SDL_CONTROLLER_BUTTON_Y: return kX;
    case SDL_CONTROLLER_BUTTON_X: return kY;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kL;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kR;
    case SDL_CONTROLLER_BUTTON_START: return kStart;
    case SDL_CONTROLLER_BUTTON_BACK: return kSelect;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kRight;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return kUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kDown;
    default: return -1;
    }
}

int main(int argc, char** argv)
{
    std::vector<const char*> pos;
    int padIndex = -1; // -1 = first available
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--pad") && i + 1 < argc) padIndex = atoi(argv[++i]);
        else pos.push_back(argv[i]);
    }
    // host "" or "auto" = find a host on the LAN
    std::string hostArg = pos.size() > 0 ? pos[0] : "auto";
    uint16_t port = pos.size() > 1 ? (uint16_t)atoi(pos[1]) : DefaultPort;
    const char* name = pos.size() > 2 ? pos[2] : "Spieler";

    // keep reading the gamepad while another window (e.g. melonDS on the same PC) has focus
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (enet_initialize() != 0) { fprintf(stderr, "enet init failed\n"); return 1; }

    // stable client id (file next to the exe) so the host gives us our old slot back after a disconnect
    uint32_t clientId = 0;
    {
        char* base = SDL_GetBasePath();
        std::string idPath = std::string(base ? base : "") + "client_id.txt";
        if (base) SDL_free(base);
        if (FILE* f = fopen(idPath.c_str(), "r")) { if (fscanf(f, "%u", &clientId) != 1) clientId = 0; fclose(f); }
        if (!clientId)
        {
            clientId = ((uint32_t)SDL_GetPerformanceCounter() * 2654435761u) ^ (uint32_t)time(nullptr) ^ 0x5A5A1234u;
            if (!clientId) clientId = 1;
            if (FILE* f = fopen(idPath.c_str(), "w")) { fprintf(f, "%u\n", clientId); fclose(f); }
        }
        if (padIndex > 0) clientId ^= (uint32_t)padIndex * 0x9E3779B9u; // several local clients on one PC
    }

    ENetAddress addr;
    addr.port = port;
    if (hostArg.empty() || hostArg == "auto")
    {
        // LAN discovery: broadcast a probe and take the first host that answers
        printf("Suche Host im Netzwerk...\n");
        ENetSocket ds = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        enet_socket_set_option(ds, ENET_SOCKOPT_BROADCAST, 1);
        enet_socket_set_option(ds, ENET_SOCKOPT_NONBLOCK, 1);
        bool found = false;
        for (int attempt = 0; attempt < 10 && !found; attempt++)
        {
            ENetAddress bc;
            bc.host = ENET_HOST_BROADCAST;
            bc.port = DiscoveryPort;
            ENetBuffer pb{};
            pb.data = (void*)DiscoveryRequest;
            pb.dataLength = strlen(DiscoveryRequest);
            enet_socket_send(ds, &bc, &pb, 1);
            enet_address_set_host(&bc, "127.0.0.1");
            enet_socket_send(ds, &bc, &pb, 1);
            uint32_t until = SDL_GetTicks() + 500;
            while (SDL_GetTicks() < until && !found)
            {
                DiscoveryReply r;
                ENetBuffer rb{};
                rb.data = &r;
                rb.dataLength = sizeof(r);
                ENetAddress from;
                int n = enet_socket_receive(ds, &from, &rb, 1);
                if (n >= (int)sizeof(r) && memcmp(r.magic, DiscoveryMagic, sizeof(r.magic)) == 0)
                {
                    addr.host = from.host;
                    addr.port = r.port;
                    char ip[64] = "?";
                    enet_address_get_host_ip(&from, ip, sizeof(ip));
                    printf("Host gefunden: %.16s (%s:%u)\n", r.name, ip, r.port);
                    found = true;
                }
                else SDL_Delay(10);
            }
        }
        enet_socket_destroy(ds);
        if (!found)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PMD Coop",
                "Kein Host im Netzwerk gefunden.\nLäuft beim Host das Spiel mit eingeschaltetem \"PMD Coop: LAN-Host\"?", nullptr);
            return 1;
        }
    }
    else
        enet_address_set_host(&addr, hostArg.c_str());
    const char* hostName = hostArg.c_str();

    ENetHost* client = enet_host_create(nullptr, 1, NumChannels, 0, 0);
    ENetPeer* peer = enet_host_connect(client, &addr, NumChannels, 0);
    if (!peer) { fprintf(stderr, "connect failed\n"); return 1; }

    const int scale = 2;
    SDL_Window* win = SDL_CreateWindow("PMD Coop - verbinde...", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       ScreenW * scale, ScreenH * 2 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, ScreenW + 8, ScreenH * 2 + 8);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ScreenW, ScreenH * 2);

    SDL_AudioDeviceID audioDev = 0;
    int audioFreq = 0;

    SDL_GameController* pad = nullptr;
    SDL_JoystickID padId = -1;
    auto tryOpenPad = [&](int devIndex) {
        if (pad || !SDL_IsGameController(devIndex)) return;
        if (padIndex >= 0 && devIndex != padIndex) return;
        pad = SDL_GameControllerOpen(devIndex);
        if (pad)
        {
            padId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
            printf("Gamepad %d: %s\n", devIndex, SDL_GameControllerName(pad));
        }
    };
    for (int i = 0; i < SDL_NumJoysticks(); i++) tryOpenPad(i);

    uint16_t kbKeys = 0, arrowKeys = 0, padKeys = 0;
    uint8_t hotkey = 0, hotkeySeq = 0;
    bool touching = false; int touchX = 0, touchY = 0;
    InputMsg lastSent{}; uint32_t lastSendTick = 0;

    int myPlayer = 0, active = 0, numPlayers = 0;
    bool connected = false, running = true, fullscreen = false;
    std::vector<uint8_t> raw;

    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
                SDL_Scancode sc = e.key.keysym.scancode;
                // hotkeys: 1-4 = use move 1-4, 5-9 and 0 = item slot 1-6
                if (e.type == SDL_KEYDOWN && !e.key.repeat && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
                {
                    hotkey = (uint8_t)(sc - SDL_SCANCODE_1 + 1); // SDL_SCANCODE_0 follows 9 -> 10
                    hotkeySeq++;
                }
                // WASD and arrows share d-pad bits: track them separately so releasing one doesn't cancel the other
                int k = keyForScancode(sc);
                if (k >= 0)
                {
                    bool arrow = sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN;
                    uint16_t& mask = arrow ? arrowKeys : kbKeys;
                    if (e.type == SDL_KEYDOWN) mask |= 1 << k; else mask &= ~(1 << k);
                }
                break;
            }
            case SDL_CONTROLLERDEVICEADDED:
                tryOpenPad(e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad && e.cdevice.which == padId)
                {
                    SDL_GameControllerClose(pad); pad = nullptr; padId = -1; padKeys = 0;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            {
                if (e.cbutton.which != padId) break; // ignore other players' pads
                int k = keyForButton(e.cbutton.button);
                if (k >= 0)
                {
                    if (e.type == SDL_CONTROLLERBUTTONDOWN) padKeys |= 1 << k; else padKeys &= ~(1 << k);
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION:
            {
                bool down = (e.type == SDL_MOUSEMOTION) ? (e.motion.state & SDL_BUTTON_LMASK) != 0
                                                        : (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT);
                if (e.type == SDL_MOUSEBUTTONUP) down = false;
                int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                // logical coordinates (SDL_RenderSetLogicalSize): bottom screen at (4, 4+192)
                int bx = mx - 4, by = my - 4 - ScreenH;
                if (down && bx >= 0 && bx < ScreenW && by >= 0 && by < ScreenH)
                {
                    touching = true; touchX = bx; touchY = by;
                }
                else if (!down || e.type != SDL_MOUSEMOTION) touching = false;
                break;
            }
            }
        }

        // analog stick -> d-pad
        uint16_t stickKeys = 0;
        if (pad)
        {
            int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            const int dz = 16000;
            if (ax > dz) stickKeys |= 1 << kRight;
            if (ax < -dz) stickKeys |= 1 << kLeft;
            if (ay > dz) stickKeys |= 1 << kDown;
            if (ay < -dz) stickKeys |= 1 << kUp;
        }

        // network
        ENetEvent ev;
        while (enet_host_service(client, &ev, 0) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT)
            {
                HelloMsg h{}; h.type = MsgHello; h.version = ProtocolVersion;
                strncpy(h.name, name, sizeof(h.name) - 1);
                h.clientId = clientId;
                enet_peer_send(peer, ChanControl, enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE));
            }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                fprintf(stderr, "Verbindung zum Host getrennt\n");
                running = false;
            }
            else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                const uint8_t* d = ev.packet->data;
                size_t len = ev.packet->dataLength;
                if (len >= sizeof(WelcomeMsg) && d[0] == MsgWelcome)
                {
                    myPlayer = d[2]; connected = true;
                    printf("Verbunden als Spieler %d\n", myPlayer);
                }
                else if (len >= sizeof(RejectMsg) && d[0] == MsgReject)
                {
                    fprintf(stderr, d[1] == 1 ? "Host ist voll\n" : "Falsche Protokollversion\n");
                    running = false;
                }
                else if (len >= sizeof(FrameHeader) && d[0] == MsgFrame)
                {
                    FrameHeader h; memcpy(&h, d, sizeof(h));
                    raw.resize(h.rawSize);
                    size_t r = ZSTD_decompress(raw.data(), raw.size(), d + sizeof(h), len - sizeof(h));
                    if (!ZSTD_isError(r) && r == h.rawSize)
                    {
                        active = h.activePlayer; numPlayers = h.numPlayers;
                        size_t vbytes = 0;
                        if (h.flags & 1)
                        {
                            vbytes = ScreenPixels * 4 * 2;
                            SDL_UpdateTexture(tex, nullptr, raw.data(), ScreenW * 4);
                        }
                        if (h.audioFrames)
                        {
                            if (!audioDev || audioFreq != (int)h.audioFreq)
                            {
                                if (audioDev) SDL_CloseAudioDevice(audioDev);
                                SDL_AudioSpec want{}, have{};
                                want.freq = h.audioFreq; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
                                audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
                                audioFreq = h.audioFreq;
                                if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
                            }
                            if (audioDev)
                            {
                                // keep latency low: drop audio if more than ~80ms is queued
                                if (SDL_GetQueuedAudioSize(audioDev) > (Uint32)(audioFreq * 4 * 80 / 1000))
                                    SDL_ClearQueuedAudio(audioDev);
                                SDL_QueueAudio(audioDev, raw.data() + vbytes, h.audioFrames * 4);
                            }
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
            }
        }

        // send input (on change, and as keepalive every 100ms)
        if (connected)
        {
            uint16_t pressed = kbKeys | arrowKeys | padKeys | stickKeys;
            InputMsg in{};
            in.type = MsgInput;
            in.keys = (uint16_t)(~pressed & 0xFFF);
            in.touching = touching ? 1 : 0;
            in.touchX = (uint8_t)touchX; in.touchY = (uint8_t)touchY;
            in.hotkey = hotkey; in.hotkeySeq = hotkeySeq;
            uint32_t now = SDL_GetTicks();
            if (memcmp(&in, &lastSent, sizeof(in)) != 0 || now - lastSendTick > 100)
            {
                enet_peer_send(peer, ChanInput, enet_packet_create(&in, sizeof(in), ENET_PACKET_FLAG_RELIABLE));
                enet_host_flush(client);
                lastSent = in; lastSendTick = now;
            }
        }

        // title / turn indicator
        char title[128];
        bool myTurn = connected && active == myPlayer;
        if (!connected) snprintf(title, sizeof(title), "PMD Coop - verbinde mit %s...", hostName);
        else if (active == 0) snprintf(title, sizeof(title), "PMD Coop - Spieler %d (ROM ohne Coop-Patch?)", myPlayer);
        else if (myTurn) snprintf(title, sizeof(title), "PMD Coop - Spieler %d - DEIN ZUG!", myPlayer);
        else snprintf(title, sizeof(title), "PMD Coop - Spieler %d - Spieler %d ist am Zug (%d Spieler)", myPlayer, active, numPlayers);
        SDL_SetWindowTitle(win, title);

        // draw: colored frame shows whose turn it is
        if (myTurn) SDL_SetRenderDrawColor(ren, 255, 192, 32, 255);
        else SDL_SetRenderDrawColor(ren, 24, 24, 32, 255);
        SDL_RenderClear(ren);
        SDL_Rect dst{4, 4, ScreenW, ScreenH * 2};
        SDL_RenderCopy(ren, tex, nullptr, &dst);
        SDL_RenderPresent(ren);
    }

    if (peer && connected) { enet_peer_disconnect(peer, 0); enet_host_flush(client); }
    enet_host_destroy(client);
    enet_deinitialize();
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    SDL_Quit();
    return 0;
}
