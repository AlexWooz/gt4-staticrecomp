// TASK-011 step 2: binds the donor VIF1, GIF and VU1 entry points to gt4::mtvu (slice170 capsule only).
// Built into ps2runtime_vulkan together with the --mtvu capsule (scripts/prepare_perf_runtime.py --mtvu).
#include "mtvu_runtime.hpp"

#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"

#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_sink.hpp"
#include "gs_stream/gs_stream_client.hpp"
#endif

namespace ps2::vif {
// Defined by the --mtvu capsule (vif.cpp): hash of the EE shadow or the worker's VIF1 unit state.
std::uint64_t verify_hash(bool worker_side);
}

namespace gt4::mtvu {
namespace {

void worker_vif_packet(const std::uint8_t *data, std::uint32_t qwc) { ps2::vif::transfer(1, data, qwc); }

void worker_vif_write(std::uint32_t reg, std::uint32_t value) { ps2::vif::write(1, reg, value); }

void worker_vu_reset() {
  ps2::vif::reset_vu_side();
  ps2::vu::reset();
}

void render_gs(int path, const std::uint8_t *data, std::uint32_t qwc) { ps2::gif::render(path, data, qwc); }

void apply_signal(std::uint32_t reg, std::uint64_t value) { ps2::gs::signal(reg, value); }

std::uint64_t vif1_state_hash(bool worker_side) { return ps2::vif::verify_hash(worker_side); }

#ifdef GT4_GS_STREAM_CLIENT
// TASK-084 step 3: posts the VSYNC the client held at the guest VBlank, from the journal drain.
bool post_vsync() { return gt4::gs_stream::post_pending_vsync(); }

// TASK-116 step 1: the flare readback answered locally in Performance mode. sync("download") defers while the client
// answers the readback, the journal posts the READ_FIFO in stream order (without waiting), and the accurate path drains
// through the client's hook.
bool defer_download() { return gt4::gs_stream::readback_local_active(); }

void post_readback(std::uint32_t qwords) { gt4::gs_stream::post_read_fifo_nowait(qwords); }
#endif

}  // namespace

// The donor-mode crash of release 1.2.2 (an indeterminate post_vsync pointer): every callback must start null.
static_assert(Donor{}.post_vsync == nullptr && Donor{}.render_gs == nullptr, "Donor callbacks start null");

void start_with_donor(const Settings &settings) {
  Donor donor;
  donor.worker_vif_packet = worker_vif_packet;
  donor.worker_vif_write = worker_vif_write;
  donor.worker_vu_reset = worker_vu_reset;
  donor.render_gs = render_gs;
  donor.vif1_state_hash = vif1_state_hash;
  donor.apply_signal = apply_signal;
#ifdef GT4_GS_STREAM_CLIENT
  // TASK-041: in pcsx2 mode privileged GS writes only feed the GS stream, which needs them in stream order with the
  // GIF output; the EE keeps its own CSR/IMR/SIGLBLID copy. GT4_MTVU_PRIV_ASYNC selects whether they wait.
  if (gt4::gs_stream::stream_renderer()) {
    set_ordered_privileged_consumer(&gt4::gs_stream::post_privileged_now);
    gt4::gs_stream::set_privileged_ordering({&queue_privileged, &take_privileged_wait_us, &privileged_async});
    // TASK-062: the VU1 worker busy time for the GS process's performance overlay (OSD_STATS).
    gt4::gs_stream::set_worker_busy_source(&worker_busy_us);
    // TASK-084 step 3: the VSYNC becomes a journal entry after the frame's VU1 output (GT4_MTVU_VBLANK_JOURNAL).
    gt4::gs_stream::set_vsync_deferral(vblank_journal_active(settings));
    donor.post_vsync = &post_vsync;
    // TASK-116 step 1: the local flare readback (Performance mode) and the journal services it needs.
    gt4::mtvu::set_download_ordering({&defer_download, &post_readback});
    gt4::gs_stream::set_readback_journal({&gt4::mtvu::queue_readback, &gt4::mtvu::drain_download});
    // IN-461: the hw renderer's GS worker can read the VU1 output ring itself (GT4_MTVU_DIRECT).
    donor.direct_gs_available = &gt4::gs_stream::mtvu_direct_available;
    donor.post_gs_job = &gt4::gs_stream::on_mtvu_job;
  }
#endif
  start(settings, donor);
#ifdef GT4_GS_STREAM_CLIENT
  // IN-522: the flare table's read without the EE waiting, only with VU1 jobs on the worker and direct GS intake.
  if (gt4::mtvu::direct_gs_intake() && gt4::gs_stream::g_deferred_table.wait_copy != nullptr) {
    gt4::mtvu::set_deferred_table_copy(gt4::gs_stream::g_deferred_table.wait_copy);
    gt4::gs_stream::g_deferred_table.patches = &gt4::mtvu::deferred_table_patches;
    gt4::gs_stream::g_deferred_table.first_job = &gt4::mtvu::deferred_table_first_job;  // TASK-178 diagnostics
    gt4::gs_stream::g_deferred_table.note = &gt4::mtvu::note_deferred_table;
  }
#endif
}

}  // namespace gt4::mtvu
