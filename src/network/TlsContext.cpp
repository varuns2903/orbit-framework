#include <orbit/network/TlsContext.hpp>
#include <orbit/utils/Logger.hpp>
#include <cstring>

namespace network {

static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void* arg) {
    auto* ctx = static_cast<TlsContext*>(arg);
    config::HttpVersion version = ctx ? ctx->get_http_version() : config::HttpVersion::Http3;

    if (version == config::HttpVersion::Http1_1) {
        static const unsigned char alpn_http1_1[] = {
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'
        };
        if (SSL_select_next_proto((unsigned char**)out, outlen, alpn_http1_1, sizeof(alpn_http1_1), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    } else if (version == config::HttpVersion::Http2) {
        static const unsigned char alpn_http2[] = {
            2, 'h', '2',
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'
        };
        if (SSL_select_next_proto((unsigned char**)out, outlen, alpn_http2, sizeof(alpn_http2), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    } else {
        static const unsigned char alpn_http3[] = {
            2, 'h', '3',
            2, 'h', '2',
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'
        };
        if (SSL_select_next_proto((unsigned char**)out, outlen, alpn_http3, sizeof(alpn_http3), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    }
    
    return SSL_TLSEXT_ERR_NOACK;
}

TlsContext::TlsContext(const std::string& cert_file, const std::string& key_file, config::HttpVersion http_version) 
    : http_version_(http_version) {
    const SSL_METHOD* method = TLS_server_method();
    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
        throw std::runtime_error("Unable to create SSL context");
    }

    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    
    // Set ALPN callback
    SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, this);

    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx_);
        throw std::runtime_error("Failed to load certificate file: " + cert_file);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx_);
        throw std::runtime_error("Failed to load private key file: " + key_file);
    }

    if (!SSL_CTX_check_private_key(ctx_)) {
        SSL_CTX_free(ctx_);
        throw std::runtime_error("Private key does not match the certificate public key");
    }

    LOG_INFO("TLS Context initialized successfully with " << cert_file);
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

ClientTlsContext::ClientTlsContext() {
    const SSL_METHOD* method = TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
        throw std::runtime_error("Unable to create Client SSL context");
    }
    
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    // Optional: Load default verify paths for CA certificates
    SSL_CTX_set_default_verify_paths(ctx_);
}

ClientTlsContext::~ClientTlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

} // namespace network
