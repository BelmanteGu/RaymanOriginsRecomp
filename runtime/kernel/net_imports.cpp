// Rede (NetDll_*): o Rayman Origins é jogável offline, então tudo responde de
// forma consistente como "sem rede". Os eventos WSA são eventos reais do
// dispatcher, porque o jogo espera neles. Todo NetDll_* recebe o "caller" em r3.
#include <cstdio>
#include <vector>
#include "function.h"
#include "objects.h"
#include "xbox_defs.h"

constexpr int32_t SOCKET_ERROR = -1;
constexpr uint32_t INVALID_SOCKET = 0xFFFFFFFF;
constexpr uint32_t WSAENETDOWN = 10050;
constexpr uint32_t WSAHOST_NOT_FOUND = 11001;
constexpr uint32_t WSA_WAIT_TIMEOUT = 258;
constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
constexpr uint32_t XNET_CONNECT_STATUS_IDLE = 0;

static thread_local uint32_t t_lastError = 0;

static uint32_t Fail(uint32_t error)
{
    t_lastError = error;
    return uint32_t(SOCKET_ERROR);
}

static uint32_t NetDll_WSAStartup(uint32_t caller, uint32_t version, void* data) { (void)caller; (void)version; (void)data; return 0; }
static uint32_t NetDll_WSACleanup(uint32_t caller) { (void)caller; return 0; }
static uint32_t NetDll_XNetStartup(uint32_t caller, void* params) { (void)caller; (void)params; return 0; }
static uint32_t NetDll_XNetCleanup(uint32_t caller) { (void)caller; return 0; }
static uint32_t NetDll_WSAGetLastError() { return t_lastError; }
static void NetDll_WSASetLastError(uint32_t error) { t_lastError = error; }

static uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, void* address) { (void)caller; (void)address; return XNET_GET_XNADDR_NONE; }
static uint32_t NetDll_XNetGetConnectStatus(uint32_t caller, void* address) { (void)caller; (void)address; return XNET_CONNECT_STATUS_IDLE; }
static uint32_t NetDll_XNetSetSystemLinkPort(uint32_t caller, uint32_t port) { (void)caller; (void)port; return 0; }

// Eventos WSA: eventos de reset manual do dispatcher.
static uint32_t NetDll_WSACreateEvent()
{
    return CreateHandle(std::make_shared<Event>(true, false));
}

static uint32_t NetDll_WSACloseEvent(uint32_t event)
{
    return CloseHandle(event) ? 1 : 0;
}

static uint32_t SetWsaEvent(uint32_t handle, bool signal)
{
    auto event = GetObjectAs<Event>(handle);
    if (!event)
        return 0;
    {
        auto lock = LockDispatcher();
        event->signaled = signal;
    }
    if (signal)
        NotifyDispatcher();
    return 1;
}

static uint32_t NetDll_WSAResetEvent(uint32_t event) { return SetWsaEvent(event, false); }

static uint32_t NetDll_WSAWaitForMultipleEvents(uint32_t count, be<uint32_t>* events, uint32_t waitAll, uint32_t timeout, uint32_t alertable)
{
    (void)alertable;
    std::vector<std::shared_ptr<KernelObject>> objects(count);
    for (uint32_t i = 0; i < count; i++)
        objects[i] = GetObject(events[i].get());
    uint32_t result = WaitForObjects(objects, waitAll != 0, timeout == 0xFFFFFFFF ? -1 : int64_t(timeout));
    return result == STATUS_TIMEOUT ? WSA_WAIT_TIMEOUT : result;
}

// Sockets: rede fora do ar.
static uint32_t NetDll_socket(uint32_t caller, uint32_t af, uint32_t type, uint32_t protocol)
{
    (void)caller; (void)af; (void)type; (void)protocol;
    t_lastError = WSAENETDOWN;
    return INVALID_SOCKET;
}

#define NET_FAIL_STUB(name, ...) static uint32_t name(__VA_ARGS__) { return Fail(WSAENETDOWN); }
NET_FAIL_STUB(NetDll_closesocket, uint32_t, uint32_t)
NET_FAIL_STUB(NetDll_bind, uint32_t, uint32_t, void*, uint32_t)
NET_FAIL_STUB(NetDll_connect, uint32_t, uint32_t, void*, uint32_t)
NET_FAIL_STUB(NetDll_send, uint32_t, uint32_t, void*, uint32_t, uint32_t)
NET_FAIL_STUB(NetDll_recv, uint32_t, uint32_t, void*, uint32_t, uint32_t)
NET_FAIL_STUB(NetDll_sendto, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*, uint32_t)
NET_FAIL_STUB(NetDll_recvfrom, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*, void*)
NET_FAIL_STUB(NetDll_WSASendTo, uint32_t, uint32_t, void*, uint32_t, void*, uint32_t, void*, uint32_t)
NET_FAIL_STUB(NetDll_WSARecvFrom, uint32_t, uint32_t, void*, uint32_t, void*, void*, void*, void*)
NET_FAIL_STUB(NetDll_setsockopt, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t)
NET_FAIL_STUB(NetDll_getsockopt, uint32_t, uint32_t, uint32_t, uint32_t, void*, void*)
NET_FAIL_STUB(NetDll_getsockname, uint32_t, uint32_t, void*, void*)
NET_FAIL_STUB(NetDll_getpeername, uint32_t, uint32_t, void*, void*)
NET_FAIL_STUB(NetDll_ioctlsocket, uint32_t, uint32_t, uint32_t, void*)
#undef NET_FAIL_STUB

static uint32_t NetDll_WSAGetOverlappedResult(uint32_t caller, uint32_t socket, void* overlapped, be<uint32_t>* transferred, uint32_t wait, be<uint32_t>* flags)
{
    (void)caller; (void)socket; (void)overlapped; (void)wait; (void)flags;
    if (transferred)
        transferred->set(0);
    t_lastError = WSAENETDOWN;
    return 0; // FALSE
}

static uint32_t NetDll_select(uint32_t caller, uint32_t nfds, void* readfds, void* writefds, void* exceptfds, void* timeout)
{
    (void)caller; (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    return 0; // nenhum socket pronto
}

static uint32_t NetDll___WSAFDIsSet(uint32_t socket, void* set) { (void)socket; (void)set; return 0; }
static uint32_t NetDll_inet_addr(const char* text) { (void)text; return 0xFFFFFFFF; } // INADDR_NONE

static uint32_t NetDll_XNetDnsLookup(uint32_t caller, const char* name, uint32_t event, be<uint32_t>* dns)
{
    (void)caller; (void)name; (void)event;
    if (dns)
        dns->set(0);
    return WSAHOST_NOT_FOUND;
}

static uint32_t NetDll_XNetDnsRelease(uint32_t caller, uint32_t dns) { (void)caller; (void)dns; return 0; }

static uint32_t NetDll_XNetQosLookup(uint32_t caller, uint32_t count, void* a, void* b, void* c, uint32_t d, void* e,
                                     void* f, uint32_t probes, uint32_t bitsPerSecond, uint32_t flags, uint32_t event,
                                     be<uint32_t>* qos)
{
    (void)caller; (void)count; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    (void)probes; (void)bitsPerSecond; (void)flags; (void)event;
    if (qos)
        qos->set(0);
    return WSAENETDOWN;
}

static uint32_t NetDll_XNetQosRelease(uint32_t caller, uint32_t qos) { (void)caller; (void)qos; return 0; }
static uint32_t NetDll_XNetConnect(uint32_t caller, void* address) { (void)caller; (void)address; return WSAENETDOWN; }
static uint32_t NetDll_XNetServerToInAddr(uint32_t caller, uint32_t address, uint32_t service, void* out) { (void)caller; (void)address; (void)service; (void)out; return WSAENETDOWN; }
static uint32_t NetDll_XNetXnAddrToInAddr(uint32_t caller, void* xnaddr, void* key, void* out) { (void)caller; (void)xnaddr; (void)key; (void)out; return WSAENETDOWN; }
static uint32_t NetDll_XNetXnAddrToMachineId(uint32_t caller, void* xnaddr, be<uint64_t>* id) { (void)caller; (void)xnaddr; if (id) id->set(0); return WSAENETDOWN; }
static uint32_t NetDll_XNetUnregisterInAddr(uint32_t caller, uint32_t address) { (void)caller; (void)address; return 0; }

GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACleanup, NetDll_WSACleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetCleanup, NetDll_XNetCleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetLastError, NetDll_WSAGetLastError);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSASetLastError, NetDll_WSASetLastError);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetConnectStatus, NetDll_XNetGetConnectStatus);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetSetSystemLinkPort, NetDll_XNetSetSystemLinkPort);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACreateEvent, NetDll_WSACreateEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACloseEvent, NetDll_WSACloseEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAResetEvent, NetDll_WSAResetEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAWaitForMultipleEvents, NetDll_WSAWaitForMultipleEvents);
GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_socket);
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_closesocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_bind);
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_connect);
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_send);
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_recv);
GUEST_FUNCTION_HOOK(__imp__NetDll_sendto, NetDll_sendto);
GUEST_FUNCTION_HOOK(__imp__NetDll_recvfrom, NetDll_recvfrom);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSASendTo, NetDll_WSASendTo);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSARecvFrom, NetDll_WSARecvFrom);
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_setsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_getsockopt, NetDll_getsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_getsockname, NetDll_getsockname);
GUEST_FUNCTION_HOOK(__imp__NetDll_getpeername, NetDll_getpeername);
GUEST_FUNCTION_HOOK(__imp__NetDll_ioctlsocket, NetDll_ioctlsocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetOverlappedResult, NetDll_WSAGetOverlappedResult);
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_select);
GUEST_FUNCTION_HOOK(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet);
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsLookup, NetDll_XNetDnsLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsRelease, NetDll_XNetDnsRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosLookup, NetDll_XNetQosLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosRelease, NetDll_XNetQosRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetConnect, NetDll_XNetConnect);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetServerToInAddr, NetDll_XNetServerToInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetXnAddrToMachineId, NetDll_XNetXnAddrToMachineId);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetUnregisterInAddr, NetDll_XNetUnregisterInAddr);
