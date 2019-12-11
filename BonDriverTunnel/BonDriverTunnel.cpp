#define _CRT_STDIO_ISO_WIDE_SPECIFIERS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <memory>
#include "IBonDriver3.h"
#include "resource.h"

namespace
{
const int TSDATASIZE = 48128;
const int BDP_MAX_WORKERS = 32;
const int BDP_GET_TS_INTERVAL_MSEC = 100;

class CBlockLock
{
public:
    CBlockLock(CRITICAL_SECTION *cs) : m_cs(cs) { EnterCriticalSection(m_cs); }
    ~CBlockLock() { LeaveCriticalSection(m_cs); }
private:
    CBlockLock(const CBlockLock&);
    CBlockLock &operator=(const CBlockLock&);
    CRITICAL_SECTION *m_cs;
};

DWORD Write(BYTE (&buf)[8 + TSDATASIZE * 10], const void *ret, const void *param = nullptr, DWORD paramSize = 0)
{
    if (paramSize <= 4 + TSDATASIZE * 10) {
        memcpy(buf, ret, 4);
        if (paramSize != 0) {
            memcpy(buf + 4, param, paramSize);
        }
        return 4 + paramSize;
    }
    return 0;
}

enum BDP_STATE {
    BDP_ST_IDLE, BDP_ST_INIT_READING, BDP_ST_INIT_READ, BDP_ST_INIT_WRITING, BDP_ST_READING,
    BDP_ST_READ, BDP_ST_WRITING, BDP_ST_CLOSE_WAIT, BDP_ST_INIT_CLOSING, BDP_ST_CLOSING
};

struct BDP_WORKER_CONTEXT {
    CRITICAL_SECTION *cs;
    bool *stopFlag;
    HANDLE hThread;
    std::unique_ptr<BDP_WORKER_CONTEXT> *workerList;
    BDP_WORKER_CONTEXT *wantToConnect;
    EXECUTION_STATE executionState;
    DWORD ringBufNum;
    DWORD sessionTimeoutMsec;
    DWORD sessionID;
    BYTE wbuf[8 + TSDATASIZE * 10];
    BDP_STATE state;
    // state>=BDP_ST_INIT_READINGかつstate<=BDP_ST_WRITINGのとき有効
    SOCKET sock;
    // state>=BDP_ST_INIT_WRITINGかつstate!=BDP_ST_INIT_CLOSINGのとき有効
    WCHAR origin[256];
};

struct BDP_MAIN_WINDOW_CONTEXT {
    CRITICAL_SECTION cs;
    bool startFlag;
    bool stopFlag;
    HANDLE hListenThread;
    HWND hwnd;
    UINT msgTaskbarCreated;
    WCHAR tip[128];
};

struct BDP_RING_BUFFER {
    DWORD bufCount;
    BYTE buf[TSDATASIZE];
};

void CloseBonDriver(HMODULE *hLib, IBonDriver **bon, IBonDriver2 **bon2, IBonDriver3 **bon3)
{
    if (*bon) {
        (*bon)->Release();
        *bon = nullptr;
        *bon2 = nullptr;
        *bon3 = nullptr;
    }
    if (*hLib) {
        FreeLibrary(*hLib);
        *hLib = nullptr;
    }
}

void CloseSocket(SOCKET sock)
{
    unsigned long x = 0;
    ioctlsocket(sock, FIONBIO, &x);
    closesocket(sock);
}

UINT WINAPI WorkerThread(void *p)
{
    BDP_WORKER_CONTEXT *ctx = static_cast<BDP_WORKER_CONTEXT*>(p);
    char initBuf[524] = {};
    char rbuf[16] = {};
    char rbufLast[16] = {};
    int initBufCount = 0;
    int rbufCount = 0;
    DWORD wbufSize = 0;
    DWORD wbufCount = 0;
    HMODULE hLib = nullptr;
    IBonDriver *bon = nullptr;
    IBonDriver2 *bon2 = nullptr;
    IBonDriver3 *bon3 = nullptr;
    CBonStructAdapter bonAdapter;
    CBonStruct2Adapter bon2Adapter;
    CBonStruct3Adapter bon3Adapter;
    std::unique_ptr<BDP_RING_BUFFER[]> ringBuf;
    DWORD ringBufRear = 0;
    DWORD ringBufFront = 0;
    DWORD getTsTick = 0;
    DWORD timeoutTick = GetTickCount();
    bool released = false;

    // BonDriverがCOMを利用するかもしれないため
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // スリープ抑止
    SetThreadExecutionState(ctx->executionState);

    for (;;) {
        if (bon && ringBuf && GetTickCount() - getTsTick >= BDP_GET_TS_INTERVAL_MSEC) {
            // 定期的にストリームを取得しておく
            BYTE *buf;
            DWORD bufSize;
            DWORD remain;
            while (bon->GetTsStream(&buf, &bufSize, &remain) && buf && bufSize != 0) {
                while (bufSize != 0) {
                    ringBuf[ringBufRear].bufCount = std::min<DWORD>(bufSize, sizeof(ringBuf[0].buf));
                    memcpy(ringBuf[ringBufRear].buf, buf, ringBuf[ringBufRear].bufCount);
                    buf += ringBuf[ringBufRear].bufCount;
                    bufSize -= ringBuf[ringBufRear].bufCount;
                    ringBufRear = (ringBufRear + 1) % ctx->ringBufNum;
                }
            }
            getTsTick = GetTickCount();
        }

        BDP_STATE state;
        bool stopFlag;
        SOCKET swapSocket = INVALID_SOCKET;
        {
            CBlockLock lock(ctx->cs);
            state = ctx->state;
            stopFlag = *ctx->stopFlag;
            if (!stopFlag && ctx->wantToConnect) {
                swapSocket = ctx->wantToConnect->sock;
                ctx->wantToConnect->state = BDP_ST_INIT_CLOSING;
            }
        }
        if (swapSocket != INVALID_SOCKET) {
            // 再接続された
            if (state <= BDP_ST_WRITING) {
                CloseSocket(ctx->sock);
            }
            ctx->sock = swapSocket;
            DWORD n = ctx->sessionID | (bon3 ? 3 : bon2 ? 2 : bon ? 1 : 0);
            initBufCount = 4;
            memcpy(initBuf, &n, initBufCount);
            CBlockLock lock(ctx->cs);
            ctx->wantToConnect = nullptr;
            ctx->state = BDP_ST_INIT_WRITING;
        }
        else if (stopFlag || state == BDP_ST_INIT_CLOSING || state == BDP_ST_CLOSING) {
            // 終了しようとしている
            CloseBonDriver(&hLib, &bon, &bon2, &bon3);
            if (state <= BDP_ST_WRITING) {
                CloseSocket(ctx->sock);
            }
            CBlockLock lock(ctx->cs);
            ctx->state = BDP_ST_IDLE;
            break;
        }
        else if (state == BDP_ST_INIT_READ) {
            // 最初のコマンドを受信した
            memcpy(ctx->origin, initBuf + 12, 512);
            ctx->origin[255] = L'\0';
            // クロスプロトコル対策のため複雑な名前にしている
            if (!memcmp(initBuf, "BD\0\0Conn", 8)) {
                // 再接続
                bool connecting = false;
                {
                    CBlockLock lock(ctx->cs);
                    for (int i = 0; ctx->workerList[i]; ++i) {
                        if (ctx->workerList[i]->state >= BDP_ST_INIT_WRITING && ctx->workerList[i]->state != BDP_ST_INIT_CLOSING &&
                            !_wcsicmp(ctx->workerList[i]->origin, ctx->origin)) {
                            // セッションIDが同じで、終了しようとしていなければ再接続できる
                            if (ctx->workerList[i]->state <= BDP_ST_CLOSE_WAIT && !ctx->workerList[i]->wantToConnect &&
                                !memcmp(&ctx->workerList[i]->sessionID, initBuf + 8, 4)) {
                                ctx->workerList[i]->wantToConnect = ctx;
                                connecting = true;
                            }
                            break;
                        }
                    }
                }
                if (connecting) {
                    for (;;) {
                        Sleep(1);
                        CBlockLock lock(ctx->cs);
                        if (*ctx->stopFlag || ctx->state == BDP_ST_INIT_CLOSING) {
                            break;
                        }
                    }
                }
                else {
                    CloseSocket(ctx->sock);
                    CBlockLock lock(ctx->cs);
                    ctx->state = BDP_ST_INIT_CLOSING;
                }
            }
            else if (!memcmp(initBuf, "BD\0\0Crea", 8)) {
                // 新しい接続
                int otherActiveOrClosing = 0;
                for (;;) {
                    {
                        CBlockLock lock(ctx->cs);
                        otherActiveOrClosing = 0;
                        for (int i = 0; ctx->workerList[i]; ++i) {
                            if (ctx->workerList[i]->state >= BDP_ST_INIT_WRITING && ctx->workerList[i]->state != BDP_ST_INIT_CLOSING &&
                                !_wcsicmp(ctx->workerList[i]->origin, ctx->origin)) {
                                if (ctx->workerList[i]->state == BDP_ST_CLOSE_WAIT && !ctx->workerList[i]->wantToConnect) {
                                    ctx->workerList[i]->state = BDP_ST_CLOSING;
                                }
                                otherActiveOrClosing = ctx->workerList[i]->state == BDP_ST_CLOSING ? 2 : 1;
                                break;
                            }
                        }
                        if (otherActiveOrClosing != 2) {
                            ctx->state = otherActiveOrClosing ? BDP_ST_INIT_CLOSING : BDP_ST_INIT_WRITING;
                            break;
                        }
                    }
                    Sleep(1);
                }
                if (otherActiveOrClosing) {
                    CloseSocket(ctx->sock);
                }
                else {
                    WCHAR libPath[MAX_PATH + 256 + 64];
                    DWORD len = GetModuleFileName(nullptr, libPath, MAX_PATH);
                    if (len && len < MAX_PATH && wcsrchr(libPath, L'\\') && !ctx->origin[wcscspn(ctx->origin, L"\\/:*?\"<>|")]) {
                        *wcsrchr(libPath, L'\\') = L'\0';
                        wcscat_s(libPath, L"\\BonDriver_");
                        wcscat_s(libPath, ctx->origin);
                        wcscat_s(libPath, L".dll");
                        hLib = LoadLibrary(libPath);
                        if (hLib) {
                            const STRUCT_IBONDRIVER *(*funcCreateBonStruct)() = reinterpret_cast<const STRUCT_IBONDRIVER*(*)()>(GetProcAddress(hLib, "CreateBonStruct"));
                            if (funcCreateBonStruct) {
                                // 特定コンパイラに依存しないI/Fを使う
                                const STRUCT_IBONDRIVER *st = funcCreateBonStruct();
                                if (st) {
                                    if (bon3Adapter.Adapt(*st)) {
                                        bon = bon2 = bon3 = &bon3Adapter;
                                    }
                                    else if (bon2Adapter.Adapt(*st)) {
                                        bon = bon2 = &bon2Adapter;
                                    }
                                    else {
                                        bonAdapter.Adapt(*st);
                                        bon = &bonAdapter;
                                    }
                                }
                            }
#ifdef _MSC_VER
                            else {
                                IBonDriver *(*funcCreateBonDriver)() = reinterpret_cast<IBonDriver*(*)()>(GetProcAddress(hLib, "CreateBonDriver"));
                                if (funcCreateBonDriver) {
                                    bon = funcCreateBonDriver();
                                    if (bon) {
                                        bon2 = dynamic_cast<IBonDriver2*>(bon);
                                        if (bon2) {
                                            bon3 = dynamic_cast<IBonDriver3*>(bon2);
                                        }
                                    }
                                }
                            }
#endif
                        }
                    }
                    DWORD n = ctx->sessionID | (bon3 ? 3 : bon2 ? 2 : bon ? 1 : 0);
                    initBufCount = 4;
                    memcpy(initBuf, &n, initBufCount);
                }
            }
            else {
                CloseSocket(ctx->sock);
                CBlockLock lock(ctx->cs);
                ctx->state = BDP_ST_INIT_CLOSING;
            }
        }
        else if (state == BDP_ST_READ) {
            // コマンドを受信した
            // 再接続のため、前回のコマンドが繰り返されたときは前回の応答をそのまま返す(4-7バイト目にはシーケンス番号が格納されている)
            if (memcmp(rbufLast, rbuf, 16)) {
                memcpy(rbufLast, rbuf, 16);
                union {
                        BOOL b;
                        DWORD n;
                } param1, param2;
                char cmd[5] = {};
                memcpy(cmd, rbuf, 4);
                memcpy(&param1, rbuf + 8, 4);
                memcpy(&param2, rbuf + 12, 4);
                wbufSize = 0;
                if (!strcmp(cmd, "GTot")) {
                    if (bon3) {
                        DWORD n = bon3->GetTotalDeviceNum();
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "GAct")) {
                    if (bon3) {
                        DWORD n = bon3->GetActiveDeviceNum();
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "SLnb")) {
                    if (bon3) {
                        BOOL b = bon3->SetLnbPower(param1.b);
                        wbufSize = Write(ctx->wbuf, &b);
                    }
                }
                else if (!strcmp(cmd, "GTun")) {
                    if (bon2) {
                        LPCWSTR tunerName = bon2->GetTunerName();
                        DWORD n = static_cast<DWORD>(tunerName ? wcslen(tunerName) + 1 : 0);
                        n = std::min<DWORD>(n, 255);
                        wbufSize = Write(ctx->wbuf, &n, tunerName, n * sizeof(WCHAR));
                    }
                }
                else if (!strcmp(cmd, "ITun")) {
                    if (bon2) {
                        BOOL b = bon2->IsTunerOpening();
                        wbufSize = Write(ctx->wbuf, &b);
                    }
                }
                else if (!strcmp(cmd, "ETun")) {
                    if (bon2) {
                        LPCWSTR tuningSpace = bon2->EnumTuningSpace(param1.n);
                        DWORD n = static_cast<DWORD>(tuningSpace ? wcslen(tuningSpace) + 1 : 0);
                        n = std::min<DWORD>(n, 255);
                        wbufSize = Write(ctx->wbuf, &n, tuningSpace, n * sizeof(WCHAR));
                    }
                }
                else if (!strcmp(cmd, "ECha")) {
                    if (bon2) {
                        LPCWSTR channelName = bon2->EnumChannelName(param1.n, param2.n);
                        DWORD n = static_cast<DWORD>(channelName ? wcslen(channelName) + 1 : 0);
                        n = std::min<DWORD>(n, 255);
                        wbufSize = Write(ctx->wbuf, &n, channelName, n * sizeof(WCHAR));
                    }
                }
                else if (!strcmp(cmd, "SCh2")) {
                    if (bon2) {
                        BOOL b = bon2->SetChannel(param1.n, param2.n);
                        wbufSize = Write(ctx->wbuf, &b);
                    }
                }
                else if (!strcmp(cmd, "GCSp")) {
                    if (bon2) {
                        DWORD n = bon2->GetCurSpace();
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "GCCh")) {
                    if (bon2) {
                        DWORD n = bon2->GetCurChannel();
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "Open")) {
                    if (bon) {
                        BOOL b = bon->OpenTuner();
                        wbufSize = Write(ctx->wbuf, &b);
                    }
                }
                else if (!strcmp(cmd, "Clos")) {
                    if (bon) {
                        bon->CloseTuner();
                        DWORD n = 0;
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "SCha")) {
                    if (bon) {
                        BOOL b = bon->SetChannel(static_cast<BYTE>(param1.n));
                        wbufSize = Write(ctx->wbuf, &b);
                    }
                }
                else if (!strcmp(cmd, "GSig")) {
                    if (bon) {
                        float f = bon->GetSignalLevel();
                        wbufSize = Write(ctx->wbuf, &f);
                    }
                }
                else if (!strcmp(cmd, "GRea")) {
                    if (bon) {
                        DWORD n = (ctx->ringBufNum + ringBufRear - ringBufFront) % ctx->ringBufNum / 10;
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
                else if (!strcmp(cmd, "GTsS")) {
                    if (bon) {
                        if (!ringBuf) {
                            ringBuf.reset(new BDP_RING_BUFFER[ctx->ringBufNum]);
                        }
                        if ((ctx->ringBufNum + ringBufRear - ringBufFront) % ctx->ringBufNum >= 10) {
                            wbufSize = 4;
                            for (int i = 0; i < 10; ++i) {
                                memcpy(ctx->wbuf + 4 + wbufSize, ringBuf[ringBufFront].buf, ringBuf[ringBufFront].bufCount);
                                wbufSize += ringBuf[ringBufFront].bufCount;
                                ringBufFront = (ringBufFront + 1) % ctx->ringBufNum;
                            }
                            memcpy(ctx->wbuf, &wbufSize, 4);
                            DWORD remain = (ctx->ringBufNum + ringBufRear - ringBufFront) % ctx->ringBufNum / 10;
                            memcpy(ctx->wbuf + 4, &remain, 4);
                            wbufSize += 4;
                        }
                        else {
                            DWORD n = 4;
                            DWORD remain = 0;
                            wbufSize = Write(ctx->wbuf, &n, &remain, 4);
                        }
                    }
                }
                else if (!strcmp(cmd, "Purg")) {
                    if (bon) {
                        bon->PurgeTsStream();
                        ringBufFront = ringBufRear;
                        DWORD n = 0;
                        wbufSize = Write(ctx->wbuf, &n);
                    }
                }
            }
            if (wbufSize == 0) {
                released = true;
                DWORD n = 0;
                wbufSize = Write(ctx->wbuf, &n);
            }
            wbufCount = 0;
            CBlockLock lock(ctx->cs);
            ctx->state = BDP_ST_WRITING;
        }
        else if (state == BDP_ST_INIT_READING || state == BDP_ST_READING) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ctx->sock, &rfds);
            timeval tv = {0, BDP_GET_TS_INTERVAL_MSEC * 1000};
            if (select(0, &rfds, nullptr, nullptr, &tv) < 0) {
                // 失敗時の高負荷を防ぐため
                Sleep(1);
            }
            if (state == BDP_ST_READING) {
                int n = recv(ctx->sock, rbuf + rbufCount, 16 - rbufCount, 0);
                if (n > 0) {
                    rbufCount += n;
                    if (rbufCount >= 16) {
                        CBlockLock lock(ctx->cs);
                        ctx->state = BDP_ST_READ;
                    }
                    timeoutTick = GetTickCount();
                }
                else if (n >= 0 || WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount() - timeoutTick > ctx->sessionTimeoutMsec) {
                    bool closing = false;
                    {
                        CBlockLock lock(ctx->cs);
                        if (!ctx->wantToConnect) {
                            ctx->state = released ? BDP_ST_CLOSING : BDP_ST_CLOSE_WAIT;
                            closing = true;
                        }
                    }
                    if (closing) {
                        CloseSocket(ctx->sock);
                    }
                }
            }
            else {
                int n = recv(ctx->sock, initBuf + initBufCount, 524 - initBufCount, 0);
                if (n > 0) {
                    initBufCount += n;
                    if (initBufCount >= 524) {
                        CBlockLock lock(ctx->cs);
                        ctx->state = BDP_ST_INIT_READ;
                    }
                    timeoutTick = GetTickCount();
                }
                else if (n >= 0 || WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount() - timeoutTick > ctx->sessionTimeoutMsec) {
                    CloseSocket(ctx->sock);
                    CBlockLock lock(ctx->cs);
                    ctx->state = BDP_ST_INIT_CLOSING;
                }
            }
        }
        else if (state == BDP_ST_INIT_WRITING || state == BDP_ST_WRITING) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(ctx->sock, &wfds);
            timeval tv = {0, BDP_GET_TS_INTERVAL_MSEC * 1000};
            if (select(0, nullptr, &wfds, nullptr, &tv) < 0) {
                // 失敗時の高負荷を防ぐため
                Sleep(1);
            }
            bool failed = false;
            if (state == BDP_ST_WRITING) {
                int n = send(ctx->sock, reinterpret_cast<char*>(ctx->wbuf) + wbufCount, wbufSize - wbufCount, 0);
                if (n > 0) {
                    wbufCount += n;
                    if (wbufCount >= wbufSize) {
                        rbufCount = 0;
                        CBlockLock lock(ctx->cs);
                        ctx->state = BDP_ST_READING;
                    }
                    timeoutTick = GetTickCount();
                }
                else if (n >= 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                    failed = true;
                }
            }
            else {
                int n = send(ctx->sock, initBuf, initBufCount, 0);
                if (n > 0) {
                    initBufCount -= n;
                    if (initBufCount <= 0) {
                        rbufCount = 0;
                        CBlockLock lock(ctx->cs);
                        ctx->state = BDP_ST_READING;
                    }
                    else {
                        memmove(initBuf, initBuf + n, initBufCount);
                    }
                    timeoutTick = GetTickCount();
                }
                else if (n >= 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                    failed = true;
                }
            }
            if (failed || GetTickCount() - timeoutTick > ctx->sessionTimeoutMsec) {
                bool closing = false;
                {
                    CBlockLock lock(ctx->cs);
                    if (!ctx->wantToConnect) {
                        ctx->state = released ? BDP_ST_CLOSING : BDP_ST_CLOSE_WAIT;
                        closing = true;
                    }
                }
                if (closing) {
                    CloseSocket(ctx->sock);
                }
            }
        }
        else {
            // BDP_ST_CLOSE_WAIT
            Sleep(BDP_GET_TS_INTERVAL_MSEC);
            if (GetTickCount() - timeoutTick > ctx->sessionTimeoutMsec) {
                CBlockLock lock(ctx->cs);
                if (!ctx->wantToConnect) {
                    ctx->state = BDP_ST_CLOSING;
                }
            }
        }
    }

    SetThreadExecutionState(ES_CONTINUOUS);
    CoUninitialize();
    return 0;
}

bool TestAcl(const sockaddr *addr, char *acl)
{
    bool ret = false;
    for (;;) {
        if (acl[0] != '+' && acl[0] != '-') {
            break;
        }
        char *sep = strchr(acl, ',');
        char *msep = strchr(acl, '/');
        if (msep && sep && msep > sep) {
            msep = nullptr;
        }
        int mm = static_cast<int>(msep ? strtol(msep + 1, nullptr, 10) : addr->sa_family == AF_INET6 ? 128 : 32);
        if (mm < 0 || mm > (addr->sa_family == AF_INET6 ? 128 : 32)) {
            break;
        }

        // 一時的に区切り文字を潰す
        if (sep) {
            sep[0] = '\0';
        }
        if (msep) {
            msep[0] = '\0';
        }
        bool err = true;
        addrinfo hints = {};
        hints.ai_flags = AI_NUMERICHOST;
        hints.ai_family = addr->sa_family;
        addrinfo *result;
        if (getaddrinfo(acl + 1, nullptr, &hints, &result) == 0) {
            if (result->ai_family == AF_INET6) {
                int i = 0;
                for (; i < 16; ++i) {
                    BYTE mask = static_cast<BYTE>(8 * i + 8 < mm ? 0xFF : 8 * i > mm ? 0 : 0xFF << (8 * i + 8 - mm));
                    if ((reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr.s6_addr[i] & mask) !=
                        (reinterpret_cast<const sockaddr_in6*>(result->ai_addr)->sin6_addr.s6_addr[i] & mask)) {
                        break;
                    }
                }
                if (i == 16) {
                    ret = acl[0] == '+';
                }
            }
            else {
                DWORD mask = mm == 0 ? 0 : 0xFFFFFFFF << (32 - mm);
                if ((ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr) & mask) ==
                    (ntohl(reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr) & mask)) {
                    ret = acl[0] == '+';
                }
            }
            freeaddrinfo(result);
            err = false;
        }
        // 区切り文字を戻す
        if (sep) {
            sep[0] = ',';
        }
        if (msep) {
            msep[0] = '/';
        }
        if (err) {
            break;
        }
        if (!sep) {
            return ret;
        }
        acl = sep + 1;
    }
    // 不正
    return false;
}

UINT WINAPI ListenThread(void *p)
{
    BDP_MAIN_WINDOW_CONTEXT &ctx = *static_cast<BDP_MAIN_WINDOW_CONTEXT*>(p);
    WCHAR portW[8];
    char port[8] = {};
    bool ipv6 = false;
    char acl[1024] = {};
    bool setExecutionState = false;
    int ringBufNum = 0;
    int sessionTimeout = 0;
    {
        WCHAR iniPath[MAX_PATH + 4];
        DWORD len = GetModuleFileName(nullptr, iniPath, MAX_PATH);
        if (len && len < MAX_PATH && wcsrchr(iniPath, L'\\')) {
            LPWSTR ext = wcsrchr(wcsrchr(iniPath, L'\\'), L'.');
            if (ext) {
                ext[0] = L'\0';
            }
            wcscat_s(iniPath, L".ini");
            GetPrivateProfileString(L"OPTION", L"PORT", L"1193", portW, 8, iniPath);
            for (int i = 0; portW[i]; ++i) {
                port[i] = static_cast<char>(portW[i]);
            }
            ipv6 = GetPrivateProfileInt(L"OPTION", L"IPV6", 0, iniPath) != 0;
            WCHAR aclW[1024];
            GetPrivateProfileString(L"OPTION", L"ACCESS_CONTROL_LIST", ipv6 ? L"+::1,+fe80::/64" : L"+127.0.0.1,+192.168.0.0/16", aclW, 1024, iniPath);
            if (wcslen(aclW) < 1023) {
                for (int i = 0; aclW[i]; ++i) {
                    acl[i] = static_cast<char>(aclW[i]);
                }
            }
            setExecutionState = GetPrivateProfileInt(L"OPTION", L"SET_EXECUTION_STATE", 1, iniPath) != 0;
            ringBufNum = GetPrivateProfileInt(L"OPTION", L"RING_BUF_NUM", 200, iniPath);
            ringBufNum = std::min(std::max(ringBufNum, 50), 5000);
            sessionTimeout = GetPrivateProfileInt(L"OPTION", L"SESSION_TIMEOUT", 180, iniPath);
            sessionTimeout = std::min(std::max(sessionTimeout, 5), 3600);
        }
    }

    SOCKET listenSock = INVALID_SOCKET;
    if (port[0]) {
        WSAData wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        addrinfo hints = {};
        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = ipv6 ? AF_INET6 : AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo *result;
        if (getaddrinfo(nullptr, port, &hints, &result) == 0) {
            listenSock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (listenSock != INVALID_SOCKET) {
                BOOL b = TRUE;
                setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&b), sizeof(b));
                if (ipv6) {
                    // デュアルスタックにはしない
                    setsockopt(listenSock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&b), sizeof(b));
                }
                // 非ブロッキングモード化。継承によりaccept()で得たソケットも同様
                unsigned long x = 1;
                ioctlsocket(listenSock, FIONBIO, &x);
                if (bind(listenSock, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0 || listen(listenSock, SOMAXCONN) != 0) {
                    CloseSocket(listenSock);
                    listenSock = INVALID_SOCKET;
                }
            }
            freeaddrinfo(result);
        }
    }
    {
        CBlockLock lock(&ctx.cs);
        ctx.startFlag = true;
        if (listenSock == INVALID_SOCKET) {
            ctx.stopFlag = true;
            WSACleanup();
            return 0;
        }
    }

    // 起動ごとにユニークであることが概ね期待できればOK(もっとランダムが望ましい)
    DWORD sessionCount = GetTickCount() >> 4;
    std::unique_ptr<BDP_WORKER_CONTEXT> workerList[BDP_MAX_WORKERS + 1];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenSock, &rfds);
        timeval tv = {1, 0};
        if (select(0, nullptr, &rfds, nullptr, &tv) < 0) {
            // 失敗時の高負荷を防ぐため
            Sleep(1);
        }
        for (;;) {
            {
                CBlockLock lock(&ctx.cs);
                // スレッドを回収
                WCHAR originLog[96] = {};
                int n = 0;
                for (int i = 0; workerList[i]; ++i) {
                    if (workerList[i]->state == BDP_ST_IDLE && WaitForSingleObject(workerList[i]->hThread, 0) != WAIT_TIMEOUT) {
                        CloseHandle(workerList[i]->hThread);
                        workerList[i].reset();
                    }
                    else {
                        if (workerList[i]->state >= BDP_ST_INIT_WRITING && workerList[i]->state != BDP_ST_INIT_CLOSING) {
                            if (wcslen(originLog) + 1 + wcslen(workerList[i]->origin) < 96) {
                                wcscat_s(originLog, L",");
                                wcscat_s(originLog, workerList[i]->origin);
                            }
                        }
                        workerList[n++].swap(workerList[i]);
                    }
                }
                if (!ctx.stopFlag || n == 0) {
                    WCHAR tip[128];
                    if (originLog[0]) {
                        swprintf_s(tip, L"%d thread%ls (%ls) p%ls", n, (n == 1 ? L"" : L"s"), originLog + 1, portW);
                    }
                    else {
                        swprintf_s(tip, L"%d thread%ls p%ls", n, (n == 1 ? L"" : L"s"), portW);
                    }
                    if (wcscmp(ctx.tip, tip)) {
                        wcscpy_s(ctx.tip, tip);
                        PostMessage(ctx.hwnd, WM_APP, 0, 0);
                    }
                    break;
                }
            }
            Sleep(1);
        }
        {
            CBlockLock lock(&ctx.cs);
            if (ctx.stopFlag && !workerList[0]) {
                break;
            }
        }

        sockaddr_storage client;
        int clientLen = sizeof(client);
        SOCKET sock = accept(listenSock, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (sock != INVALID_SOCKET &&
            ((ipv6 && client.ss_family == AF_INET6) || (!ipv6 && client.ss_family == AF_INET)) &&
            TestAcl(reinterpret_cast<sockaddr*>(&client), acl)) {
            CBlockLock lock(&ctx.cs);
            for (int i = 0; i < BDP_MAX_WORKERS; ++i) {
                if (!workerList[i]) {
                    workerList[i].reset(new BDP_WORKER_CONTEXT);
                    workerList[i]->cs = &ctx.cs;
                    workerList[i]->stopFlag = &ctx.stopFlag;
                    workerList[i]->workerList = workerList;
                    workerList[i]->wantToConnect = nullptr;
                    workerList[i]->executionState = setExecutionState ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED : ES_CONTINUOUS;
                    workerList[i]->ringBufNum = ringBufNum;
                    workerList[i]->sessionTimeoutMsec = sessionTimeout * 1000;
                    workerList[i]->sessionID = (++sessionCount) << 4;
                    workerList[i]->state = BDP_ST_INIT_READING;
                    workerList[i]->sock = sock;
                    workerList[i]->hThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, WorkerThread, workerList[i].get(), 0, nullptr));
                    if (workerList[i]->hThread) {
                        sock = INVALID_SOCKET;
                    }
                    else {
                        workerList[i].reset();
                    }
                    break;
                }
            }
        }
        if (sock != INVALID_SOCKET) {
            CloseSocket(sock);
        }
    }

    CloseSocket(listenSock);
    WSACleanup();
    return 0;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    BDP_MAIN_WINDOW_CONTEXT *ctx = reinterpret_cast<BDP_MAIN_WINDOW_CONTEXT*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (uMsg != WM_CREATE && !ctx) {
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    switch (uMsg) {
    case WM_CREATE:
        ctx = static_cast<BDP_MAIN_WINDOW_CONTEXT*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
        ctx->hwnd = hwnd;
        ctx->msgTaskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
        ctx->hListenThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ListenThread, ctx, 0, nullptr));
        if (ctx->hListenThread) {
            for (;;) {
                Sleep(1);
                CBlockLock lock(&ctx->cs);
                if (ctx->startFlag) {
                    if (!ctx->stopFlag) {
                        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
                        return 0;
                    }
                    break;
                }
            }
            WaitForSingleObject(ctx->hListenThread, INFINITE);
            CloseHandle(ctx->hListenThread);
        }
        return -1;
    case WM_DESTROY:
        {
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            {
                CBlockLock lock(&ctx->cs);
                ctx->stopFlag = true;
            }
            WaitForSingleObject(ctx->hListenThread, INFINITE);
            CloseHandle(ctx->hListenThread);
            NOTIFYICONDATA nid = {};
            nid.cbSize = NOTIFYICONDATA_V2_SIZE;
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIcon(NIM_DELETE, &nid);
            PostQuitMessage(0);
        }
        return 0;
    case WM_TIMER:
        if (wParam != 1) {
            break;
        }
        KillTimer(hwnd, 1);
        // FALL THROUGH!
    case WM_APP:
        {
            NOTIFYICONDATA nid = {};
            nid.cbSize = NOTIFYICONDATA_V2_SIZE;
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_APP + 1;
            {
                CBlockLock lock(&ctx->cs);
                wcsncpy_s(nid.szTip, ctx->tip, _TRUNCATE);
            }
            int iconID = nid.szTip[0] == L'0' ? IDI_ICON1 : IDI_ICON2;
            if (LoadIconMetric(GetModuleHandle(nullptr), MAKEINTRESOURCE(iconID), LIM_SMALL, &nid.hIcon) == S_OK) {
                nid.uFlags |= NIF_ICON;
            }
            if (!Shell_NotifyIcon(NIM_MODIFY, &nid) && !Shell_NotifyIcon(NIM_ADD, &nid)) {
                SetTimer(hwnd, 1, 5000, nullptr);
            }
            if (nid.uFlags & NIF_ICON) {
                DestroyIcon(nid.hIcon);
            }
        }
        return 0;
    case WM_APP + 1:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            HMENU hMenu = LoadMenu(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_MENU1));
            if (hMenu) {
                POINT point;
                GetCursorPos(&point);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(GetSubMenu(hMenu, 0), 0, point.x, point.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
            }
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BUTTON_CLOSE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    default:
        if (uMsg == ctx->msgTaskbarCreated) {
            PostMessage(hwnd, WM_APP, 0, 0);
        }
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
}

#ifdef __MINGW32__
__declspec(dllexport) // ASLRを無効にしないため(CVE-2018-5392)
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
#endif
{
    static_cast<void>(hInstance);
    static_cast<void>(hPrevInstance);
    static_cast<void>(lpCmdLine);
    static_cast<void>(nCmdShow);
    SetDllDirectory(L"");

    // BonDriverがCOMを利用するかもしれないため
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 非表示のメインウィンドウを作成
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"BonDriverTunnel";
    if (RegisterClassEx(&wc) != 0) {
        BDP_MAIN_WINDOW_CONTEXT ctx = {};
        InitializeCriticalSection(&ctx.cs);
        if (CreateWindowEx(0, L"BonDriverTunnel", L"BonDriverTunnel", 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, &ctx)) {
            MSG msg;
            while (GetMessage(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        DeleteCriticalSection(&ctx.cs);
    }

    CoUninitialize();
    return 0;
}
