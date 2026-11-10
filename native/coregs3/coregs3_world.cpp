// TASK-090 CoreGS 3.0: the world item selection rule. See coregs3_world.hpp.
#include "coregs3_world.hpp"

namespace gt4::coregs3 {

const char *world_exclusion_name(WorldExclusion exclusion) noexcept {
  switch (exclusion) {
  case WorldExclusion::none: return "selected";
  case WorldExclusion::not_world_pass: return "not-world-pass";
  case WorldExclusion::itop_other: return "itop-other";
  case WorldExclusion::shape_refused: return "shape-refused";
  case WorldExclusion::no_position: return "no-position";
  case WorldExclusion::no_state: return "no-state";
  case WorldExclusion::mirror: return "mirror-view";
  }
  return "unknown";
}

WorldExclusion world_item_exclusion(const WorldItemInput &item) noexcept {
  if (item.pass != kPassSky && item.pass != kPassFarUnderlay && item.pass != kPassCourse)
    return WorldExclusion::not_world_pass;
  if (item.pass != kPassCourse) {
    // The sky and far-underlay draws are entirely in the picture; their items are not filtered by program.
    return item.has_position ? WorldExclusion::none : WorldExclusion::no_position;
  }
  if (!item.run_valid) return WorldExclusion::itop_other;
  switch (item.dispatch) {
  case 0x0B08u:  // the nine-qword first-power glass/lamp family
  case 0x00A0u:  // the two-key morph family
  case 0x1278u:  // the float/normal pre-pass into the body/paint output
  case 0x0080u:  // the main view
    break;
  default: return WorldExclusion::itop_other;
  }
  if (!item.has_position) return WorldExclusion::no_position;
  if (item.shape_status != WorldShapeStatus::ok && item.shape_status != WorldShapeStatus::morph_uv_first_key &&
      item.shape_status != WorldShapeStatus::morph_colour_first_key)
    return WorldExclusion::shape_refused;
  if (!item.state_known || !item.xyoffset_known) return WorldExclusion::no_state;
  if (item.xyoffset != kMainViewXyOffset) return WorldExclusion::mirror;
  if (item.test_zero) return WorldExclusion::no_state;
  if (!item.test_known || item.test == 0) return WorldExclusion::no_state;
  return WorldExclusion::none;
}

}  // namespace gt4::coregs3
