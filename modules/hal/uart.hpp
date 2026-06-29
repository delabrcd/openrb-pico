/*
 * orb::hal — byte UART interface (portable; NO SDK). A minimal raw byte transport (no
 * flow control / parity policy here): blocking single-byte and span writes, and a
 * non-blocking readable()/read_byte() pair. Bytes are std::byte -- the logic layer uses
 * std::as_bytes / std::span to feed it, keeping "raw bytes" distinct from text/ints.
 */
#ifndef ORB_HAL_UART_HPP
#define ORB_HAL_UART_HPP

#include <concepts>
#include <cstddef>
#include <span>

namespace orb::hal {

template <typename T>
concept ByteUart = requires(T u, std::byte b, std::span<const std::byte> bytes) {
    { u.write_byte(b) };                            // blocking raw TX of one byte
    { u.write(bytes) };                             // blocking raw TX of a span
    { u.readable() } -> std::convertible_to<bool>;  // is a byte available (non-blocking)
    { u.read_byte() } -> std::convertible_to<std::byte>;  // precondition: readable()
};

}  // namespace orb::hal

#endif  // ORB_HAL_UART_HPP
