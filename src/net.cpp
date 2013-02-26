// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"
#include "irc.h"
#include "db.h"
#include "net.h"
#include "init.h"
#include "strlcpy.h"
#include "addrman.h"

#ifdef WIN32
#include <string.h>
#else
#include <netinet/in.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniwget.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace std;
using namespace boost;

static const int MAX_OUTBOUND_CONNECTIONS = 8;

void ThreadMessageHandler2(void* parg);
void ThreadSocketHandler2(void* parg);
void ThreadOpenConnections2(void* parg);
void ThreadOpenAddedConnections2(void* parg);
#ifdef USE_UPNP
void ThreadMapPort2(void* parg);
#endif
void ThreadDNSAddressSeed2(void* parg);
bool OpenNetworkConnection(const CAddress& addrConnect);



//
// Global state variables
//
bool fClient = false;
bool fAllowDNS = false;
static bool fUseUPnP = false;
uint64 nLocalServices = (fClient ? 0 : NODE_NETWORK);
CAddress addrLocalHost(CService("0.0.0.0", 0), nLocalServices);
static CNode* pnodeLocalHost = NULL;
uint64 nLocalHostNonce = 0;
array<int, THREAD_MAX> vnThreadsRunning;
static SOCKET hListenSocket = INVALID_SOCKET;
CAddrMan addrman;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64> mapAlreadyAskedFor;


set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;



unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", GetDefaultPort()));
}

void CNode::PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd)
{
    // Filter out duplicate requests
    if (pindexBegin == pindexLastGetBlocksBegin && hashEnd == hashLastGetBlocksEnd)
        return;
    pindexLastGetBlocksBegin = pindexBegin;
    hashLastGetBlocksEnd = hashEnd;

    PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
}



bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    loop
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
            if (strLine.size() >= 9000)
                return true;
        }
        else if (nBytes <= 0)
        {
            if (fShutdown)
                return false;
            if (nBytes < 0)
            {
                int nErr = WSAGetLastError();
                if (nErr == WSAEMSGSIZE)
                    continue;
                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
                {
                    Sleep(10);
                    continue;
                }
            }
            if (!strLine.empty())
                return true;
            if (nBytes == 0)
            {
                // socket closed
                printf("socket closed\n");
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                printf("recv failed: %d\n", nErr);
                return false;
            }
        }
    }
}



bool GetMyExternalIP2(const CService& addrConnect, const char* pszGet, const char* pszKeyword, CNetAddr& ipRet)
{
    SOCKET hSocket;
    if (!ConnectSocket(addrConnect, hSocket))
        return error("GetMyExternalIP() : connection to %s failed", addrConnect.ToString().c_str());

    send(hSocket, pszGet, strlen(pszGet), MSG_NOSIGNAL);

    string strLine;
    while (RecvLine(hSocket, strLine))
    {
        if (strLine.empty()) // HTTP response is separated from headers by blank line
        {
            loop
            {
                if (!RecvLine(hSocket, strLine))
                {
                    closesocket(hSocket);
                    return false;
                }
                if (pszKeyword == NULL)
                    break;
                if (strLine.find(pszKeyword) != string::npos)
                {
                    strLine = strLine.substr(strLine.find(pszKeyword) + strlen(pszKeyword));
                    break;
                }
            }
            closesocket(hSocket);
            if (strLine.find("<") != string::npos)
                strLine = strLine.substr(0, strLine.find("<"));
            strLine = strLine.substr(strspn(strLine.c_str(), " \t\n\r"));
            while (strLine.size() > 0 && isspace(strLine[strLine.size()-1]))
                strLine.resize(strLine.size()-1);
            CService addr(strLine,0,true);
            printf("GetMyExternalIP() received [%s] %s\n", strLine.c_str(), addr.ToString().c_str());
            if (!addr.IsValid() || !addr.IsRoutable())
                return false;
            ipRet.SetIP(addr);
            return true;
        }
    }
    closesocket(hSocket);
    return error("GetMyExternalIP() : connection closed");
}

// We now get our external IP from the IRC server first and only use this as a backup
bool GetMyExternalIP(CNetAddr& ipRet)
{
    CService addrConnect;
    const char* pszGet;
    const char* pszKeyword;

    if (fNoListen||fUseProxy)
        return false;

    for (int nLookup = 0; nLookup <= 1; nLookup++)
    for (int nHost = 1; nHost <= 2; nHost++)
    {
        // We should be phasing out our use of sites like these.  If we need
        // replacements, we should ask for volunteers to put this simple
        // php file on their web server that prints the client IP:
        //  <?php echo $_SERVER["REMOTE_ADDR"]; ?>
        if (nHost == 1)
        {
            addrConnect = CService("91.198.22.70",80); // checkip.dyndns.org

            if (nLookup == 1)
            {
                CService addrIP("checkip.dyndns.org", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET / HTTP/1.1\r\n"
                     "Host: checkip.dyndns.org\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = "Address:";
        }
        else if (nHost == 2)
        {
            addrConnect = CService("74.208.43.192", 80); // www.showmyip.com

            if (nLookup == 1)
            {
                CService addrIP("www.showmyip.com", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET /simple/ HTTP/1.1\r\n"
                     "Host: www.showmyip.com\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = NULL; // Returns just IP address
        }

        if (GetMyExternalIP2(addrConnect, pszGet, pszKeyword, ipRet))
            return true;
    }

    return false;
}

void ThreadGetMyExternalIP(void* parg)
{
    // Wait for IRC to get it first
    if (GetBoolArg("-irc", false))
    {
        for (int i = 0; i < 2 * 60; i++)
        {
            Sleep(1000);
            if (fGotExternalIP || fShutdown)
                return;
        }
    }

    // Fallback in case IRC fails to get it
    if (GetMyExternalIP(addrLocalHost))
    {
        printf("GetMyExternalIP() returned %s\n", addrLocalHost.ToStringIP().c_str());
        if (addrLocalHost.IsRoutable())
        {
            // If we already connected to a few before we had our IP, go back and addr them.
            // setAddrKnown automatically filters any duplicate sends.
            CAddress addr(addrLocalHost);
            addr.nTime = GetAdjustedTime();
            CRITICAL_BLOCK(cs_vNodes)
                BOOST_FOREACH(CNode* pnode, vNodes)
                    pnode->PushAddress(addr);
        }
    }
}





void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}





void AbandonRequests(void (*fn)(void*, CDataStream&), void* param1)
{
    // If the dialog might get closed before the reply comes back,
    // call this in the destructor so it doesn't get called after it's deleted.
    CRITICAL_BLOCK(cs_vNodes)
    {
        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            CRITICAL_BLOCK(pnode->cs_mapRequests)
            {
                for (map<uint256, CRequestTracker>::iterator mi = pnode->mapRequests.begin(); mi != pnode->mapRequests.end();)
                {
                    CRequestTracker& tracker = (*mi).second;
                    if (tracker.fn == fn && tracker.param1 == param1)
                        pnode->mapRequests.erase(mi++);
                    else
                        mi++;
                }
            }
        }
    }
}







//
// Subscription methods for the broadcast and subscription system.
// Channel numbers are message numbers, i.e. MSG_TABLE and MSG_PRODUCT.
//
// The subscription system uses a meet-in-the-middle strategy.
// With 100,000 nodes, if senders broadcast to 1000 random nodes and receivers
// subscribe to 1000 random nodes, 99.995% (1 - 0.99^1000) of messages will get through.
//

bool AnySubscribed(unsigned int nChannel)
{
    if (pnodeLocalHost->IsSubscribed(nChannel))
        return true;
    CRITICAL_BLOCK(cs_vNodes)
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->IsSubscribed(nChannel))
                return true;
    return false;
}

bool CNode::IsSubscribed(unsigned int nChannel)
{
    if (nChannel >= vfSubscribe.size())
        return false;
    return vfSubscribe[nChannel];
}

void CNode::Subscribe(unsigned int nChannel, unsigned int nHops)
{
    if (nChannel >= vfSubscribe.size())
        return;

    if (!AnySubscribed(nChannel))
    {
        // Relay subscribe
        CRITICAL_BLOCK(cs_vNodes)
            BOOST_FOREACH(CNode* pnode, vNodes)
                if (pnode != this)
                    pnode->PushMessage("subscribe", nChannel, nHops);
    }

    vfSubscribe[nChannel] = true;
}

void CNode::CancelSubscribe(unsigned int nChannel)
{
    if (nChannel >= vfSubscribe.size())
        return;

    // Prevent from relaying cancel if wasn't subscribed
    if (!vfSubscribe[nChannel])
        return;
    vfSubscribe[nChannel] = false;

    if (!AnySubscribed(nChannel))
    {
        // Relay subscription cancel
        CRITICAL_BLOCK(cs_vNodes)
            BOOST_FOREACH(CNode* pnode, vNodes)
                if (pnode != this)
                    pnode->PushMessage("sub-cancel", nChannel);
    }
}









CNode* FindNode(const CNetAddr& ip)
{
    CRITICAL_BLOCK(cs_vNodes)
    {
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CNetAddr)pnode->addr == ip)
                return (pnode);
    }
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    CRITICAL_BLOCK(cs_vNodes)
    {
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CService)pnode->addr == addr)
                return (pnode);
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, int64 nTimeout)
{
    if ((CNetAddr)addrConnect == (CNetAddr)addrLocalHost)
        return NULL;

    // Look for an existing connection
    CNode* pnode = FindNode((CService)addrConnect);
    if (pnode)
    {
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();
        return pnode;
    }

    /// debug print
    printf("trying connection %s lastseen=%.1fhrs\n",
        addrConnect.ToString().c_str(),
        (double)(addrConnect.nTime - GetAdjustedTime())/3600.0);

    addrman.Attempt(addrConnect);

    // Connect
    SOCKET hSocket;
    if (ConnectSocket(addrConnect, hSocket))
    {
        /// debug print
        printf("connected %s\n", addrConnect.ToString().c_str());

        // Set to non-blocking
#ifdef WIN32
        u_long nOne = 1;
        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
            printf("ConnectSocket() : ioctlsocket nonblocking setting failed, error %d\n", WSAGetLastError());
#else
        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
            printf("ConnectSocket() : fcntl nonblocking setting failed, error %d\n", errno);
#endif

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, false);
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();
        CRITICAL_BLOCK(cs_vNodes)
            vNodes.push_back(pnode);

        pnode->nTimeConnected = GetTime();
        return pnode;
    }
    else
    {
        return NULL;
    }
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET)
    {
        if (fDebug)
            printf("%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
        printf("disconnecting node %s\n", addr.ToString().c_str());
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
        vRecv.clear();
    }
}

void CNode::Cleanup()
{
    // All of a nodes broadcasts and subscriptions are automatically torn down
    // when it goes down, so a node has to stay up to keep its broadcast going.

    // Cancel subscriptions
    for (unsigned int nChannel = 0; nChannel < vfSubscribe.size(); nChannel++)
        if (vfSubscribe[nChannel])
            CancelSubscribe(nChannel);
}


void CNode::PushVersion()
{
    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64 nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (fUseProxy ? CAddress(CService("0.0.0.0",0)) : addr);
    CAddress addrMe = (fUseProxy || !addrLocalHost.IsRoutable() ? CAddress(CService("0.0.0.0",0)) : addrLocalHost);
    RAND_bytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight);
}





std::map<CNetAddr, int64> CNode::setBanned;
CCriticalSection CNode::cs_setBanned;

void CNode::ClearBanned()
{
    setBanned.clear();
}

bool CNode::IsBanned(CNetAddr ip)
{
    bool fResult = false;
    CRITICAL_BLOCK(cs_setBanned)
    {
        std::map<CNetAddr, int64>::iterator i = setBanned.find(ip);
        if (i != setBanned.end())
        {
            int64 t = (*i).second;
            if (GetTime() < t)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::Misbehaving(int howmuch)
{
    if (addr.IsLocal())
    {
        printf("Warning: local node %s misbehaving\n", addr.ToString().c_str());
        return false;
    }

    nMisbehavior += howmuch;
    if (nMisbehavior >= GetArg("-banscore", 100))
    {
        int64 banTime = GetTime()+GetArg("-bantime", 60*60*24);  // Default 24-hour ban
        CRITICAL_BLOCK(cs_setBanned)
            if (setBanned[addr] < banTime)
                setBanned[addr] = banTime;
        CloseSocketDisconnect();
        printf("Disconnected %s for misbehavior (score=%d)\n", addr.ToString().c_str(), nMisbehavior);
        return true;
    }
    return false;
}












void ThreadSocketHandler(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadSocketHandler(parg));
    try
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        ThreadSocketHandler2(parg);
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        PrintException(&e, "ThreadSocketHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadSocketHandler exiting\n");
}

void ThreadSocketHandler2(void* parg)
{
    printf("ThreadSocketHandler started\n");
    list<CNode*> vNodesDisconnected;
    int nPrevNodeCount = 0;

    loop
    {
        //
        // Disconnect nodes
        //
        CRITICAL_BLOCK(cs_vNodes)
        {
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecv.empty() && pnode->vSend.empty()))
                {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();
                    pnode->Cleanup();

                    // hold in disconnected pool until all refs are released
                    pnode->nReleaseTime = max(pnode->nReleaseTime, GetTime() + 15 * 60);
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                     TRY_CRITICAL_BLOCK(pnode->cs_vRecv)
                      TRY_CRITICAL_BLOCK(pnode->cs_mapRequests)
                       TRY_CRITICAL_BLOCK(pnode->cs_inventory)
                        fDelete = true;
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if (vNodes.size() != nPrevNodeCount)
        {
            nPrevNodeCount = vNodes.size();
            MainFrameRepaint();
        }


        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        if(hListenSocket != INVALID_SOCKET)
        {
            FD_SET(hListenSocket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket);
            have_fds = true;
        }
        CRITICAL_BLOCK(cs_vNodes)
        {
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                FD_SET(pnode->hSocket, &fdsetRecv);
                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = max(hSocketMax, pnode->hSocket);
                have_fds = true;
                TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                    if (!pnode->vSend.empty())
                        FD_SET(pnode->hSocket, &fdsetSend);
            }
        }

        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        if (fShutdown)
            return;
        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                printf("socket select error %d\n", nErr);
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            Sleep(timeout.tv_usec/1000);
        }


        //
        // Accept new connections
        //
        if (hListenSocket != INVALID_SOCKET && FD_ISSET(hListenSocket, &fdsetRecv))
        {
            struct sockaddr_in sockaddr;
            socklen_t len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr*)&sockaddr, &len);
            CAddress addr;
            int nInbound = 0;

            if (hSocket != INVALID_SOCKET)
                addr = CAddress(sockaddr);

            CRITICAL_BLOCK(cs_vNodes)
                BOOST_FOREACH(CNode* pnode, vNodes)
                if (pnode->fInbound)
                    nInbound++;

            if (hSocket == INVALID_SOCKET)
            {
                if (WSAGetLastError() != WSAEWOULDBLOCK)
                    printf("socket error accept failed: %d\n", WSAGetLastError());
            }
            else if (nInbound >= GetArg("-maxconnections", 125) - MAX_OUTBOUND_CONNECTIONS)
            {
                CRITICAL_BLOCK(cs_setservAddNodeAddresses)
                    if (!setservAddNodeAddresses.count(addr))
                        closesocket(hSocket);
            }
            else if (CNode::IsBanned(addr))
            {
                printf("connection from %s dropped (banned)\n", addr.ToString().c_str());
                closesocket(hSocket);
            }
            else
            {
                printf("accepted connection %s\n", addr.ToString().c_str());
                CNode* pnode = new CNode(hSocket, addr, true);
                pnode->AddRef();
                CRITICAL_BLOCK(cs_vNodes)
                    vNodes.push_back(pnode);
            }
        }


        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        CRITICAL_BLOCK(cs_vNodes)
        {
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (fShutdown)
                return;

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_CRITICAL_BLOCK(pnode->cs_vRecv)
                {
                    CDataStream& vRecv = pnode->vRecv;
                    unsigned int nPos = vRecv.size();

                    if (nPos > ReceiveBufferSize()) {
                        if (!pnode->fDisconnect)
                            printf("socket recv flood control disconnect (%d bytes)\n", vRecv.size());
                        pnode->CloseSocketDisconnect();
                    }
                    else {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vRecv.resize(nPos + nBytes);
                            memcpy(&vRecv[nPos], pchBuf, nBytes);
                            pnode->nLastRecv = GetTime();
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                printf("socket closed\n");
                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    printf("socket recv error %d\n", nErr);
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                {
                    CDataStream& vSend = pnode->vSend;
                    if (!vSend.empty())
                    {
                        int nBytes = send(pnode->hSocket, &vSend[0], vSend.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vSend.erase(vSend.begin(), vSend.begin() + nBytes);
                            pnode->nLastSend = GetTime();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                printf("socket send error %d\n", nErr);
                                pnode->CloseSocketDisconnect();
                            }
                        }
                        if (vSend.size() > SendBufferSize()) {
                            if (!pnode->fDisconnect)
                                printf("socket send flood control disconnect (%d bytes)\n", vSend.size());
                            pnode->CloseSocketDisconnect();
                        }
                    }
                }
            }

            //
            // Inactivity checking
            //
            if (pnode->vSend.empty())
                pnode->nLastSendEmpty = GetTime();
            if (GetTime() - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    printf("socket no message in first 60 seconds, %d %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0);
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastSend > 90*60 && GetTime() - pnode->nLastSendEmpty > 90*60)
                {
                    printf("socket not sending\n");
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastRecv > 90*60)
                {
                    printf("socket inactivity timeout\n");
                    pnode->fDisconnect = true;
                }
            }
        }
        CRITICAL_BLOCK(cs_vNodes)
        {
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        Sleep(10);
    }
}









#ifdef USE_UPNP
void ThreadMapPort(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadMapPort(parg));
    try
    {
        vnThreadsRunning[THREAD_UPNP]++;
        ThreadMapPort2(parg);
        vnThreadsRunning[THREAD_UPNP]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(&e, "ThreadMapPort()");
    } catch (...) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(NULL, "ThreadMapPort()");
    }
    printf("ThreadMapPort exiting\n");
}

void ThreadMapPort2(void* parg)
{
    printf("ThreadMapPort started\n");

    char port[6];
    sprintf(port, "%d", GetListenPort());

    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#else
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1)
    {
        if (!addrLocalHost.IsRoutable())
        {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
                printf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else
            {
                if(externalIPAddress[0])
                {
                    printf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    CAddress addrExternalFromUPnP(CService(externalIPAddress, 0), nLocalServices);
                    if (addrExternalFromUPnP.IsRoutable())
                        addrLocalHost = addrExternalFromUPnP;
                }
                else
                    printf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "Bitcoin " + FormatFullVersion();
#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port, port, lanaddr, strDesc.c_str(), "TCP", 0);
#else
        /* miniupnpc 1.6 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port, port, lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

        if(r!=UPNPCOMMAND_SUCCESS)
            printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                port, port, lanaddr, r, strupnperror(r));
        else
            printf("UPnP Port Mapping successful.\n");
        int i = 1;
        loop {
            if (fShutdown || !fUseUPnP)
            {
                r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port, "TCP", 0);
                printf("UPNP_DeletePortMapping() returned : %d\n", r);
                freeUPNPDevlist(devlist); devlist = 0;
                FreeUPNPUrls(&urls);
                return;
            }
            if (i % 600 == 0) // Refresh every 20 minutes
            {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port, port, lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port, port, lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    printf("UPnP Port Mapping successful.\n");;
            }
            Sleep(2000);
            i++;
        }
    } else {
        printf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist); devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
        loop {
            if (fShutdown || !fUseUPnP)
                return;
            Sleep(2000);
        }
    }
}

void MapPort(bool fMapPort)
{
    if (fUseUPnP != fMapPort)
    {
        fUseUPnP = fMapPort;
    }
    if (fUseUPnP && vnThreadsRunning[THREAD_UPNP] < 1)
    {
        if (!CreateThread(ThreadMapPort, NULL))
            printf("Error: ThreadMapPort(ThreadMapPort) failed\n");
    }
}
#else
void MapPort(bool /* unused fMapPort */)
{
    // Intentionally left blank.
}
#endif









// DNS seeds
// Each pair gives a source name and a seed name.
// The first name is used as information source for addrman.
// The second name should resolve to a list of seed addresses.
static const char *strDNSSeed[][2] = {
    {"xf2.org", "bitseed.xf2.org"},
    {"bluematt.me", "dnsseed.bluematt.me"},
    {"bitcoin.sipa.be", "seed.bitcoin.sipa.be"},
    {"dashjr.org", "dnsseed.bitcoin.dashjr.org"},
};

void ThreadDNSAddressSeed(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadDNSAddressSeed(parg));
    try
    {
        vnThreadsRunning[THREAD_DNSSEED]++;
        ThreadDNSAddressSeed2(parg);
        vnThreadsRunning[THREAD_DNSSEED]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        PrintException(&e, "ThreadDNSAddressSeed()");
    } catch (...) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadDNSAddressSeed exiting\n");
}

void ThreadDNSAddressSeed2(void* parg)
{
    printf("ThreadDNSAddressSeed started\n");
    int found = 0;

    if (!fTestNet)
    {
        printf("Loading addresses from DNS seeds (could take a while)\n");

        for (unsigned int seed_idx = 0; seed_idx < ARRAYLEN(strDNSSeed); seed_idx++) {
            vector<CNetAddr> vaddr;
            vector<CAddress> vAdd;
            if (LookupHost(strDNSSeed[seed_idx][1], vaddr))
            {
                BOOST_FOREACH(CNetAddr& ip, vaddr)
                {
                    int nOneDay = 24*3600;
                    CAddress addr = CAddress(CService(ip, GetDefaultPort()));
                    addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
            }
            addrman.Add(vAdd, CNetAddr(strDNSSeed[seed_idx][0], true));
        }
    }

    printf("%d addresses found from DNS seeds\n", found);
}












unsigned int pnSeed[] =
{
    0xe473042e, 0xb177f2ad, 0xd63f3fb2, 0xf864f736, 0x44a23ac7, 0xcf6d9650, 0xd648042e, 0x0536f447,
    0x3c654ed0, 0x3e16a5bc, 0xa38e09b0, 0xdfae795b, 0xabfeca5b, 0x94ad7840, 0xf3b9f1c7, 0xbe70e0ad,
    0x3bbd09b0, 0x8d0c7dd5, 0x3b2a7332, 0x1a06175e, 0x581f175e, 0xca0d2dcc, 0x0fdbc658, 0xcf591ec7,
    0x295a12b2, 0xb4707bce, 0x68bb09b0, 0x4e735747, 0x89709553, 0x05a7814e, 0x5b8ec658, 0x402c5512,
    0xe80d0905, 0x17681a5e, 0xc02aa748, 0x9f811741, 0x5f321cb0, 0x23e1ee47, 0xaf7f170c, 0xaa240ab0,
    0xedea6257, 0x76106bc1, 0x2cf310cc, 0x08612acb, 0x9c682e4e, 0x8e963c6c, 0x443c795b, 0x22e246b8,
    0xfa1f2dcc, 0x90118140, 0x3821042e, 0x33c3fd2e, 0x10046d5b, 0x40d14b3e, 0x7fb8f8ce, 0x67696550,
    0xeeecbe58, 0x4f341745, 0x46b8fbd5, 0xc8463932, 0x6b73e862, 0x4c715932, 0x4a6785d5, 0xce3a64c2,
    0xde9604c7, 0x9b06884f, 0x18002a45, 0xea9bc345, 0xc4f1c658, 0xe475c1c7, 0xdd3e795b, 0x9722175e,
    0x34562f4e, 0x66c46e4e, 0x40bb1243, 0x7d9171d0, 0x17b8dbd5, 0x63cbfd2e, 0x1a08b8d8, 0x6175a73b,
    0x228d2660, 0x8627c658, 0x9c566644, 0x38cca5bc, 0x3089de5b, 0x92e25f5d, 0xa393f73f, 0xcc92dc3e,
    0x27487446, 0x62cbfd2e, 0x9d983b45, 0xf72a09b0, 0xf75f042e, 0x6434bb6a, 0xb29e77d8, 0x19be4fd9,
    0x76443243, 0x9dd72645, 0x694cef43, 0x89c2efd5, 0x5f1c5058, 0x46c6e45b, 0xe1391b40, 0x77ccefd5,
    0x472e5a6d, 0x85709553, 0xdd4f5d4c, 0x64ef5a46, 0x7f0ae502, 0xcf08d850, 0x3460042e, 0xeafa2d42,
    0x793c9044, 0x9d094746, 0x1ab9b153, 0xbfe9a5bc, 0x34771fb0, 0xb7722e32, 0x1168964b, 0x19b06ab8,
    0x19243b25, 0x13188045, 0xb4070905, 0x728ebb5d, 0x44f24ac8, 0xa317fead, 0x642f6a57, 0x3d951f32,
    0x3d312e4e, 0xfac4d048, 0xefc4dd50, 0x52b9f1c7, 0xc14d3cc3, 0x0219ea44, 0x3b79d058, 0xfa217242,
    0x39c80647, 0xfb697252, 0x1d495a42, 0x0aa81f4e, 0x58249ab8, 0xe6a8e6c3, 0x2bc4dad8, 0x85963c6c,
    0xa4ce09b0, 0x2005f536, 0x5cc2703e, 0x1992de43, 0x74e86b4c, 0xe7085653, 0xf5e15a51, 0xb4872b60,
    0x29e2b162, 0xa07ea053, 0x8229fd18, 0x4562ec4d, 0x8dec814e, 0x36cfa4cf, 0x96461032, 0x3c8770de,
    0xd10a1f5f, 0x95934641, 0x97cd65d0, 0x2e35324a, 0x2566ba1f, 0x1ca1a9d1, 0xb808b8d5, 0xf9a24a5d,
    0xafc8d431, 0xe4b8d9b2, 0x0f5321b2, 0x330bc658, 0x74b347ce, 0x972babd5, 0x044f7d4f, 0x06562f4e,
    0x8b8d3c6c, 0x3507c658, 0xe4174e4d, 0xf1c009b0, 0x52249ab8, 0x27211772, 0xf6a9ba59, 0x7a391b40,
    0x855dc6c0, 0x291f20b2, 0xe29bc345, 0x90963c6c, 0x0af70732, 0x4242a91f, 0x4c531d48, 0xa32df948,
    0x627e3044, 0x65be1f54, 0x1a0cbf83, 0x6a443532, 0x8d5f1955, 0xbafa8132, 0x3534bdd5, 0xca019dd9,
    0x8a0d9332, 0x5584e7d8, 0x7cd1f25e, 0xeabe3fb2, 0x2945d0d1, 0x46415718, 0x70d6042e, 0x99eb76d0,
    0x9ece09b0, 0xb3777418, 0x5e5e91d9, 0x237a3ab0, 0xf512b62e, 0x45dec347, 0x59b7f862, 0x4c443b25,
    0x3cc6484b, 0x9a8ec6d1, 0x021eea44, 0xc9483944, 0xfd567e32, 0xfd204bb2, 0xc5330bcc, 0x5202894e,
    0xf9e309b0, 0x4cc17557, 0xdb9064ae, 0xe19e77d8, 0x25857f60, 0xeb4a15ad, 0x1f47f554, 0xea4472d9,
    0xd20de593, 0xf5733b25, 0x11892b54, 0x5729d35f, 0xe6188cd1, 0x488b132e, 0x541c534a, 0xa8e854ae,
    0xa255a66c, 0x33688763, 0xc6629ac6, 0xc20a6265, 0xcd92a059, 0x72029d3b, 0x4c298f5e, 0x51452e4e,
    0xbb065058, 0x15fd2dcc, 0xf40c135e, 0x615a0bad, 0x0c6a6805, 0x4971a7ad, 0x17f2a5d5, 0xf8babf47,
    0xb61f50ad, 0x4e1451b1, 0xf72d9252, 0x5c2abe58, 0xbd987c61, 0x084ae5cf, 0x20781fb0, 0x38b0f160,
    0x18aac705, 0x14f86dc1, 0x5556f481, 0x0a36c144, 0xeb446e4c, 0x2c1c0d6c, 0xbd0ff860, 0x869f92db,
    0x36c94f4c, 0x05502444, 0x148fe55b, 0xd5301e59, 0xd57a8f45, 0x110dc04a, 0x8670fc36, 0xee733b25,
    0xca56f481, 0x2a5c3bae, 0x844b0905, 0x1e51fe53, 0x0241c244, 0x59c0614e, 0x94e70a55, 0x7312fead,
    0xb735be44, 0xa55d0905, 0x2f63962e, 0x14a4e15b, 0x63f8f05c, 0x62d0d262, 0x3cab41ad, 0x87f1b1cb,
    0x018da6b8, 0xb3967dd5, 0xcb56f481, 0x685ad718, 0x3b4aeeca, 0x8d106bc1, 0x51180905, 0x72660f48,
    0x1521a243, 0x5b56f481, 0x6390e560, 0xdd61464e, 0x58353b25, 0x553fc062, 0x27c45d59, 0xacc62e4e,
    0x0d5a1cd9, 0x7f65f442, 0xbdeef660, 0xf1bd1855, 0xf8473cae, 0x13b120b2, 0x442440d0, 0x53fd4352,
    0xa305fc57, 0x458be84d, 0x639ce1c3, 0xebaaee47, 0x95e2c247, 0xf056f481, 0x6256f481, 0x1d87c65e,
    0x0a453418, 0x5beb175e, 0xd64f1618, 0xc360795b, 0x2fbf5753, 0xa8c58e53, 0x651cec52, 0x9d37b043,
    0x124a9758, 0x5242e4a9, 0x89913c6c, 0x880efe2e, 0x2f2f2f0c, 0x72b26751, 0x2896e46d, 0x80f4166c,
    0x320d59ad, 0xc50151d0, 0x11a8aa43, 0xccf56057, 0x5fbad118, 0x4719b151, 0x2b5f4bc0, 0x4d7a4a50,
    0xad06e047, 0x62ef5a46, 0x5aebde58, 0xdf7aa66c, 0x851acb50, 0x66b9a559, 0x3e9bb153, 0xcc512f2e,
    0xc073b08e, 0xd519be58, 0xe981ea4d, 0x12fd50cb, 0x378739ad, 0x06683cae, 0xa22310b2, 0xc185c705,
    0x8741b545, 0xa26c8318, 0x22d5bc43, 0x39201ec0, 0x68581e3e, 0xdc9bcf62, 0xd508cc82, 0xb149675b,
    0x4c9609b0, 0x84feb84c, 0x08291e2e, 0xfd2253b2, 0x1fd269c1, 0xc9483932, 0x4d641fb0, 0x7d37c918,
    0xa9de20ad, 0x77e2d655, 0x6d421b59, 0xd7668f80, 0xced09b62, 0xa9e5a5bc, 0xa4074e18, 0x60fc5ecc,
    0x01300148, 0x68062444, 0xb4224847, 0xed3aa443, 0xb772fb43, 0x9f56f481, 0x220dfd18, 0x8e1c3d6c,
    0xc44f09b0, 0x7df2bb73, 0xe22fb844, 0xea534242, 0xb6a755d4, 0xa036654b, 0x138ece5b, 0xda65d3c3,
    0x955871bc, 0x792124b0, 0xfc82594c, 0x851d494b, 0x2c7aee47, 0x26af46b8, 0x1416252e, 0xa8abb944,
    0x36c49d25, 0x674f645d, 0x363646b8, 0x9e1a2942, 0x66d0c154, 0xc6c2a545, 0x3570f2ad, 0xe7d547c7,
    0x7d104932, 0x18cb9c18, 0x1dcfa4cf, 0xd156f481, 0x2a02b91f, 0x3eeb3fa8, 0xcac4175e, 0x34146d42,
    0x994c4d46, 0x5666f440, 0x85d6713e, 0x5ecb296c, 0x0ea0ae46, 0x87e69f42, 0xc58409b0, 0x1f3436ae,
    0x21dc6a57, 0x4ad1cd42, 0xfb8c1a4c, 0x52d3dab2, 0x3769894b, 0xb52f1c62, 0x3677916d, 0x82b3fe57,
    0x493d4ac6, 0x9f963c6c, 0x5d91ff60, 0x458e0dad, 0xa49d0947, 0x491a3e18, 0x4aadcd5b, 0x0e46494b,
    0x1d1610ad, 0x1a10af5d, 0x4956f481, 0x207a3eae, 0x77e73244, 0xfa3b8742, 0x3261fc36, 0xfcebf536,
    0x1662e836, 0xf655f636, 0xa2dbd0ad, 0x23036693, 0x30448432, 0xa2b03463, 0x30730344, 0x8e4a6882,
    0x0c50a1cb, 0xc8d8c06b, 0xc9cd6191, 0xf443db50, 0xa9553c50, 0x23145847, 0xc35da66c, 0x29c12a60,
    0x55c2b447, 0x7434f75c, 0x61660640, 0xde2a7018, 0xc639494c, 0x1c306fce, 0x19b89244, 0xd29a6462,
    0x462cd1b2, 0x29902f44, 0x2817fa53, 0x21a30905, 0x7777ae46, 0x288443a1, 0x7bee5148, 0xc2a8b043,
    0xf5c3d35f, 0x2311ef84, 0x57de08a4, 0x6b221bb2, 0xf2625846, 0x4b9e09b0, 0xa24f880e, 0x22b11447,
    0xb3a0c744, 0x919e77d8, 0xec8b64ae, 0xff5c8d45, 0x7b15b484, 0x32679a5f, 0xba80b62e, 0x05c25c61,
    0x60014746, 0x5e8fb04c, 0xe67c0905, 0x4329c658, 0xac8fe555, 0xf875e647, 0x67406386, 0x35ceea18,
    0xbb79484b, 0xd7b9fa62, 0x238209b0, 0x208a1d32, 0x9630995e, 0x039c1318, 0x6e48006c, 0x60582344,
    0xadbb0150, 0x853fd462, 0x03772e4e, 0x652ce960, 0x49b630ad, 0x9993af43, 0x3735b34b, 0x548a07d9,
    0x55a44aad, 0xa23d1bcc, 0xfdbb2f4e, 0x530b24a0, 0x0a44b451, 0x6827c657, 0x1f66494b, 0x4e680a47,
    0x77e7b747, 0xa5eb3fa8, 0x6649764a, 0xd4e76c4b, 0x2c691fb0, 0xf1292e44, 0xc6d6c774, 0x85d23775,
    0x28275f4d, 0x259ae46d, 0x02424e81, 0x5f16be58, 0xe707c658, 0x49eae5c7, 0xd5d147ad, 0x9a7abdc3,
    0xe8ac7fc7, 0x84ec3aae, 0xc24942d0, 0x294aa318, 0x08ac3d18, 0x8894042e, 0xb24609b0, 0x9bcaab58,
    0xc400f712, 0xd5c512b8, 0x2c02cc62, 0x25080fd8, 0xed74a847, 0x18a5ec5e, 0x9850ec6d, 0xf8909758,
    0x7f56f481, 0x4496f23c, 0xae27784f, 0xcb7cd93e, 0x06e32860, 0x50b9a84f, 0x3660434a, 0x09161f5f,
    0x900486bc, 0x08055459, 0xe7ec1017, 0x7e39494c, 0x4f443b25, 0x14751a8a, 0x717d03d4, 0xbd0e24d8,
    0x054b6f56, 0x854c496c, 0xd92a454a, 0xc39bd054, 0x6093614b, 0x9dbad754, 0x5bf0604a, 0x99f22305
};

void DumpAddresses()
{
    CAddrDB adb;
    adb.WriteAddrman(addrman);
}

void ThreadDumpAddress2(void* parg)
{
    vnThreadsRunning[THREAD_DUMPADDRESS]++;
    while (!fShutdown)
    {
        DumpAddresses();
        vnThreadsRunning[THREAD_DUMPADDRESS]--;
        Sleep(100000);
        vnThreadsRunning[THREAD_DUMPADDRESS]++;
    }
    vnThreadsRunning[THREAD_DUMPADDRESS]--;
}

void ThreadDumpAddress(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadDumpAddress(parg));
    try
    {
        ThreadDumpAddress2(parg);
    }
    catch (std::exception& e) {
        PrintException(&e, "ThreadDumpAddress()");
    }
    printf("ThreadDumpAddress exiting\n");
}

void ThreadOpenConnections(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadOpenConnections(parg));
    try
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        ThreadOpenConnections2(parg);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(&e, "ThreadOpenConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenConnections()");
    }
    printf("ThreadOpenConnections exiting\n");
}

void ThreadOpenConnections2(void* parg)
{
    printf("ThreadOpenConnections started\n");

    // Connect to specific addresses
    if (mapArgs.count("-connect"))
    {
        for (int64 nLoop = 0;; nLoop++)
        {
            BOOST_FOREACH(string strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr(CService(strAddr, GetDefaultPort(), fAllowDNS));
                if (addr.IsValid())
                    OpenNetworkConnection(addr);
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    Sleep(500);
                    if (fShutdown)
                        return;
                }
            }
        }
    }

    // Initiate network connections
    int64 nStart = GetTime();
    loop
    {
        int nOutbound = 0;

        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        Sleep(500);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;

        // Limit outbound connections
        loop
        {
            nOutbound = 0;
            CRITICAL_BLOCK(cs_vNodes)
                BOOST_FOREACH(CNode* pnode, vNodes)
                    if (!pnode->fInbound)
                        nOutbound++;
            int nMaxOutboundConnections = MAX_OUTBOUND_CONNECTIONS;
            nMaxOutboundConnections = min(nMaxOutboundConnections, (int)GetArg("-maxconnections", 125));
            if (nOutbound < nMaxOutboundConnections)
                break;
            vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
            Sleep(2000);
            vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
            if (fShutdown)
                return;
        }

        // Add seed nodes if IRC isn't working
        bool fTOR = (fUseProxy && addrProxy.GetPort() == 9050);
        if (addrman.size()==0 && (GetTime() - nStart > 60 || fTOR) && !fTestNet)
        {
            std::vector<CAddress> vAdd;
            for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
            {
                // It'll only connect to one or two seed nodes because once it connects,
                // it'll get a pile of addresses with newer timestamps.
                // Seed nodes are given a random 'last seen time' of between one and two
                // weeks ago.
                const int64 nOneWeek = 7*24*60*60;
                struct in_addr ip;
                memcpy(&ip, &pnSeed[i], sizeof(ip));
                CAddress addr(CService(ip, GetDefaultPort()));
                addr.nTime = GetTime()-GetRand(nOneWeek)-nOneWeek;
                vAdd.push_back(addr);
            }
            addrman.Add(vAdd, CNetAddr("127.0.0.1"));
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        set<vector<unsigned char> > setConnected;
        CRITICAL_BLOCK(cs_vNodes)
            BOOST_FOREACH(CNode* pnode, vNodes)
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                }

        int64 nANow = GetAdjustedTime();

        int nTries = 0;
        loop
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select(10 + min(nOutbound,8)*10);

            // if we selected an invalid address, restart
            if (!addr.IsIPv4() || !addr.IsValid() || setConnected.count(addr.GetGroup()) || addr == addrLocalHost)
                break;

            nTries++;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect);
    }
}

void ThreadOpenAddedConnections(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadOpenAddedConnections(parg));
    try
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        ThreadOpenAddedConnections2(parg);
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(&e, "ThreadOpenAddedConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenAddedConnections()");
    }
    printf("ThreadOpenAddedConnections exiting\n");
}

void ThreadOpenAddedConnections2(void* parg)
{
    printf("ThreadOpenAddedConnections started\n");

    if (mapArgs.count("-addnode") == 0)
        return;

    vector<vector<CService> > vservAddressesToAdd(0);
    BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"])
    {
        vector<CService> vservNode(0);
        if(Lookup(strAddNode.c_str(), vservNode, GetDefaultPort(), fAllowDNS, 0))
        {
            vservAddressesToAdd.push_back(vservNode);
            CRITICAL_BLOCK(cs_setservAddNodeAddresses)
                BOOST_FOREACH(CService& serv, vservNode)
                    setservAddNodeAddresses.insert(serv);
        }
    }
    loop
    {
        vector<vector<CService> > vservConnectAddresses = vservAddressesToAdd;
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fAllowDNS)
        CRITICAL_BLOCK(cs_vNodes)
            BOOST_FOREACH(CNode* pnode, vNodes)
                for (vector<vector<CService> >::iterator it = vservConnectAddresses.begin(); it != vservConnectAddresses.end(); it++)
                    BOOST_FOREACH(CService& addrNode, *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = vservConnectAddresses.erase(it);
                            it--;
                            break;
                        }
        BOOST_FOREACH(vector<CService>& vserv, vservConnectAddresses)
        {
            OpenNetworkConnection(CAddress(*(vserv.begin())));
            Sleep(500);
            if (fShutdown)
                return;
        }
        if (fShutdown)
            return;
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        Sleep(120000); // Retry every 2 minutes
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        if (fShutdown)
            return;
    }
}

bool OpenNetworkConnection(const CAddress& addrConnect)
{
    //
    // Initiate outbound network connection
    //
    if (fShutdown)
        return false;
    if ((CNetAddr)addrConnect == (CNetAddr)addrLocalHost || !addrConnect.IsIPv4() ||
        FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect))
        return false;

    vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    CNode* pnode = ConnectNode(addrConnect);
    vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
    if (fShutdown)
        return false;
    if (!pnode)
        return false;
    pnode->fNetworkNode = true;

    return true;
}








void ThreadMessageHandler(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadMessageHandler(parg));
    try
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        ThreadMessageHandler2(parg);
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(&e, "ThreadMessageHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(NULL, "ThreadMessageHandler()");
    }
    printf("ThreadMessageHandler exiting\n");
}

void ThreadMessageHandler2(void* parg)
{
    printf("ThreadMessageHandler started\n");
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (!fShutdown)
    {
        vector<CNode*> vNodesCopy;
        CRITICAL_BLOCK(cs_vNodes)
        {
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            // Receive messages
            TRY_CRITICAL_BLOCK(pnode->cs_vRecv)
                ProcessMessages(pnode);
            if (fShutdown)
                return;

            // Send messages
            TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                SendMessages(pnode, pnode == pnodeTrickle);
            if (fShutdown)
                return;
        }

        CRITICAL_BLOCK(cs_vNodes)
        {
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        // Wait and allow messages to bunch up.
        // Reduce vnThreadsRunning so StopNode has permission to exit while
        // we're sleeping, but we must always check fShutdown after doing this.
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        Sleep(100);
        if (fRequestShutdown)
            StartShutdown();
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        if (fShutdown)
            return;
    }
}






bool BindListenPort(string& strError)
{
    strError = "";
    int nOne = 1;
    addrLocalHost.SetPort(GetListenPort());

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        printf("%s\n", strError.c_str());
        return false;
    }
#endif

    // Create socket for listening for incoming connections
    hListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif

#ifndef WIN32
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
#endif

#ifdef WIN32
    // Set to non-blocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    // The sockaddr_in structure specifies the address family,
    // IP address, and port for the socket that is being bound
    struct sockaddr_in sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = INADDR_ANY; // bind to all IPs on this computer
    sockaddr.sin_port = htons(GetListenPort());
    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to port %d on this computer.  Bitcoin is probably already running."), ntohs(sockaddr.sin_port));
        else
            strError = strprintf("Error: Unable to bind to port %d on this computer (bind returned error %d)", ntohs(sockaddr.sin_port), nErr);
        printf("%s\n", strError.c_str());
        return false;
    }
    printf("Bound to port %d\n", ntohs(sockaddr.sin_port));

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    return true;
}

void StartNode(void* parg)
{
#ifdef USE_UPNP
#if USE_UPNP
    fUseUPnP = GetBoolArg("-upnp", true);
#else
    fUseUPnP = GetBoolArg("-upnp", false);
#endif
#endif

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

#ifdef WIN32
    // Get local host IP
    char pszHostName[1000] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
                if (!addr.IsLocal())
                {
                    addrLocalHost.SetIP(addr);
                    break;
                }
    }
#else
    // Get local host IP
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            char pszIP[100];
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                if (inet_ntop(ifa->ifa_addr->sa_family, (void*)&(s4->sin_addr), pszIP, sizeof(pszIP)) != NULL)
                    printf("ipv4 %s: %s\n", ifa->ifa_name, pszIP);

                // Take the first IP that isn't loopback 127.x.x.x
                CAddress addr(CService(s4->sin_addr, GetListenPort()), nLocalServices);
                if (addr.IsValid() && !addr.IsLocal())
                {
                    addrLocalHost = addr;
                    break;
                }
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                if (inet_ntop(ifa->ifa_addr->sa_family, (void*)&(s6->sin6_addr), pszIP, sizeof(pszIP)) != NULL)
                    printf("ipv6 %s: %s\n", ifa->ifa_name, pszIP);
            }
        }
        freeifaddrs(myaddrs);
    }
#endif
    printf("addrLocalHost = %s\n", addrLocalHost.ToString().c_str());

    if (fUseProxy || mapArgs.count("-connect") || fNoListen)
    {
        // Proxies can't take incoming connections
        addrLocalHost.SetIP(CNetAddr("0.0.0.0"));
        printf("addrLocalHost = %s\n", addrLocalHost.ToString().c_str());
    }
    else
    {
        CreateThread(ThreadGetMyExternalIP, NULL);
    }

    //
    // Start threads
    //

    if (!GetBoolArg("-dnsseed", true))
        printf("DNS seeding disabled\n");
    else
        if (!CreateThread(ThreadDNSAddressSeed, NULL))
            printf("Error: CreateThread(ThreadDNSAddressSeed) failed\n");

    // Map ports with UPnP
    if (fHaveUPnP)
        MapPort(fUseUPnP);

    // Get addresses from IRC and advertise ours
    if (!CreateThread(ThreadIRCSeed, NULL))
        printf("Error: CreateThread(ThreadIRCSeed) failed\n");

    // Send and receive from sockets, accept connections
    if (!CreateThread(ThreadSocketHandler, NULL))
        printf("Error: CreateThread(ThreadSocketHandler) failed\n");

    // Initiate outbound connections from -addnode
    if (!CreateThread(ThreadOpenAddedConnections, NULL))
        printf("Error: CreateThread(ThreadOpenAddedConnections) failed\n");

    // Initiate outbound connections
    if (!CreateThread(ThreadOpenConnections, NULL))
        printf("Error: CreateThread(ThreadOpenConnections) failed\n");

    // Process messages
    if (!CreateThread(ThreadMessageHandler, NULL))
        printf("Error: CreateThread(ThreadMessageHandler) failed\n");

    // Dump network addresses
    if (!CreateThread(ThreadDumpAddress, NULL))
        printf("Error; CreateThread(ThreadDumpAddress) failed\n");

    // Generate coins in the background
    GenerateBitcoins(GetBoolArg("-gen", false), pwalletMain);
}

bool StopNode()
{
    printf("StopNode()\n");
    fShutdown = true;
    nTransactionsUpdated++;
    int64 nStart = GetTime();
    do
    {
        int nThreadsRunning = 0;
        for (int n = 0; n < THREAD_MAX; n++)
            nThreadsRunning += vnThreadsRunning[n];
        if (nThreadsRunning == 0)
            break;
        if (GetTime() - nStart > 20)
            break;
        Sleep(20);
    } while(true);
    if (vnThreadsRunning[THREAD_SOCKETHANDLER] > 0) printf("ThreadSocketHandler still running\n");
    if (vnThreadsRunning[THREAD_OPENCONNECTIONS] > 0) printf("ThreadOpenConnections still running\n");
    if (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0) printf("ThreadMessageHandler still running\n");
    if (vnThreadsRunning[THREAD_MINER] > 0) printf("ThreadBitcoinMiner still running\n");
    if (vnThreadsRunning[THREAD_RPCSERVER] > 0) printf("ThreadRPCServer still running\n");
    if (fHaveUPnP && vnThreadsRunning[THREAD_UPNP] > 0) printf("ThreadMapPort still running\n");
    if (vnThreadsRunning[THREAD_DNSSEED] > 0) printf("ThreadDNSAddressSeed still running\n");
    if (vnThreadsRunning[THREAD_ADDEDCONNECTIONS] > 0) printf("ThreadOpenAddedConnections still running\n");
    if (vnThreadsRunning[THREAD_DUMPADDRESS] > 0) printf("ThreadDumpAddresses still running\n");
    while (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0 || vnThreadsRunning[THREAD_RPCSERVER] > 0)
        Sleep(20);
    Sleep(50);
    DumpAddresses();
    return true;
}

class CNetCleanup
{
public:
    CNetCleanup()
    {
    }
    ~CNetCleanup()
    {
        // Close sockets
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                closesocket(pnode->hSocket);
        if (hListenSocket != INVALID_SOCKET)
            if (closesocket(hListenSocket) == SOCKET_ERROR)
                printf("closesocket(hListenSocket) failed with error %d\n", WSAGetLastError());

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}
instance_of_cnetcleanup;
