Helpers of the game-side CPU work (TASK-134), used by ../pgo-all.sh and by hand: configure.cmd and build.cmd (a game build in
build\<name>, with T134_ARGS for extra CMake arguments and VU1GEN for another VU1 folder), ee_configure.cmd and ee_build.cmd
(the EE library from its translated units), ab.sh (interleaved A/B of the lap).
Spec II needs its VU0 programs in private\ee-spec2\vu0-gen and the image in private\ee-spec2\vu0-image: see docs/BUILD.md, TASK-134.
