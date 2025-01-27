#include "oidc_filter.h"
#include <boost/beast.hpp>
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "external/com_google_googleapis/google/rpc/code.pb.h"
#include "spdlog/spdlog.h"
#include "src/common/http/headers.h"
#include "src/common/http/http.h"
#include "src/common/utilities/random.h"
#include "state_cookie_codec.h"
#include "absl/time/clock.h"
#include <limits>
#include <algorithm>

namespace beast = boost::beast;    // from <boost/beast.hpp>
namespace http = beast::http;      // from <boost/beast/http.hpp>
namespace net = boost::asio;       // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;  // from <boost/asio/ip/tcp.hpp>

namespace authservice {
namespace filters {
namespace oidc {

namespace {
const char *filter_name_ = "oidc";
const char *mandatory_scope_ = "openid";

const std::map<const char *, const char *> standard_headers = {
    {common::http::headers::CacheControl,
     common::http::headers::CacheControlDirectives::NoCache},
    {common::http::headers::Pragma,
     common::http::headers::PragmaDirectives::NoCache},
};
}  // namespace

OidcFilter::OidcFilter(common::http::ptr_t http_ptr,
                       const authservice::config::oidc::OIDCConfig &idp_config,
                       TokenResponseParserPtr parser,
                       common::session::TokenEncryptorPtr cryptor)
    : http_ptr_(http_ptr),
      idp_config_(idp_config),
      parser_(parser),
      cryptor_(cryptor) {
  spdlog::trace("{}", __func__);
}

void OidcFilter::SetHeader(
    ::google::protobuf::RepeatedPtrField<
        ::envoy::api::v2::core::HeaderValueOption> *headers,
    absl::string_view name, absl::string_view value) {
  auto header_value_option = headers->Add();
  auto header = header_value_option->mutable_header();
  header->set_key(name.data());
  header->set_value(value.data());
}

void OidcFilter::SetStandardResponseHeaders(
    ::envoy::service::auth::v2::CheckResponse *response) {
  for (auto to_add : standard_headers) {
    SetHeader(response->mutable_denied_response()->mutable_headers(),
              to_add.first, to_add.second);
  }
}

void OidcFilter::SetRedirectHeaders(
    absl::string_view redirect_url,
    ::envoy::service::auth::v2::CheckResponse *response) {
  response->mutable_denied_response()->mutable_status()->set_code(
      envoy::type::StatusCode::Found);
  SetHeader(response->mutable_denied_response()->mutable_headers(),
            common::http::headers::Location, redirect_url.data());
}

std::string OidcFilter::EncodeCookieTimeoutDirective(int64_t timeout) {
  return std::string(common::http::headers::SetCookieDirectives::MaxAge) + "=" + std::to_string(timeout);
}

std::string OidcFilter::GetCookieName(const std::string &cookie) const {
  if (idp_config_.cookie_name_prefix() == "") {
    return "__Host-authservice-" + cookie + "-cookie";
  }
  return "__Host-" + idp_config_.cookie_name_prefix() + "-authservice-" +
         cookie + "-cookie";
}

std::string OidcFilter::GetStateCookieName() const {
  return GetCookieName("state");
}

std::string OidcFilter::GetIdTokenCookieName() const {
  return GetCookieName("id-token");
}

std::string OidcFilter::GetAccessTokenCookieName() const {
  return GetCookieName("access-token");
}

std::string OidcFilter::EncodeHeaderValue(const std::string &preamble,
                                          const std::string &value) {
  if (preamble != "") {
    return preamble + " " + value;
  }
  return value;
}

void OidcFilter::SetStateCookie(
    ::google::protobuf::RepeatedPtrField<
        ::envoy::api::v2::core::HeaderValueOption> *headers,
    absl::string_view value, int64_t timeout) {
  auto timeout_directive = EncodeCookieTimeoutDirective(timeout);
  std::set<absl::string_view> token_set_cookie_header_directives =
      {common::http::headers::SetCookieDirectives::HttpOnly,
       common::http::headers::SetCookieDirectives::SameSiteLax,
       common::http::headers::SetCookieDirectives::Secure, "Path=/",
       timeout_directive};
  auto state_cookie_header = common::http::http::EncodeSetCookie(
      GetStateCookieName(), value, token_set_cookie_header_directives);
  SetHeader(headers, common::http::headers::SetCookie, state_cookie_header);
}

absl::optional<std::string> OidcFilter::CookieFromHeaders(
    const ::google::protobuf::Map<::std::string, ::std::string> &headers,
    const std::string &cookie) {
  const auto &cookie_header_value = headers.find(common::http::headers::Cookie);
  if (cookie_header_value == headers.cend()) {
    return absl::nullopt;
  }
  auto cookies = common::http::http::DecodeCookies(cookie_header_value->second);
  if (!cookies.has_value()) {
    return absl::nullopt;
  }
  const auto &iter = cookies->find(cookie);
  if (iter != cookies->cend()) {
    return iter->second;
  }
  return absl::nullopt;
}

google::rpc::Code OidcFilter::RedirectToIdP(
    ::envoy::service::auth::v2::CheckResponse *response) {
  common::utilities::RandomGenerator generator;
  auto state = generator.Generate(32).Str();
  auto nonce = generator.Generate(32).Str();
  std::set<absl::string_view> scopes = {mandatory_scope_};
  for (const auto &scope : idp_config_.scopes()) {
    scopes.insert(scope);
  }

  auto callback = common::http::http::ToUrl(idp_config_.callback());
  auto encoded_scopes = absl::StrJoin(scopes, " ");
  std::multimap<absl::string_view, absl::string_view> params = {
      {"response_type", "code"},
      {"scope", encoded_scopes},
      {"client_id", idp_config_.client_id()},
      {"nonce", nonce},
      {"state", state},
      {"redirect_uri", callback}};
  auto query = common::http::http::EncodeQueryData(params);

  // Set redirect
  SetRedirectHeaders(
      absl::StrJoin(
          {common::http::http::ToUrl(idp_config_.authorization()), query}, "?"),
      response);

  // Create a secure state cookie that contains the state and nonce.
  StateCookieCodec codec;
  auto state_token = codec.Encode(state, nonce);
  auto encrypted_state_token = cryptor_->Encrypt(state_token);
  SetStateCookie(response->mutable_denied_response()->mutable_headers(),
                 encrypted_state_token, idp_config_.timeout());
  return google::rpc::Code::UNAUTHENTICATED;
}

google::rpc::Code OidcFilter::Process(
    const ::envoy::service::auth::v2::CheckRequest *request,
    ::envoy::service::auth::v2::CheckResponse *response) {
  spdlog::trace("{}", __func__);
  spdlog::debug(
      "Call from {}@{} to {}@{}", request->attributes().source().principal(),
      request->attributes().source().address().socket_address().address(),
      request->attributes().destination().principal(),
      request->attributes().destination().address().socket_address().address());
  if (!request->attributes().request().has_http()) {
    spdlog::info("{}: missing http in request", __func__);
    SetStandardResponseHeaders(response);
    ::grpc::Status err(::grpc::StatusCode::INVALID_ARGUMENT,
                       "missing http parameters");
    return google::rpc::Code::INVALID_ARGUMENT;
  }

  // Verify the request was via a secure scheme.
  /* TODO: Reinstate..
  if (request->attributes().request().http().scheme() != https_scheme_) {
    spdlog::info("invalid request scheme, wanted {}, got {}", https_scheme_,
                 request->attributes().request().http().scheme());
    ::grpc::Status err(::grpc::StatusCode::INVALID_ARGUMENT,
                       "invalid request scheme");
    return err;
  }
   */

  // Check if an id_token header already exists. If so let request
  // progress. It is up to the downstream system to validate the header is
  // valid.
  auto headers = request->attributes().request().http().headers();
  if (headers.contains(idp_config_.id_token().header())) {
    return google::rpc::Code::OK;
  }

  // Check if we have a valid id_token cookie and optionally an access token
  // cookie, If not go through authentication redirection dance.
  auto id_token_cookie = CookieFromHeaders(headers, GetIdTokenCookieName());
  if (id_token_cookie.has_value()) {
    auto id_token = cryptor_->Decrypt(*id_token_cookie);
    if (id_token.has_value()) {
      auto value = EncodeHeaderValue(idp_config_.id_token().preamble(),
                                     id_token.value());
      // We have a valid token. Append to headers and let processing continue.
      SetHeader(response->mutable_ok_response()->mutable_headers(),
                idp_config_.id_token().header(), value);
      // If it exists, extract the access token cookie and forward
      if (idp_config_.has_access_token()) {
        auto access_token_cookie =
            CookieFromHeaders(headers, GetAccessTokenCookieName());
        if (access_token_cookie.has_value()) {
          auto access_token = cryptor_->Decrypt(*access_token_cookie);
          if (access_token.has_value()) {
            auto value = EncodeHeaderValue(
                idp_config_.access_token().preamble(), access_token.value());
            // We have a valid token. Append to headers and let processing
            // continue.
            SetHeader(response->mutable_ok_response()->mutable_headers(),
                      idp_config_.access_token().header(), value);
            return google::rpc::Code::OK;
          } else {
            spdlog::info("{}: access token cookie decryption failed", __func__);
          }
        } else {
          spdlog::info("{}: access token cookie missing", __func__);
        }
      } else {
        return google::rpc::Code::OK;
      }
    } else {
      spdlog::info("{}: id token cookie decryption failed", __func__);
    }
  }
  // Set standard headers
  SetStandardResponseHeaders(response);
  spdlog::trace("{}: checking handler for {}://{}-{}", __func__,
                request->attributes().request().http().scheme(),
                request->attributes().request().http().host(),
                request->attributes().request().http().path());

  auto callback_host = idp_config_.callback().hostname();
  auto path_parts = common::http::http::DecodePath(
      request->attributes().request().http().path());
  if (request->attributes().request().http().host() == callback_host &&
      path_parts[0] == idp_config_.callback().path()) {
    return RetrieveToken(request, response, path_parts[1]);
  }
  return RedirectToIdP(response);
}

// Performs an HTTP POST and prints the response
google::rpc::Code OidcFilter::RetrieveToken(
    const ::envoy::service::auth::v2::CheckRequest *request,
    ::envoy::service::auth::v2::CheckResponse *response,
    absl::string_view query) {
  spdlog::trace("{}", __func__);

  // Best effort at deleting state cookie for all cases.
  SetStateCookie(response->mutable_denied_response()->mutable_headers(),
                 "deleted", 0);

  // Extract state and nonce from encrypted cookie.
  auto encrypted_state_cookie = CookieFromHeaders(
      request->attributes().request().http().headers(), GetStateCookieName());
  if (!encrypted_state_cookie.has_value()) {
    spdlog::info("{}: missing state cookie", __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }
  auto state_cookie = cryptor_->Decrypt(encrypted_state_cookie.value());
  if (!state_cookie.has_value()) {
    spdlog::info("{}: invalid state cookie", __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }
  StateCookieCodec codec;
  auto state_and_nonce = codec.Decode(state_cookie.value());
  if (!state_and_nonce.has_value()) {
    spdlog::info("{}: invalid state cookie encoding", __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }

  // Extract expected state and authorization code from request
  auto query_data = common::http::http::DecodeQueryData(query);
  if (!query_data.has_value()) {
    spdlog::info("{}: form data is invalid", __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }
  const auto state = query_data->find("state");
  const auto code = query_data->find("code");
  if (state == query_data->end() || code == query_data->end()) {
    spdlog::info(
        "{}: form data does not contain expected state and code parameters",
        __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }
  if (state->second != state_and_nonce->first) {
    spdlog::info("{}: mismatch state", __func__);
    ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                         "OIDC protocol error");
    return google::rpc::Code::INVALID_ARGUMENT;
  }

  // Build headers
  auto authorization = common::http::http::EncodeBasicAuth(
      idp_config_.client_id(), idp_config_.client_secret());
  std::map<absl::string_view, absl::string_view> headers = {
      {common::http::headers::ContentType,
       common::http::headers::ContentTypeDirectives::FormUrlEncoded},
      {common::http::headers::Authorization, authorization},
  };

  // Build body
  auto redirect_uri = common::http::http::ToUrl(idp_config_.callback());
  std::multimap<absl::string_view, absl::string_view> params = {
      {"code", code->second},
      {"redirect_uri", redirect_uri},
      {"grant_type", "authorization_code"},
  };

  auto retrieve_token_response = http_ptr_->Post(
      idp_config_.token(), headers, common::http::http::EncodeFormData(params));
  if (retrieve_token_response == nullptr) {
    spdlog::info("{}: HTTP error encountered: {}", __func__,
                 "IdP connection error");
    ::grpc::Status error(::grpc::StatusCode::INTERNAL, "IdP connection error");
    return google::rpc::Code::INTERNAL;
  }
  if (retrieve_token_response->result() != boost::beast::http::status::ok) {
    spdlog::info("{}: HTTP token response error: {}", __func__,
                 retrieve_token_response->result_int());
    ::grpc::Status error(::grpc::StatusCode::UNKNOWN, "IdP connection error");
    return google::rpc::Code::UNKNOWN;
  } else {
    auto token = parser_->Parse(idp_config_.client_id(),
        std::string(state_and_nonce->second.data(), state_and_nonce->second.size()),
        retrieve_token_response->body());
    if (!token.has_value()) {
      spdlog::info("{}: Invalid token response", __func__);
      ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                           "Invalid token response");
      return google::rpc::Code::INVALID_ARGUMENT;
    }
    auto expiry = token->Expiry();
    auto timeout = expiry.has_value() ? *expiry : std::numeric_limits<int64_t>::max();
    auto timeout_directive = EncodeCookieTimeoutDirective(timeout);
    std::set<absl::string_view> token_set_cookie_header_directives = {
        common::http::headers::SetCookieDirectives::HttpOnly,
        common::http::headers::SetCookieDirectives::SameSiteLax,
        common::http::headers::SetCookieDirectives::Secure, "Path=/",
        timeout_directive};
    // Check whether access_token forwarding is configured and if it is we have
    // an access token in our token response.
    if (idp_config_.has_access_token()) {
      auto access_token = token->AccessToken();
      if (!access_token.has_value()) {
        spdlog::info("{}: Missing expected access_token", __func__);
        ::grpc::Status error(::grpc::StatusCode::INVALID_ARGUMENT,
                             "Missing expected access_token");
        return google::rpc::Code::INVALID_ARGUMENT;
      }
      auto cookie_value = cryptor_->Encrypt(*access_token);
      auto token_set_cookie_header = common::http::http::EncodeSetCookie(
          GetAccessTokenCookieName(), cookie_value,
          token_set_cookie_header_directives);
      SetHeader(response->mutable_denied_response()->mutable_headers(),
                common::http::headers::SetCookie, token_set_cookie_header);
    }
    SetRedirectHeaders(idp_config_.landing_page(), response);
    auto cookie_value = cryptor_->Encrypt(token->IDToken().jwt_);
    auto token_set_cookie_header = common::http::http::EncodeSetCookie(
        GetIdTokenCookieName(), cookie_value,
        token_set_cookie_header_directives);
    SetHeader(response->mutable_denied_response()->mutable_headers(),
              common::http::headers::SetCookie, token_set_cookie_header);
    return google::rpc::Code::UNAUTHENTICATED;
  }
}

absl::string_view OidcFilter::Name() const { return filter_name_; }

}  // namespace oidc
}  // namespace filters
}  // namespace authservice
