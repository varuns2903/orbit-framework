#include <gtest/gtest.h>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/RequestContext.hpp>

#include <memory>
#include <string>

// orbit::http::RequestContext (#202): type-indexed per-request storage for
// middleware, plus HttpRequest's set/get/ensure forwarding to it.

using orbit::http::HttpRequest;
using orbit::http::RequestContext;

namespace {

struct AuthInfo {
    std::string user_id;
};

struct Tenant {
    int id = 0;
};

} // namespace

TEST(RequestContextTest, SetAndGetRoundTrip) {
    RequestContext ctx;
    EXPECT_EQ(ctx.get<AuthInfo>(), nullptr);
    ctx.set(AuthInfo{"user-1"});
    AuthInfo* auth = ctx.get<AuthInfo>();
    ASSERT_NE(auth, nullptr);
    EXPECT_EQ(auth->user_id, "user-1");
}

TEST(RequestContextTest, SetReplacesAnExistingValue) {
    RequestContext ctx;
    ctx.set(AuthInfo{"first"});
    AuthInfo& ref = ctx.set(AuthInfo{"second"});
    EXPECT_EQ(ref.user_id, "second");
    EXPECT_EQ(ctx.get<AuthInfo>()->user_id, "second");
}

TEST(RequestContextTest, DistinctTypesDoNotCollide) {
    RequestContext ctx;
    ctx.set(AuthInfo{"u"});
    ctx.set(Tenant{42});
    ASSERT_NE(ctx.get<AuthInfo>(), nullptr);
    ASSERT_NE(ctx.get<Tenant>(), nullptr);
    EXPECT_EQ(ctx.get<AuthInfo>()->user_id, "u");
    EXPECT_EQ(ctx.get<Tenant>()->id, 42);
}

TEST(RequestContextTest, HasReflectsWhetherATypeIsSet) {
    RequestContext ctx;
    EXPECT_FALSE(ctx.has<AuthInfo>());
    ctx.set(AuthInfo{});
    EXPECT_TRUE(ctx.has<AuthInfo>());
}

TEST(RequestContextTest, EnsureCreatesOnceThenReturnsTheSameValue) {
    RequestContext ctx;
    Tenant& first = ctx.ensure<Tenant>(7);
    EXPECT_EQ(first.id, 7);
    Tenant& second = ctx.ensure<Tenant>(99); // already set: args ignored
    EXPECT_EQ(&first, &second);
    EXPECT_EQ(second.id, 7);
}

TEST(RequestContextTest, EraseRemovesAndIsIdempotent) {
    RequestContext ctx;
    ctx.set(AuthInfo{"u"});
    EXPECT_TRUE(ctx.erase<AuthInfo>());
    EXPECT_EQ(ctx.get<AuthInfo>(), nullptr);
    EXPECT_FALSE(ctx.erase<AuthInfo>());
}

TEST(RequestContextTest, ConstAccessWorks) {
    RequestContext ctx;
    ctx.set(AuthInfo{"u"});
    const RequestContext& cref = ctx;
    const AuthInfo* auth = cref.get<AuthInfo>();
    ASSERT_NE(auth, nullptr);
    EXPECT_EQ(auth->user_id, "u");
}

// More distinct types than the inline capacity (4): the overflow path.
TEST(RequestContextTest, MoreThanFourDistinctTypesStillWork) {
    struct A { int v = 1; };
    struct B { int v = 2; };
    struct C { int v = 3; };
    struct D { int v = 4; };
    struct E { int v = 5; };
    struct F { int v = 6; };
    RequestContext ctx;
    ctx.set(A{});
    ctx.set(B{});
    ctx.set(C{});
    ctx.set(D{});
    ctx.set(E{});
    ctx.set(F{});
    EXPECT_EQ(ctx.get<A>()->v, 1);
    EXPECT_EQ(ctx.get<B>()->v, 2);
    EXPECT_EQ(ctx.get<C>()->v, 3);
    EXPECT_EQ(ctx.get<D>()->v, 4);
    EXPECT_EQ(ctx.get<E>()->v, 5);
    EXPECT_EQ(ctx.get<F>()->v, 6);
    EXPECT_TRUE(ctx.erase<C>()); // from the inline slots
    EXPECT_TRUE(ctx.erase<E>()); // from the overflow
    EXPECT_EQ(ctx.get<C>(), nullptr);
    EXPECT_EQ(ctx.get<E>(), nullptr);
    EXPECT_EQ(ctx.get<A>()->v, 1); // the others survive the erase
    EXPECT_EQ(ctx.get<F>()->v, 6);
}

TEST(RequestContextTest, SharedPtrAndPointerValuesWork) {
    RequestContext ctx;
    auto shared = std::make_shared<int>(10);
    ctx.set(shared);
    ASSERT_NE(ctx.get<std::shared_ptr<int>>(), nullptr);
    EXPECT_EQ(**ctx.get<std::shared_ptr<int>>(), 10);
}

TEST(RequestContextTest, NamedAttributesAreSeparateFromTypedStorage) {
    RequestContext ctx;
    EXPECT_EQ(ctx.attr("tenant"), nullptr);
    ctx.set_attr("tenant", "acme");
    const std::string* value = ctx.attr("tenant");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "acme");
    // A typed AuthInfo and a named "AuthInfo" string attribute don't collide.
    ctx.set(AuthInfo{"typed"});
    ctx.set_attr("AuthInfo", "stringy");
    EXPECT_EQ(ctx.get<AuthInfo>()->user_id, "typed");
    EXPECT_EQ(*ctx.attr("AuthInfo"), "stringy");
}

// --- HttpRequest's set/get/ensure forwarding ---

TEST(HttpRequestContextTest, ForwardsToContext) {
    HttpRequest req;
    EXPECT_EQ(req.get<AuthInfo>(), nullptr);
    req.set(AuthInfo{"from-request"});
    ASSERT_NE(req.get<AuthInfo>(), nullptr);
    EXPECT_EQ(req.get<AuthInfo>()->user_id, "from-request");
    EXPECT_EQ(req.context.get<AuthInfo>()->user_id, "from-request") << "req.context is the same storage";
}

TEST(HttpRequestContextTest, EnsureForwardsWithArguments) {
    HttpRequest req;
    Tenant& t = req.ensure<Tenant>(3);
    EXPECT_EQ(t.id, 3);
    EXPECT_EQ(req.ensure<Tenant>(999).id, 3);
}

TEST(HttpRequestContextTest, ConstRequestCanRead) {
    HttpRequest req;
    req.set(AuthInfo{"u"});
    const HttpRequest& cref = req;
    ASSERT_NE(cref.get<AuthInfo>(), nullptr);
    EXPECT_EQ(cref.get<AuthInfo>()->user_id, "u");
}
