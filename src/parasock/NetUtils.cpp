//
// NetUtils.cpp
//
// These are some utility routines that used to live in the file with the
// proxy server code, which I moved out to be in their own place.  Some code
// used by SockBuf also.
//

#include <memory>
#include <sstream>
#include <iostream>

#include "Helpers.h"
#include "NetUtils.h"


// 
// Socket routines for cross-platform and error handling
// Also serves as debug hook to watch what we are putting on the wire.
//

int socksend(SOCKET sock, const char * buf, int bufsize, const TIMEOUT to) {
	int sent = 0;

	do {
		CodeBlock() {
			MYPOLLFD fds;
			fds.fd = sock;
			fds.events = POLLOUT;
			int pollRes = poll(&fds, 1, to.GetMilliseconds());
			int errorno = WSAGetLastError();
			if	(pollRes < 0 && (errorno == EAGAIN || errorno == EINTR))
				continue;

			if(pollRes < 1) {
				printf("SEND breaking loop on poll, what does this mean?\n");
				break;
			}
		}

		int sendRes = send(sock, buf + sent, bufsize - sent, 0);
		if(sendRes < 0) {
			int errorno = WSAGetLastError();
			if(errorno == EAGAIN || errorno == EINTR)
				continue;

			int err = WSAGetLastError();
			return sendRes;
		}
		sent += sendRes;
	} while (sent < bufsize);

	return sent;
}

int socksendto(
	SOCKET sock, struct sockaddr_in * sin,
	const char * buf, int bufsize,
	const TIMEOUT to
) {
	int sent = 0;
 
	do {
		MYPOLLFD fds;
		fds.fd = sock;
		fds.events = POLLOUT;

		int pollRes = poll(&fds, 1, to.GetMilliseconds());
		int errorno = WSAGetLastError();
		if(pollRes < 0 && (errorno == EAGAIN || errorno == EINTR))
			continue;
		if(pollRes < 1)
			break;

		int sendToRes = sendto(
			sock,
			buf + sent,
			bufsize - sent,
			0,
			(struct sockaddr *)sin,
			sizeof(struct sockaddr_in)
		);

		if(sendToRes < 0) {
			int errorno = WSAGetLastError();
			if(errorno ==  EAGAIN)
				continue;

			return sendToRes;
		}
		sent += sendToRes;
	} while (sent < bufsize);

	return sent;
}

int sockrecvfrom(
	SOCKET sock,
	struct sockaddr_in * sin,
	char * buf,
	int bufsize,
	const TIMEOUT to
) {
	MYPOLLFD fds;
	fds.fd = sock;
	fds.events = POLLIN;

	if (poll(&fds, 1, to.GetMilliseconds()) <1 )
		return 0;

	SASIZETYPE sasize = sizeof(struct sockaddr_in);
	int errorno;
	int res;
	do {
		res = recvfrom(
			sock, reinterpret_cast<char*>(buf),
			bufsize,
			0,
			(struct sockaddr *)sin, &sasize
		);
		errorno = WSAGetLastError();
	} while ((res < 0) && ((errorno == EAGAIN) || (errorno == EINTR)));

	return res;
}

inline int mypoll(MYPOLLFD *fds, unsigned int nfds, int timeout){
	fd_set readfd;
	fd_set writefd;
	fd_set oobfd;
	struct timeval tv;
	unsigned i;
	int num;
	SOCKET maxfd = 0;

	tv.tv_sec = timeout/1000;
	tv.tv_usec = (timeout%1000)*1000;
	FD_ZERO(&readfd);
	FD_ZERO(&writefd);
	FD_ZERO(&oobfd);
	for(i=0; i < nfds; i++){
		if((fds[i].events&POLLIN))FD_SET(fds[i].fd, &readfd);
		if((fds[i].events&POLLOUT))FD_SET(fds[i].fd, &writefd);
		if((fds[i].events&POLLPRI))FD_SET(fds[i].fd, &oobfd);
		fds[i].revents = 0;
		if(fds[i].fd > maxfd) maxfd = fds[i].fd;
	}
	if ((num = select(((int)(maxfd))+1, &readfd, &writefd, &oobfd, &tv)) < 1) {
		return num;
	}
	for (i=0; i<nfds; i++){
		if(FD_ISSET(fds[i].fd, &readfd)) fds[i].revents |= POLLIN;
		if(FD_ISSET(fds[i].fd, &writefd)) fds[i].revents |= POLLOUT;
		if(FD_ISSET(fds[i].fd, &oobfd)) fds[i].revents |= POLLPRI;
	}
	return num;
}
