// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Implements chapter 12.2 (renderer selection and dispatch front end) at milestone M0.

#include "hw_gs_dispatch.hpp"

namespace gt4::hw {

Dispatch::Dispatch(const EnvironmentLookup &lookup)
    : renderer_(parse_renderer(lookup("GT4_RENDERER"))), options_(parse_options(lookup)) {}

Dispatch::~Dispatch() { stop(); }

bool Dispatch::start(std::string &error) {
  switch (renderer_) {
  case Renderer::donor:
    return true;
  case Renderer::parallel:
    error = "GT4_RENDERER=parallel: the paraLLEl-GS backend is not part of this build";
    return false;
  case Renderer::hw:
    break;
  }
  if (backend_) return true;
  backend_ = std::make_unique<Backend>();
  if (!backend_->init(options_, error)) {
    backend_.reset();
    return false;
  }
  return true;
}

void Dispatch::stop() {
  backend_.reset();
}

} // namespace gt4::hw
