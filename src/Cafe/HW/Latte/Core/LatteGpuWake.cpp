#include "Cafe/HW/Latte/Core/LatteGpuWake.h"

#include <cstdlib>

namespace
{
	// Brought up on first use and kept for the life of the process: the
	// threads that notify it come and go with each title, and freeing it
	// under one of them would be the only way this could fail.
	struct Wake
	{
		retro_eventcount_t ec{};
		Wake()
		{
			if (!retro_eventcount_init(&ec))
				std::abort();
		}
	};
}

retro_eventcount_t* LatteGpuWake_Get()
{
	static Wake s_wake;
	return &s_wake.ec;
}

void LatteGpuWake_Notify()
{
	retro_eventcount_notify(LatteGpuWake_Get());
}
