#pragma once
// Fault injection for robustness tests. Not used by the app.
//
// After `afterBytes` bytes have been written through Core's output streams (extraction output files,
// archive output files and volumes), every further write fails with ERROR_DISK_FULL. 0 turns it off.
// The environment variable LYCAN_TEST_FAIL_WRITE_AFTER=<bytes> sets the initial value (used by CLI tests).
#include <cstdint>

namespace lycan {

void SetTestWriteFault(uint64_t afterBytes);

// Called by the output streams before each write of `size` bytes; true = pretend the disk is full.
bool TestWriteShouldFail(uint64_t size);

// Pretend the destination volume has this many free bytes (0 = use the real value).
void SetTestFreeSpace(uint64_t bytes);
// Free bytes available to the current user on the volume that holds `path` (an existing or not-yet-existing
// folder); false when it cannot be determined (network share, unusual file system, ...).
bool QueryFreeSpace(const wchar_t* path, uint64_t& freeBytes);

}  // namespace lycan
