#include <orbit/server/QuicHttp3Session.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/http/HttpResponse.hpp>

#include <orbit/server/QuicConnection.hpp>
#include <orbit/routing/Router.hpp>
#include <iostream>

#ifdef _WIN32
#undef DELETE
#undef ERROR
#endif

namespace server {

QuicHttp3Session::QuicHttp3Session(QuicConnection& quic_conn) : quic_conn_(quic_conn) {
}

QuicHttp3Session::~QuicHttp3Session() {
    if (httpconn_) {
        nghttp3_conn_del(httpconn_);
    }
}

bool QuicHttp3Session::init() {
    nghttp3_callbacks callbacks{};
    callbacks.acked_stream_data = on_acked_stream_data;
    callbacks.stream_close = on_stream_close;
    callbacks.recv_data = on_recv_data;
    callbacks.deferred_consume = on_deferred_consume;
    callbacks.begin_headers = on_begin_headers;
    callbacks.recv_header = on_recv_header;
    callbacks.end_headers = on_end_headers;
    callbacks.end_stream = on_end_stream;

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);

    int rv = nghttp3_conn_server_new(&httpconn_, &callbacks, &settings, nullptr, this);
    if (rv != 0) {
        LOG_ERROR("nghttp3_conn_server_new failed: " << nghttp3_strerror(rv));
        return false;
    }
    
    // We should bind the control streams here using nghttp3_conn_bind_control_stream 
    // etc., but ngtcp2 handles this via bidi/uni stream creation.
    // Actually, nghttp3 requires creating a local control stream and QPACK streams.
    
    return true;
}

nghttp3_ssize QuicHttp3Session::process_stream_data(int64_t stream_id, const uint8_t* data, size_t datalen, bool fin) {
    if (!httpconn_) return 0;
    
    nghttp3_ssize consumed = nghttp3_conn_read_stream(httpconn_, stream_id, data, datalen, fin);
    if (consumed < 0) {
        LOG_ERROR("nghttp3_conn_read_stream failed: rv=" << consumed << " msg=" << nghttp3_strerror(static_cast<int>(consumed)));
    }
    return consumed;
}

int QuicHttp3Session::write_streams() {
    if (!httpconn_) return 0;
    
    // This function will poll nghttp3 for data to write, and feed it into ngtcp2.
    // It's a loop that calls nghttp3_conn_writev_stream and then ngtcp2_conn_writev_stream
    return 0;
}

std::shared_ptr<Http3Stream> QuicHttp3Session::get_or_create_stream(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }
    auto stream = std::make_shared<Http3Stream>(stream_id);
    streams_[stream_id] = stream;
    return stream;
}

int QuicHttp3Session::on_acked_stream_data(nghttp3_conn * /*conn*/, int64_t /*stream_id*/, uint64_t /*datalen*/, void * /*conn_user_data*/, void * /*stream_user_data*/) {
    return 0;
}

int QuicHttp3Session::on_stream_close(nghttp3_conn * /*conn*/, int64_t stream_id, uint64_t /*app_error_code*/, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    session->streams_.erase(stream_id);
    return 0;
}

int QuicHttp3Session::on_recv_data(nghttp3_conn * /*conn*/, int64_t stream_id, const uint8_t *data, size_t datalen, void *conn_user_data, void * /*stream_user_data*/) {
    LOG_DEBUG("H3: on_recv_data stream=" << stream_id << " len=" << datalen);
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    stream->body_buffer.append(reinterpret_cast<const char*>(data), datalen);
    // read_stream's return value excludes DATA payload delivered here; the
    // application returns that credit once it has taken the bytes.
    session->quic_conn_.extend_stream_credit(stream_id, datalen);
    return 0;
}

int QuicHttp3Session::on_end_stream(nghttp3_conn *, int64_t stream_id, void *conn_user_data, void *) {
    // The request (with its body) is complete.
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    if (stream->headers_complete && !stream->dispatched) session->handle_request(stream);
    return 0;
}

int QuicHttp3Session::on_deferred_consume(nghttp3_conn *, int64_t stream_id, size_t consumed, void *conn_user_data, void *) {
    // Data nghttp3 held back earlier has now been processed: return its credit.
    static_cast<QuicHttp3Session*>(conn_user_data)->quic_conn_.extend_stream_credit(stream_id, consumed);
    return 0;
}

int QuicHttp3Session::on_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data, void *stream_user_data) {
    LOG_DEBUG("H3: on_begin_headers stream=" << stream_id);
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    nghttp3_conn_set_stream_user_data(conn, stream_id, stream.get());
    return 0;
}

int QuicHttp3Session::on_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token, nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags, void *conn_user_data, void *stream_user_data) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    
    auto name_buf = nghttp3_rcbuf_get_buf(name);
    auto value_buf = nghttp3_rcbuf_get_buf(value);
    
    std::string header_name(reinterpret_cast<const char*>(name_buf.base), name_buf.len);
    std::string header_value(reinterpret_cast<const char*>(value_buf.base), value_buf.len);
    
    
    if (header_name == ":method") {
        if (header_value == "GET") stream->request.method = http::HttpMethod::GET;
        else if (header_value == "POST") stream->request.method = http::HttpMethod::POST;
        else if (header_value == "PUT") stream->request.method = http::HttpMethod::PUT;
        else if (header_value == "DELETE") stream->request.method = http::HttpMethod::DELETE;
        else if (header_value == "PATCH") stream->request.method = http::HttpMethod::PATCH;
        else stream->request.method = http::HttpMethod::UNKNOWN;
    } else if (header_name == ":path") {
        stream->request.uri = header_value;
    } else if (header_name == ":scheme") {
        // scheme
    } else if (header_name == ":authority") {
        stream->request.set_header("Host", std::string(header_value));
    } else {
        // Owned storage: views into a growing vector dangled on reallocation.
        stream->request.set_header(header_name, std::string(header_value));
    }
    
    return 0;
}

int QuicHttp3Session::on_end_headers(nghttp3_conn *conn, int64_t stream_id, int fin, void *conn_user_data, void *stream_user_data) {
    LOG_DEBUG("H3: on_end_headers stream=" << stream_id << " fin=" << fin);
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    stream->headers_complete = true;
    
    if (fin && !stream->dispatched) {
        session->handle_request(stream);
    }
    return 0;
}

void QuicHttp3Session::handle_request(std::shared_ptr<Http3Stream> stream) {
    stream->dispatched = true;
    stream->request.body = stream->body_buffer;
    LOG_DEBUG("HTTP/3 Request received on stream " << stream->stream_id << " URI: " << stream->request.uri);
    
    stream->response_body = "Hello from Antigravity HTTP/3!\n";
    std::string cl = std::to_string(stream->response_body.size());
    
    std::vector<nghttp3_nv> nva = {
        nghttp3_nv{ (uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP3_NV_FLAG_NONE },
        nghttp3_nv{ (uint8_t*)"server", (uint8_t*)"antigravity", 6, 11, NGHTTP3_NV_FLAG_NONE },
        nghttp3_nv{ (uint8_t*)"content-type", (uint8_t*)"text/plain", 12, 10, NGHTTP3_NV_FLAG_NONE },
        nghttp3_nv{ (uint8_t*)"content-length", (uint8_t*)cl.c_str(), 14, cl.size(), NGHTTP3_NV_FLAG_NONE }
    };
    
    // Set up data reader
    nghttp3_data_reader dr;
    dr.read_data = [](nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec, size_t veccnt, uint32_t *pflags, void *conn_user_data, void *stream_user_data) -> nghttp3_ssize {
        (void)conn;
        (void)stream_id;
        (void)veccnt;
        (void)conn_user_data;
        auto s = static_cast<Http3Stream*>(stream_user_data);
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        if (s->response_given || s->response_body.empty()) {
            return 0;
        }
        // The buffer stays untouched until the stream closes: nghttp3 and
        // ngtcp2 reference it until the peer acknowledges the data.
        vec[0].base = (uint8_t*)s->response_body.data();
        vec[0].len = s->response_body.size();
        s->response_given = true;
        return 1;
    };
    
    int rv = nghttp3_conn_submit_response(httpconn_, stream->stream_id, nva.data(), nva.size(), &dr);
    if (rv != 0) {
        LOG_ERROR("nghttp3_conn_submit_response failed: " << nghttp3_strerror(rv));
    }
}

} // namespace server
