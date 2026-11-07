#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "util/helpers/EventWake.h"

// log.txt, without a lock on the way in. Each line reserves its place in the
// file with one atomic add and is written there with one positional write, so
// lines from every thread come out whole and none is lost - without relying on
// the system to keep appends whole, which Wine for one does not. Nothing is
// buffered in the process, so a line is in the file (the page cache) as soon
// as the call returns: what a crash report needs, without the mutex every
// logging thread used to queue on, and that a thread crashing while it held it
// left the crash handler waiting on for good.
//
// Opening and closing (log.txt switched on and off, a crash) take turns with
// each other - one at a time, the rest asleep until it is done - but never
// with a write. A close takes the file away first, then closes it once no
// write that read it is still in flight; the last one out notifies.
class LogFileSink
{
public:
#ifdef _WIN32
	using Handle = HANDLE;
	static Handle None() { return INVALID_HANDLE_VALUE; }
#else
	using Handle = int;
	static Handle None() { return -1; }
#endif

	LogFileSink() = default;
	LogFileSink(const LogFileSink&) = delete;
	LogFileSink& operator=(const LogFileSink&) = delete;
	~LogFileSink() { Close(); }

	bool IsOpen() const { return m_file.load(std::memory_order_acquire) != nullptr; }

	// Created empty, unless it is already open. False if it could not be.
	bool Open(const std::filesystem::path& path)
	{
		if (IsOpen())
			return true;
		BeginControl();
		bool ok = IsOpen();
		if (!ok)
		{
			const Handle h = OpenHandle(path);
			ok = h != None();
			if (ok)
				m_file.store(new File{h}, std::memory_order_seq_cst);
		}
		EndControl();
		return ok;
	}

	void Write(const char* data, size_t size)
	{
		if (size == 0)
			return;
		m_inFlight.fetch_add(1, std::memory_order_seq_cst);
		File* f = m_file.load(std::memory_order_seq_cst);
		if (f)
			WriteAt(f->handle, data, size, f->end.fetch_add(size, std::memory_order_relaxed));
		if (m_inFlight.fetch_sub(1, std::memory_order_seq_cst) == 1)
			m_wake.Notify(); // a Close waiting for the last write
	}

	void Close()
	{
		if (!IsOpen())
			return;
		BeginControl();
		File* f = m_file.exchange(nullptr, std::memory_order_seq_cst);
		if (f)
		{
			// A write that read the file before it went is let finish
			m_wake.WaitUntil([this] { return m_inFlight.load(std::memory_order_seq_cst) == 0; });
			CloseHandleOf(f->handle);
			delete f;
		}
		EndControl();
	}

private:
	struct File
	{
		Handle handle;
		std::atomic<uint64_t> end{0};
	};

	void BeginControl()
	{
		for (;;)
		{
			if (!m_control.exchange(true, std::memory_order_acquire))
				return;
			m_wake.WaitUntil([this] { return !m_control.load(std::memory_order_acquire); });
		}
	}
	void EndControl()
	{
		m_control.store(false, std::memory_order_release);
		m_wake.Notify();
	}

#ifdef _WIN32
	static Handle OpenHandle(const std::filesystem::path& path)
	{
		return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	static void WriteAt(Handle h, const char* data, size_t size, uint64_t offset)
	{
		while (size)
		{
			OVERLAPPED ov{};
			ov.Offset = (DWORD)offset;
			ov.OffsetHigh = (DWORD)(offset >> 32);
			DWORD done = 0;
			const DWORD chunk = size > 0x40000000 ? 0x40000000 : (DWORD)size;
			if (!WriteFile(h, data, chunk, &done, &ov) || done == 0)
				return;
			data += done;
			size -= done;
			offset += done;
		}
	}
	static void CloseHandleOf(Handle h) { CloseHandle(h); }
#else
	static Handle OpenHandle(const std::filesystem::path& path)
	{
		return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	}
	static void WriteAt(Handle h, const char* data, size_t size, uint64_t offset)
	{
		while (size)
		{
			const ssize_t done = ::pwrite(h, data, size, (off_t)offset);
			if (done < 0 && errno == EINTR)
				continue;
			if (done <= 0)
				return;
			data += done;
			size -= (size_t)done;
			offset += (uint64_t)done;
		}
	}
	static void CloseHandleOf(Handle h) { ::close(h); }
#endif

	std::atomic<File*> m_file{nullptr};
	std::atomic<uint32_t> m_inFlight{0};
	std::atomic_bool m_control{false};
	EventWake m_wake;
};
