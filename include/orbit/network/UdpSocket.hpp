#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/network/Socket.hpp>
#include <string>

namespace orbit::network {

class UdpSocket : public Socket {
public:
    UdpSocket();
    
    // Bind to a specific port
    void bind(int port);
    
    // Receive datagram and return sender address
    ssize_t recv_from(char* buffer, size_t length, sockaddr_in& sender_addr);
    
    // Send datagram to specific address
    ssize_t send_to(const char* buffer, size_t length, const sockaddr_in& dest_addr);
};

} // namespace network
