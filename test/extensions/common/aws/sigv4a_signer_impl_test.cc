#include <cstdint>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/crypto/utility.h"
#include "source/common/http/message_impl.h"
#include "source/extensions/common/aws/sigv4a_key_derivation.h"
#include "source/extensions/common/aws/sigv4a_signer_impl.h"
#include "source/extensions/common/aws/utility.h"

#include "test/extensions/common/aws/mocks.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Aws {
namespace {

class SigV4ASignerImplTest : public testing::Test {
public:
  SigV4ASignerImplTest()
      : credentials_provider_(new NiceMock<MockCredentialsProvider>()),
        message_(new Http::RequestMessageImpl()), credentials_("akid", "secret"),
        token_credentials_("akid", "secret", "token") {
    // 20180102T030405Z
    time_system_.setSystemTime(std::chrono::milliseconds(1514862245000));
  }

  void addMethod(const std::string& method) { message_->headers().setMethod(method); }

  void addPath(const std::string& path) { message_->headers().setPath(path); }

  void addHeader(const std::string& key, const std::string& value) {
    message_->headers().addCopy(Http::LowerCaseString(key), value);
  }

  void setBody(const std::string& body) { message_->body().add(body); }

  CredentialsProviderSharedPtr getTestCredentialsProvider() {
    return CredentialsProviderSharedPtr(credentials_provider_);
  }

  enum SigningType { NormalSign, EmptyPayload, UnsignedPayload };

  SigV4ASignerImpl getTestSigner(const bool query_string, uint16_t expiration_time = 0) {
    if (query_string && !expiration_time) {
      // Default expiration time is 5 seconds
      expiration_time = 5;
    }
    return SigV4ASignerImpl{"service",
                            "region",
                            getTestCredentialsProvider(),
                            time_system_,
                            Extensions::Common::Aws::AwsSigningHeaderExclusionVector{},
                            query_string,
                            expiration_time};
  }

  void ecdsaVerifyCanonicalRequest(std::string canonical_request, SigningType signing_type,
                                   Http::RequestMessagePtr& message, bool sign_body,
                                   bool query_string, absl::string_view override_region,
                                   const uint16_t expiration_time = 5) {
    auto& crypto_util = Envoy::Common::Crypto::UtilitySingleton::get();

    EC_KEY* ec_key = SigV4AKeyDerivation::derivePrivateKey(
        absl::string_view(credentials_.accessKeyId()->data(), credentials_.accessKeyId()->size()),
        absl::string_view(credentials_.secretAccessKey()->data(),
                          credentials_.secretAccessKey()->size()));
    SigV4AKeyDerivation::derivePublicKey(ec_key);

    EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
    // Sign the message using our signing algorithm
    auto signer_ = getTestSigner(query_string, expiration_time);

    switch (signing_type) {
    case EmptyPayload:
      signer_.signEmptyPayload(message->headers(), override_region);
      break;
    case NormalSign:
      signer_.sign(*message, sign_body, override_region);
      break;
    case UnsignedPayload:
      signer_.signUnsignedPayload(message->headers(), override_region);
      break;
    }

    std::string short_date = "20180102";
    std::string credential_scope = fmt::format(fmt::runtime("{}/service/aws4_request"), short_date);
    std::string long_date = "20180102T030400Z";
    std::string string_to_sign =
        fmt::format(fmt::runtime(SigV4ASignatureConstants::SigV4AStringToSignFormat),
                    SigV4ASignatureConstants::SigV4AAlgorithm, long_date, credential_scope,
                    Hex::encode(crypto_util.getSha256Digest(Buffer::OwnedImpl(canonical_request))));
    auto hash = crypto_util.getSha256Digest(Buffer::OwnedImpl(string_to_sign));
    std::vector<uint8_t> signature;

    if (query_string) {
      auto query_parameters =
          Http::Utility::QueryParamsMulti::parseQueryString(message->headers().getPathValue());

      auto signature_hex = query_parameters.getFirstValue("X-Amz-Signature");
      ASSERT(signature_hex.has_value());
      signature = Hex::decode(signature_hex.value());
    } else {

      // Extract the signature that is generated
      EXPECT_THAT(message->headers()
                      .get(Http::CustomHeaders::get().Authorization)[0]
                      ->value()
                      .getStringView(),
                  testing::StartsWith(
                      "AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                      "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                      "Signature="));
      std::vector<std::string> v =
          absl::StrSplit(message->headers()
                             .get(Http::CustomHeaders::get().Authorization)[0]
                             ->value()
                             .getStringView(),
                         "Signature=");

      signature = Hex::decode(v[1]);
    }
    // Check that the signature generated by our algorithm can be verified by the matching public
    // key
    EXPECT_EQ(
        1, ECDSA_verify(0, hash.data(), hash.size(), signature.data(), signature.size(), ec_key));
    EC_KEY_free(ec_key);
  }
  NiceMock<MockCredentialsProvider>* credentials_provider_;
  Event::SimulatedTimeSystem time_system_;
  Http::RequestMessagePtr message_;
  Credentials credentials_;
  Credentials token_credentials_;
  absl::optional<std::string> region_;
};

// No authorization header should be present when the credentials are empty
TEST_F(SigV4ASignerImplTest, AnonymousCredentials) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(Credentials()));

  auto signer_ = getTestSigner(false);
  signer_.sign(*message_);
  EXPECT_TRUE(message_->headers().get(Http::CustomHeaders::get().Authorization).empty());
}

// HTTP :method header is required
TEST_F(SigV4ASignerImplTest, MissingMethodException) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  auto signer_ = getTestSigner(false);
  EXPECT_THROW_WITH_MESSAGE(signer_.sign(*message_), EnvoyException,
                            "Message is missing :method header");
  EXPECT_TRUE(message_->headers().get(Http::CustomHeaders::get().Authorization).empty());
}

// HTTP :path header is required
TEST_F(SigV4ASignerImplTest, MissingPathException) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  auto signer_ = getTestSigner(false);
  EXPECT_THROW_WITH_MESSAGE(signer_.sign(*message_), EnvoyException,
                            "Message is missing :path header");
  EXPECT_TRUE(message_->headers().get(Http::CustomHeaders::get().Authorization).empty());
}

TEST_F(SigV4ASignerImplTest, QueryStringDoesntModifyAuthorization) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  addPath("/");
  addHeader("Authorization", "testValue");
  auto signer_ = getTestSigner(true);
  signer_.sign(*message_);
  EXPECT_EQ(message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value(),
            "testValue");
}

// Verify we sign the date header
TEST_F(SigV4ASignerImplTest, SignDateHeader) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  addPath("/");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_);
  EXPECT_FALSE(message_->headers().get(SigV4ASignatureHeaders::get().ContentSha256).empty());
  EXPECT_EQ(
      "20180102T030400Z",
      message_->headers().get(SigV4ASignatureHeaders::get().Date)[0]->value().getStringView());
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

// Verify we sign the security token header if the token is present in the credentials
TEST_F(SigV4ASignerImplTest, SignSecurityTokenHeader) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(token_credentials_));
  addMethod("GET");
  addPath("/");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_);
  EXPECT_EQ("token", message_->headers()
                         .get(SigV4ASignatureHeaders::get().SecurityToken)[0]
                         ->value()
                         .getStringView());
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith(
          "AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
          "SignedHeaders=x-amz-content-sha256;x-amz-date;x-amz-region-set;x-amz-security-token, "
          "Signature="));
}

// Verify we sign the content header as the hashed empty string if the body is empty
TEST_F(SigV4ASignerImplTest, SignEmptyContentHeader) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  addPath("/");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_, true);
  EXPECT_EQ(SigV4ASignatureConstants::HashedEmptyString,
            message_->headers()
                .get(SigV4ASignatureHeaders::get().ContentSha256)[0]
                ->value()
                .getStringView());
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

// Verify we sign the content header correctly when we have a body
TEST_F(SigV4ASignerImplTest, SignContentHeader) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("POST");
  addPath("/");
  setBody("test1234");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_, true);
  EXPECT_EQ("937e8d5fbb48bd4949536cd65b8d35c426b80d2f830c5c308e2cdec422ae2244",
            message_->headers()
                .get(SigV4ASignatureHeaders::get().ContentSha256)[0]
                ->value()
                .getStringView());
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

// Verify we sign the content header correctly when we have a body with region override
TEST_F(SigV4ASignerImplTest, SignContentHeaderOverrideRegion) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("POST");
  addPath("/");
  setBody("test1234");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_, true, "region1");
  EXPECT_EQ("937e8d5fbb48bd4949536cd65b8d35c426b80d2f830c5c308e2cdec422ae2244",
            message_->headers()
                .get(SigV4ASignatureHeaders::get().ContentSha256)[0]
                ->value()
                .getStringView());
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

// Verify we sign some extra headers
TEST_F(SigV4ASignerImplTest, SignExtraHeaders) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  addPath("/");
  addHeader("a", "a_value");
  addHeader("b", "b_value");
  addHeader("c", "c_value");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_);
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=a;b;c;x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

// Verify signing a host header
TEST_F(SigV4ASignerImplTest, SignHostHeader) {
  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));
  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");
  auto signer_ = getTestSigner(false);
  signer_.sign(*message_);
  EXPECT_THAT(
      message_->headers().get(Http::CustomHeaders::get().Authorization)[0]->value().getStringView(),
      testing::StartsWith("AWS4-ECDSA-P256-SHA256 Credential=akid/20180102/service/aws4_request, "
                          "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-region-set, "
                          "Signature="));
}

TEST_F(SigV4ASignerImplTest, SignAndVerify) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  auto canonical_request = R"EOF(GET
/

host:www.example.com
x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
x-amz-date:20180102T030400Z
x-amz-region-set:ap-southeast-2

host;x-amz-content-sha256;x-amz-date;x-amz-region-set
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, NormalSign, message_, false, false,
                              "ap-southeast-2");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyMultiRegion) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/

host:www.example.com
x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
x-amz-date:20180102T030400Z
x-amz-region-set:ap-southeast-2,us-east-1

host;x-amz-content-sha256;x-amz-date;x-amz-region-set
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, NormalSign, message_, false, false,
                              "ap-southeast-2,us-east-1");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyUnsignedPayload) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/

host:www.example.com
x-amz-content-sha256:UNSIGNED-PAYLOAD
x-amz-date:20180102T030400Z
x-amz-region-set:ap-southeast-2

host;x-amz-content-sha256;x-amz-date;x-amz-region-set
UNSIGNED-PAYLOAD)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, UnsignedPayload, message_, false, false,
                              "ap-southeast-2");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyEmptyPayloadMultiRegion) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/

host:www.example.com
x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
x-amz-date:20180102T030400Z
x-amz-region-set:ap-southeast-2,us-east-*

host;x-amz-content-sha256;x-amz-date;x-amz-region-set
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, EmptyPayload, message_, false, false,
                              "ap-southeast-2,us-east-*");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyEmptyPayloadMultiRegionQuery) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/
X-Amz-Algorithm=AWS4-ECDSA-P256-SHA256&X-Amz-Credential=akid%2F20180102%2Fservice%2Faws4_request&X-Amz-Date=20180102T030400Z&X-Amz-Expires=5&X-Amz-Region-Set=ap-southeast-2%2Cus-east-%2A&X-Amz-SignedHeaders=host%3Bx-amz-content-sha256
host:www.example.com
x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855

host;x-amz-content-sha256
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, EmptyPayload, message_, false, true,
                              "ap-southeast-2,us-east-*");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyUnsignedPayloadQuery) {

  addMethod("GET");
  addPath("/");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/
X-Amz-Algorithm=AWS4-ECDSA-P256-SHA256&X-Amz-Credential=akid%2F20180102%2Fservice%2Faws4_request&X-Amz-Date=20180102T030400Z&X-Amz-Expires=5&X-Amz-Region-Set=ap-southeast-2%2Cus-east-%2A&X-Amz-SignedHeaders=host%3Bx-amz-content-sha256
host:www.example.com
x-amz-content-sha256:UNSIGNED-PAYLOAD

host;x-amz-content-sha256
UNSIGNED-PAYLOAD)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, UnsignedPayload, message_, false, true,
                              "ap-southeast-2,us-east-*");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyEmptyPayloadMultiRegionQueryStringExist) {

  addMethod("GET");
  addPath("/?query1=aaaaaa&anotherquery=2");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/
X-Amz-Algorithm=AWS4-ECDSA-P256-SHA256&X-Amz-Credential=akid%2F20180102%2Fservice%2Faws4_request&X-Amz-Date=20180102T030400Z&X-Amz-Expires=5&X-Amz-Region-Set=ap-southeast-2%2Cus-east-%2A&X-Amz-SignedHeaders=host%3Bx-amz-content-sha256&anotherquery=2&query1=aaaaaa
host:www.example.com
x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855

host;x-amz-content-sha256
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, EmptyPayload, message_, false, true,
                              "ap-southeast-2,us-east-*");
}

TEST_F(SigV4ASignerImplTest, SignAndVerifyUnsignedPayloadQueryCustomExpiration) {

  addMethod("GET");
  addPath("/?query1=aaaaaa&anotherquery=2");
  addHeader("host", "www.example.com");

  std::string canonical_request = R"EOF(GET
/
X-Amz-Algorithm=AWS4-ECDSA-P256-SHA256&X-Amz-Credential=akid%2F20180102%2Fservice%2Faws4_request&X-Amz-Date=20180102T030400Z&X-Amz-Expires=200&X-Amz-Region-Set=ap-southeast-2%2Cus-east-%2A&X-Amz-SignedHeaders=host%3Bx-amz-content-sha256&anotherquery=2&query1=aaaaaa
host:www.example.com
x-amz-content-sha256:UNSIGNED-PAYLOAD

host;x-amz-content-sha256
UNSIGNED-PAYLOAD)EOF";

  ecdsaVerifyCanonicalRequest(canonical_request, UnsignedPayload, message_, false, true,
                              "ap-southeast-2,us-east-*", 200);
}

// Verify query string signing defaults to 5s
TEST_F(SigV4ASignerImplTest, QueryStringDefault5s) {

  Http::TestRequestHeaderMapImpl headers{};

  EXPECT_CALL(*credentials_provider_, getCredentials()).WillOnce(Return(credentials_));

  headers.setMethod("GET");
  // Simple path, 1 extra header
  headers.setPath("/example/path");
  headers.addCopy(Http::LowerCaseString("host"), "example.service.zz");
  headers.addCopy("testheader", "value1");
  SigV4ASignerImpl querysigner("service", "region", getTestCredentialsProvider(), time_system_,
                               Extensions::Common::Aws::AwsSigningHeaderExclusionVector{}, true);

  querysigner.signUnsignedPayload(headers);
  EXPECT_TRUE(absl::StrContains(headers.getPathValue(), "X-Amz-Expires=5&"));
}

} // namespace
} // namespace Aws
} // namespace Common
} // namespace Extensions
} // namespace Envoy
