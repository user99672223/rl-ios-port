#ifndef RL_SIMULATOR_JIT_PROBE_H
#define RL_SIMULATOR_JIT_PROBE_H
// Simulator-only smoke test of the Darwin split mapping: writes `mov w0, #42; ret` through the
// RW view, flushes, calls it through the RX view. Returns the call's result, or -1..-3 on setup
// failure. On device builds it returns -100 and does nothing.
int rl_simulator_jit_probe(void);
#endif
