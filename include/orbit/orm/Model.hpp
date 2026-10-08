#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/orm/MongoQueryBuilder.hpp>
#include <orbit/orm/QueryBuilder.hpp>

/**
 * @brief Registers a standard C++ struct as an ORM Model mapping to a database table.
 * 
 * Must be used in the global namespace after defining the struct and its NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE macro.
 * 
 * @param Type The C++ struct/class name.
 * @param TableName The database table name as a string.
 */
#define ORBIT_REGISTER_MODEL(Type, TableName) \
    template <typename DBClient> \
    inline ::orbit::orm::QueryBuilder<DBClient, Type> query_##Type(std::shared_ptr<DBClient> db) { \
        return ::orbit::orm::QueryBuilder<DBClient, Type>(db, TableName); \
    }

/**
 * @brief Registers a standard C++ struct as an ORM Model mapping to a MongoDB Collection.
 * 
 * @param Type The C++ struct/class name.
 */
/// Like ORBIT_REGISTER_MODEL, for a model whose primary key column is not "id".
#define ORBIT_REGISTER_MODEL_WITH_KEY(Type, TableName, KeyColumn) \
    template <typename DBClient> \
    inline ::orbit::orm::QueryBuilder<DBClient, Type> query_##Type(std::shared_ptr<DBClient> db) { \
        ::orbit::orm::QueryBuilder<DBClient, Type> q(db, TableName); \
        q.primary_key(KeyColumn); \
        return q; \
    }

#define ORBIT_REGISTER_MONGO_MODEL(Type) \
    inline ::orbit::orm::MongoQueryBuilder<Type> query_mongo_##Type(std::shared_ptr<::orbit::database::MongoClient> db) { \
        return ::orbit::orm::MongoQueryBuilder<Type>(db); \
    }
