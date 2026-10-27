#pragma once
namespace ps2x {
// Windows: on an unhandled SEH exception (access violation etc.) print the code,
// faulting module+offset and thread to stderr and write a minidump to
// GT3_CRASH_DUMP (default ps2EntryRunner-crash.dmp in the working directory).
// Intermittent crashes otherwise end the process without any trace.
void installCrashReporter();
// Test runs (every --boot-seconds run): no Windows error dialogs (critical
// error, fault, missing file), and an unhandled exception ends the process
// with exit code 3 once its stack and minidump are written, instead of going
// on to Windows Error Reporting.
void setTestRunErrorMode();
// Windows: PS2X_SAMPLE_PROFILE=<seconds> starts a sampling profiler that logs
// the busiest threads and functions to stderr every <seconds>.
void startSamplingProfiler();
}
