#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <orbit/config/Config.hpp>

namespace network {

/**
 * @brief Server TLS settings: certificates (with SNI) and ALPN.
 *
 * get() returns one SSL_CTX for the server's lifetime. During each
 * handshake a servername callback moves the connection to the context
 * holding the certificate for the requested name, so certificates can be
 * chosen by SNI and replaced (reload()) without touching that SSL_CTX.
 */
class TlsContext {
public:
    TlsContext(const std::string& cert_file, const std::string& key_file, config::HttpVersion http_version);
    /// @param default_cert Served when no SNI certificate matches the name.
    /// @param sni_certs Chosen by the DNS names in each one's subjectAltName (or CN).
    TlsContext(const config::TlsCertificate& default_cert, std::vector<config::TlsCertificate> sni_certs,
               config::HttpVersion http_version);
    ~TlsContext();

    // Delete copy semantics
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* get() const { return ctx_; }
    config::HttpVersion get_http_version() const { return http_version_; }

    /**
     * @brief Re-reads every certificate and key file. Thread-safe.
     *
     * New handshakes use the new certificates; established connections are
     * unaffected. If any file fails to load, the current certificates stay.
     * @param error Receives the reason on failure.
     */
    bool reload(std::string* error = nullptr);

    /// True if a certificate or key file changed since it was last loaded.
    bool files_changed() const;

    /// The certificate file that serves @p server_name (for tests and logs).
    std::string certificate_for(const std::string& server_name) const;

    /// Configures a context to match this one (protocol floor, ALPN).
    void configure(SSL_CTX* ctx);

    struct CertSet;

private:
    SSL_CTX* ctx_{nullptr};
    config::HttpVersion http_version_{config::HttpVersion::Http1_1};
    config::TlsCertificate default_cert_;
    std::vector<config::TlsCertificate> sni_certs_;

    mutable std::mutex mutex_;
    std::shared_ptr<const CertSet> certs_;
    std::string stamp_; // sizes and modification times of the loaded files

    std::shared_ptr<const CertSet> current() const;
    std::string file_stamp() const;
    static int servername_cb(SSL* ssl, int* alert, void* arg);
};

class ClientTlsContext {
public:
    /**
     * @param verify_peer Verify the server certificate chain. Hostname checks
     *        are enabled per connection with SSL_set1_host().
     * @param ca_file Optional PEM bundle of trusted CAs; the system store is
     *        used when empty.
     */
    explicit ClientTlsContext(bool verify_peer = true, const std::string& ca_file = "");
    ~ClientTlsContext();

    ClientTlsContext(const ClientTlsContext&) = delete;
    ClientTlsContext& operator=(const ClientTlsContext&) = delete;

    SSL_CTX* get() const { return ctx_; }

private:
    SSL_CTX* ctx_{nullptr};
};

} // namespace network
