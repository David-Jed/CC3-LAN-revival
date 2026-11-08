/*
 * lanfix - LAN play fix for SAGE-engine C&C games (C&C3, Kane's Wrath, Red Alert 3).
 *
 * Built as a stand-in dsound.dll placed next to the game executable. Sound calls are
 * passed straight through to the system dsound.dll. On load, the game's own import
 * table is patched (in memory only) so that:
 *   - gethostbyname(<this PC>) lists the real home-network adapter first
 *     (the game otherwise takes whichever address Windows lists first, e.g. VirtualBox)
 *   - WSAIoctl SIO_ROUTING_INTERFACE_QUERY answers with that adapter (RA3 uses this to
 *     find its own address; with a VPN up Windows would answer with the VPN address)
 *   - WSAIoctl interface lists put that adapter first as well
 *   - sendto(255.255.255.255) becomes a subnet broadcast on that adapter, so VPNs
 *     that win the route for limited broadcasts don't swallow LAN discovery
 *   - bind() to any specific address other than that adapter (a VPN address, or one
 *     this PC no longer has) is redirected to it
 *   - connect() on an unbound UDP socket binds it to that adapter first. RA3 finds its
 *     own address with socket/connect/getsockname, and with a VPN up Windows would
 *     otherwise pick the VPN address (the game then advertises an unreachable IP and
 *     sees its own lobby broadcasts as a second player)
 * Settings live in lanfix.ini next to the DLL; activity is written to lanfix.log.
 */
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LANFIX_VERSION "1.2.0"
#define MAX_LOCAL 32

/* ---- configuration ---------------------------------------------------------------- */

static char g_dir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_adapterSetting[128] = "auto";
static int g_enabled = 1;
static int g_fixBroadcast = 1;
static int g_fixBind = 1;
static int g_logEnabled = 1;

/* ---- chosen adapter (network byte order) ------------------------------------------ */

static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static ULONG g_ip;
static ULONG g_mask;
static int g_hasGateway;
static ULONG g_local[MAX_LOCAL];
static int g_localCount;

/* ---- logging ---------------------------------------------------------------------- */

static CRITICAL_SECTION g_logLock;

static void lf_log(const char *fmt, ...)
{
    FILE *f;
    SYSTEMTIME t;
    va_list ap;

    if (!g_logEnabled)
        return;
    EnterCriticalSection(&g_logLock);
    f = fopen(g_logPath, "a");
    if (f) {
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static const char *ip_str(ULONG ip, char *buf)
{
    const unsigned char *b = (const unsigned char *)&ip;
    sprintf(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

/* ---- adapter selection ------------------------------------------------------------ */

static const char *const VIRTUAL_HINTS[] = {
    "virtualbox", "vmware",    "hyper-v",  "vethernet", "virtual",  "tap-",
    "tap adapter", "wintun",   "wireguard", "nordlynx", "openvpn",  "tailscale",
    "zerotier",   "radmin",    "hamachi",  "docker",    "wsl",      "loopback",
    "bluetooth",  "npcap",     "vpn",      "tunnel",    "anyconnect", "globalprotect",
    "pangp",      "fortinet",  "mullvad",  "proton",    "expressvpn", "surfshark",
};

static void to_lower_utf8(const WCHAR *w, char *out, int outLen)
{
    char *p;
    out[0] = 0;
    if (w)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outLen, NULL, NULL);
    for (p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
}

static int looks_virtual(const char *name, const char *desc)
{
    int i;
    for (i = 0; i < (int)(sizeof(VIRTUAL_HINTS) / sizeof(VIRTUAL_HINTS[0])); i++)
        if (strstr(name, VIRTUAL_HINTS[i]) || strstr(desc, VIRTUAL_HINTS[i]))
            return 1;
    return 0;
}

static int is_private(ULONG ip)
{
    ULONG h = ntohl(ip);
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8;
}

static int is_local(ULONG ip)
{
    int i;
    if ((ntohl(ip) >> 24) == 127)
        return 1;
    for (i = 0; i < g_localCount; i++)
        if (g_local[i] == ip)
            return 1;
    return 0;
}

static BOOL CALLBACK init_adapter(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    IP_ADAPTER_ADDRESSES *list = NULL, *a;
    ULONG size = 16384, rc;
    int bestScore = -1, tries;
    ULONG manualIp = inet_addr(g_adapterSetting);
    int isAuto = _stricmp(g_adapterSetting, "auto") == 0;
    char name[256], desc[256], lname[256], ldesc[256], ipb[16], maskb[16];
    char lsetting[128];

    (void)once; (void)param; (void)ctx;
    strncpy(lsetting, g_adapterSetting, sizeof(lsetting) - 1);
    lsetting[sizeof(lsetting) - 1] = 0;
    _strlwr(lsetting);

    for (tries = 0; tries < 4; tries++) {
        list = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, size);
        if (!list)
            return TRUE;
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, list, &size);
        if (rc == NO_ERROR)
            break;
        HeapFree(GetProcessHeap(), 0, list);
        list = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW) {
            lf_log("GetAdaptersAddresses failed: %lu", rc);
            return TRUE;
        }
    }
    if (!list)
        return TRUE;

    lf_log("lanfix %s  Adapter=%s FixBroadcast=%d FixBind=%d", LANFIX_VERSION,
           g_adapterSetting, g_fixBroadcast, g_fixBind);

    for (a = list; a; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        int score, virt, up = a->OperStatus == IfOperStatusUp;
        int hasGw = a->FirstGatewayAddress != NULL;
        int physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;

        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc), NULL, NULL);
        to_lower_utf8(a->FriendlyName, lname, sizeof(lname));
        to_lower_utf8(a->Description, ldesc, sizeof(ldesc));
        virt = looks_virtual(lname, ldesc);

        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            ULONG ip, mask = 0;
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            ip = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
            if (up && g_localCount < MAX_LOCAL)
                g_local[g_localCount++] = ip;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);

            if (!up || (ntohl(ip) >> 16) == 0xA9FE) /* down, or 169.254.x.x */
                score = -1;
            else if (!isAuto)
                score = (manualIp != INADDR_NONE) ? (ip == manualIp ? 1000 : -1)
                        : (strstr(lname, lsetting) || strstr(ldesc, lsetting)) ? 1000 : -1;
            else
                score = (virt ? 0 : 400) + (physical ? 200 : 0) + (hasGw ? 100 : 0) +
                        (is_private(ip) ? 50 : 0) - (int)(a->Ipv4Metric > 49 ? 49 : a->Ipv4Metric);

            lf_log("  adapter %-15s/%-15s score=%4d up=%d gw=%d phys=%d virt=%d metric=%lu  %s [%s]",
                   ip_str(ip, ipb), ip_str(mask, maskb), score, up, hasGw, physical, virt,
                   a->Ipv4Metric, name, desc);
            if (score > bestScore) {
                bestScore = score;
                g_ip = ip;
                g_mask = mask;
                g_hasGateway = hasGw;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, list);

    if (g_ip)
        lf_log("chosen adapter: %s mask %s", ip_str(g_ip, ipb), ip_str(g_mask, maskb));
    else
        lf_log("no usable adapter found%s; leaving networking unchanged",
               isAuto ? "" : " for the Adapter setting");
    return TRUE;
}

static void lf_init(void)
{
    InitOnceExecuteOnce(&g_initOnce, init_adapter, NULL, NULL);
}

/* ---- hooks ------------------------------------------------------------------------ */

typedef struct {
    struct hostent he;
    char *addrList[MAX_LOCAL + 1];
    ULONG addrs[MAX_LOCAL];
    char *aliases[1];
    char name[256];
} HostCopy;

static __declspec(thread) HostCopy t_host;
static volatile LONG g_loggedHost, g_loggedBcast, g_loggedBind, g_loggedIoctl, g_loggedRoute,
    g_loggedConnect;

static struct hostent *WSAAPI hk_gethostbyname(const char *name)
{
    struct hostent *h = gethostbyname(name);
    char host[256], ipb[16];
    int i, n = 0;

    if (!h || !name || h->h_addrtype != AF_INET || h->h_length != 4)
        return h;
    if (gethostname(host, sizeof(host)) != 0 || _stricmp(host, name) != 0)
        return h;
    lf_init();
    if (!g_ip)
        return h;

    t_host.addrs[n++] = g_ip;
    for (i = 0; h->h_addr_list[i] && n < MAX_LOCAL; i++) {
        ULONG ip = *(ULONG *)h->h_addr_list[i];
        if (ip != g_ip)
            t_host.addrs[n++] = ip;
    }
    for (i = 0; i < n; i++)
        t_host.addrList[i] = (char *)&t_host.addrs[i];
    t_host.addrList[n] = NULL;
    t_host.aliases[0] = NULL;
    strncpy(t_host.name, h->h_name ? h->h_name : name, sizeof(t_host.name) - 1);
    t_host.he.h_name = t_host.name;
    t_host.he.h_aliases = t_host.aliases;
    t_host.he.h_addrtype = AF_INET;
    t_host.he.h_length = 4;
    t_host.he.h_addr_list = t_host.addrList;

    if (InterlockedExchange(&g_loggedHost, 1) == 0)
        lf_log("gethostbyname(%s): putting %s first (Windows listed %s first)", name,
               ip_str(g_ip, ipb), ip_str(*(ULONG *)h->h_addr_list[0], host));
    return &t_host.he;
}

static int WSAAPI hk_sendto(SOCKET s, const char *buf, int len, int flags,
                            const struct sockaddr *to, int tolen)
{
    if (g_fixBroadcast && to && tolen >= (int)sizeof(struct sockaddr_in) &&
        to->sa_family == AF_INET &&
        ((const struct sockaddr_in *)to)->sin_addr.s_addr == INADDR_BROADCAST) {
        lf_init();
        if (g_ip && g_mask && g_mask != 0xFFFFFFFF) {
            struct sockaddr_in d = *(const struct sockaddr_in *)to;
            char ipb[16];
            d.sin_addr.s_addr = (g_ip & g_mask) | ~g_mask;
            if (InterlockedIncrement(&g_loggedBcast) <= 3)
                lf_log("sendto 255.255.255.255:%u -> %s", ntohs(d.sin_port),
                       ip_str(d.sin_addr.s_addr, ipb));
            return sendto(s, buf, len, flags, (const struct sockaddr *)&d, sizeof(d));
        }
    }
    return sendto(s, buf, len, flags, to, tolen);
}

static int WSAAPI hk_bind(SOCKET s, const struct sockaddr *addr, int len)
{
    if (g_fixBind && addr && len >= (int)sizeof(struct sockaddr_in) &&
        addr->sa_family == AF_INET) {
        ULONG ip = ((const struct sockaddr_in *)addr)->sin_addr.s_addr;
        if (ip != INADDR_ANY && (ntohl(ip) >> 24) != 127) {
            lf_init();
            if (g_ip && ip != g_ip) {
                struct sockaddr_in d = *(const struct sockaddr_in *)addr;
                char a[16], b[16];
                d.sin_addr.s_addr = g_ip;
                if (InterlockedIncrement(&g_loggedBind) <= 5)
                    lf_log("bind %s:%u (%s) -> %s", ip_str(ip, a), ntohs(d.sin_port),
                           is_local(ip) ? "another adapter" : "not on this PC", ip_str(g_ip, b));
                return bind(s, (const struct sockaddr *)&d, sizeof(d));
            }
        }
    }
    return bind(s, addr, len);
}

static int WSAAPI hk_connect(SOCKET s, const struct sockaddr *name, int len)
{
    if (g_fixBind && name && len >= (int)sizeof(struct sockaddr_in) &&
        name->sa_family == AF_INET &&
        (ntohl(((const struct sockaddr_in *)name)->sin_addr.s_addr) >> 24) != 127) {
        int type = 0, typeLen = sizeof(type);
        struct sockaddr_in cur;
        int curLen = sizeof(cur);
        ULONG dst = ((const struct sockaddr_in *)name)->sin_addr.s_addr;
        /* Only UDP sockets that aren't bound yet (getsockname fails on those). */
        if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &typeLen) == 0 &&
            type == SOCK_DGRAM && getsockname(s, (struct sockaddr *)&cur, &curLen) != 0) {
            lf_init();
            /* Skip when the chosen adapter can't reach dst (no gateway, other subnet):
               binding first would only make the connect fail. */
            if (g_ip && (g_hasGateway || (dst & g_mask) == (g_ip & g_mask))) {
                struct sockaddr_in local;
                char a[16], b[16];
                memset(&local, 0, sizeof(local));
                local.sin_family = AF_INET;
                local.sin_addr.s_addr = g_ip;
                if (bind(s, (const struct sockaddr *)&local, sizeof(local)) == 0 &&
                    InterlockedIncrement(&g_loggedConnect) <= 5)
                    lf_log("connect(UDP -> %s:%u): bound to %s first", ip_str(dst, a),
                           ntohs(((const struct sockaddr_in *)name)->sin_port), ip_str(g_ip, b));
            }
        }
    }
    return connect(s, name, len);
}

static int WSAAPI hk_WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inLen, LPVOID out,
                              DWORD outLen, LPDWORD returned, LPWSAOVERLAPPED ov,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int rc = WSAIoctl(s, code, in, inLen, out, outLen, returned, ov, cr);
    DWORD i;

    if (rc != 0 || ov || cr || !out || !returned)
        return rc;
    if (code == SIO_ROUTING_INTERFACE_QUERY) {
        struct sockaddr_in *res = (struct sockaddr_in *)out;
        lf_init();
        if (g_ip && outLen >= sizeof(*res) && res->sin_family == AF_INET &&
            res->sin_addr.s_addr != g_ip && (ntohl(res->sin_addr.s_addr) >> 24) != 127) {
            char a[16], b[16], c[16] = "?";
            if (in && inLen >= sizeof(struct sockaddr_in) &&
                ((struct sockaddr *)in)->sa_family == AF_INET)
                ip_str(((struct sockaddr_in *)in)->sin_addr.s_addr, c);
            if (InterlockedIncrement(&g_loggedRoute) <= 5)
                lf_log("SIO_ROUTING_INTERFACE_QUERY(%s): Windows answered %s -> %s", c,
                       ip_str(res->sin_addr.s_addr, a), ip_str(g_ip, b));
            res->sin_addr.s_addr = g_ip;
        }
    } else if (code == SIO_GET_INTERFACE_LIST) {
        INTERFACE_INFO *ii = (INTERFACE_INFO *)out, tmp;
        DWORD n = *returned / sizeof(INTERFACE_INFO);
        lf_init();
        for (i = 1; g_ip && i < n; i++) {
            if (ii[i].iiAddress.AddressIn.sin_addr.s_addr == g_ip) {
                tmp = ii[0]; ii[0] = ii[i]; ii[i] = tmp;
                if (InterlockedExchange(&g_loggedIoctl, 1) == 0)
                    lf_log("SIO_GET_INTERFACE_LIST: moved chosen adapter to the front");
                break;
            }
        }
    } else if (code == SIO_ADDRESS_LIST_QUERY) {
        SOCKET_ADDRESS_LIST *sl = (SOCKET_ADDRESS_LIST *)out;
        SOCKET_ADDRESS tmp;
        lf_init();
        for (i = 1; g_ip && i < (DWORD)sl->iAddressCount; i++) {
            const struct sockaddr *sa = sl->Address[i].lpSockaddr;
            if (sa && sa->sa_family == AF_INET &&
                ((const struct sockaddr_in *)sa)->sin_addr.s_addr == g_ip) {
                tmp = sl->Address[0]; sl->Address[0] = sl->Address[i]; sl->Address[i] = tmp;
                if (InterlockedExchange(&g_loggedIoctl, 1) == 0)
                    lf_log("SIO_ADDRESS_LIST_QUERY: moved chosen adapter to the front");
                break;
            }
        }
    }
    return rc;
}

/* ---- import table patching -------------------------------------------------------- */

static int patch_iat(HMODULE mod, const char *dll, const char *func, void *hook)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    FARPROC real = GetProcAddress(GetModuleHandleA(dll), func);
    int patched = 0;

    if (!dir.VirtualAddress)
        return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (_stricmp((const char *)(base + d->Name), dll) != 0)
            continue;
        iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk) : NULL;
        for (; iat->u1.Function; iat++) {
            /* Match by name when imported by name; otherwise (ordinal imports, as modern
               ws2_32.lib produces) match by the resolved address. */
            int match = real && iat->u1.Function == (ULONG_PTR)real;
            if (names) {
                if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                    strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name,
                           func) == 0)
                    match = 1;
                names++;
            }
            if (match) {
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)hook;
                    VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), old, &old);
                    patched++;
                }
            }
        }
    }
    return patched;
}

static void install_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    char exePath[MAX_PATH];
    int n1, n2, n3, n4, n5;

    GetModuleFileNameA(exe, exePath, MAX_PATH);
    n1 = patch_iat(exe, "ws2_32.dll", "gethostbyname", (void *)hk_gethostbyname);
    n2 = patch_iat(exe, "ws2_32.dll", "sendto", (void *)hk_sendto);
    n3 = patch_iat(exe, "ws2_32.dll", "bind", (void *)hk_bind);
    n4 = patch_iat(exe, "ws2_32.dll", "WSAIoctl", (void *)hk_WSAIoctl);
    n5 = patch_iat(exe, "ws2_32.dll", "connect", (void *)hk_connect);
    lf_log("loaded into %s  hooks: gethostbyname=%d sendto=%d bind=%d WSAIoctl=%d connect=%d",
           exePath, n1, n2, n3, n4, n5);
}

/* ---- dsound pass-through ---------------------------------------------------------- */

static HMODULE g_realDsound;

static FARPROC real_dsound(const char *name)
{
    if (!g_realDsound) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 12)
            return NULL;
        strcat(path, "\\dsound.dll");
        g_realDsound = LoadLibraryA(path);
        if (!g_realDsound)
            lf_log("could not load system dsound.dll from %s (error %lu)", path, GetLastError());
    }
    return g_realDsound ? GetProcAddress(g_realDsound, name) : NULL;
}

#define DSOUND_FORWARD(fn, params, args)                                 \
    typedef HRESULT(WINAPI *fn##_t) params;                              \
    HRESULT WINAPI lf_##fn params                                        \
    {                                                                    \
        fn##_t real = (fn##_t)real_dsound(#fn);                          \
        return real ? real args : E_FAIL;                                \
    }

DSOUND_FORWARD(DirectSoundCreate, (void *g, void **ds, void *u), (g, ds, u))
DSOUND_FORWARD(DirectSoundCreate8, (void *g, void **ds, void *u), (g, ds, u))
DSOUND_FORWARD(DirectSoundEnumerateA, (void *cb, void *ctx), (cb, ctx))
DSOUND_FORWARD(DirectSoundEnumerateW, (void *cb, void *ctx), (cb, ctx))
DSOUND_FORWARD(DirectSoundCaptureCreate, (void *g, void **dc, void *u), (g, dc, u))
DSOUND_FORWARD(DirectSoundCaptureCreate8, (void *g, void **dc, void *u), (g, dc, u))
DSOUND_FORWARD(DirectSoundCaptureEnumerateA, (void *cb, void *ctx), (cb, ctx))
DSOUND_FORWARD(DirectSoundCaptureEnumerateW, (void *cb, void *ctx), (cb, ctx))
DSOUND_FORWARD(GetDeviceID, (void *src, void *dst), (src, dst))
DSOUND_FORWARD(DllGetClassObject, (void *clsid, void *iid, void **ppv), (clsid, iid, ppv))
DSOUND_FORWARD(DirectSoundFullDuplexCreate,
               (void *cg, void *rg, void *cd, void *bd, HWND w, DWORD lvl, void **fd,
                void **cb, void **b, void *u),
               (cg, rg, cd, bd, w, lvl, fd, cb, b, u))

HRESULT WINAPI lf_DllCanUnloadNow(void)
{
    typedef HRESULT(WINAPI * fn_t)(void);
    fn_t real = (fn_t)real_dsound("DllCanUnloadNow");
    return real ? real() : S_FALSE;
}

/* ---- entry point ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    char *slash;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_logLock);
    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    slash = strrchr(g_dir, '\\');
    if (slash)
        *slash = 0;
    sprintf(g_iniPath, "%s\\lanfix.ini", g_dir);
    sprintf(g_logPath, "%s\\lanfix.log", g_dir);

    g_enabled = GetPrivateProfileIntA("LanFix", "Enabled", 1, g_iniPath);
    g_fixBroadcast = GetPrivateProfileIntA("LanFix", "FixBroadcast", 1, g_iniPath);
    g_fixBind = GetPrivateProfileIntA("LanFix", "FixBind", 1, g_iniPath);
    g_logEnabled = GetPrivateProfileIntA("LanFix", "Log", 1, g_iniPath);
    GetPrivateProfileStringA("LanFix", "Adapter", "auto", g_adapterSetting,
                             sizeof(g_adapterSetting), g_iniPath);

    if (g_enabled)
        install_hooks();
    else
        lf_log("lanfix %s disabled in lanfix.ini; passing sound calls through only", LANFIX_VERSION);
    return TRUE;
}
