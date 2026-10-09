# Database & C++20 Coroutines

Orbit provides seamless integration with PostgreSQL using raw asynchronous networking (`io_uring`/`epoll`) bridged perfectly into **C++20 Coroutines** via `Task` handlers and `Awaitable<T>` helpers.

This allows you to write non-blocking database queries exactly like Node.js or Python without spanning unnecessary OS threads!

## Setup

Include the necessary headers:
```cpp
#include "database/PostgresClient.hpp"
#include "database/PostgresCoro.hpp"
#include "concurrency/Task.hpp"
```

## Awaiting Database Queries

Define a handler that returns `orbit::concurrency::Task` instead of `void`. You can then use the `co_await` keyword for database operations.

```cpp
using namespace orbit::database;
using namespace orbit::concurrency;

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

## ORM Queries

`ORBIT_REGISTER_MODEL(Type, "table")` gives a query builder for a JSON-serialisable struct:

```cpp
struct Item { std::string name; int qty; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Item, name, qty)
ORBIT_REGISTER_MODEL(Item, "items")

// SELECT with sorting and paging
auto page = co_await query_Item(db).where(orbit::orm::Col("qty") > 0)
                                   .order_by("qty", orbit::orm::Order::Desc)
                                   .limit(20).offset(40)
                                   .get_async();

uint64_t n = co_await query_Item(db).where(orbit::orm::Col("qty") == 0).count_async();

co_await query_Item(db).insert_async(Item{"widget", 3});

// UPDATE / DELETE resume with the number of rows affected
uint64_t changed = co_await query_Item(db).where(orbit::orm::Col("name") == "widget")
                                          .update_async({{"qty", 5}});
uint64_t removed = co_await query_Item(db).where(orbit::orm::Col("qty") <= 0).remove_async();
```

`update_async()` and `remove_async()` refuse to run (`std::logic_error`) without a `where()` condition, so a forgotten filter cannot rewrite or empty a table; call `.all()` to really affect every row. Table and column names must be plain identifiers; SQL reserved words among them (`user`, `order`, `group`, ...) are quoted for the database (`"user"` on PostgreSQL, `` `user` `` on MySQL), while other names stay unquoted so PostgreSQL's usual lower-case folding still applies. `select_statement()`, `count_statement()`, `insert_statement()`, `update_statement()` and `delete_statement()` return the generated SQL and parameters without running it.

### Primary keys and generated ids

A model's primary key is the `id` field unless it says otherwise. When the key
is **unset** — `0`, `""` or null — `insert_async()` leaves it out so the
database generates it, and `update_async(model)` never rewrites it:

```cpp
struct Ticket { int id = 0; std::string title; int qty = 0; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Ticket, id, title, qty)
ORBIT_REGISTER_MODEL(Ticket, "tickets")      // key: "id"

// INSERT ... RETURNING *: resumes with the stored row, generated id included
Ticket created = co_await query_Ticket(db).create_async(Ticket{0, "write docs", 1});

// Another key column:
ORBIT_REGISTER_MODEL_WITH_KEY(Sku, "skus", "code")   // or .primary_key("code")
```

`create_async()` uses `RETURNING`, which PostgreSQL and MariaDB 10.5+ support;
MySQL Server does not and reports an error.

### Errors and types

A query that fails throws `orbit::orm::DatabaseError` from the `co_await`
(`get_async`, `count_async`, `insert_async`, `create_async`, `update_async`,
`remove_async`), with the database's message. It no longer looks like "0 rows"
or an empty result. In a coroutine handler an uncaught `DatabaseError` reaches
the router's error handling (`on_error`, else a 500); catch it where a failure
has a meaning of its own, such as a duplicate key.

Rows are read by the model's field types: a `std::string` field receives the
column's text even when it looks like a number (`"42"`, `"007"`), integers and
floats are parsed exactly, booleans accept `t`/`f`, `true`/`false` and `1`/`0`,
and JSON object or array fields parse JSON text. Columns the model does not
declare are ignored, and a field with no column in the result keeps the value
of a default-constructed model. A value that does not fit — `NULL` in a
non-optional field, `"abc"` in an `int` — throws `DatabaseError`.

## Parameters and SQL Injection

Never build SQL by concatenating request data. Bind values instead:

```cpp
// $1, $2, ... are bound by PostgreSQL; the values never become SQL text.
co_await query_async(pg_client, "SELECT * FROM users WHERE email = $1 AND age > $2",
                     {email, std::to_string(min_age)});
```

The ORM does this for you. `orbit::orm::Col("name") == value`, `where(field, op, value)`
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

## Schema Migrations

`orbit::orm::MigrationRunner` (`<orbit/orm/MigrationRunner.hpp>`, PostgreSQL)
applies the `.sql` files in a directory in name order (`001_users.sql`,
`002_index.sql`, ...) and records each in an `orbit_migrations` table, so
every file runs once. Each file runs in a transaction together with its
tracking row: a failing file leaves nothing behind and stops the run. An
advisory lock keeps several instances starting at once from applying the
same file twice. Statements that cannot run in a transaction (such as
`CREATE INDEX CONCURRENTLY`) are not supported.

At start-up, before serving, `migrate_sync` connects, migrates and returns,
using its own short-lived event loop:

```cpp
#include <orbit/orm/MigrationRunner.hpp>

int main() {
    auto result = orbit::orm::migrate_sync("host=localhost dbname=app user=app", "migrations");
    if (!result.ok()) {
        std::cerr << "migrations failed: " << result.error << "\n";
        return 1;
    }
    std::cout << result.summary() << "\n";   // e.g. "Successfully applied 2 migrations."

    orbit::server::App app;
    // ... routes ...
    app.listen();
}
```

From a coroutine, with a connected client, `co_await
MigrationRunner<PostgresClient>::run(db, "migrations")` gives the same
`MigrationResult` (`applied` files, `error`, `summary()`).
`run_migrations(db, dir, writer)` answers it over HTTP (200 with the
summary, or a 500 whose details go only to the log), for an admin endpoint.

## Request Lifetime in Coroutine Handlers

A coroutine handler can use its `HttpRequest&` (headers, body, params,
`req.json()`) for as long as it runs: across every `co_await`, on whichever
thread it resumes, and even after it has sent the response. On HTTP/1.1 the
connection then parses the next request into fresh storage while the handler
still holds the old one. No copying is needed.

This holds for the request the coroutine takes as a **parameter**. A
coroutine started from a plain handler gets the same guarantee when the
request is passed on as a parameter too:

```cpp
app.get("/items/:id", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
    show_item(req, w);  // Task show_item(HttpRequest&, std::shared_ptr<ResponseWriter>)
});
```

A request captured by reference in a lambda coroutine (`[&req]() -> Task`)
is not covered: the coroutine cannot see it.

## Helpers that Return Values

`orbit::concurrency::Awaitable<T>` is a coroutine a handler can `co_await`
for a result, so database access can live in small helpers:

```cpp
using orbit::concurrency::Awaitable;

Awaitable<std::optional<Item>> find_item(std::shared_ptr<PostgresClient> db, int id) {
    auto items = co_await query_Item(db).where(orbit::orm::Col("id") == id).get_async();
    if (items.empty()) co_return std::nullopt;
    co_return items.front();
}

Task show_item(HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
    auto item = co_await find_item(db, std::stoi(req.params.at("id")));
    HttpResponse res;
    if (item) res.json(nlohmann::json(*item)); else res.status(HttpStatus::NotFound);
    w->send(std::move(res));
}
```

An `Awaitable` starts only when awaited and resumes its caller when it
finishes. An exception thrown inside it is rethrown at the `co_await`;
uncaught there, it is handled like any handler exception (`on_error`, else a
500). Handlers themselves keep returning `Task`.

## Hooking into Router

Simply pass your Coroutine handler to the standard `app.get()` router:

```cpp
app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    db_handler(req, res); // Starts the coroutine Task asynchronously
});
```

## Errors in Coroutine Handlers

An exception that escapes a coroutine handler — before or after a `co_await`,
on whichever thread resumed it — is handled like one from a synchronous
handler: the router's `on_error` handler runs, or else the client gets
`500 Internal Server Error`. The exception text goes to the log, never to the
client. This works for any `Task` coroutine that takes the request's
`std::shared_ptr<ResponseWriter>` as a parameter (that is how the error is
routed back to its request).

Two cases are only logged:

- the coroutine had already sent its response, or part of it (a second
  response is impossible);
- the coroutine has no `ResponseWriter` parameter, e.g. a background helper.

A coroutine exception never terminates the server.

Pass the writer to the coroutine as a **parameter**, not a lambda capture: the
coroutine finds its request through its parameters.

```cpp
app.get("/items", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
    auto load = [](std::shared_ptr<ResponseWriter> writer) -> orbit::concurrency::Task {
        // ... co_await ...; an exception here becomes a 500
    };
    load(w);            // not: [w]() -> Task { ... }()
});
```

## Connection Pools

`orbit::database::ConnectionPool` hands out connected clients and takes them back. Given a health check, it also drops clients that have stopped working (a database restart, a failover, an idle connection cut by a firewall) and connects replacements in the background, retrying failed attempts with exponential backoff:

```cpp
#include <orbit/database/ConnectionPool.hpp>

orbit::database::PoolOptions<orbit::database::PostgresClient> opts;
opts.health_check = [](orbit::database::PostgresClient& c) { return c.is_healthy(); };
opts.initial_backoff = std::chrono::milliseconds(100); // then 200, 400, ... up to max_backoff
opts.max_backoff = std::chrono::seconds(30);

auto pool = std::make_shared<orbit::database::ConnectionPool<orbit::database::PostgresClient>>(
    8, [&] { return std::make_shared<orbit::database::PostgresClient>(&proactor, conninfo); }, opts);

pool->init([](auto client, auto done) { client->connect(done); },
           [](bool all_connected) { /* connections that failed are retried */ });

pool->acquire([pool](std::shared_ptr<orbit::database::PostgresClient> db) {
    db->execute("SELECT 1", {}, [pool, db](const orbit::database::ResultSet& res) {
        pool->release(db); // an unhealthy client is replaced instead of reused
    });
});
```

A client is checked when it is acquired and when it is released; a dead one is never handed out. Replacements go to waiting `acquire()` calls first. Retries wait on a detached thread by default; set `opts.schedule` to run them on your own timers. `idle_count()`, `waiting_count()` and `reconnecting_count()` expose the pool's state, for example for a readiness check.
