# Enhancements for Orbit HTTP Framework

Based on the current architecture and industry standards, here are the key enhancements to focus on for the future of the framework:

## 1. Developer Ergonomics
- **Lightweight ORM**: Currently, database interactions (PostgreSQL, MariaDB, MongoDB, Redis) are handled via raw queries inside C++20 coroutines. Implementing a compile-time ORM or a safe Query Builder would prevent SQL injection and improve the developer experience.
- **API Generation**: Add support for automatically generating OpenAPI (Swagger) documentation from the route definitions.
- **GraphQL & gRPC**: Incorporate native support for GraphQL schemas and gRPC server capabilities to make Orbit a complete microservices powerhouse.

## 2. Advanced Features
- **Hot Reloading**: Implement zero-downtime binary/configuration reloads without dropping active HTTP streams or WebSocket connections.
- **ACME Auto-TLS**: Build Let's Encrypt integration to automatically provision, configure, and renew SSL/TLS certificates.
- **Prometheus Metrics Endpoint**: Implement a built-in `/metrics` route to expose telemetry (RPS, memory usage, latency distributions) for Grafana scraping.

## 3. Tooling and Integrations
- **CLI Tooling**: Create an `orbit-cli` for scaffolding new projects, generating middleware boilerplates, and managing database migrations.
- **Fuzz Testing**: Given the extensive custom parsing required for HTTP/3, WebSockets, and multipart forms, integrating `libFuzzer` in the CMake configuration is essential for finding edge-case crashes.
