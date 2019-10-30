#include "BonDriver_Tunnel.h"
#include <string.h>
#include <wchar.h>

namespace
{
IBonDriver *g_this;
HINSTANCE g_hModule;
}

CProxyClient3::CProxyClient3(LPCSTR addr, LPCSTR port, LPCWSTR origin, int connectTimeout, int sendRecvTimeout)
    : m_connectTimeout(connectTimeout)
    , m_sendRecvTimeout(sendRecvTimeout)
    , m_sock(INVALID_SOCKET)
    , m_sessionID(0)
    , m_sequenceNum(0)
    , m_tsBufSize(0)
{
    InitializeCriticalSection(&m_cs);
    strncpy_s(m_addr, addr, _TRUNCATE);
    strncpy_s(m_port, port, _TRUNCATE);
    wcsncpy_s(m_origin, origin, _TRUNCATE);
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
    return WriteAndRead4(&n, "GCSp") ? n : 0;
}

const DWORD CProxyClient3::GetCurChannel()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    return WriteAndRead4(&n, "GCCh") ? n : 0;
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
    m_tsBufSize = 0;
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
    return (WriteAndRead4(&n, "GRea") ? n : 0) + (m_tsBufSize != 0 ? 1 : 0);
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
        if (m_tsBufSize == 0) {
            ++m_sequenceNum;
            for (int retry = 0;; ++retry) {
                DWORD n;
                if (Write("GTsS") && ReadAll(&n, 4)) {
                    if (n < 4 || n - 4 > sizeof(m_tsBuf)) {
                        // 戻り値が異常
                        closesocket(m_sock);
                        m_sock = INVALID_SOCKET;
                        break;
                    }
                    if (ReadAll(&m_tsRemain, 4) && ReadAll(m_tsBuf, n - 4)) {
                        m_tsBufSize = n - 4;
                        break;
                    }
                }
                if (retry >= 1) {
                    break;
                }
                Connect("BD\0\0Conn");
            }
        }
        *ppDst = m_tsBuf;
        *pdwSize = m_tsBufSize;
        if (pdwRemain) {
            *pdwRemain = m_tsBufSize == 0 ? 0 : m_tsRemain;
        }
        m_tsBufSize = 0;
        return TRUE;
    }
    return FALSE;
}

void CProxyClient3::PurgeTsStream()
{
    CBlockLock lock(&m_cs);
    DWORD n;
    WriteAndRead4(&n, "Purg");
    m_tsBufSize = 0;
}

void CProxyClient3::Release()
{
    DWORD n;
    if (Write("Rele") && ReadAll(&n, 4)) {
        closesocket(m_sock);
    }
    WSACleanup();
    DeleteCriticalSection(&m_cs);
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
            m_sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (m_sock != INVALID_SOCKET) {
                unsigned long x = 1;
                ioctlsocket(m_sock, FIONBIO, &x);
                bool connected = connect(m_sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) == 0;
                if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set wfds;
                    FD_ZERO(&wfds);
                    FD_SET(m_sock, &wfds);
                    timeval tv = {m_connectTimeout, 0};
                    connected = select(0, nullptr, &wfds, nullptr, (m_connectTimeout > 0 ? &tv : nullptr)) == 1;
                }
                x = 0;
                ioctlsocket(m_sock, FIONBIO, &x);
                if (!connected) {
                    closesocket(m_sock);
                    m_sock = INVALID_SOCKET;
                }
            }
            freeaddrinfo(result);
        }
        if (m_sock != INVALID_SOCKET) {
            if (m_sendRecvTimeout > 0) {
                DWORD to = m_sendRecvTimeout * 1000;
                setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
                setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
            }
            char buf[524] = {};
            memcpy(buf, cmd, 8);
            memcpy(buf + 8, &m_sessionID, 4);
            memcpy(buf + 12, m_origin, (wcslen(m_origin) + 1) * sizeof(WCHAR));
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
            n %= 256;
            if (n == 0) {
                return false;
            }
            if (ReadAll(buf, n * sizeof(WCHAR))) {
                buf[n] = L'\0';
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
        memcpy(buf + 4, &m_sequenceNum, 4);
        if (param1) {
            memcpy(buf + 8, param1, 4);
        }
        if (param2) {
            memcpy(buf + 12, param2, 4);
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

extern "C" BONAPI IBonDriver * CreateBonDriver(void)
{
    if (!g_this) {
        WCHAR pathBuf[MAX_PATH + 4];
        LPWSTR origin = nullptr;
        char addr[64] = {};
        char port[8] = {};
        WCHAR optionOrigin[256];
        int connectTimeout = 0;
        int sendRecvTimeout = 0;
        {
            // DLLの名前から接続先ドライバ名を抽出
            WCHAR path[MAX_PATH];
            DWORD len = GetModuleFileName(g_hModule, path, MAX_PATH);
            if (len && len < MAX_PATH) {
                len = GetLongPathName(path, pathBuf, MAX_PATH);
                if (len && len < MAX_PATH && wcsrchr(pathBuf, L'\\')) {
                    origin = wcsrchr(pathBuf, L'\\') + 1;
                    if (_wcsnicmp(origin, L"BonDriver_", 10) == 0) {
                        origin += 10;
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
                    }
                    else {
                        origin = nullptr;
                    }
                }
            }
        }

        WSAData wsaData;
        if (origin && WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
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
