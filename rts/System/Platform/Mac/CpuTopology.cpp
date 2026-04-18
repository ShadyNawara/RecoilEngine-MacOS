#include "System/Platform/CpuTopology.h"

// macOS stubs: Apple Silicon and Intel Macs don't expose the detailed
// performance/efficiency core masks or per-CPU cache topology used on
// Linux/Windows, and thread pinning isn't supported on Darwin in the same
// way. Returning empty data disables CPU-aware thread pinning; the thread
// pool still runs, it just doesn't pin.

namespace cpu_topology {

ThreadPinPolicy GetThreadPinPolicy() {
	return THREAD_PIN_POLICY_NONE;
}

ProcessorMasks GetProcessorMasks() {
	return {};
}

ProcessorCaches GetProcessorCache() {
	return {};
}

}
