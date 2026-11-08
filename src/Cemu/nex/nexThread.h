#pragma once

void nexThread_registerService(nexService* service);
bool nexThread_isCurrentThread();
// Ends the nex thread's wait now: a request has been queued
void nexThread_wake();
// Asleep until one of the sockets has something to read, or the nex thread's
// wake if withWake (which it drains), or the timeout; false on the timeout
bool nexWaitForSockets(const SOCKET* sockets, size_t count, uint32 timeoutMs, bool withWake);
