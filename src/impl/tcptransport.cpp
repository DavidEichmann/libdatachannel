/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tcptransport.hpp"
#include "internals.hpp"

#if RTC_ENABLE_WEBSOCKET

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>

namespace rtc::impl {

using namespace std::placeholders;
using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

TcpTransport::TcpTransport(string hostname, string service, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(true), mHostname(std::move(hostname)),
      mService(std::move(service)), mSock(INVALID_SOCKET) {

	PLOG_DEBUG << "Initializing TCP transport";
}

TcpTransport::TcpTransport(socket_t sock, state_callback callback)
    : Transport(nullptr, std::move(callback)), mIsActive(false), mSock(sock) {

	PLOG_DEBUG << "Initializing TCP transport with socket";

	// Set non-blocking
	ctl_t nbio = 1;
	if (::ioctlsocket(mSock, FIONBIO, &nbio) < 0)
		throw std::runtime_error("Failed to set socket non-blocking mode");

	// Retrieve hostname and service
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	if (::getpeername(mSock, reinterpret_cast<struct sockaddr *>(&addr), &addrlen) < 0)
		throw std::runtime_error("getsockname failed");

	char node[MAX_NUMERICNODE_LEN];
	char serv[MAX_NUMERICSERV_LEN];
	if (::getnameinfo(reinterpret_cast<struct sockaddr *>(&addr), addrlen, node,
	                  MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,
	                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		throw std::runtime_error("getnameinfo failed");

	mHostname = node;
	mService = serv;
}

TcpTransport::~TcpTransport() { stop(); }

void TcpTransport::start() {
	Transport::start();

	if (mSock == INVALID_SOCKET) {
		connect();
	} else {
		changeState(State::Connected);
		setPoll(PollService::Direction::In);
	}
}

bool TcpTransport::stop() {
	if (!Transport::stop())
		return false;

	close();
	return true;
}

bool TcpTransport::send(message_ptr message) {
	std::lock_guard lock(mSendMutex);
	if (state() != State::Connected)
		throw std::runtime_error("Connection is not open");

	if (state() != State::Connected)
		return false;

	if (!message)
		return trySendQueue();

	PLOG_VERBOSE << "Send size=" << (message ? message->size() : 0);
	return outgoing(message);
}

void TcpTransport::incoming(message_ptr message) {
	if (!message)
		return;

	PLOG_VERBOSE << "Incoming size=" << message->size();
	recv(message);
}

bool TcpTransport::outgoing(message_ptr message) {
	// mSendMutex must be locked
	// Flush the queue, and if nothing is pending, try to send directly
	if (trySendQueue() && trySendMessage(message))
		return true;

	mSendQueue.push(message);
	setPoll(PollService::Direction::Both);
	return false;
}

string TcpTransport::remoteAddress() const { return mHostname + ':' + mService; }

void TcpTransport::connect() {
	PLOG_DEBUG << "Connecting to " << mHostname << ":" << mService;
	changeState(State::Connecting);

	// Resolve hostname
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_ADDRCONFIG;

	struct addrinfo *result = nullptr;
	if (getaddrinfo(mHostname.c_str(), mService.c_str(), &hints, &result))
		throw std::runtime_error("Resolution failed for \"" + mHostname + ":" + mService + "\"");

	// Chain connection attempt to each address
	auto attempt = [this, result](struct addrinfo *ai, auto recurse) {
		if (!ai) {
			PLOG_WARNING << "Connection to " << mHostname << ":" << mService << " failed";
			freeaddrinfo(result);
			changeState(State::Failed);
			return;
		}

		try {
			prepare(ai->ai_addr, socklen_t(ai->ai_addrlen));

		} catch (const std::runtime_error &e) {
			PLOG_DEBUG << e.what();
			recurse(ai->ai_next, recurse);
		}

		// Poll out event callback
		auto callback = [this, result, ai, recurse](PollService::Event event) mutable {
			try {
				if (event == PollService::Event::Error)
					throw std::runtime_error("TCP connection failed");

				if (event == PollService::Event::Timeout)
					throw std::runtime_error("TCP connection timed out");

				if (event != PollService::Event::Out)
					return;

				int err = 0;
				socklen_t errlen = sizeof(err);
				if (::getsockopt(mSock, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0)
					throw std::runtime_error("Failed to get socket error code");

				if (err != 0) {
					std::ostringstream msg;
					msg << "TCP connection failed, errno=" << err;
					throw std::runtime_error(msg.str());
				}

				// Success
				PLOG_INFO << "TCP connected";
				freeaddrinfo(result);
				changeState(State::Connected);
				setPoll(PollService::Direction::In);

			} catch (const std::runtime_error &e) {
				PLOG_DEBUG << e.what();
				PollService::Instance().remove(mSock);
				::closesocket(mSock);
				mSock = INVALID_SOCKET;
				recurse(ai->ai_next, recurse);
			}
		};

		const auto timeout = 10s;
		PollService::Instance().add(mSock,
		                            {PollService::Direction::Out, timeout, std::move(callback)});
	};

	attempt(result, attempt);
}

void TcpTransport::prepare(const sockaddr *addr, socklen_t addrlen) {
	try {
		char node[MAX_NUMERICNODE_LEN];
		char serv[MAX_NUMERICSERV_LEN];
		if (getnameinfo(addr, addrlen, node, MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,
		                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
			PLOG_DEBUG << "Trying address " << node << ":" << serv;
		}

		PLOG_VERBOSE << "Creating TCP socket";

		// Create socket
		mSock = ::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if (mSock == INVALID_SOCKET)
			throw std::runtime_error("TCP socket creation failed");

		// Set non-blocking
		ctl_t nbio = 1;
		if (::ioctlsocket(mSock, FIONBIO, &nbio) < 0)
			throw std::runtime_error("Failed to set socket non-blocking mode");

#ifdef __APPLE__
		// MacOS lacks MSG_NOSIGNAL and requires SO_NOSIGPIPE instead
		const sockopt_t enabled = 1;
		if (::setsockopt(mSock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
			throw std::runtime_error("Failed to disable SIGPIPE for socket");
#endif

		// Initiate connection
		int ret = ::connect(mSock, addr, addrlen);
		if (ret < 0 && sockerrno != SEINPROGRESS && sockerrno != SEWOULDBLOCK) {
			std::ostringstream msg;
			msg << "TCP connection to " << node << ":" << serv << " failed, errno=" << sockerrno;
			throw std::runtime_error(msg.str());
		}

	} catch (...) {
		if (mSock != INVALID_SOCKET) {
			::closesocket(mSock);
			mSock = INVALID_SOCKET;
		}
		throw;
	}
}

void TcpTransport::setPoll(PollService::Direction direction) {
	PollService::Instance().add(mSock,
	                            {direction, nullopt, std::bind(&TcpTransport::process, this, _1)});
}

void TcpTransport::close() {
	std::lock_guard lock(mSendMutex);
	if (mSock != INVALID_SOCKET) {
		PLOG_DEBUG << "Closing TCP socket";
		PollService::Instance().remove(mSock);
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
	changeState(State::Disconnected);
}

bool TcpTransport::trySendQueue() {
	// mSendMutex must be locked
	while (auto next = mSendQueue.peek()) {
		message_ptr message = std::move(*next);
		if (!trySendMessage(message)) {
			mSendQueue.exchange(message);
			return false;
		}
		mSendQueue.pop();
	}

	return true;
}

bool TcpTransport::trySendMessage(message_ptr &message) {
	// mSendMutex must be locked
	auto data = reinterpret_cast<const char *>(message->data());
	auto size = message->size();
	while (size) {
#if defined(__APPLE__) || defined(_WIN32)
		int flags = 0;
#else
		int flags = MSG_NOSIGNAL;
#endif
		int len = ::send(mSock, data, int(size), flags);
		if (len < 0) {
			if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK) {
				message = make_message(message->end() - size, message->end());
				return false;
			} else {
				PLOG_ERROR << "Connection closed, errno=" << sockerrno;
				throw std::runtime_error("Connection closed");
			}
		}

		data += len;
		size -= len;
	}
	message = nullptr;
	return true;
}

void TcpTransport::process(PollService::Event event) {
	try {
		switch (event) {
		case PollService::Event::Error: {
			PLOG_WARNING << "TCP connection terminated";
			break;
		}

		case PollService::Event::Timeout: {
			PLOG_VERBOSE << "TCP is idle";
			incoming(make_message(0));
			return;
		}

		case PollService::Event::Out: {
			if (trySendQueue())
				setPoll(PollService::Direction::In);

			return;
		}

		case PollService::Event::In: {
			const size_t bufferSize = 4096;
			char buffer[bufferSize];
			int len;
			while ((len = ::recv(mSock, buffer, bufferSize, 0)) > 0) {
				auto *b = reinterpret_cast<byte *>(buffer);
				incoming(make_message(b, b + len));
			}

			if (len == 0)
				break; // clean close

			if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
				PLOG_WARNING << "TCP connection lost";
				break;
			}

			return;
		}

		default:
			// Ignore
			return;
		}

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}

	PLOG_INFO << "TCP disconnected";
	PollService::Instance().remove(mSock);
	changeState(State::Disconnected);
	recv(nullptr);
}

} // namespace rtc::impl

#endif
