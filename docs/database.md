# Database & C++20 Coroutines

Orbit provides seamless integration with PostgreSQL using raw asynchronous networking (`io_uring`/`epoll`) bridged perfectly into **C++20 Coroutines** via `Task<T>`.

This allows you to write non-blocking database queries exactly like Node.js or Python without spanning unnecessary OS threads!

## Setup

Include the necessary headers:
```cpp
#include "database/PostgresClient.hpp"
#include "database/PostgresCoro.hpp"
#include "concurrency/Task.hpp"
```

## Awaiting Database Queries

Define a handler that returns `concurrency::Task` instead of `void`. You can then use the `co_await` keyword for database operations.

```cpp
using namespace database;
using namespace concurrency;

Task db_handler(HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
    // Initialize PostgresClient using the active Proactor event loop
    auto pg = std::make_shared<PostgresClient>(&writer->proactor(), "dbname=postgres");
    
    // 1. Asynchronously await database connection
    bool connected = co_await connect_async(pg);
    if (!connected) {
        writer->send(HttpResponse().status(HttpStatus::InternalServerError).send("DB Failed"));
        co_return; 
    }

    // 2. Asynchronously await query results without blocking the server
    PGresult* res = co_await query_async(pg, "SELECT current_timestamp;");
    if (res) {
        std::string ts = PQgetvalue(res, 0, 0);
        writer->send(HttpResponse().status(HttpStatus::OK).send("DB Time: " + ts));
        PQclear(res);
    }
}
```

## Parameters and SQL Injection

Never build SQL by concatenating request data. Bind values instead:

```cpp
// $1, $2, ... are bound by PostgreSQL; the values never become SQL text.
co_await query_async(pg_client, "SELECT * FROM users WHERE email = $1 AND age > $2",
                     {email, std::to_string(min_age)});
```

The ORM does this for you. `orm::Col("name") == value`, `where(field, op, value)`
and `insert_async(model)` bind every value (as `$n` parameters on PostgreSQL, or
as literals escaped with `mysql_real_escape_string` on MariaDB/MySQL). Column and
table names cannot be bound, so they must be plain identifiers (`name`,
`users.created_at`) and `where()` only accepts the operators `=`, `!=`, `<>`,
`<`, `<=`, `>`, `>=`, `[NOT] LIKE` and `[NOT] ILIKE`; anything else throws
`std::invalid_argument`.

## Reading Typed Columns

Every column arrives as text. `Row::get_as<T>` converts it to `bool`, an integer type, a floating-point type or `std::string`, and gives `std::nullopt` for NULL, a missing column, or text that does not convert exactly (`"12abc"` is not 12; a value out of range for `T` is refused):

```cpp
auto rows = co_await query_async(db, "SELECT id, price, active, note FROM items");
for (const auto& row : rows) {
    int64_t id     = row.value_or<int64_t>("id", 0);
    double price   = row.value_or<double>("price", 0.0);
    bool active    = row.get_as<bool>("active").value_or(false); // t/f, true/false, 1/0
    if (row.is_null("note")) { /* ... */ }
}
```

## Prepared Statements

`execute()` / `execute_async()` prepare a statement the first time its SQL is seen on a connection and reuse the plan afterwards, which saves parsing and planning for statements run often:

```cpp
auto user = co_await execute_async(db, "SELECT * FROM users WHERE id = $1", {std::to_string(id)});
```

Up to 256 statements are cached per connection (`set_max_prepared_statements()`); beyond that they run unprepared. The cache is per server session, so it starts empty after a reconnect.

## Transactions

```cpp
co_await begin_async(db);
auto debit  = co_await execute_async(db, "UPDATE accounts SET balance = balance - $1 WHERE id = $2", {amount, from});
auto credit = co_await execute_async(db, "UPDATE accounts SET balance = balance + $1 WHERE id = $2", {amount, to});
if (debit.ok() && credit.ok()) {
    auto committed = co_await commit_async(db);
} else {
    co_await rollback_async(db);
}
```

After a failed statement, PostgreSQL rolls the whole transaction back on `COMMIT`; `commit()` then reports a failure ("transaction was rolled back after an earlier error") rather than success. `in_transaction()` tells whether a transaction block is open.

## Timeouts and Reconnecting

`set_statement_timeout()` (before `connect()`) limits how long each statement may run on the server; a longer one fails with "canceling statement due to statement timeout". To detect an unreachable server, add libpq's `connect_timeout`, `keepalives_idle` or `tcp_user_timeout` to the connection string.

`is_healthy()` reports whether the connection is usable. After the server drops it (a restart, a failover, `pg_terminate_backend`), queries fail, `is_healthy()` turns false, and `connect()` / `reconnect()` (or `co_await connect_async(db)`) establish a new session, with the statement timeout applied again.

## Hooking into Router

Simply pass your Coroutine handler to the standard `app.get()` router:

```cpp
app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    db_handler(req, res); // Starts the coroutine Task asynchronously
});
```
