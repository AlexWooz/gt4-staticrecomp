// SPDX-License-Identifier: MIT
// TASK-180 test2: CPU cluster, frequency and thermal diagnostics on Android (android_diagnostics.cpp). No behaviour change.
#pragma once

#include <cstdio>

namespace gt4::android_diag {

// Starts the sampler (once) and logs the cluster layout.
void start();
// F7: the counts start again; the thermal status and frequencies are logged.
void interval_start();
// F8: the share of each heavy thread on each cluster since F7, the frequencies and the thermal status.
void report(std::FILE *output);

}  // namespace gt4::android_diag
