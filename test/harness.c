/*
 * Test harness for lanfix: a 32-bit exe that imports ws2_32 and dsound the same way the
 * games do, then exercises each hooked call. Run it with and without lanfix's dsound.dll
 * next to it and compare.
 *
 * Usage: harness.exe [stale-ip] [other-adapter-ip]
 *   stale-ip          an address this PC doesn't have (default 192.168.0.48)
 *   other-adapter-ip  an address of another local adapter, e.g. a VPN's (optional)
 */
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <mmsystem.h>
#define COBJMACROS
#include <dsound.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    WSADATA wsa;
    char host[256], path[MAX_PATH] = "(not loaded)";
    struct hostent *h;
    struct sockaddr_in a, got;
    int i, rc, len;
    SOCKET s;
    BOOL on = TRUE;
    INTERFACE_INFO ii[32];
    DWORD ret = 0;
    LPDIRECTSOUND ds = NULL;
    HRESULT hr;
    const char *stale = argc > 1 ? argv[1] : "192.168.0.48";

    WSAStartup(MAKEWORD(2, 2), &wsa);

    hr = DirectSoundCreate(NULL, &ds, NULL);
    GetModuleFileNameA(GetModuleHandleA("dsound.dll"), path, MAX_PATH);
    printf("dsound.dll loaded from: %s\n", path);
    printf("DirectSoundCreate: hr=0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (ds)
        IDirectSound_Release(ds);

    gethostname(host, sizeof(host));
    h = gethostbyname(host);
    printf("gethostbyname(%s):", host);
    for (i = 0; h && h->h_addr_list[i]; i++)
        printf(" %s", inet_ntoa(*(struct in_addr *)h->h_addr_list[i]));
    printf("\n  first = %s\n", h && h->h_addr_list[0] ? inet_ntoa(*(struct in_addr *)h->h_addr_list[0]) : "-");

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(stale);
    rc = bind(s, (struct sockaddr *)&a, sizeof(a));
    len = sizeof(got);
    getsockname(s, (struct sockaddr *)&got, &len);
    printf("bind(%s): rc=%d err=%d  bound to %s:%u\n", stale, rc, rc ? WSAGetLastError() : 0,
           inet_ntoa(got.sin_addr), ntohs(got.sin_port));

    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(47801);
    a.sin_addr.s_addr = INADDR_BROADCAST;
    rc = sendto(s, "lanfix-test", 11, 0, (struct sockaddr *)&a, sizeof(a));
    printf("sendto(255.255.255.255:47801): rc=%d err=%d\n", rc, rc < 0 ? WSAGetLastError() : 0);

    rc = WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, ii, sizeof(ii), &ret, NULL, NULL);
    printf("SIO_GET_INTERFACE_LIST: rc=%d first = %s\n", rc,
           rc == 0 && ret ? inet_ntoa(ii[0].iiAddress.AddressIn.sin_addr) : "-");

    memset(&a, 0, sizeof(a));
    memset(&got, 0, sizeof(got));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    rc = WSAIoctl(s, SIO_ROUTING_INTERFACE_QUERY, &a, sizeof(a), &got, sizeof(got), &ret, NULL, NULL);
    printf("SIO_ROUTING_INTERFACE_QUERY(8.8.8.8): rc=%d local = %s\n", rc,
           rc == 0 ? inet_ntoa(got.sin_addr) : "-");
    closesocket(s);

    /* RA3's own-address detection: unbound UDP socket, connect, getsockname. */
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof(a));
    memset(&got, 0, sizeof(got));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    rc = connect(s, (struct sockaddr *)&a, sizeof(a));
    len = sizeof(got);
    if (rc == 0)
        rc = getsockname(s, (struct sockaddr *)&got, &len);
    printf("UDP connect(8.8.8.8)+getsockname: rc=%d local = %s\n", rc,
           rc == 0 ? inet_ntoa(got.sin_addr) : "-");
    closesocket(s);

    if (argc > 2) {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr(argv[2]);
        rc = bind(s, (struct sockaddr *)&a, sizeof(a));
        len = sizeof(got);
        getsockname(s, (struct sockaddr *)&got, &len);
        printf("bind(%s): rc=%d err=%d  bound to %s:%u\n", argv[2], rc, rc ? WSAGetLastError() : 0,
               inet_ntoa(got.sin_addr), ntohs(got.sin_port));
        closesocket(s);
    }

    WSACleanup();
    return 0;
}
