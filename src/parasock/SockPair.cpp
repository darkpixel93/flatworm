// 
// SockPair.cpp
//
// Abstraction to represent a pair of sockets used by the proxy
// (a "pair of SOCKs", if you will :-P).
//
// This abstraction is central to the idea of mediating the connection
// and rewriting the data between them, with the attachment of filters.
//

#include "Helpers.h"
#include "NetUtils.h"
#include "SockBuf.h"
#include "SockPair.h"
#include "Filter.h"
#include "DeadFilter.h"

void SockPair::doBidirectionalFilteredProxyEx(
	size_t (&readSoFar)[DIRECTION_MAX],
	const TIMEOUT timeo,
	Filter* (&filter)[DIRECTION_MAX]
) {
	size_t readInitial[DIRECTION_MAX];
	size_t sentSoFar[DIRECTION_MAX];
	CodeBlock() {
		DIRECTION which;
		ForEachDirection(which) {
			readInitial[which] = readSoFar[which];
			sentSoFar[which] = 0;
		}
	}

	bool timedOut;
	bool socketClosed[2];
	bool readAZero[2];
	doBidirectionalFilteredProxyCore(
		readSoFar,
		sentSoFar,
		socketClosed,
		readAZero,
		timedOut,
		timeo,
		filter
	);

	CodeBlock() {
		DIRECTION which;
		ForEachDirection(which) {
			if (
				socketClosed[which]
				&& (
					filter[which]->currentInstruction()->type
					!= Instruction::QuitFilter
				)
			) {
				throw "Socket closed during communication";
			}
		}
	}
}


size_t SockPair::doUnidirectionalProxyCore(
	const DIRECTION which,
	bool& timedOut,
	bool& socketClosed,
	const TIMEOUT timeout
) {
	// If you don't know how much you want, just read while data is available

	size_t readSoFar[DIRECTION_MAX];
	size_t sentSoFar[DIRECTION_MAX];
	CodeBlock() {
		DIRECTION whichZero;
		ForEachDirection(whichZero) {
			readSoFar[whichZero] = 0;
			sentSoFar[whichZero] = 0;
		}
	}

	DeadFilter deadFilterClient (*this, CLIENT);
	DeadFilter deadFilterServer (*this, SERVER);
	Filter* filter[DIRECTION_MAX] = { &deadFilterClient, &deadFilterServer };

	bool readAZero[DIRECTION_MAX];
	bool socketClosedPair[DIRECTION_MAX];
	doBidirectionalFilteredProxyCore(
		readSoFar,
		sentSoFar,
		socketClosedPair,
		readAZero,
		timedOut,
		timeout,
		filter
	);
	socketClosed = socketClosedPair[which];

	return sentSoFar[which];
}


void SockPair::filterHelper(
	const DIRECTION which,
	const size_t newDataOffset,
	const size_t readSoFar,
	Filter& filter,
	bool disconnected
) {

	size_t length = sockbuf[which]->uncommittedBytes.length();
	const size_t newChars = length - newDataOffset;

	filter.runWrapper(
		sockbuf[which]->uncommittedBytes,
		newDataOffset,
		readSoFar,
		disconnected
	);

	const Instruction* instruction = filter.currentInstruction();

	if (disconnected) {
		// can't read from not yet connected or closed socket
		Assert(instruction->type == Instruction::QuitFilter);
	}

	// new concept is that you always have a filter attached for safety

	if (instruction->commitSize > 0) {
		Assert(length >= instruction->commitSize);
		sockbuf[which]->uncommittedBytes.erase(0, instruction->commitSize);
	}
}


// Simultaneously read and filter multiple buffered sockets
// This is so that if you have a long response from a server and a long
// send from a client, they may not hold each other up.  A 1GB upload and
// a 1GB download may happen chunked, simultaneously
//
// Passing in an unknown waitServer or unknown waitClient has special semantics.
// **you will wait for however much the socket offers until it disconnects**
//
// Passing in zero you still are willing to read until disconnect, it's just
// not the operation's focus.  You will read and buffer from the socket until
// it closes or until another wait condition is reached
//
// If you know the socket has already disconnected, PASS IN ZERO
//
void SockPair::doBidirectionalFilteredProxyCore(
	size_t (&readSoFar)[2],
	size_t (&sentSoFar)[2],
	bool (&socketClosed)[2],
	bool (&readAZero)[2],
	bool& timedOut,
	const TIMEOUT timeo,
	Filter* (&filter)[DIRECTION_MAX]
) {
	timedOut = false;

	size_t received[DIRECTION_MAX];
	size_t sent[DIRECTION_MAX];
	CodeBlock() {
		DIRECTION which;
		ForEachDirection(which) {
			readAZero[which] = false;
			received[which] = 0;
			sent[which] = 0;
			filter[which]->setupfirstInstruction();
			if (sockbuf[which]->sock == INVALID_SOCKET) {
				socketClosed[which] = true;	
				if (
					filter[which]->currentInstruction()->type 
					!= Instruction::QuitFilter
				) {
					// nothing.  so if that's cool with you, fine...
					filterHelper(which, 0, 0, *filter[which], true);
				}
			} else
				socketClosed[which] = false;
		}
	}

	// We loop until an exit condition is reached
	size_t sleeptime = 0;
	Knowable<size_t> needToRead[DIRECTION_MAX] = { UNKNOWN, UNKNOWN };
	size_t needToWrite[DIRECTION_MAX];
	do {

		// start by saying we read nothing
LTryAgain:
		CodeBlock() {
			DIRECTION which;
			ForEachDirection(which) {
				// must always attach a filter.
				// when filter quits, we stop and return...
				Assert(filter[which] != NULL);

				if (sockbuf[which]->sock == INVALID_SOCKET)
					needToWrite[which] = 0;
				else if (sockbuf[which]->placeholders.empty())
					needToWrite[which] = 0;
				else {
					needToWrite[which] = 0;
					if (sockbuf[which]->placeholders.front()->contentsKnown) {
						std::deque<Placeholder*>::iterator it =
							sockbuf[which]->placeholders.begin();
						while (
							(it != sockbuf[which]->placeholders.end())
							&& ((*it)->contentsKnown))
						{
							Assert(!(*it)->contents.empty());
							needToWrite[which] += (*it)->contents.length();
							it++;
						}
					}
				}

				const Instruction* instruction;
				instruction = filter[which]->currentInstruction();

				if ((sockbuf[which]->sock == INVALID_SOCKET) || readAZero[which]) {

					needToRead[which] = 0;

				} else if (instruction->type == Instruction::QuitFilter) {

					// if we don't know what filter we're using we shouldn't
					// read data.  In the future, you will always have to 
					// supply a filter I think.
					needToRead[which] = 0;

				} else {
					size_t buflen = sockbuf[which]->uncommittedBytes.length();
					size_t rawlen = sockbuf[which]->unfilteredBytes.length();
					const size_t buflenInitial = buflen;

					switch(instruction->type) {
					case Instruction::UntilDelimiter: {
						UntilDelimiterInstruction const * inst =
							dynamic_cast<UntilDelimiterInstruction const *>(instruction);

						size_t delimPos = sockbuf[which]->unfilteredBytes.find(inst->delimiter);
						if (delimPos != std::string::npos) {
							sockbuf[which]->uncommittedBytes +=
								sockbuf[which]->unfilteredBytes.substr(0, delimPos+inst->delimiter.length());
							sockbuf[which]->unfilteredBytes.erase(0, delimPos+inst->delimiter.length());
							buflen += delimPos+inst->delimiter.length();
							needToRead[which] = 0;
						} else {
							/* sockbuf[which]->uncommittedBytes = sockbuf[which]->unfilteredBytes;
							sockbuf[which]->unfilteredBytes.clear(); */  // try leaving the unfilteredBytes data

							// don't know how much we need, but let the later bit take care of it
							needToRead[which] = UNKNOWN;
						}
						break;
					}

					case Instruction::BytesExact: {
						BytesExactInstruction const * inst =
							dynamic_cast<BytesExactInstruction const *>(instruction);
						if (buflen + rawlen >= inst->exactByteCount) {
							// just take enough unfilteredBytes data to get up to size
							const size_t difference = inst->exactByteCount - buflen;
							sockbuf[which]->uncommittedBytes += sockbuf[which]->unfilteredBytes.substr(0, difference);
							buflen += difference;
							sockbuf[which]->unfilteredBytes.erase(0, difference);
							rawlen -= difference;

							needToRead[which] = 0;
						} else {
							needToRead[which] = inst->exactByteCount - (buflen + rawlen);
						}
						break;
					}

					case Instruction::BytesMax: {
						BytesMaxInstruction const * inst =
							dynamic_cast<BytesMaxInstruction const *>(instruction);

						if (inst->maxByteCount <= buflen) {
							needToRead[which] = 0;
						} else {
							// not enough uncommittedBytes data in buffer already, but try unfilteredBytes source first...

							if (buflen + rawlen>= inst->maxByteCount) {
								// just take enough unfilteredBytes data to get up to size
								const size_t difference = inst->maxByteCount - buflen;
								sockbuf[which]->uncommittedBytes += sockbuf[which]->unfilteredBytes.substr(0, difference);
								buflen += difference;
								sockbuf[which]->unfilteredBytes.erase(0, difference);
								rawlen -= difference;

								needToRead[which] = 0;
							} else {
								// the unfilteredBytes data couldn't satisfy, might as well take it all...
								sockbuf[which]->uncommittedBytes += sockbuf[which]->unfilteredBytes;
								buflen += rawlen;

								sockbuf[which]->unfilteredBytes.clear();
								rawlen = 0;

								needToRead[which] = inst->maxByteCount - buflen;
							}
						}
						break;
					}
							
					case Instruction::BytesUnknown: {
						// the unfilteredBytes data won't satisfy, might as well take it all...
						sockbuf[which]->uncommittedBytes += sockbuf[which]->unfilteredBytes;
						buflen += rawlen;

						sockbuf[which]->unfilteredBytes.clear();
						rawlen = 0;
						needToRead[which] = UNKNOWN;

						break;
					}

					default:
						NotReached();
						break;
					}

					// filter the uncommittedBytes data that we just "read"
					// we currently don't queue writing it yet
					received[which] = buflen;
					readSoFar[which] += buflen - buflenInitial;
					if (buflen - buflenInitial > 0) {
						filterHelper(which, buflenInitial, readSoFar[which], *filter[which], false);

						// okay, there's got to be a better way to do this...
						// but now we have a new instruction,
						// so we might need to recompute...
						goto LTryAgain; 
					}
				}
			}
		}

		if (
			(needToRead[SERVER].isKnownToBe(0))
			&& (needToWrite[SERVER] == 0)
			&& (needToWrite[CLIENT] == 0)
			&& (needToRead[CLIENT].isKnownToBe(0))
		) {
			break;
		}

		// Okay, now we know what we're doing.  We reset the sizes each time which is 
		// somewhat inefficient but I'm trying this angle...

		Knowable<size_t> needToReadLast[DIRECTION_MAX] = { UNKNOWN, UNKNOWN };
		size_t needToWriteLast[DIRECTION_MAX];

		MYPOLLFD fds[DIRECTION_MAX];
		CodeBlock() {
			DIRECTION which;
			ForEachDirection(which) {
				needToReadLast[which] = needToRead[which];
				needToWriteLast[which] = needToWrite[which];

				fds[which].fd = sockbuf[which]->sock;
				fds[which].events = 0;

				if (!needToRead[which].isKnown() || (needToRead[which].getKnownValue() > 0))
					fds[which].events |= POLLIN;
				
				if(needToWrite[which] > 0)
					fds[which].events |= POLLOUT;
			}
		}

		// do the poll of the sockets and check the result
		CodeBlock() {
			int pollRes = poll(fds, DIRECTION_MAX, timeo.GetMilliseconds());
			if(pollRes == SOCKET_ERROR) {
				int errorno = WSAGetLastError();
				if(errorno == EINTR) {
					usleep(SLEEPTIME);
	 				continue;
				}
				if(errorno == EAGAIN)
					continue;
				throw "Poll error not EINTR or EAGAIN";
			}
			if(pollRes == 0) { // the timeout period elapsed without the necessary data being fulfilled
				timedOut = true;
				return;
			}

			DIRECTION which;
			ForEachDirection(which) {
			if( fds[which].revents & (POLLERR|POLLHUP|POLLNVAL ))
				throw "POLLERR|POLLHUP|POLLNVAL";
			}
		}

		CodeBlock() { // do the sends, small chunks so we can time out?
			DIRECTION which;
			ForEachDirection(which) {
				if(fds[which].revents & POLLOUT) {
					Assert(needToWrite[which] > 0);

					size_t charsSent = 0;

					while (
						!sockbuf[which]->placeholders.empty()
						&& sockbuf[which]->placeholders.front()->contentsKnown
					) {
						Placeholder* placeholder = sockbuf[which]->placeholders.front();
						Assert(!placeholder->contents.empty());

						size_t len = placeholder->contents.length();
						std::deque<Placeholder*>::iterator it = sockbuf[which]->placeholders.begin();
						Placeholder* first = it == sockbuf[which]->placeholders.end() ? NULL : *(it++);
						Placeholder* second = it == sockbuf[which]->placeholders.end() ? NULL : *(it++);
						Placeholder* third = it == sockbuf[which]->placeholders.end() ? NULL : *(it++);
						int res;
						std::string soFar;
						if (true) {
							// is same as send on bound socket?
							// Should it be okay that we pass in the sin at all times?
							res = socksendto(
								sockbuf[which]->sock,
								&sockbuf[which]->sin,
								placeholder->contents.c_str(),
								static_cast<int>(len), timeo.GetMilliseconds()
							); 
						} else {
							for (size_t index = 0; index < len; index++) {
								res = socksendto(
									sockbuf[which]->sock,
									&sockbuf[which]->sin,
									placeholder->contents.c_str() + index,
									static_cast<int>(1),
									timeo.GetMilliseconds()
								);
								if (res != 1)
									break;
								soFar += placeholder->contents[index];
							}
							if (res == 1)
								res = static_cast<int>(len);
						}

						if (res < 0) {
							int errcode = WSAGetLastError();

							if (errcode == WSAECONNABORTED) {
								socketClosed[which] = true;
								sockbuf[which]->shutdownAndClose(); // cleanup
								break;
							}

							if (errcode == WSAECONNRESET) {
								socketClosed[which] = true;
								sockbuf[which]->shutdownAndClose(); // cleanup
								break;
							}

							// Not simply a connection closed by server or client :-(
							throw "General socket writing exception.";
						}

						sockbuf[which]->bytesWrittenSoFar += placeholder->contents;
						Assert(res == len);
						sent[which] += static_cast<size_t>(res);
						sentSoFar[which] += static_cast<size_t>(res);
						Assert(static_cast<size_t>(res) <= needToWrite[which]);
						needToWrite[which] -= res;
						
						delete placeholder;

						sockbuf[which]->placeholders.pop_front();
					}
				}
			}
		}

		CodeBlock() { // do the receives
			DIRECTION which;
			ForEachDirection(which) {
				if ((fds[which].revents & POLLIN)) {
					Assert(
						!needToRead[which].isKnown()
						|| (needToRead[which].getKnownValue() > 0)
					);

					
					size_t buflen = sockbuf[which]->uncommittedBytes.length();
					if (
						(filter[which]->currentInstruction()->type == Instruction::UntilDelimiter)
						|| (filter[which]->currentInstruction()->type == Instruction::BytesExact)
					) {
						// we put the unfilteredBytes data in when it is ready...
						// so then we know the proper offset to tell the filter
					} else {
						// why read a socket when we have unfilteredBytes data?
						Assert(sockbuf[which]->unfilteredBytes.empty());
					}

					while (
						true
						/* !needToRead[which].isKnown() || (needToRead[which].getKnownValue() > 0) */
					) {
						char buffer[BUFSIZE];
						int len = sockrecvfrom(
							sockbuf[which]->sock,
							&sockbuf[which]->sin,
							buffer,
							BUFSIZE,
							timeo.GetMilliseconds()
						);

						if (len == 0) {
							readAZero[which] = true;
							needToRead[which] = 0;

							// is disconnect the right semantics?
							filterHelper(
								which,
								buflen,
								readSoFar[which],
								*filter[which],
								true
							); 

							// we got all the currently available data, we will
							// poll again later... (unless we were waiting)
							break; 

						} else if (len < 0) { 

							// error, or possibly we just need to retry later?
							int errorno = WSAGetLastError();
							if((errorno == EAGAIN) || (errorno == EINTR))
								break;
							
							if (errorno == WSAECONNABORTED) {
								socketClosed[which] = true;
								sockbuf[which]->shutdownAndClose();
								filterHelper(
									which,
									buflen,
									readSoFar[which],
									*filter[which],
									true
								);
								break;
							}

							if (errorno == WSAECONNRESET) {
								socketClosed[which] = true;
								sockbuf[which]->shutdownAndClose();
								filterHelper(
									which,
									buflen,
									readSoFar[which],
									*filter[which],
									true
								);
								break;
							}

							// Neither client nor server reset. :-/
							throw "Socket reading exception not due to reset.";

						} else {

							sockbuf[which]->bytesReadSoFar.append(buffer, len);
							received[which] += len;

							// better timeout handling?  will be easier when code is tightened
							// here we got data
							const size_t buflenInitial = buflen;
							const Instruction* instruction = filter[which]->currentInstruction();
							Assert(instruction->type != Instruction::QuitFilter);
							if (instruction->type == Instruction::BytesUnknown) {

								// If we don't know how much data we're expecting, any amount is fine
								sockbuf[which]->uncommittedBytes.append(buffer, len);
								buflen += len;

							} else if (instruction->type == Instruction::BytesMax) {
								if (static_cast<size_t>(len) <= needToRead[which].getKnownValue()) {

									// not enough data to fulfill our entire request
									// we should still offer the filter the opportunity to run, though...
									sockbuf[which]->uncommittedBytes.append(buffer, len);
									buflen += len;

								} else {

									// too much data received, we don't want the filter to see it all
									// because we might want a different filter to run
									sockbuf[which]->uncommittedBytes.append(buffer, needToRead[which].getKnownValue());
									buflen += needToRead[which].getKnownValue();
									sockbuf[which]->unfilteredBytes.append(buffer + needToRead[which].getKnownValue(), len - needToRead[which].getKnownValue());
								}

							} else if (instruction->type == Instruction::UntilDelimiter) {
								UntilDelimiterInstruction const * inst =
									dynamic_cast<UntilDelimiterInstruction const *>(instruction);

								sockbuf[which]->unfilteredBytes.append(buffer, len);
								size_t delimPos = sockbuf[which]->unfilteredBytes.find(inst->delimiter);
								if (delimPos != std::string::npos) {
									sockbuf[which]->uncommittedBytes += 
										sockbuf[which]->unfilteredBytes.substr(
											0, delimPos + inst->delimiter.length()
										);
									sockbuf[which]->unfilteredBytes.erase(0, delimPos + inst->delimiter.length());
									buflen += delimPos + inst->delimiter.length();
								}
							} else if (instruction->type == Instruction::BytesExact) {
								UntilDelimiterInstruction const * inst =
									dynamic_cast<UntilDelimiterInstruction const *>(instruction);

								if (static_cast<size_t>(len) >= needToRead[which].getKnownValue()) {
									sockbuf[which]->uncommittedBytes += sockbuf[which]->unfilteredBytes;
									buflen += sockbuf[which]->unfilteredBytes.length();
									sockbuf[which]->unfilteredBytes.clear();
									sockbuf[which]->uncommittedBytes.append(buffer, needToRead[which].getKnownValue());
									buflen += needToRead[which].getKnownValue();
									sockbuf[which]->unfilteredBytes.append(buffer + needToRead[which].getKnownValue(), len - needToRead[which].getKnownValue());
								} else {
									sockbuf[which]->unfilteredBytes.append(buffer, len);
								}

							} else {
								NotReached();
							}

							const size_t difference = buflen - buflenInitial;
							readSoFar[which] += difference;

							if (difference > 0) {
								filterHelper(which, buflenInitial, readSoFar[which], *filter[which], false);
							}

							instruction = filter[which]->currentInstruction();
							if (instruction->type == Instruction::BytesMax) {
								BytesMaxInstruction const * inst =
									dynamic_cast<BytesMaxInstruction const*>(instruction);

								needToRead[which] = inst->maxByteCount;
							} else if (instruction->type == Instruction::UntilDelimiter) {
								UntilDelimiterInstruction const * inst =
									dynamic_cast<UntilDelimiterInstruction const *>(instruction);

								if (difference > 0)
									needToRead[which] = 0;
							} else if (instruction->type == Instruction::QuitFilter) {
								needToRead[which] = 0;
							}
						}

						// TEMPORARY, WE DO NOT LOOP DUE TO POSSIBLE 
						// DEPENDENCIES OF SENDS/READS
						break; 
					}
				}
			}
		}

		if(sleeptime > 0) {
			if(sleeptime > timeo.GetMilliseconds()) {
				throw "Sleep exceeds timeout in sock mapping.";
			}
			usleep(static_cast<DWORD>(sleeptime * SLEEPTIME));
			sleeptime = 0;
		}
	} while (true);

	CodeBlock() {
		DIRECTION which;
		ForEachDirection(which) {
			Assert(filter[which]->currentInstruction()->type == Instruction::QuitFilter);
			if (sockbuf[which]->definitelyHasFutureWrites()) {
				if (sockbuf[which]->disconnected) {
					throw "Socket dropped with pending write operations.";
				} else {
					// we should have proxied all the ready data in the loop,
					// only excuse is a dead socket...
					Assert(!sockbuf[which]->hasKnownWritesPending()); 
				}
			}
		}
	}
}
