/*
 * platform::pico — byte UART implementation (boundary: may include the SDK). Wraps a
 * uart_inst_t with raw (no flow-control) byte I/O. Satisfies orb::hal::ByteUart. The
 * instance + pins + baud are construction parameters (board-specific); the HAL contract is
 * only the byte transport. uart_putc_raw is used (not uart_putc) to avoid the SDK's CR->
 * CRLF translation -- the log/MIDI streams are already byte-exact.
 */
#ifndef ORB_PLATFORM_PICO_UART_HPP
#define ORB_PLATFORM_PICO_UART_HPP

#include <cstddef>
#include <span>

#include "hardware/gpio.h"
#include "hardware/uart.h"

namespace orb::platform::pico {

class Uart {
   public:
    Uart(uart_inst_t* inst, unsigned tx_pin, unsigned rx_pin, unsigned baud) : inst_(inst) {
        uart_init(inst_, baud);
        gpio_set_function(tx_pin, GPIO_FUNC_UART);
        gpio_set_function(rx_pin, GPIO_FUNC_UART);
    }

    void write_byte(std::byte b) const { uart_putc_raw(inst_, static_cast<char>(b)); }
    void write(std::span<const std::byte> bytes) const {
        for (std::byte b : bytes) uart_putc_raw(inst_, static_cast<char>(b));
    }
    bool readable() const { return uart_is_readable(inst_); }
    std::byte read_byte() const { return static_cast<std::byte>(uart_getc(inst_)); }

   private:
    uart_inst_t* inst_;
};

}  // namespace orb::platform::pico

#endif  // ORB_PLATFORM_PICO_UART_HPP
