#include <orbit/network/TlsContext.hpp>
#include <orbit/utils/Logger.hpp>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <openssl/x509v3.h>

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

// Certificates in force: one context per certificate, indexed by the names
// it serves. Replaced as a whole on reload.
struct TlsContext::CertSet {
    SSL_CTX* fallback = nullptr;
    std::string fallback_file;
    std::unordered_map<std::string, std::pair<SSL_CTX*, std::string>> exact;
    std::unordered_map<std::string, std::pair<SSL_CTX*, std::string>> wildcard; // "*.example.com" -> "example.com"
    std::vector<SSL_CTX*> owned;

    ~CertSet() {
        for (SSL_CTX* ctx : owned) SSL_CTX_free(ctx);
    }

    // Exact name first, then a wildcard for its parent domain, then the default.
    std::pair<SSL_CTX*, std::string> select(std::string name) const {
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!name.empty() && name.back() == '.') name.pop_back();
        if (!name.empty()) {
            if (auto it = exact.find(name); it != exact.end()) return it->second;
            size_t dot = name.find('.');
            if (dot != std::string::npos) {
                if (auto it = wildcard.find(name.substr(dot + 1)); it != wildcard.end()) return it->second;
            }
        }
        return {fallback, fallback_file};
    }
};

namespace {

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The DNS names a certificate is for: its subjectAltName dNSName entries,
// or its subject CN when it has none (RFC 6125 section 6.4.4).
std::vector<std::string> certificate_names(X509* cert) {
    std::vector<std::string> names;
    auto* sans = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans) {
        for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
            const GENERAL_NAME* gn = sk_GENERAL_NAME_value(sans, i);
            if (gn->type != GEN_DNS) continue;
            const ASN1_STRING* dns = gn->d.dNSName;
            names.push_back(lowercase(std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(dns)),
                                                  static_cast<size_t>(ASN1_STRING_length(dns)))));
        }
        GENERAL_NAMES_free(sans);
    }
    if (names.empty()) {
        char cn[256] = {};
        int len = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
        if (len > 0) names.push_back(lowercase(std::string(cn, static_cast<size_t>(len))));
    }
    return names;
}

std::string openssl_error() {
    unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (code == 0) return "unknown error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

} // namespace

void TlsContext::configure(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // The ALPN callback is looked up on the connection's current context,
    // which the servername callback may have switched to this one.
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, this);
}

TlsContext::TlsContext(const std::string& cert_file, const std::string& key_file, config::HttpVersion http_version)
    : TlsContext(config::TlsCertificate{cert_file, key_file}, {}, http_version) {}

TlsContext::TlsContext(const config::TlsCertificate& default_cert, std::vector<config::TlsCertificate> sni_certs,
                       config::HttpVersion http_version)
    : http_version_(http_version), default_cert_(default_cert), sni_certs_(std::move(sni_certs)) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        throw std::runtime_error("Unable to create SSL context");
    }
    configure(ctx_);
    // Called for every handshake, with or without SNI: picks the certificate.
    SSL_CTX_set_tlsext_servername_callback(ctx_, servername_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx_, this);

    std::string error;
    if (!reload(&error)) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error(error);
    }
    // The base context also carries the default certificate, so it can
    // complete a handshake on its own.
    if (SSL_CTX_use_certificate_chain_file(ctx_, default_cert_.cert_file.c_str()) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx_, default_cert_.key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        std::string reason = openssl_error();
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Failed to load certificate " + default_cert_.cert_file + ": " + reason);
    }

    LOG_INFO("TLS Context initialized successfully with " << default_cert_.cert_file
             << (sni_certs_.empty() ? "" : " and " + std::to_string(sni_certs_.size()) + " SNI certificate(s)"));
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

bool TlsContext::reload(std::string* error) {
    auto set = std::make_shared<CertSet>();
    std::string stamp = file_stamp(); // before reading: a change during the load is seen next time

    auto load = [&](const config::TlsCertificate& files, std::string& reason) -> SSL_CTX* {
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) {
            reason = "unable to create SSL context";
            return nullptr;
        }
        set->owned.push_back(ctx);
        configure(ctx);
        if (SSL_CTX_use_certificate_chain_file(ctx, files.cert_file.c_str()) <= 0) {
            reason = "failed to load certificate " + files.cert_file + ": " + openssl_error();
            return nullptr;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, files.key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
            reason = "failed to load private key " + files.key_file + ": " + openssl_error();
            return nullptr;
        }
        if (!SSL_CTX_check_private_key(ctx)) {
            reason = "private key " + files.key_file + " does not match certificate " + files.cert_file;
            return nullptr;
        }
        return ctx;
    };

    std::string reason;
    set->fallback = load(default_cert_, reason);
    set->fallback_file = default_cert_.cert_file;
    for (const auto& files : sni_certs_) {
        if (!set->fallback) break;
        SSL_CTX* ctx = load(files, reason);
        if (!ctx) {
            set->fallback = nullptr;
            break;
        }
        for (const std::string& name : certificate_names(SSL_CTX_get0_certificate(ctx))) {
            auto& table = name.rfind("*.", 0) == 0 ? set->wildcard : set->exact;
            std::string key = name.rfind("*.", 0) == 0 ? name.substr(2) : name;
            table.emplace(key, std::make_pair(ctx, files.cert_file)); // the first certificate listed wins
        }
    }
    if (!set->fallback) {
        if (current()) LOG_ERROR("TLS reload failed, keeping the current certificates: " << reason);
        if (error) *error = reason; // at startup the constructor throws it
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bool first = certs_ == nullptr;
    certs_ = std::move(set);
    stamp_ = std::move(stamp);
    if (!first) LOG_INFO("TLS certificates reloaded");
    return true;
}

std::shared_ptr<const TlsContext::CertSet> TlsContext::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return certs_;
}

std::string TlsContext::file_stamp() const {
    std::string stamp;
    auto add = [&stamp](const std::string& path) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        auto time = std::filesystem::last_write_time(path, ec);
        stamp += path + '|' + std::to_string(ec ? 0 : size) + '|' +
                 std::to_string(ec ? 0 : time.time_since_epoch().count()) + '\n';
    };
    add(default_cert_.cert_file);
    add(default_cert_.key_file);
    for (const auto& files : sni_certs_) {
        add(files.cert_file);
        add(files.key_file);
    }
    return stamp;
}

bool TlsContext::files_changed() const {
    std::string now = file_stamp();
    std::lock_guard<std::mutex> lock(mutex_);
    return now != stamp_;
}

std::string TlsContext::certificate_for(const std::string& server_name) const {
    auto set = current();
    return set ? set->select(server_name).second : std::string();
}

int TlsContext::servername_cb(SSL* ssl, int* alert, void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    auto set = self->current();
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    SSL_CTX* chosen = set->select(name ? name : "").first;
    // The connection takes its own reference, so a reload cannot free the
    // context under a handshake in progress.
    if (SSL_set_SSL_CTX(ssl, chosen) != chosen) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    return SSL_TLSEXT_ERR_OK;
}

ClientTlsContext::ClientTlsContext(bool verify_peer, const std::string& ca_file) {
    const SSL_METHOD* method = TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
        throw std::runtime_error("Unable to create Client SSL context");
    }
    
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    if (!ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ctx_, ca_file.c_str(), nullptr) != 1) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            throw std::runtime_error("Failed to load CA file: " + ca_file);
        }
    } else {
        SSL_CTX_set_default_verify_paths(ctx_);
    }

    // Without SSL_VERIFY_PEER the handshake succeeds against any certificate.
    SSL_CTX_set_verify(ctx_, verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
}

ClientTlsContext::~ClientTlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

} // namespace network
