#include <orbit/database/PostgresClient.hpp>
#include <iostream>

namespace orbit::database {

PostgresClient::PostgresClient(network::Proactor* proactor, const std::string& conninfo)
    : proactor_(proactor), conninfo_(conninfo) {}

PostgresClient::~PostgresClient() {
    close_connection();
}

void PostgresClient::close_connection() {
    if (conn_) {
        proactor_->remove(PQsocket(conn_));
        PQfinish(conn_);
        conn_ = nullptr;
    }
    connected_ = false;
    prepared_.clear(); // prepared statements belong to the server session
}

bool PostgresClient::is_healthy() const {
    return connected_ && conn_ && PQstatus(conn_) == CONNECTION_OK;
}

bool PostgresClient::in_transaction() const {
    if (!conn_) return false;
    PGTransactionStatusType st = PQtransactionStatus(conn_);
    return st == PQTRANS_INTRANS || st == PQTRANS_INERROR || st == PQTRANS_ACTIVE;
}

void PostgresClient::reconnect(std::function<void(bool)> callback) {
    close_connection();
    connect(std::move(callback));
}

void PostgresClient::connect(std::function<void(bool)> callback) {
    if (conn_) close_connection();
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
        finish_connect(std::move(callback));
    } else if (status == PGRES_POLLING_FAILED) {
        callback(false);
    }
}

void PostgresClient::finish_connect(std::function<void(bool)> callback) {
    if (statement_timeout_.count() <= 0) {
        callback(true);
        return;
    }
    // A plain integer: no value is spliced from outside.
    std::string sql = "SET statement_timeout = " + std::to_string(statement_timeout_.count());
    query(sql, [cb = std::move(callback)](const ResultSet& res) { cb(res.ok()); });
}

void PostgresClient::begin(std::function<void(const ResultSet&)> callback) {
    query("BEGIN", std::move(callback));
}

void PostgresClient::commit(std::function<void(const ResultSet&)> callback) {
    // COMMIT of a failed transaction "succeeds" with a ROLLBACK tag; report
    // it as the failure it is.
    bool failed = conn_ && PQtransactionStatus(conn_) == PQTRANS_INERROR;
    query("COMMIT", [failed, cb = std::move(callback)](const ResultSet& res) {
        if (failed && res.ok()) {
            cb(ResultSet::failure("transaction was rolled back after an earlier error"));
        } else {
            cb(res);
        }
    });
}

void PostgresClient::rollback(std::function<void(const ResultSet&)> callback) {
    query("ROLLBACK", std::move(callback));
}

void PostgresClient::execute(const std::string& sql, const std::vector<std::optional<std::string>>& params,
                             std::function<void(const ResultSet&)> callback) {
    if (!connected_) {
        callback(ResultSet::failure("not connected"));
        return;
    }
    auto run_prepared = [params](std::shared_ptr<PostgresClient> self, const std::string& name,
                                 std::function<void(const ResultSet&)> cb) {
        std::vector<const char*> values;
        values.reserve(params.size());
        for (const auto& p : params) values.push_back(p ? p->c_str() : nullptr);
        if (PQsendQueryPrepared(self->conn_, name.c_str(), static_cast<int>(values.size()),
                                values.empty() ? nullptr : values.data(), nullptr, nullptr, 0) == 0) {
            cb(ResultSet::failure(PQerrorMessage(self->conn_)));
            return;
        }
        self->handle_query(std::move(cb));
    };

    auto self = shared_from_this();
    if (auto it = prepared_.find(sql); it != prepared_.end()) {
        run_prepared(self, it->second, std::move(callback));
        return;
    }
    if (prepared_.size() >= max_prepared_) {
        query(sql, params, std::move(callback)); // cache full: run unprepared
        return;
    }

    std::string name = "orbit_" + std::to_string(next_statement_++);
    if (PQsendPrepare(conn_, name.c_str(), sql.c_str(), static_cast<int>(params.size()), nullptr) == 0) {
        callback(ResultSet::failure(PQerrorMessage(conn_)));
        return;
    }
    handle_query([self, sql, name, run_prepared, cb = std::move(callback)](const ResultSet& prepared) mutable {
        if (!prepared.ok()) {
            cb(prepared); // e.g. a syntax error: nothing is cached
            return;
        }
        self->prepared_[sql] = name;
        run_prepared(self, name, std::move(cb));
    });
}

void PostgresClient::query(const std::string& sql, std::function<void(const ResultSet&)> callback) {
    if (!connected_) {
        callback(ResultSet::failure("not connected"));
        return;
    }

    if (PQsendQuery(conn_, sql.c_str()) == 0) {
        callback(ResultSet::failure(PQerrorMessage(conn_)));
        return;
    }

    handle_query(std::move(callback));
}

void PostgresClient::query(const std::string& sql, const std::vector<std::optional<std::string>>& params,
                           std::function<void(const ResultSet&)> callback) {
    if (!connected_) {
        callback(ResultSet::failure("not connected"));
        return;
    }

    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
        values.push_back(p ? p->c_str() : nullptr);
    }

    if (PQsendQueryParams(conn_, sql.c_str(), static_cast<int>(values.size()), nullptr,
                          values.empty() ? nullptr : values.data(), nullptr, nullptr, 0) == 0) {
        callback(ResultSet::failure(PQerrorMessage(conn_)));
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
        callback(ResultSet::failure(PQerrorMessage(conn_)));
        return;
    }

    // Flush done, now wait for read
    if (PQconsumeInput(conn_) == 0) {
        callback(ResultSet::failure(PQerrorMessage(conn_)));
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
    // Collect every result: a multi-statement query stops at its first
    // failing statement, and that error must not be masked.
    PGresult* res = nullptr;
    std::string error;
    while (PGresult* next = PQgetResult(conn_)) {
        ExecStatusType st = PQresultStatus(next);
        if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK && st != PGRES_EMPTY_QUERY && error.empty()) {
            const char* msg = PQresultErrorMessage(next);
            error = (msg && *msg) ? msg : PQresStatus(st);
        }
        if (res) PQclear(res);
        res = next;
    }

    if (!error.empty()) {
        if (res) PQclear(res);
        while (!error.empty() && (error.back() == '\n' || error.back() == ' ')) error.pop_back();
        callback(ResultSet::failure(error));
        return;
    }

    if (!res) {
        callback(ResultSet{});
        return;
    }

    ExecStatusType status = PQresultStatus(res);

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
        std::vector<std::optional<std::string>> vals;
        vals.reserve(num_fields);
        for (int c = 0; c < num_fields; ++c) {
            if (PQgetisnull(res, r, c)) {
                vals.push_back(std::nullopt);
            } else {
                vals.emplace_back(std::string(PQgetvalue(res, r, c), static_cast<size_t>(PQgetlength(res, r, c))));
            }
        }
        rows.emplace_back(std::move(vals), col_map);
    }

    PQclear(res);
    callback(ResultSet(std::move(rows), affected_rows));
}

} // namespace database
