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

## Hooking into Router

Simply pass your Coroutine handler to the standard `app.get()` router:

```cpp
app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    db_handler(req, res); // Starts the coroutine Task asynchronously
});
```

## Connection Pools

`database::ConnectionPool` hands out connected clients and takes them back. Given a health check, it also drops clients that have stopped working (a database restart, a failover, an idle connection cut by a firewall) and connects replacements in the background, retrying failed attempts with exponential backoff:

```cpp
#include <orbit/database/ConnectionPool.hpp>

database::PoolOptions<database::PostgresClient> opts;
opts.health_check = [](database::PostgresClient& c) { return c.is_healthy(); };
opts.initial_backoff = std::chrono::milliseconds(100); // then 200, 400, ... up to max_backoff
opts.max_backoff = std::chrono::seconds(30);

auto pool = std::make_shared<database::ConnectionPool<database::PostgresClient>>(
    8, [&] { return std::make_shared<database::PostgresClient>(&proactor, conninfo); }, opts);

pool->init([](auto client, auto done) { client->connect(done); },
           [](bool all_connected) { /* connections that failed are retried */ });

pool->acquire([pool](std::shared_ptr<database::PostgresClient> db) {
    db->execute("SELECT 1", {}, [pool, db](const database::ResultSet& res) {
        pool->release(db); // an unhealthy client is replaced instead of reused
    });
});
```

A client is checked when it is acquired and when it is released; a dead one is never handed out. Replacements go to waiting `acquire()` calls first. Retries wait on a detached thread by default; set `opts.schedule` to run them on your own timers. `idle_count()`, `waiting_count()` and `reconnecting_count()` expose the pool's state, for example for a readiness check.
