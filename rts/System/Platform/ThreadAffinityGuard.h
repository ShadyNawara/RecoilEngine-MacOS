#ifndef THREAD_AFFINITY_GUARD_H__
#define THREAD_AFFINITY_GUARD_H__

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_types.h>
#include <mach/thread_policy.h>
#else
#include <sched.h>
#endif

class ThreadAffinityGuard {
private:
#ifdef _WIN32
	DWORD_PTR savedAffinity;
	HANDLE threadHandle;
#elif defined(__APPLE__)
	// macOS doesn't expose "get affinity" — we only remember what the user
	// asked for and reset to the no-affinity tag on destruction. This is a
	// best-effort hint, as THREAD_AFFINITY_POLICY is advisory on Darwin.
	thread_port_t machThread;
	thread_affinity_policy_data_t savedAffinity;
#else
	cpu_set_t savedAffinity;
	pid_t tid;
#endif
	bool affinitySaved;

public:
	// Constructor: Saves the current thread's affinity
	ThreadAffinityGuard();

	// Destructor: Restores the saved affinity if it was successfully stored
	~ThreadAffinityGuard();

	// Delete copy constructor to prevent copying
	ThreadAffinityGuard(const ThreadAffinityGuard&) = delete;

	// Delete copy assignment operator to prevent assignment
	ThreadAffinityGuard& operator=(const ThreadAffinityGuard&) = delete;
};

#endif
