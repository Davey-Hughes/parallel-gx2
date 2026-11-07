#include "CemuLogging.h"
#include "Common/precompiled.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iterator>

#include <new>

#include "LogFileSink.h"

#include <fmt/printf.h>

uint64 s_loggingFlagMask = cemuLog_getFlag(LogType::Force);

class LoggingDispatcher
{
  private:
	static inline class DefaultLoggingCallbacks : public LoggingCallbacks
	{
	} s_defaultCallbacks;

	// No lock: every thread that logs reads this, and a reader-writer lock
	// there was a shared lock on every line. The callbacks are statics that
	// outlive any line still being dispatched to them after a switch.
	std::atomic<LoggingCallbacks*> m_loggingCallbacks{&s_defaultCallbacks};

  public:
	void Log(std::string_view filter, std::string_view message)
	{
		m_loggingCallbacks.load(std::memory_order_acquire)->Log(filter, message);
	}

	void Log(std::string_view message)
	{
		Log("", message);
	}

	void Log(std::string_view filter, std::wstring_view message)
	{
		m_loggingCallbacks.load(std::memory_order_acquire)->Log(filter, message);
	}

	void Log(std::wstring_view message)
	{
		Log("", message);
	}

	void setCallbacks(LoggingCallbacks* loggingCallbacks)
	{
		cemu_assert_debug(m_loggingCallbacks.load() == &s_defaultCallbacks);
		m_loggingCallbacks.store(loggingCallbacks, std::memory_order_release);
	}

	void clearCallbacks()
	{
		cemu_assert_debug(m_loggingCallbacks.load() != &s_defaultCallbacks);
		m_loggingCallbacks.store(&s_defaultCallbacks, std::memory_order_release);
	}
} s_loggingDispatcher;

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks)
{
	s_loggingDispatcher.setCallbacks(loggingCallbacks);
}

void cemuLog_clearCallbacks()
{
	s_loggingDispatcher.clearCallbacks();
}

// log.txt (LogFileSink.h). Never destroyed, as a detached thread may still
// be logging at exit.
static LogFileSink& LogFile()
{
	alignas(LogFileSink) static unsigned char s_storage[sizeof(LogFileSink)];
	static LogFileSink* s_sink = new (s_storage) LogFileSink();
	return *s_sink;
}

const std::map<LogType, std::string> g_logging_window_mapping
{
	{LogType::UnsupportedAPI,     "Unsupported API calls"},
	{LogType::APIErrors, 		  "Invalid API usage"},
	{LogType::CoreinitLogging,    "Coreinit Logging"},
	{LogType::CoreinitFile,       "Coreinit File-Access"},
	{LogType::CoreinitThreadSync, "Coreinit Thread-Synchronization"},
	{LogType::CoreinitMem,        "Coreinit Memory"},
	{LogType::CoreinitMP,         "Coreinit MP"},
	{LogType::CoreinitThread,     "Coreinit Thread"},
	{LogType::NN_NFP,             "nn::nfp"},
	{LogType::NN_FP,              "nn::fp"},
	{LogType::NN_BOSS,            "nn::boss"},
	{LogType::GX2,                "GX2"},
	{LogType::SoundAPI,           "Audio"},
	{LogType::InputAPI,           "Input"},
	{LogType::Socket,             "Socket"},
	{LogType::Save,               "Save"},
	{LogType::SysApi,             "System APIs"},
	{LogType::H264,               "H264"},
	{LogType::NFC,                "NFC"},
	{LogType::NTAG,               "NTAG"},
	{LogType::Patches,            "Graphic pack patches"},
	{LogType::TextureCache,       "Texture cache"},
	{LogType::TextureReadback,    "Texture readback"},
	{LogType::OpenGLLogging,      "OpenGL debug output"},
	{LogType::VulkanValidation,   "Vulkan validation layer"},
};

bool cemuLog_advancedPPCLoggingEnabled()
{
	return GetConfig().advanced_ppc_logging;
}

fs::path cemuLog_GetLogFilePath()
{
    return ActiveSettings::GetUserDataPath("log.txt");
}

static std::atomic<bool> s_fileLoggingEnabled{true};

void cemuLog_setFileLoggingEnabled(bool enabled)
{
	if (s_fileLoggingEnabled.exchange(enabled) == enabled)
		return;
	if (enabled)
	{
		cemuLog_createLogFile(false);
		return;
	}
	LogFile().Close();
}

void cemuLog_createLogFile(bool triggeredByCrash)
{
	// A crash still writes one: the frontend's log is the first thing a report
	// leaves out, and this is the file that says what happened.
	if (!triggeredByCrash && !s_fileLoggingEnabled.load())
		return;
	if (LogFile().IsOpen())
		return;
	if (!LogFile().Open(cemuLog_GetLogFilePath()))
		cemu_assert_debug(false);
}

void cemuLog_writeLineToLog(std::string_view text, bool date, bool new_line)
{
	// The whole line in one buffer, for one write
	std::string line;
	line.reserve(16 + text.size() + 1);
	if (date)
	{
		const auto now = std::chrono::system_clock::now();
		const auto temp_time = std::chrono::system_clock::to_time_t(now);
		std::tm time{};
#ifdef _WIN32
		localtime_s(&time, &temp_time);
#else
		localtime_r(&temp_time, &time);
#endif
		fmt::format_to(std::back_inserter(line), "[{:02d}:{:02d}:{:02d}.{:03d}] ", time.tm_hour, time.tm_min, time.tm_sec,
			(int)std::chrono::duration_cast<std::chrono::milliseconds>(now - std::chrono::time_point_cast<std::chrono::seconds>(now)).count());
	}
	line.append(text);
	if (new_line)
		line.push_back('\n');

	LogFile().Write(line.data(), line.size());
}

bool cemuLog_log(LogType type, std::string_view text)
{
	if (!cemuLog_isLoggingEnabled(type))
		return false;

	if (LaunchSettings::Verbose())
		std::cout << text << std::endl;

	cemuLog_writeLineToLog(text);

	const auto it = std::find_if(g_logging_window_mapping.cbegin(), g_logging_window_mapping.cend(),
		[type](const auto& entry) { return entry.first == type; });
	if (it == g_logging_window_mapping.cend())
		s_loggingDispatcher.Log(text);
	else
		s_loggingDispatcher.Log(it->second, text);

	return true;
}

bool cemuLog_log(LogType type, std::u8string_view text)
{
	std::basic_string_view<char> s((char*)text.data(), text.size());
	return cemuLog_log(type, s);
}

void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize)
{
	const uint8* dataU8 = static_cast<const uint8*>(data);
	std::vector<char> hexLine;
	hexLine.resize(6 + lineSize * 3 + 1, ' ');
	for (size_t i = 0; i < size; i += lineSize)
	{
		sprintf(hexLine.data(), "%04X: ", (int)i);
		size_t remainingSize = std::min<size_t>(lineSize, size - i);
		for (size_t j=0; j<remainingSize; j++)
			sprintf(hexLine.data() + 6 + j * 3, "%02X ", dataU8[j]);
		dataU8 += remainingSize;
		if (remainingSize == lineSize)
			hexLine.resize(6 + remainingSize * 3 + 1);
		cemuLog_log(type, hexLine.data());
	}
}

// Nothing is held back in the process any more - a line is written as it is
// logged - so all this still does is make sure there is a file to have been
// written to.
void cemuLog_waitForFlush()
{
	cemuLog_createLogFile(false);
}

void cemuLog_setActiveLoggingFlags(uint64 flagMask)
{
	s_loggingFlagMask = flagMask | cemuLog_getFlag(LogType::Force);
}
