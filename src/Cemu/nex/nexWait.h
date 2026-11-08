#pragma once

// What the nex thread, and the logins that run before it takes over, sleep on
// instead of polling every millisecond: select() on the services' sockets and
// on a wake socket - a UDP socket on the loopback connected to itself, so a
// byte sent to it from any thread (a service registered, a request queued)
// ends the wait at once. The timeout is only for the protocol's own clocks.

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nexWait
{
#ifdef _WIN32
	using Socket = SOCKET;
	inline constexpr Socket kNoSocket = INVALID_SOCKET;
	inline void CloseSocket(Socket s) { closesocket(s); }
	inline void SetNonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
	using SockLen = int;
#else
	using Socket = int;
	inline constexpr Socket kNoSocket = -1;
	inline void CloseSocket(Socket s) { close(s); }
	inline void SetNonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
	using SockLen = socklen_t;
#endif

	struct Wake
	{
		Socket sock = kNoSocket;

		Wake()
		{
			Socket s = socket(AF_INET, SOCK_DGRAM, 0);
			if (s == kNoSocket)
				return;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = 0;
			SockLen len = sizeof(addr);
			if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 ||
				getsockname(s, (sockaddr*)&addr, &len) != 0 ||
				connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
			{
				CloseSocket(s);
				return;
			}
			SetNonBlocking(s);
			sock = s;
		}

		// Any thread; a wake already pending makes this one redundant
		void Signal()
		{
			if (sock == kNoSocket)
				return;
			const char b = 0;
			send(sock, &b, 1, 0);
		}

		void Drain()
		{
			char buf[64];
			while (recv(sock, buf, sizeof(buf), 0) > 0)
				;
		}
	};

	// Asleep until one of the sockets has something to read, or the wake if
	// given (drained), or the timeout; false on the timeout
	inline bool WaitForSockets(const Socket* sockets, size_t count, uint32_t timeoutMs, Wake* wake)
	{
		fd_set readable;
		FD_ZERO(&readable);
		long long maxFd = -1;
		auto add = [&](Socket s) {
			if (s == kNoSocket)
				return;
			FD_SET(s, &readable);
			if ((long long)s > maxFd)
				maxFd = (long long)s;
		};
		for (size_t i = 0; i < count; i++)
			add(sockets[i]);
		if (wake)
			add(wake->sock);
		if (maxFd < 0)
			return false;
		timeval tv;
		tv.tv_sec = (long)(timeoutMs / 1000);
		tv.tv_usec = (long)((timeoutMs % 1000) * 1000);
		const int r = select((int)maxFd + 1, &readable, nullptr, nullptr, &tv);
		if (r <= 0)
			return false;
		if (wake && wake->sock != kNoSocket && FD_ISSET(wake->sock, &readable))
			wake->Drain();
		return true;
	}
}
