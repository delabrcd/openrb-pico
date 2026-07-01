/*
 * orb::core::SeamAnchor<T> — the ONE typed pointer permitted per C-ABI seam.
 *
 * TinyUSB / FreeRTOS / FatFs call into the firmware through fixed `extern "C"` thunks that
 * cannot carry a `this`. Each seam TU keeps a single SeamAnchor<Handler> the thunk reaches
 * its owning object through. bind() is called ONCE during init, on the boot core, BEFORE the
 * vendor stack that fires the seam is started (tud_init/tuh_init); the pointer is read-only
 * thereafter, so there is no cross-core race even when the thunk fires on the other core.
 *
 * This is the deliberate, contained exception to the project's "no raw pointers" rule: the
 * raw pointer lives here and nowhere else. Keep EXACTLY ONE anchor per seam set — more than
 * one is the slippery slope back to ambient globals. The thunk converts the vendor's raw
 * ptr+len into std::span/typed values and dispatches with a direct concrete call:
 *
 *   // one seam TU
 *   namespace { orb::core::SeamAnchor<GuitarHidDriver> g_hid; }
 *   void bind_guitar_hid(GuitarHidDriver& h) { g_hid.bind(h); }
 *   extern "C" void tuh_hid_report_received_cb(uint8_t a, uint8_t i,
 *                                              uint8_t const* r, uint16_t n) {
 *       if (g_hid) g_hid->on_report(a, i, std::span{r, n});   // direct call, no vtable
 *   }
 */
#pragma once

namespace orb::core {

template <typename T>
class SeamAnchor {
   public:
    // Bind the handler once, at init, before the vendor stack starts.
    void bind(T& handler) { handler_ = &handler; }

    // True once bound. Thunks should guard on this so an unbound seam drops the callback
    // rather than dereferencing null (pair with a debug assert at the call site).
    explicit operator bool() const { return handler_ != nullptr; }

    // Reach the handler from inside the seam thunk. Undefined if unbound — guard with the
    // bool conversion above.
    T* operator->() const { return handler_; }
    T& operator*() const { return *handler_; }

   private:
    T* handler_ = nullptr;
};

}  // namespace orb::core
