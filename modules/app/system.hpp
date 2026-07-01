/*
 * orb::app::System — the composition root for the whole firmware.
 *
 * One object owns every module as a plain member (no heap, no pointers) and, in its
 * constructor, constructs them in dependency order and injects references — proper static
 * dependency injection. It is placed in `static std::optional<System>` in main.cpp and
 * `.emplace()`d inside init() AFTER set_sys_clock_khz, so hardware-touching member ctors
 * (Uart, GPIO) run post-clock. Ctors only store references + POD setup; heavy bring-up
 * (tud_init/tuh_init, hub reset, seam binds) stays in the namespaced init()/run() paths.
 *
 * SKELETON (DI re-architecture P0): intentionally empty and unreferenced. Members and
 * constructor injection are filled in incrementally P1..P6, at which point every
 * anonymous-namespace singleton and free-function forwarder is retired. See the plan in
 * docs (rearchitect/di-classes) and AGENTS.md.
 */
#pragma once

namespace orb::app {

class System {
   public:
    System() = default;

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Members (leaves -> services -> app/orchestration) and constructor injection land in
    // P1+. Accessors used by main's wiring and the seam binds are added alongside them.
};

}  // namespace orb::app
