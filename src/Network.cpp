/* Copyright 2010 Jukka Jyl�nki

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

/** @file Network.cpp
	@brief */

#include <string>
#include <sstream>

#include <cassert>

#ifdef LINUX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#endif

#include "kNet/Network.h"

#include "kNet/DebugMemoryLeakCheck.h"

#include "kNet/TCPMessageConnection.h"
#include "kNet/UDPMessageConnection.h"
#include "kNet/NetworkWorkerThread.h"
#include "kNet/NetworkLogging.h"

namespace kNet
{

const int cMaxTCPSendSize = 256 * 1024;
const int cMaxUDPSendSize = 1400;

std::string Network::GetErrorString(int error)
{
#ifdef WIN32
	void *lpMsgBuf = 0;

	HRESULT hresult = HRESULT_FROM_WIN32(error);
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		0, hresult, 0 /*Default language*/, (LPTSTR) &lpMsgBuf, 0, 0);

	// Copy message to C++ -style string, since the data need to be freed before return.
	std::string message = (LPCSTR)lpMsgBuf;

	LocalFree(lpMsgBuf);
	return message;
#else
	return std::string(strerror(error));
#endif
}

int Network::GetLastError()
{
#ifdef WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

std::string Network::GetLastErrorString()
{
	return GetErrorString(GetLastError());
}

std::string FormatBytes(size_t numBytes)
{
	char str[256];
	if (numBytes >= 1000 * 1000)
		sprintf(str, "%.3fMB", (float)numBytes / 1024.f / 1024.f);
	else if (numBytes >= 1000)
		sprintf(str, "%.3fKB", (float)numBytes / 1024.f);
	else
		sprintf(str, "%dB", (int)numBytes);
	return std::string(str);
}

Network::Network()
{
#ifdef WIN32
	memset(&wsaData, 0, sizeof(wsaData));
#endif
	Init();
}

Network::~Network()
{
	StopServer();
	DeInit();
}

void PrintLocalIP()
{
	char ac[80];
	if (gethostname(ac, sizeof(ac)) == KNET_SOCKET_ERROR)
	{
		LOG(LogError, "Error getting local host name!");
		return;
	}
	LOG(LogInfo, "Host name is %s", ac);

	struct hostent *phe = gethostbyname(ac);
	if (phe == 0)
	{
		LOG(LogError, "Bad host lookup.");
		return;
	}

	for (int i = 0; phe->h_addr_list[i] != 0; ++i)
	{
		struct in_addr addr;
		memcpy(&addr, phe->h_addr_list[i], sizeof(struct in_addr));
		LOG(LogInfo, "Address %d: %s", i, inet_ntoa(addr)); ///\todo inet_ntoa is deprecated! doesn't handle IPv6!
	}
}

void Network::PrintAddrInfo(const addrinfo *ptr)
{
	if (!ptr)
	{
		LOG(LogError, "Null pointer passed to Network::PrintAddrInfo!");
		return;
	}

	LOG(LogInfo, "\tFlags: 0x%x\n", ptr->ai_flags);
	LOG(LogInfo, "\tFamily: ");
	switch(ptr->ai_family)
	{
	case AF_UNSPEC:
		LOG(LogInfo, "Unspecified\n");
		break;
	case AF_INET:
		LOG(LogInfo, "AF_INET (IPv4)\n");
		break;
	case AF_INET6:
		LOG(LogInfo, "AF_INET6 (IPv6)\n");
		break;
#ifdef WIN32
	case AF_NETBIOS:
		LOG(LogInfo, "AF_NETBIOS (NetBIOS)\n");
		break;
#endif
	default:
		LOG(LogInfo, "Other %u\n", ptr->ai_family);
		break;
	}
	LOG(LogInfo, "\tSocket type: ");
	switch(ptr->ai_socktype)
	{
	case 0:
		LOG(LogInfo, "Unspecified\n");
		break;
	case SOCK_STREAM:
		LOG(LogInfo, "SOCK_STREAM (stream)\n");
		break;
	case SOCK_DGRAM:
		LOG(LogInfo, "SOCK_DGRAM (datagram) \n");
		break;
	case SOCK_RAW:
		LOG(LogInfo, "SOCK_RAW (raw) \n");
		break;
	case SOCK_RDM:
		LOG(LogInfo, "SOCK_RDM (reliable message datagram)\n");
		break;
	case SOCK_SEQPACKET:
		LOG(LogInfo, "SOCK_SEQPACKET (pseudo-stream packet)\n");
		break;
	default:
		LOG(LogInfo, "Other %u\n", ptr->ai_socktype);
		break;
	}
	LOG(LogInfo, "\tProtocol: ");
	switch(ptr->ai_protocol)
	{
	case 0:
		LOG(LogInfo, "Unspecified\n");
		break;
	case IPPROTO_TCP:
		LOG(LogInfo, "IPPROTO_TCP (TCP)\n");
		break;
	case IPPROTO_UDP:
		LOG(LogInfo, "IPPROTO_UDP (UDP) \n");
		break;
	default:
		LOG(LogInfo, "Other %u\n", ptr->ai_protocol);
		break;
	}
	LOG(LogInfo, "\tLength of this sockaddr: %d\n", (int)ptr->ai_addrlen);
	LOG(LogInfo, "\tCanonical name: %s\n", ptr->ai_canonname);

	char address[256];
	sprintf(address, "%d.%d.%d.%d",
		(unsigned int)(unsigned char)ptr->ai_addr->sa_data[2], (unsigned int)(unsigned char)ptr->ai_addr->sa_data[3],
		(unsigned int)(unsigned char)ptr->ai_addr->sa_data[4], (unsigned int)(unsigned char)ptr->ai_addr->sa_data[5]);

	LOG(LogInfo, "Address of this sockaddr: %s.\n", address);

}

void Network::PrintHostNameInfo(const char *hostname, const char *port)
{
	addrinfo hints;

	//--------------------------------
	// Setup the hints address info structure
	// which is passed to the getaddrinfo() function
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	//--------------------------------
	// Call getaddrinfo(). If the call succeeds,
	// the result variable will hold a linked list
	// of addrinfo structures containing response
	// information
	addrinfo *result = NULL;
	unsigned long dwRetval = (unsigned long)getaddrinfo(hostname, port, &hints, &result);
	if (dwRetval != 0)
	{
		LOG(LogError, "getaddrinfo failed with error: %d\n", (int)dwRetval);
		return;
	}

	LOG(LogInfo, "getaddrinfo returned success\n");

	int i = 1;

	// Retrieve each address and print out the hex bytes
	for (addrinfo *ptr = result; ptr != NULL; ptr = ptr->ai_next)
	{
		LOG(LogInfo, "getaddrinfo response %d\n", i++);
		PrintAddrInfo(ptr);
	}

	freeaddrinfo(result);

	PrintLocalIP();
}

void Network::Init()
{
#ifdef WIN32
	// Initialize Winsock
	int result = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (result != 0)
	{
		LOG(LogError, "Network::Init: WSAStartup failed: %s(%d)!", GetErrorString(result).c_str(), result);
		return;
	}
#endif

	// Fetch the local system host name for later use. The local address is cached here 
	// to avoid multiple queries to namespace providers.
	char str[256];
	int ret = gethostname(str, 256); // For more information, see http://msdn.microsoft.com/en-us/library/ms738527(VS.85).aspx .
	if (ret == 0)
	{
		localHostName = str;
		LOG(LogInfo, "Network::Init successful. gethostname returned %s", str);
	}
	else
	{
		int error = GetLastError();
		LOG(LogError, "Network::Init: gethostname failed! Error: %s(%d). Using 'localhost' as the local host name", GetErrorString(error).c_str(), error);
		localHostName = "localhost";
	}
}

NetworkWorkerThread *Network::GetOrCreateWorkerThread()
{
	static const int maxConnectionsPerThread = 1;

	// Find an existing thread with sufficiently low load.
	for(size_t i = 0; i < workerThreads.size(); ++i)
		if (workerThreads[i]->NumConnections() + workerThreads[i]->NumServers() < maxConnectionsPerThread)
			return workerThreads[i];

	// No appropriate thread found. Create a new one.
	NetworkWorkerThread *workerThread = new NetworkWorkerThread();
	workerThread->StartThread();
	workerThreads.push_back(workerThread);
	LOG(LogInfo, "Created a new NetworkWorkerThread. There are now %d worker threads.", (int)workerThreads.size());
	return workerThread;
}

void Network::AssignConnectionToWorkerThread(Ptr(MessageConnection) connection)
{
	GetOrCreateWorkerThread()->AddConnection(connection);
}

NetworkServer *Network::StartServer(unsigned short port, SocketTransportLayer transport, INetworkServerListener *serverListener, bool allowAddressReuse)
{
	Socket *listenSock = OpenListenSocket(port, transport, allowAddressReuse);
	if (listenSock == 0)
	{
		LOG(LogError, "Failed to start server. Could not open listen port to %d using %s.", (int)port, 
			transport == SocketOverTCP ? "TCP" : "UDP");
		return 0;
	}

	std::vector<Socket *> listenSockets;
	listenSockets.push_back(listenSock);

	server = new NetworkServer(this, listenSockets);
	server->RegisterServerListener(serverListener);

	GetOrCreateWorkerThread()->AddServer(server);

	LOG(LogInfo, "Server up (%s). Waiting for client to connect.", listenSock->ToString().c_str());

	return server;
}

NetworkServer *Network::StartServer(const std::vector<std::pair<unsigned short, SocketTransportLayer> > &listenPorts, 
	INetworkServerListener *serverListener, bool allowAddressReuse)
{
	if (listenPorts.size() == 0)
	{
		LOG(LogError, "Failed to start server, since you did not provide a list of ports to listen to in Network::StartServer()!");
		return 0;
	}

	std::vector<Socket *> listenSockets;

	for(size_t i = 0; i < listenPorts.size(); ++i)
	{
		Socket *listenSock = OpenListenSocket(listenPorts[i].first, listenPorts[i].second, allowAddressReuse);
		if (listenSock)
			listenSockets.push_back(listenSock);
	}

	if (listenSockets.size() == 0)
	{
		LOG(LogError, "Failed to start server. No ports to listen to!");
		return 0;
	}

	server = new NetworkServer(this, listenSockets);
	server->RegisterServerListener(serverListener);

	GetOrCreateWorkerThread()->AddServer(server);

	LOG(LogInfo, "Server up and listening on the following ports: ");
	{
		std::stringstream ss;
		ss << "UDP ";
		for(size_t i = 0; i < listenSockets.size(); ++i)
			if (listenSockets[i]->TransportLayer() == SocketOverUDP)
				ss << listenSockets[i]->LocalPort() << " ";
		LOG(LogInfo, ss.str().c_str());
	}
	{
		std::stringstream ss;
		ss << "TCP ";
		for(size_t i = 0; i < listenSockets.size(); ++i)
			if (listenSockets[i]->TransportLayer() == SocketOverTCP)
				ss << listenSockets[i]->LocalPort() << " ";
		LOG(LogInfo, ss.str().c_str());
	}

	return server;
}

void Network::StopServer()
{
	for(size_t i = 0; i < workerThreads.size(); ++i)
		workerThreads[i]->RemoveServer(server);

	///\todo This is a forceful stop. Perhaps have a benign teardown as well?
	server = 0;
	LOG(LogVerbose, "Network::StopServer: Deinitialized NetworkServer.");
}

void Network::CloseSocket(Socket *socket)
{
	if (!socket)
	{
		LOG(LogError, "Network::CloseSocket() called with a null socket pointer!");
		return;
	}

	for(std::list<Socket>::iterator iter = sockets.begin(); iter != sockets.end(); ++iter)
		if (&*iter == socket)
		{
			socket->Close();
			// The Socket pointers MessageConnection objects have are pointers to this list,
			// so after calling this function with a Socket pointer, the Socket is deleted for good.
			sockets.erase(iter);
			LOG(LogInfo, "Network::CloseSocket: Closed socket %p.", socket);
			return;
		}
	LOG(LogError, "Network::CloseSocket: Tried to close a nonexisting socket %p!", socket);
}

void Network::CloseConnection(Ptr(MessageConnection) connection)
{
	LOG(LogVerbose, "Network::CloseConnection: Closing down connection %p.", connection.ptr());
	if (!connection)
		return;

	for(size_t i = 0; i < workerThreads.size(); ++i)
		workerThreads[i]->RemoveConnection(connection);

	CloseSocket(connection->socket);
	connection->socket = 0;
}

void Network::DeInit()
{
	LOG(LogVerbose, "Network::DeInit: Closing down network worker thread.");
	PolledTimer timer;

	for(size_t i = 0; i < workerThreads.size(); ++i)
	{
		workerThreads[i]->StopThread();
		delete workerThreads[i];
	}
	workerThreads.clear();

	while(sockets.size() > 0)
	{
		sockets.front().Close();
		sockets.pop_front();
	}
#ifdef WIN32
	WSACleanup();
#endif
	LOG(LogWaits, "Network::DeInit: Deinitialized kNet Network object, took %f msecs.", timer.MSecsElapsed());
}

Socket *Network::OpenListenSocket(unsigned short port, SocketTransportLayer transport, bool allowAddressReuse)
{
	addrinfo *result = NULL;
	addrinfo *ptr = NULL;
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = (transport == SocketOverTCP) ? SOCK_STREAM : SOCK_DGRAM;
	hints.ai_protocol = (transport == SocketOverTCP) ? IPPROTO_TCP : IPPROTO_UDP;

	char strPort[256];
	sprintf(strPort, "%d", (unsigned int)port);

	int ret = getaddrinfo(NULL, strPort, &hints, &result);
	if (ret != 0)
	{
		LOG(LogError, "getaddrinfo failed: %s(%d)", GetErrorString(ret).c_str(), ret);
		return 0;
	}

	SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	LOG(LogInfo, "Network::OpenListenSocket: Created listenSocket 0x%8X.", (unsigned int)listenSocket);

	if (listenSocket == INVALID_SOCKET)
	{
		int error = GetLastError();
		LOG(LogError, "Error at socket(): %s(%d)", GetErrorString(error).c_str(), error);
		freeaddrinfo(result);
		return 0;
	}

	if (allowAddressReuse)
	{
		// Allow other sockets to be bound to this address after this. 
		// (Possibly unsecure, only enable for development purposes - to avoid having to wait for the server listen socket 
		//  to time out if the server crashes.)
#ifdef WIN32
		BOOL val = TRUE;
		ret = setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));
#else
		int val = 1;
		ret = setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
#endif
		if (ret != 0)
		{
			int error = GetLastError();
			LOG(LogError, "setsockopt to SO_REUSEADDR failed: %s(%d)", GetErrorString(error).c_str(), error);
		}
	}

	// It is safe to cast to a sockaddr_in, since we've specifically queried for AF_INET addresses.
	sockaddr_in localAddress = *(sockaddr_in*)&result->ai_addr;

	// Setup the listening socket - bind it to a local port.
	// If we are setting up a TCP socket, the socket will be only for listening and accepting incoming connections.
	// If we are setting up an UDP socket, all connection initialization and data transfers will be managed through this socket.
	ret = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (ret == KNET_SOCKET_ERROR)
	{
		int error = GetLastError();
		LOG(LogError, "bind failed: %s(%d) when trying to bind to port %d with transport %s", 
			GetErrorString(error).c_str(), error, (int)port, transport == SocketOverTCP ? "TCP" : "UDP");
		closesocket(listenSocket);
		freeaddrinfo(result);
		return 0;
	}

	freeaddrinfo(result);

	// For a reliable TCP socket, start the server with a call to listen().
	if (transport == SocketOverTCP)
	{
		// Transition the bound socket to a listening state.
		ret = listen(listenSocket, SOMAXCONN);
		if (ret == KNET_SOCKET_ERROR)
		{
			int error = GetLastError();
			LOG(LogError, "Error at listen(): %s(%d)", GetErrorString(error).c_str(), error);
			closesocket(listenSocket);
			return 0;
		}
	}

	EndPoint localEndPoint = EndPoint::FromSockAddrIn(localAddress);

	// We are starting up a server listen socket, which is not bound to an address. Use null address for the remote endpoint.
	EndPoint remoteEndPoint;
	remoteEndPoint.Reset();

	const size_t maxSendSize = (transport == SocketOverTCP ? cMaxTCPSendSize : cMaxUDPSendSize);
	sockets.push_back(Socket(listenSocket, localEndPoint, localHostName.c_str(), remoteEndPoint, "", transport, ServerListenSocket, maxSendSize));
	Socket *listenSock = &sockets.back();
	listenSock->SetBlocking(false);

	return listenSock;
}

Socket *Network::ConnectSocket(const char *address, unsigned short port, SocketTransportLayer transport)
{
	addrinfo *result = NULL;
	addrinfo *ptr = NULL;
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = (transport == SocketOverTCP) ? SOCK_STREAM : SOCK_DGRAM;
	hints.ai_protocol = (transport == SocketOverTCP) ? IPPROTO_TCP : IPPROTO_UDP;

	char strPort[256];
	sprintf(strPort, "%d", (unsigned int)port);
	int ret = getaddrinfo(address, strPort, &hints, &result);
	if (ret != 0)
	{
		LOG(LogError, "Network::Connect: getaddrinfo failed: %s(%d)", GetErrorString(ret).c_str(), ret);
		return 0;
	}

#ifdef WIN32
	SOCKET connectSocket = WSASocket(result->ai_family, result->ai_socktype, result->ai_protocol,
		NULL, 0, WSA_FLAG_OVERLAPPED);
#else
	SOCKET connectSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	LOG(LogInfo, "A call to socket() returned a new socket 0x%8X.", (unsigned int)connectSocket);
#endif
	if (connectSocket == INVALID_SOCKET)
	{
		int error = GetLastError();
		LOG(LogError, "Network::Connect: Error at socket(): %s(%d)", GetErrorString(error).c_str(), error);
		freeaddrinfo(result);
		return 0;
	}

	// Connect to server.
#ifdef WIN32
	ret = WSAConnect(connectSocket, result->ai_addr, (int)result->ai_addrlen, 0, 0, 0, 0);
#else
	ret = connect(connectSocket, result->ai_addr, (int)result->ai_addrlen);
#endif

	if (ret == KNET_SOCKET_ERROR)
	{
		closesocket(connectSocket);
		connectSocket = INVALID_SOCKET;
	}

	freeaddrinfo(result);

	if (connectSocket == INVALID_SOCKET)
	{
		LOG(LogError, "Unable to connect to server!");
		return 0;
	}

	sockaddr_in sockname;
	socklen_t socknamelen = sizeof(sockname);
	getsockname(connectSocket, (sockaddr*)&sockname, &socknamelen); ///\todo Check return value.

	sockaddr_in peername;
	socklen_t peernamelen = sizeof(peername);
	getpeername(connectSocket, (sockaddr*)&peername, &peernamelen); ///\todo Check return value.

	EndPoint localEndPoint = EndPoint::FromSockAddrIn(sockname);
	EndPoint remoteEndPoint = EndPoint::FromSockAddrIn(peername);
	std::string remoteHostName = remoteEndPoint.IPToString();

	const size_t maxSendSize = (transport == SocketOverTCP) ? cMaxTCPSendSize : cMaxUDPSendSize;
	Socket socket(connectSocket, localEndPoint, localHostName.c_str(), remoteEndPoint, remoteHostName.c_str(), transport, ClientSocket, maxSendSize);

	socket.SetBlocking(false);
	sockets.push_back(socket);

	Socket *sock = &sockets.back();

	return sock;
}

Ptr(MessageConnection) Network::Connect(const char *address, unsigned short port, 
	SocketTransportLayer transport, IMessageHandler *messageHandler, Datagram *connectMessage)
{
	Socket *socket = ConnectSocket(address, port, transport);
	if (!socket)
		return 0;

	if (transport == SocketOverUDP)
	{
		SendUDPConnectDatagram(*socket, connectMessage);
		LOG(LogInfo, "Network::Connect: Sent a UDP Connection Start datagram to to %s.", socket->ToString().c_str());
	}
	else
		LOG(LogInfo, "Network::Connect: Connected a TCP socket to %s.", socket->ToString().c_str());

	Ptr(MessageConnection) connection;
	if (transport == SocketOverTCP)
		connection = new TCPMessageConnection(this, 0, socket, ConnectionOK);
	else
		connection = new UDPMessageConnection(this, 0, socket, ConnectionPending);

	connection->RegisterInboundMessageHandler(messageHandler);
	GetOrCreateWorkerThread()->AddConnection(connection);

	return connection;
}

Socket *Network::CreateUDPSlaveSocket(Socket *serverListenSocket, const EndPoint &remoteEndPoint, const char *remoteHostName)
{
	if (!serverListenSocket)
	{
		LOG(LogError, "Network::CreateUDPSlaveSocket called with null serverListenSocket handle!");
		return 0;
	}

	SOCKET udpSocket = serverListenSocket->GetSocketHandle();
	if (udpSocket == INVALID_SOCKET)
	{
		LOG(LogError, "Network::CreateUDPSlaveSocket called with a UDP server socket with invalid internal socket handle!");
		return 0;
	}

	sockets.push_back(Socket(udpSocket, serverListenSocket->LocalEndPoint(),
		serverListenSocket->LocalAddress(), remoteEndPoint, remoteHostName, SocketOverUDP, ServerClientSocket, cMaxUDPSendSize));
	Socket *socket = &sockets.back();
	socket->SetBlocking(false);

	LOG(LogInfo, "Network::CreateUDPSlaveSocket: Connected an UDP socket to %s.", socket->ToString().c_str());
	return socket;
}

Socket *Network::StoreSocket(const Socket &cp)
{
	sockets.push_back(cp);
	return &sockets.back();
}

void Network::SendUDPConnectDatagram(Socket &socket, Datagram *connectMessage)
{
	OverlappedTransferBuffer *sendData = socket.BeginSend();
	if (!sendData)
	{
		LOG(LogError, "Network::SendUDPConnectDatagram: socket.BeginSend failed! Cannot send UDP connection datagram!");
	}
	if (connectMessage)
	{
		///\todo Craft the proper connection attempt datagram.
		sendData->buffer.len = std::min<int>(connectMessage->size, sendData->buffer.len);
		memcpy(sendData->buffer.buf, connectMessage->data, sendData->buffer.len);
		LOG(LogVerbose, "Network::SendUDPConnectDatagram: Sending UDP connect message of size %d.", (int)sendData->buffer.len);
	}
	else
	{
		///\todo Craft the proper connection attempt datagram.
		sendData->buffer.len = std::min<int>(8, sendData->buffer.len);
		memset(sendData->buffer.buf, 0, sendData->buffer.len);
		LOG(LogVerbose, "Network::SendUDPConnectDatagram: Sending null UDP connect message of size %d.", (int)sendData->buffer.len);
	}
	socket.EndSend(sendData);
}

} // ~kNet
