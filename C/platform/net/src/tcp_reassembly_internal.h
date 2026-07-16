#pragma once

#include <cstddef>
#include <cstdint>

namespace sao::net::internal {

bool tcp_packet_remote_ipv4(const uint8_t* packet, size_t size,
                            uint8_t remote_ipv4_out[4],
                            uint16_t* remote_port_out) noexcept;

}  // namespace sao::net::internal
