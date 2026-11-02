// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Implements chapter 12.2 (renderer selection and dispatch front end) at milestone M0.
//
// The donor hooks in the game splice (alt_active, alt_present, ...) will call these
// neutral entry points. M0 implements selection and the HWR lifetime; the splice itself
// is a later step, so the game path does not reach HWR yet.
#pragma once

#include "hw_gs_backend.hpp"
#include "hw_gs_options.hpp"

#include <memory>
#include <string>

namespace gt4::hw {

class Dispatch {
public:
  // Reads GT4_RENDERER and every GT4_HW_* variable through `lookup`. Throws OptionError
  // on an unknown value; nothing is started yet.
  explicit Dispatch(const EnvironmentLookup &lookup);
  ~Dispatch();
  Dispatch(const Dispatch &) = delete;
  Dispatch &operator=(const Dispatch &) = delete;

  Renderer renderer() const { return renderer_; }
  const Options &options() const { return options_; }
  bool hw_active() const { return renderer_ == Renderer::hw && backend_ != nullptr; }

  // donor: no-op. parallel: refused, because the paraLLEl-GS backend is not part of this
  // tree. hw: creates the HWR backend. Returns false with `error` on any failure.
  bool start(std::string &error);
  void stop();

  Backend *backend() { return backend_.get(); }

private:
  Renderer renderer_;
  Options options_;
  std::unique_ptr<Backend> backend_;
};

} // namespace gt4::hw
