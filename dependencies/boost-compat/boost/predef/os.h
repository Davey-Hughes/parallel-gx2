// The BOOST_OS_* tests of Boost.Predef the emulator uses, detected the way
// Boost.Predef does (same macros, same order, so an earlier match hides a
// later one: iOS before macOS, Linux before Unix-only).
#pragma once

#if defined(__APPLE__) && defined(__MACH__) && defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
#define BOOST_OS_IOS 1
#else
#define BOOST_OS_IOS 0
#endif

#if !BOOST_OS_IOS && (defined(linux) || defined(__linux) || defined(__linux__) || defined(__gnu_linux__))
#define BOOST_OS_LINUX 1
#else
#define BOOST_OS_LINUX 0
#endif

#if !BOOST_OS_IOS && !BOOST_OS_LINUX && (defined(macintosh) || defined(Macintosh) || (defined(__APPLE__) && defined(__MACH__)))
#define BOOST_OS_MACOS 1
#else
#define BOOST_OS_MACOS 0
#endif

#if !BOOST_OS_IOS && !BOOST_OS_LINUX && !BOOST_OS_MACOS && \
	(defined(BSD) || defined(_SYSTYPE_BSD) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
	defined(__NetBSD__) || defined(__DragonFly__) || defined(__bsdi__))
#define BOOST_OS_BSD 1
#else
#define BOOST_OS_BSD 0
#endif

#if defined(unix) || defined(__unix) || defined(_XOPEN_SOURCE) || defined(_POSIX_SOURCE)
#define BOOST_OS_UNIX 1
#else
#define BOOST_OS_UNIX 0
#endif

#if !BOOST_OS_IOS && !BOOST_OS_LINUX && !BOOST_OS_MACOS && !BOOST_OS_BSD && \
	(defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__))
#define BOOST_OS_WINDOWS 1
#else
#define BOOST_OS_WINDOWS 0
#endif
