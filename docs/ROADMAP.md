# Orbit Framework - Master Roadmap

This document outlines the ultimate goals and roadmap for the Orbit Framework to evolve into a production-ready, widely adopted ecosystem. 

## 1. Build System & Packaging
- [x] **1. Use CMake as the primary build system**
- [x] **2. Provide clean `find_package()` support**
- [x] **3. Export CMake targets** (e.g., `OrbitFramework::core`)
- [x] **4. Support CMake `FetchContent`** seamlessly
- [x] **5. Publish to vcpkg**
- [x] **6. Publish to Conan**
- [x] **7. Automatically manage dependencies**
- [x] **18. Provide prebuilt binaries/releases**
- [x] **20. Support both static and shared libraries**
- [x] **42. Keep core dependencies lightweight**
- [x] **43. Avoid forcing unused features/dependencies on users**

## 2. Platform & Modularity
- [x] **8. Make features modular and optional**
- [x] **19. Support Linux, Windows, and macOS** 
- [x] **9. HTTP/1.1**
- [x] **10. HTTP/2**
- [x] **11. HTTP/3** (QUIC)
- [x] **12. REST** (Router & Middleware)
- [x] **13. WebSockets**
- [ ] **55. Strongly-Typed WebSocket EventRouter (Auto-JSON mapping, Rooms & Session State)**
- [x] **14. GraphQL** (HTTP adapter middleware)
- [x] **15. gRPC** (GrpcServer wrapper)
- [x] **16. TLS/SSL** (OpenSSL integration)

## 2.5 Database & ORM
- [x] **51. Database Abstraction Layer (DBAL) & unified ResultSet**
- [x] **52. Automatic JSON serialization for database queries**
- [ ] **53. Full Object-Relational Mapper (ORM)**
- [ ] **54. Database Migrations support**

## 3. Tooling & Developer Experience
- [x] **21. Provide a CLI** (`orbit-cli`)
- [x] **22. `orbit new <project>`**
- [x] **23. `orbit build`**
- [x] **24. `orbit run`**
- [x] **17. Provide sensible default configuration**
- [ ] **44. Provide one-command installation**
- [x] **45. Provide one-command project creation**
- [x] **46. Provide one-command development run**
- [ ] **47. Provide one-command release/production build**
- [ ] **50. Build the developer experience around: install → create → code → run → deploy**

## 4. Documentation & Education
- [x] **25. Provide project templates/scaffolding**
- [x] **26. Provide a 5-minute Hello World example**
- [x] **27. Provide examples for every major feature**
- [ ] **28. Provide Docker support**
- [ ] **29. Provide production deployment documentation**
- [ ] **32. Provide API documentation**
- [ ] **33. Provide migration guides**
- [ ] **48. Provide clear README: Install → Create → Run → Deploy**
- [ ] **49. Provide comprehensive documentation**

## 5. Testing & CI/CD
- [x] **35. Automated CI/CD**
- [x] **36. Test Linux, Windows and macOS**
- [ ] **37. Unit tests**
- [ ] **38. Integration tests**
- [ ] **39. HTTP/protocol compliance tests**
- [ ] **40. Performance benchmarks**
- [ ] **41. Security/dependency scanning**

## 6. Maintenance & Lifespan
- [ ] **30. Use semantic versioning**
- [ ] **31. Maintain API/ABI compatibility where possible**
- [ ] **34. Maintain a detailed changelog**
