#include <orbit/database/MysqlClient.hpp>
#include <iostream>

namespace database {

MysqlClient::MysqlClient(network::Proactor& proactor, const Config& config)
    : proactor_(proactor), config_(config) {
    mysql_ = mysql_init(nullptr);
    if (!mysql_) {
        throw std::runtime_error("mysql_init failed");
    }
    mysql_options(mysql_, MYSQL_OPT_NONBLOCK, nullptr);
}

MysqlClient::~MysqlClient() {
    close();
}

void MysqlClient::close() {
    if (mysql_) {
        mysql_close(mysql_); // Could technically be async too with mysql_close_start, but typically fast enough
        mysql_ = nullptr;
    }
}

void MysqlClient::wait_for_status(int status, std::function<void()> callback) {
    int fd = mysql_get_socket(mysql_);
    if (status & MYSQL_WAIT_READ) {
        proactor_.async_wait_read(fd, callback);
    } else if (status & MYSQL_WAIT_WRITE) {
        proactor_.async_wait_write(fd, callback);
    } else if (status & MYSQL_WAIT_TIMEOUT) {
        // Fallback to read/write polling with timeout logic if needed, simplify for now
        proactor_.async_wait_read(fd, callback);
    } else {
        // Status 0 means complete!
        callback();
    }
}

// ConnectAwaiter
void MysqlClient::ConnectAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro = h;
    status = mysql_real_connect_start(&ret_ptr, client.get_mysql(), 
                                      client.config_.host.c_str(), 
                                      client.config_.user.c_str(), 
                                      client.config_.password.c_str(), 
                                      client.config_.dbname.c_str(), 
                                      static_cast<unsigned int>(client.config_.port), 
                                      nullptr, 0);
    resume_loop();
}

void MysqlClient::ConnectAwaiter::resume_loop() {
    if (status == 0) {
        coro.resume();
    } else {
        client.wait_for_status(status, [this]() {
            status = mysql_real_connect_cont(&ret_ptr, client.get_mysql(), status);
            resume_loop();
        });
    }
}

void MysqlClient::ConnectAwaiter::await_resume() {
    if (!ret_ptr) {
        throw std::runtime_error(std::string("MySQL Connect Error: ") + mysql_error(client.get_mysql()));
    }
    client.connected_ = true;
}

MysqlClient::ConnectAwaiter MysqlClient::connect_async() {
    return ConnectAwaiter{*this, nullptr, 0, {}};
}

// QueryAwaiter
void MysqlClient::QueryAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro = h;
    status = mysql_real_query_start(&err, client.get_mysql(), query.c_str(), query.length());
    resume_loop();
}

void MysqlClient::QueryAwaiter::resume_loop() {
    if (status == 0) {
        if (err != 0) {
            coro.resume();
            return;
        }
        process_result_start();
    } else {
        client.wait_for_status(status, [this]() {
            status = mysql_real_query_cont(&err, client.get_mysql(), status);
            resume_loop();
        });
    }
}

void MysqlClient::QueryAwaiter::process_result_start() {
    status = mysql_store_result_start(&res, client.get_mysql());
    process_result_cont();
}

void MysqlClient::QueryAwaiter::process_result_cont() {
    if (status == 0) {
        if (!res) {
            // No result set (e.g. INSERT, UPDATE)
            if (mysql_field_count(client.get_mysql()) == 0) {
                result.affected_rows = mysql_affected_rows(client.get_mysql());
            } else {
                err = 1; // Error reading result
            }
            coro.resume();
        } else {
            unsigned int num_fields = mysql_num_fields(res);
            MYSQL_ROW current_row;
            while ((current_row = mysql_fetch_row(res))) {
                Row r;
                for(unsigned int i = 0; i < num_fields; i++) {
                    r.columns.push_back(current_row[i] ? current_row[i] : "");
                }
                result.rows.push_back(std::move(r));
            }
            mysql_free_result(res);
            coro.resume();
        }
    } else {
        client.wait_for_status(status, [this]() {
            status = mysql_store_result_cont(&res, client.get_mysql(), status);
            process_result_cont();
        });
    }
}

MysqlClient::QueryResult MysqlClient::QueryAwaiter::await_resume() {
    if (err != 0) {
        throw std::runtime_error(std::string("MySQL Query Error: ") + mysql_error(client.get_mysql()));
    }
    return result;
}

MysqlClient::QueryAwaiter MysqlClient::query_async(std::string query) {
    return QueryAwaiter{*this, std::move(query), 0, 0, {}, {}, nullptr};
}

} // namespace database
