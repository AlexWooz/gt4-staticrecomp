# TASK-133: free C: space before the next build

Owner explicitly requested build cleanup on2026-10-26. Stage2 is reviewed; perform this
bounded cache relocation before stage3 builds. Do not clean other agents' worktrees.

## Confirmed inventory and allowed paths

At orchestrator inspection: C: had about0.17GiB free, O: about125.94GiB free.
C:/Antigravity/gt4-wt-133-rc7-ra-controls/launcher/target contains3.57GiB in7400files.
This root is an ordinary directory, not a junction. Destination did not exist:
O:/gt4recomp-scratch/task133-cargo-target

Only this exact Cargo target cache may be removed from C: under this authorization.
The source code, Cargo.lock, .git, launcher/assets, main private inputs, baseline/candidate
native builds, distributions, published release installers/zips, cards, ISOs and other
agents' checkouts are outside this cleanup. No broad private/ deletion, cache sweeps,
registry changes, machine-wide environment changes, process killing or junctions.

## Procedure and safety gates

1. Recheck free space, source/destination and active processes using this cache. Do not
   interrupt another process; if the cache is in use, report the exact blocker and stop.
2. Resolve absolute source/destination paths. Require exact equality to the two paths
   above; destination stays under O:/gt4recomp-scratch and source stays under the task133
   workspace. Inspect the source tree for reparse points and refuse to recurse through any.
   If the destination now exists, stop and report; do not overwrite unrelated contents.
3. Write the inventory/receipt on O:, not the nearly full C:. Copy the cache to the new
   destination with native PowerShell Copy-Item -LiteralPath, preserving relative layout.
   Record relative file paths, sizes and SHA256 hashes, and verify every copied file against
   the original. Do not delete anything after an incomplete/failed copy or verification.
4. Immediately before deletion revalidate the resolved source path and absence of reparse
   points. Only after the complete copy verifies, remove the exact original target directory
   with native PowerShell Remove-Item -LiteralPath -Recurse. No shell chaining/cmd deletion,
   computed unverified recursive targets, parent deletion or cleanup outside this allowlist.
5. For subsequent task133 Rust/test/release commands set session-scoped CARGO_TARGET_DIR=
   O:/gt4recomp-scratch/task133-cargo-target. Do not create a symlink/junction or edit global
   Cargo/user configuration. Native candidate builds also stay in fresh O: output folders.
6. Measure actual free space afterward and record copied/verified files, source/destination,
   recovered space and receipt path. Do not invent an amount freed. Retain stage1/2 logs,
   reports, native artifacts and immutable release references at their existing locations.

## Result and continuation

Expected result: the task133 Cargo cache is preserved/verified on O:, its original C: target
folder is gone, and all future task133 Rust builds use O:. Report cleanup evidence in the
stage3 handoff. If successful, continue only TASK-133-STAGE-3-LAUNCHER-RA.md. If blocked, stop
before a new build. No broader cleanup permission is implied. Do not modify product code
as part of cleanup, publish/upload a release or start another task.
