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

## ORM Queries

`ORBIT_REGISTER_MODEL(Type, "table")` gives a query builder for a JSON-serialisable struct:

```cpp
struct Item { std::string name; int qty; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Item, name, qty)
ORBIT_REGISTER_MODEL(Item, "items")

// SELECT with sorting and paging
auto page = co_await query_Item(db).where(orm::Col("qty") > 0)
                                   .order_by("qty", orm::Order::Desc)
                                   .limit(20).offset(40)
                                   .get_async();

uint64_t n = co_await query_Item(db).where(orm::Col("qty") == 0).count_async();

co_await query_Item(db).insert_async(Item{"widget", 3});

// UPDATE / DELETE resume with the number of rows affected
uint64_t changed = co_await query_Item(db).where(orm::Col("name") == "widget")
                                          .update_async({{"qty", 5}});
uint64_t removed = co_await query_Item(db).where(orm::Col("qty") <= 0).remove_async();
```

`update_async()` and `remove_async()` refuse to run (`std::logic_error`) without a `where()` condition, so a forgotten filter cannot rewrite or empty a table; call `.all()` to really affect every row. Table and column names must be plain identifiers; SQL reserved words among them (`user`, `order`, `group`, ...) are quoted for the database (`"user"` on PostgreSQL, `` `user` `` on MySQL), while other names stay unquoted so PostgreSQL's usual lower-case folding still applies. `select_statement()`, `count_statement()`, `update_statement()` and `delete_statement()` return the generated SQL and parameters without running it.

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
