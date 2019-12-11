#include "BonDriver_Tunnel.h"
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define closesocket(sock) close(sock)
#endif

namespace
{
IBonDriver *g_this;
#ifdef _WIN32
HINSTANCE g_hModule;
#endif

inline void SwapBytes4(void *y, const void *x)
{
#ifndef _WIN32
    int le = 1;
    if (!reinterpret_cast<BYTE*>(&le)[0]) {
        // Big-endian
        const BYTE *bx = static_cast<const BYTE*>(x);
        BYTE *by = static_cast<BYTE*>(y);
        by[3] = bx[0];
        by[2] = bx[1];
        by[1] = bx[2];
        by[0] = bx[3];
        return;
    }
#endif
    memcpy(y, x, 4);
}

inline void SwapBytes4(void *x)
{
#ifdef _WIN32
    static_cast<void>(x);
#else
    BYTE y[4];
    SwapBytes4(y, x);
    memcpy(x, y, 4);
#endif
}

#ifndef _WIN32
void GetIniString(const char *key, const char *def, char *ret, DWORD retSize, const char *fileName)
{
    FILE *fp = fopen(fileName, "re");
    if (fp) {
        char buf[1024];
        DWORD bufLen = 0;
        int c;
        do {
            c = fgetc(fp);
            if (c >= 0 && static_cast<char>(c) && static_cast<char>(c) != '\n') {
                buf[bufLen++] = static_cast<char>(c);
                if (bufLen >= sizeof(buf)) {
                    break;
                }
                continue;
            }
            buf[bufLen] = '\0';
            bufLen = 0;
            char *var = buf + strspn(buf, "\t\r ");
            while (*var && strchr("\t\r ", var[strlen(var) - 1])) {
                var[strlen(var) - 1] = '\0';
            }
            char *val = strchr(var, '=');
            if (val) {
                *val = '\0';
                while (*var && strchr("\t\r ", var[strlen(var) - 1])) {
                    var[strlen(var) - 1] = '\0';
                }
                if (!strcmp(key, var)) {
                    val += 1 + strspn(val + 1, "\t\r ");
                    if ((*val == '"' || *val == '\'') && val[1] && *val == val[strlen(val) - 1]) {
                        val[strlen(val) - 1] = '\0';
                        ++val;
                    }
                    if (strlen(val) < retSize) {
                        strcpy(ret, val);
                    }
                    fclose(fp);
                    return;
                }
            }
        }
        while (c >= 0 && static_cast<char>(c));
        fclose(fp);
    }
    *ret = '\0';
    if (strlen(def) < retSize) {
        strcpy(ret, def);
    }
}
#endif
}

CProxyClient3::CProxyClient3(const char *addr, const char *port,
#ifdef _WIN32
                             LPCWSTR origin,
#else
                             const char *origin,
#endif
                             int connectTimeout, int sendRecvTimeout)
    : m_connectTimeout(connectTimeout)
    , m_sendRecvTimeout(sendRecvTimeout)
    , m_sock(INVALID_SOCKET)
    , m_sessionID(0)
    , m_sequenceNum(0)
{
#ifdef _WIN32
    InitializeCriticalSection(&m_cs);
    strncpy_s(m_addr, addr, _TRUNCATE);
    strncpy_s(m_port, port, _TRUNCATE);
    wcsncpy_s(m_origin, origin, _TRUNCATE);
#else
    m_addr[0] = m_port[0] = m_origin[0] = '\0';
    if (strlen(addr) < sizeof(m_addr) && strlen(port) < sizeof(m_port) && strlen(origin) < sizeof(m_origin)) {
        strcpy(m_addr, addr);
        strcpy(m_port, port);
        strcpy(m_origin, origin);
    }
#endif
}

DWORD CProxyClient3::CreateBon()
{
    CBlockLock lock(&m_cs);
    DWORD n = Connect("BD\0\0Crea");
    m_sessionID = n & 0xFFFFFFF0;
    return n & 0xF;
}

const DWORD CProxyClient3::GetTotalDeviceNum()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GTot") ? n : 0;
}

const DWORD CProxyClient3::GetActiveDeviceNum()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GAct") ? n : 0;
}

const BOOL CProxyClient3::SetLnbPower(const BOOL bEnable)
{
    CBlockLock lock(&m_cs);
    BOOL b;
    return WriteAndRead4(&b, "SLnb", &bEnable) ? b : FALSE;
}

LPCWSTR CProxyClient3::GetTunerName()
{
    CBlockLock lock(&m_cs);
    if (WriteAndReadString(m_tunerName, "GTun")) {
        return m_tunerName;
    }
    return nullptr;
}

const BOOL CProxyClient3::IsTunerOpening()
{
    CBlockLock lock(&m_cs);
    BOOL b;
    return WriteAndRead4(&b, "ITun") ? b : FALSE;
}

LPCWSTR CProxyClient3::EnumTuningSpace(const DWORD dwSpace)
{
    CBlockLock lock(&m_cs);
    if (WriteAndReadString(m_tuningSpace, "ETun", &dwSpace)) {
        return m_tuningSpace;
    }
    return nullptr;
}

LPCWSTR CProxyClient3::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
    CBlockLock lock(&m_cs);
    if (WriteAndReadString(m_channelName, "ECha", &dwSpace, &dwChannel)) {
        return m_channelName;
    }
    return nullptr;
}

const BOOL CProxyClient3::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
    CBlockLock lock(&m_cs);
    BOOL b;
    return WriteAndRead4(&b, "SCh2", &dwSpace, &dwChannel) ? b : FALSE;
}

const DWORD CProxyClient3::GetCurSpace()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GCSp") ? n : 0xFFFFFFFF;
}

const DWORD CProxyClient3::GetCurChannel()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GCCh") ? n : 0xFFFFFFFF;
}

const BOOL CProxyClient3::OpenTuner()
{
    CBlockLock lock(&m_cs);
    BOOL b;
    return WriteAndRead4(&b, "Open") ? b : FALSE;
}

void CProxyClient3::CloseTuner()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    WriteAndRead4(&n, "Clos");
}

const BOOL CProxyClient3::SetChannel(const BYTE bCh)
{
    CBlockLock lock(&m_cs);
    DWORD ch = bCh;
    BOOL b;
    return WriteAndRead4(&b, "SCha", &ch) ? b : FALSE;
}

const float CProxyClient3::GetSignalLevel()
{
    CBlockLock lock(&m_cs);
    float f;
    return WriteAndRead4(&f, "GSig") ? f : 0;
}

const DWORD CProxyClient3::WaitTsStream(const DWORD dwTimeOut)
{
    // 実装しない(中断用のイベントを指定できないなど使い勝手が悪く、利用例を知らないため)
    static_cast<void>(dwTimeOut);
    return WAIT_ABANDONED;
}

const DWORD CProxyClient3::GetReadyCount()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GRea") ? n : 0;
}

const BOOL CProxyClient3::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
    // 実装しない(仕様上、安全な利用法がおそらく無く、利用例を知らないため)
    static_cast<void>(pDst);
    static_cast<void>(pdwSize);
    static_cast<void>(pdwRemain);
    return FALSE;
}

const BOOL CProxyClient3::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
    CBlockLock lock(&m_cs);
    if (ppDst && pdwSize) {
        DWORD tsBufSize = 0;
        DWORD tsRemain = 0;
        ++m_sequenceNum;
        for (int retry = 0;; ++retry) {
            DWORD n;
            if (Write("GTsS") && ReadAll(&n, 4)) {
                SwapBytes4(&n);
                if (n < 4 || n - 4 > sizeof(m_tsBuf)) {
                    // 戻り値が異常
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                    break;
                }
                if (ReadAll(&tsRemain, 4) && ReadAll(m_tsBuf, n - 4)) {
                    SwapBytes4(&tsRemain);
                    tsBufSize = n - 4;
                    break;
                }
            }
            if (retry >= 1) {
                break;
            }
            Connect("BD\0\0Conn");
        }
        *ppDst = m_tsBuf;
        *pdwSize = tsBufSize;
        if (pdwRemain) {
            *pdwRemain = tsBufSize == 0 ? 0 : tsRemain;
        }
        return TRUE;
    }
    return FALSE;
}

void CProxyClient3::PurgeTsStream()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    WriteAndRead4(&n, "Purg");
}

void CProxyClient3::Release()
{
    DWORD n;
    if (Write("Rele") && ReadAll(&n, 4)) {
        closesocket(m_sock);
    }
#ifdef _WIN32
    WSACleanup();
    DeleteCriticalSection(&m_cs);
#endif
    g_this = nullptr;
    delete this;
}

DWORD CProxyClient3::Connect(const char (&cmd)[9])
{
    if (m_sock == INVALID_SOCKET) {
        addrinfo hints = {};
        hints.ai_flags = AI_NUMERICHOST;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo *result;
        if (getaddrinfo(m_addr, m_port, &hints, &result) == 0) {
            m_sock = socket(result->ai_family, result->ai_socktype
#ifndef _WIN32
                                | SOCK_CLOEXEC | SOCK_NONBLOCK
#endif
                            , result->ai_protocol);
            if (m_sock != INVALID_SOCKET) {
#ifdef _WIN32
                unsigned long x = 1;
                ioctlsocket(m_sock, FIONBIO, &x);
#endif
                bool connected = connect(m_sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) == 0;
                if (!connected &&
#ifdef _WIN32
                    WSAGetLastError() == WSAEWOULDBLOCK
#else
                    errno == EINPROGRESS
#endif
                    ) {
                    fd_set wfds;
                    FD_ZERO(&wfds);
                    FD_SET(m_sock, &wfds);
                    timeval tv = {m_connectTimeout, 0};
                    connected = select(static_cast<int>(m_sock) + 1, nullptr, &wfds, nullptr, (m_connectTimeout > 0 ? &tv : nullptr)) == 1;
                }
#ifdef _WIN32
                x = 0;
                ioctlsocket(m_sock, FIONBIO, &x);
#else
                int x = 0;
                ioctl(m_sock, FIONBIO, &x);
#endif
                if (!connected) {
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                }
            }
            freeaddrinfo(result);
        }
        if (m_sock != INVALID_SOCKET) {
            if (m_sendRecvTimeout > 0) {
#ifdef _WIN32
                DWORD to = m_sendRecvTimeout * 1000;
#else
                timeval to = {m_sendRecvTimeout, 0};
#endif
                setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
                setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
            }
            char buf[524] = {};
            memcpy(buf, cmd, 8);
            SwapBytes4(buf + 8, &m_sessionID);
#ifdef _WIN32
            memcpy(buf + 12, m_origin, (wcslen(m_origin) + 1) * sizeof(WCHAR));
#else
            // TODO: 今のところASCIIのみ対応
            for (int i = 0; m_origin[i]; ++i) {
                buf[12 + i * 2] = m_origin[i];
            }
#endif
            for (int n = 0, m; n < 524; n += m) {
                m = send(m_sock, buf + n, 524 - n, 0);
                if (m <= 0) {
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                    return 0;
                }
            }
            DWORD n;
            if (ReadAll(&n, 4)) {
                SwapBytes4(&n);
                return n;
            }
        }
    }
    return 0;
}

bool CProxyClient3::WriteAndRead4(void *buf, const char (&cmd)[5], const void *param1, const void *param2)
{
    ++m_sequenceNum;
    for (int retry = 0;; ++retry) {
        if (Write(cmd, param1, param2) && ReadAll(buf, 4)) {
            SwapBytes4(buf);
            return true;
        }
        if (retry >= 1) {
            break;
        }
        Connect("BD\0\0Conn");
    }
    return false;
}

bool CProxyClient3::WriteAndReadString(WCHAR (&buf)[256], const char (&cmd)[5], const void *param1, const void *param2)
{
    ++m_sequenceNum;
    for (int retry = 0;; ++retry) {
        DWORD n;
        if (Write(cmd, param1, param2) && ReadAll(&n, 4)) {
            SwapBytes4(&n);
            n %= 256;
            if (n == 0) {
                return false;
            }
            if (ReadAll(buf, n * sizeof(WCHAR))) {
                buf[n] = L'\0';
#ifndef _WIN32
                int le = 1;
                if (!reinterpret_cast<BYTE*>(&le)[0]) {
                    // Big-endian
                    for (int i = 0; buf[i]; ++i) {
                        buf[i] = static_cast<WCHAR>(buf[i] << 8 | buf[i] >> 8);
                    }
                }
#endif
                return true;
            }
        }
        if (retry >= 1) {
            break;
        }
        Connect("BD\0\0Conn");
    }
    return false;
}

bool CProxyClient3::Write(const char (&cmd)[5], const void *param1, const void *param2)
{
    if (m_sock != INVALID_SOCKET) {
        char buf[16] = {};
        memcpy(buf, cmd, 4);
        SwapBytes4(buf + 4, &m_sequenceNum);
        if (param1) {
            SwapBytes4(buf + 8, param1);
        }
        if (param2) {
            SwapBytes4(buf + 12, param2);
        }
        for (int n = 0, m; n < 16; n += m) {
            m = send(m_sock, buf + n, 16 - n, 0);
            if (m <= 0) {
                closesocket(m_sock);
                m_sock = INVALID_SOCKET;
                return false;
            }
        }
        return true;
    }
    return false;
}

bool CProxyClient3::ReadAll(void *buf, DWORD len)
{
    if (m_sock != INVALID_SOCKET) {
        int m;
        for (DWORD n = 0; n < len; n += m) {
            m = recv(m_sock, static_cast<char*>(buf) + n, len - n, 0);
            if (m <= 0) {
                closesocket(m_sock);
                m_sock = INVALID_SOCKET;
                return false;
            }
        }
        return true;
    }
    return false;
}

#ifdef _WIN32
BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved)
{
    static_cast<void>(lpReserved);
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        break;
    case DLL_PROCESS_DETACH:
        if (g_this) {
            OutputDebugString(L"BonDriver_Tunnel::DllMain(): Driver Is Not Released!\n");
            g_this->Release();
        }
        break;
    }
    return TRUE;
}
#endif

extern "C" BONAPI IBonDriver * CreateBonDriver(void)
{
    if (!g_this) {
        char addr[64] = {};
        char port[8] = {};
        int connectTimeout = 0;
        int sendRecvTimeout = 0;
#ifdef _WIN32
        WCHAR pathBuf[MAX_PATH + 4];
        LPWSTR origin = nullptr;
        WCHAR optionOrigin[256];
        {
            // DLLの名前から接続先ドライバ名を抽出
            WCHAR path[MAX_PATH];
            DWORD len = GetModuleFileName(g_hModule, path, MAX_PATH);
            if (len && len < MAX_PATH) {
                len = GetLongPathName(path, pathBuf, MAX_PATH);
                if (len && len < MAX_PATH && wcsrchr(pathBuf, L'\\')) {
                    origin = wcsrchr(pathBuf, L'\\') + 1;
                    LPWSTR ext = wcsrchr(origin, L'.');
                    if (!ext) {
                        ext = origin + wcslen(origin);
                    }
                    ext[0] = L'\0';
                    wcscat_s(pathBuf, L".ini");
                    WCHAR addrW[64];
                    GetPrivateProfileString(L"OPTION", L"ADDRESS", L"", addrW, 64, pathBuf);
                    for (int i = 0; addrW[i]; ++i) {
                        addr[i] = static_cast<char>(addrW[i]);
                    }
                    WCHAR portW[8];
                    GetPrivateProfileString(L"OPTION", L"PORT", L"1193", portW, 8, pathBuf);
                    for (int i = 0; portW[i]; ++i) {
                        port[i] = static_cast<char>(portW[i]);
                    }
                    GetPrivateProfileString(L"OPTION", L"ORIGIN", L"", optionOrigin, 256, pathBuf);
                    connectTimeout = GetPrivateProfileInt(L"OPTION", L"CONNECT_TIMEOUT", 5, pathBuf);
                    sendRecvTimeout = GetPrivateProfileInt(L"OPTION", L"SEND_RECV_TIMEOUT", 5, pathBuf);
                    ext[0] = L'\0';
                    if (optionOrigin[0]) {
                        origin = optionOrigin;
                    }
                    else if (_wcsnicmp(origin, L"BonDriver_", 10) == 0) {
                        origin += 10;
                    }
                    else {
                        origin = nullptr;
                    }
                }
            }
        }
#else
        char pathBuf[1024];
        char *origin = nullptr;
        char optionOrigin[256];
        {
            // DLLの名前から接続先ドライバ名を抽出
            Dl_info info;
            if (dladdr(reinterpret_cast<void*>(CreateBonDriver), &info) && strlen(info.dli_fname) < sizeof(pathBuf) - 4) {
                strcpy(pathBuf, info.dli_fname);
                if (strrchr(pathBuf, '/')) {
                    origin = strrchr(pathBuf, '/') + 1;
                    char *ext = strrchr(origin, '.');
                    if (!ext) {
                        ext = origin + strlen(origin);
                    }
                    strcpy(ext, ".ini");
                    GetIniString("ADDRESS", "", addr, 64, pathBuf);
                    GetIniString("PORT", "1193", port, 8, pathBuf);
                    GetIniString("ORIGIN", "", optionOrigin, 256, pathBuf);
                    char ret[16];
                    GetIniString("CONNECT_TIMEOUT", "5", ret, 16, pathBuf);
                    connectTimeout = static_cast<int>(strtol(ret, nullptr, 10));
                    GetIniString("SEND_RECV_TIMEOUT", "5", ret, 16, pathBuf);
                    sendRecvTimeout = static_cast<int>(strtol(ret, nullptr, 10));
                    ext[0] = '\0';
                    if (optionOrigin[0]) {
                        origin = optionOrigin;
                    }
                    else if (strncmp(origin, "BonDriver_", 10) == 0) {
                        origin += 10;
                    }
                    else {
                        origin = nullptr;
                    }
                }
            }
        }
#endif

#ifdef _WIN32
        WSAData wsaData;
        if (origin && WSAStartup(MAKEWORD(2, 2), &wsaData) == 0)
#else
        if (origin)
#endif
        {
            // 接続
            CProxyClient3 *down = new CProxyClient3(addr, port, origin, connectTimeout, sendRecvTimeout);
            DWORD type = down->CreateBon();
            if (type == 0) {
                // 初期化に失敗
                down->Release();
            }
            else if (type == 1) {
                g_this = new CProxyClient(down);
            }
            else if (type == 2) {
                g_this = new CProxyClient2(down);
            }
            else {
                g_this = down;
            }
        }
    }
    return g_this;
}
