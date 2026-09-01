#include <orbit/database/PostgresClient.hpp>
#include <iostream>

namespace database {

PostgresClient::PostgresClient(network::Proactor* proactor, const std::string& conninfo)
    : proactor_(proactor), conninfo_(conninfo) {}

PostgresClient::~PostgresClient() {
    if (conn_) {
        proactor_->remove(PQsocket(conn_));
        PQfinish(conn_);
    }
}

void PostgresClient::connect(std::function<void(bool)> callback) {
    conn_ = PQconnectStart(conninfo_.c_str());
    if (PQstatus(conn_) == CONNECTION_BAD) {
        callback(false);
        return;
    }
    
    PQsetnonblocking(conn_, 1);
    handle_connect(std::move(callback));
}

void PostgresClient::handle_connect(std::function<void(bool)> callback) {
    PostgresPollingStatusType status = PQconnectPoll(conn_);
    int fd = PQsocket(conn_);

    auto self = shared_from_this();
    if (status == PGRES_POLLING_READING) {
        proactor_->async_wait_read(fd, [self, cb = std::move(callback)]() {
            self->handle_connect(cb);
        });
    } else if (status == PGRES_POLLING_WRITING) {
        proactor_->async_wait_write(fd, [self, cb = std::move(callback)]() {
            self->handle_connect(cb);
        });
    } else if (status == PGRES_POLLING_OK) {
        connected_ = true;
        callback(true);
    } else if (status == PGRES_POLLING_FAILED) {
        callback(false);
    }
}

void PostgresClient::query(const std::string& sql, std::function<void(const ResultSet&)> callback) {
    if (!connected_) {
        callback(ResultSet{});
        return;
    }

    if (PQsendQuery(conn_, sql.c_str()) == 0) {
        callback(ResultSet{});
        return;
    }

    handle_query(std::move(callback));
}

void PostgresClient::handle_query(std::function<void(const ResultSet&)> callback) {
    int flush_res = PQflush(conn_);
    if (flush_res == 1) {
        // needs more writing
        auto self = shared_from_this();
        proactor_->async_wait_write(PQsocket(conn_), [self, cb = std::move(callback)]() {
            self->handle_query(cb);
        });
        return;
    } else if (flush_res == -1) {
        callback(ResultSet{});
        return;
    }

    // Flush done, now wait for read
    if (PQconsumeInput(conn_) == 0) {
        callback(ResultSet{});
        return;
    }

    if (PQisBusy(conn_)) {
        auto self = shared_from_this();
        proactor_->async_wait_read(PQsocket(conn_), [self, cb = std::move(callback)]() {
            self->handle_query(cb);
        });
        return;
    }

    // Not busy, can get result
    PGresult* res = PQgetResult(conn_);
    // Read all remaining results until null
    while (PGresult* next = PQgetResult(conn_)) {
        PQclear(res);
        res = next;
    }

    if (!res) {
        callback(ResultSet{});
        return;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        PQclear(res);
        callback(ResultSet{});
        return;
    }

    uint64_t affected_rows = 0;
    if (status == PGRES_COMMAND_OK) {
        const char* cmd_tuples = PQcmdTuples(res);
        if (cmd_tuples && cmd_tuples[0] != '\0') {
            try { affected_rows = std::stoull(cmd_tuples); } catch (...) {}
        }
    }

    int num_fields = PQnfields(res);
    auto col_map = std::make_shared<std::unordered_map<std::string, size_t>>();
    for (int i = 0; i < num_fields; ++i) {
        (*col_map)[PQfname(res, i)] = static_cast<size_t>(i);
    }

    int num_rows = PQntuples(res);
    std::vector<Row> rows;
    rows.reserve(num_rows);

    for (int r = 0; r < num_rows; ++r) {
        std::vector<std::string> vals;
        vals.reserve(num_fields);
        for (int c = 0; c < num_fields; ++c) {
            if (PQgetisnull(res, r, c)) {
                vals.push_back("");
            } else {
                vals.push_back(PQgetvalue(res, r, c));
            }
        }
        rows.emplace_back(std::move(vals), col_map);
    }

    PQclear(res);
    callback(ResultSet(std::move(rows), affected_rows));
}

} // namespace database
