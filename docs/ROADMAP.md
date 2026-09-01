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
- [ ] **18. Provide prebuilt binaries/releases**
- [ ] **20. Support both static and shared libraries**
- [x] **42. Keep core dependencies lightweight**
- [x] **43. Avoid forcing unused features/dependencies on users**

## 2. Platform & Modularity
- [x] **8. Make features modular and optional**
- [ ] **19. Support Linux, Windows, and macOS** 
- [ ] **9. HTTP/1.1** (Active)
- [ ] **10. HTTP/2** (Active)
- [ ] **11. HTTP/3** (Active)
- [ ] **12. REST**
- [ ] **13. WebSockets** (Active)
- [ ] **14. GraphQL**
- [ ] **15. gRPC**
- [ ] **16. TLS/SSL**

## 3. Tooling & Developer Experience
- [ ] **21. Provide a CLI** (`orbit-cli`)
- [ ] **22. `orbit new <project>`**
- [ ] **23. `orbit build`**
- [ ] **24. `orbit run`**
- [ ] **17. Provide sensible default configuration**
- [ ] **44. Provide one-command installation**
- [ ] **45. Provide one-command project creation**
- [ ] **46. Provide one-command development run**
- [ ] **47. Provide one-command release/production build**
- [ ] **50. Build the developer experience around: install → create → code → run → deploy**

## 4. Documentation & Education
- [ ] **25. Provide project templates/scaffolding**
- [ ] **26. Provide a 5-minute Hello World example**
- [ ] **27. Provide examples for every major feature**
- [ ] **28. Provide Docker support**
- [ ] **29. Provide production deployment documentation**
- [ ] **32. Provide API documentation**
- [ ] **33. Provide migration guides**
- [ ] **48. Provide clear README: Install → Create → Run → Deploy**
- [ ] **49. Provide comprehensive documentation**

## 5. Testing & CI/CD
- [ ] **35. Automated CI/CD**
- [ ] **36. Test Linux, Windows and macOS**
- [ ] **37. Unit tests**
- [ ] **38. Integration tests**
- [ ] **39. HTTP/protocol compliance tests**
- [ ] **40. Performance benchmarks**
- [ ] **41. Security/dependency scanning**

## 6. Maintenance & Lifespan
- [ ] **30. Use semantic versioning**
- [ ] **31. Maintain API/ABI compatibility where possible**
- [ ] **34. Maintain a detailed changelog**
