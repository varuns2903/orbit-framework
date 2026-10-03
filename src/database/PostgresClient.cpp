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
