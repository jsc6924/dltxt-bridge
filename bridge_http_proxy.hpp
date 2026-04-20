#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace bridge_http {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

inline constexpr unsigned short local_http_port = 9286;

struct LocalHttpResponse {
    http::status status = http::status::ok;
    std::string body;
    std::string content_type = "text/plain";
};

struct ParsedUrl {
    bool use_ssl = false;
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

inline LocalHttpResponse make_text_response(http::status status, std::string body) {
    return LocalHttpResponse{status, std::move(body), "text/plain; charset=utf-8"};
}

inline LocalHttpResponse make_json_response(http::status status, const json& body) {
    return LocalHttpResponse{status, body.dump(), "application/json"};
}

inline ParsedUrl parse_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("Unsupported URL: missing scheme");
    }

    ParsedUrl parsed;
    parsed.scheme = std::string(url.substr(0, scheme_end));
    parsed.use_ssl = parsed.scheme == "https";
    if (!parsed.use_ssl && parsed.scheme != "http") {
        throw std::invalid_argument("Unsupported URL scheme: " + parsed.scheme);
    }

    std::string_view remainder = url.substr(scheme_end + 3);
    const auto path_start = remainder.find('/');
    std::string_view authority = path_start == std::string_view::npos ? remainder : remainder.substr(0, path_start);
    parsed.target = path_start == std::string_view::npos ? "/" : std::string(remainder.substr(path_start));

    const auto port_separator = authority.rfind(':');
    if (port_separator != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        parsed.host = std::string(authority.substr(0, port_separator));
        parsed.port = std::string(authority.substr(port_separator + 1));
    } else {
        parsed.host = std::string(authority);
        parsed.port = parsed.use_ssl ? "443" : "80";
    }

    if (parsed.host.empty()) {
        throw std::invalid_argument("Unsupported URL: missing host");
    }

    return parsed;
}

inline std::string origin_for_url(const ParsedUrl& parsed) {
    const bool uses_default_port = (parsed.use_ssl && parsed.port == "443") || (!parsed.use_ssl && parsed.port == "80");
    return parsed.scheme + "://" + parsed.host + (uses_default_port ? "" : ":" + parsed.port);
}

inline std::string resolve_url(std::string_view base_url, std::string_view href) {
    if (href.rfind("https://", 0) == 0 || href.rfind("http://", 0) == 0) {
        return std::string(href);
    }

    ParsedUrl parsed_base = parse_url(base_url);
    if (href.rfind("//", 0) == 0) {
        return parsed_base.scheme + ":" + std::string(href);
    }

    if (!href.empty() && href.front() == '/') {
        return origin_for_url(parsed_base) + std::string(href);
    }

    return origin_for_url(parsed_base) + "/" + std::string(href);
}

inline void configure_ssl_context(ssl::context& context) {
    context.set_default_verify_paths();
    context.set_verify_mode(ssl::verify_peer);

#ifdef _WIN32
    HCERTSTORE cert_store_handle = CertOpenSystemStoreW(static_cast<HCRYPTPROV_LEGACY>(0), L"ROOT");
    if (cert_store_handle == nullptr) {
        return;
    }

    X509_STORE* x509_store = SSL_CTX_get_cert_store(context.native_handle());
    PCCERT_CONTEXT current_certificate = nullptr;
    while ((current_certificate = CertEnumCertificatesInStore(cert_store_handle, current_certificate)) != nullptr) {
        const unsigned char* encoded_certificate = current_certificate->pbCertEncoded;
        X509* certificate = d2i_X509(nullptr, &encoded_certificate, static_cast<long>(current_certificate->cbCertEncoded));
        if (certificate == nullptr) {
            ERR_clear_error();
            continue;
        }

        if (X509_STORE_add_cert(x509_store, certificate) != 1) {
            const unsigned long error = ERR_peek_last_error();
            if (ERR_GET_LIB(error) != ERR_LIB_X509 || ERR_GET_REASON(error) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
                ERR_clear_error();
            } else {
                ERR_clear_error();
            }
        }

        X509_free(certificate);
    }

    CertCloseStore(cert_store_handle, 0);
#endif
}

inline std::optional<std::string> extract_application_id(std::string_view text) {
    static const std::regex application_id_pattern(R"(_ApplicationId\s*=\s*[\"']([A-Za-z0-9]+)[\"'])");
    std::string text_string(text);
    std::smatch match;
    if (!std::regex_search(text_string, match, application_id_pattern)) {
        return std::nullopt;
    }

    return match[1].str();
}

inline std::vector<std::string> extract_preload_script_urls(std::string_view html, std::string_view base_url) {
    static const std::regex link_pattern(R"(<link\b[^>]*>)", std::regex::icase);
    static const std::regex rel_pattern(R"(\brel\s*=\s*[\"']preload[\"'])", std::regex::icase);
    static const std::regex as_pattern(R"(\bas\s*=\s*[\"']script[\"'])", std::regex::icase);
    static const std::regex href_pattern(R"(\bhref\s*=\s*[\"']([^\"']+)[\"'])", std::regex::icase);

    std::vector<std::string> urls;
    std::string html_string(html);
    auto begin = std::sregex_iterator(html_string.begin(), html_string.end(), link_pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string tag = it->str();
        if (!std::regex_search(tag, rel_pattern) || !std::regex_search(tag, as_pattern)) {
            continue;
        }

        std::smatch href_match;
        if (!std::regex_search(tag, href_match, href_pattern)) {
            continue;
        }

        urls.push_back(resolve_url(base_url, href_match[1].str()));
    }

    return urls;
}

inline const json* find_nested(const json& value, std::initializer_list<std::string_view> keys) {
    const json* current = &value;
    for (const std::string_view key : keys) {
        if (!current->is_object()) {
            return nullptr;
        }

        auto it = current->find(std::string(key));
        if (it == current->end()) {
            return nullptr;
        }

        current = &(*it);
    }

    return current;
}

inline std::string json_string_field(const json& value, std::string_view key) {
    if (!value.is_object()) {
        return {};
    }

    auto it = value.find(std::string(key));
    if (it == value.end() || !it->is_string()) {
        return {};
    }

    return it->get<std::string>();
}

inline json build_search_payload(std::string_view query) {
    return json{
        {"functions", json::array({
            json{
                {"name", "search-all"},
                {"params", json{{"text", std::string(query)}, {"types", json::array({102, 106})}}}
            }
        })}
    };
}

inline json build_details_payload(const std::vector<std::string>& object_ids) {
    json items_json = json::array();
    for (const auto& object_id : object_ids) {
        items_json.push_back(json{{"objectId", object_id}});
    }

    return json{{"itemsJson", std::move(items_json)}, {"skipAccessories", false}};
}

inline json transform_single_word(const json& word) {
    json transformed{
        {"id", ""},
        {"spell", ""},
        {"pron", ""},
        {"accent", ""},
        {"excerpt", ""},
        {"details", json::array()},
        {"subDetails", json::array()}
    };

    const json* word_node = find_nested(word, {"word"});
    if (word_node != nullptr) {
        transformed["id"] = json_string_field(*word_node, "objectId");
        transformed["spell"] = json_string_field(*word_node, "spell");
        transformed["pron"] = json_string_field(*word_node, "pron");
        transformed["accent"] = json_string_field(*word_node, "accent");
        transformed["excerpt"] = json_string_field(*word_node, "excerpt");
    }

    std::map<std::string, std::size_t> subdetail_index_by_id;

    const json* details = find_nested(word, {"details"});
    if (details != nullptr && details->is_array()) {
        for (const auto& detail : *details) {
            transformed["details"].push_back(json{
                {"id", json_string_field(detail, "objectId")},
                {"title", json_string_field(detail, "title")}
            });
        }
    }

    const json* subdetails = find_nested(word, {"subdetails"});
    if (subdetails != nullptr && subdetails->is_array()) {
        for (const auto& subdetail : *subdetails) {
            const std::string id = json_string_field(subdetail, "objectId");
            transformed["subDetails"].push_back(json{
                {"id", id},
                {"title", json_string_field(subdetail, "title")},
                {"detailId", json_string_field(subdetail, "detailsId")},
                {"examples", json::array()}
            });
            subdetail_index_by_id[id] = transformed["subDetails"].size() - 1;
        }
    }

    const json* examples = find_nested(word, {"examples"});
    if (examples != nullptr && examples->is_array()) {
        for (const auto& example : *examples) {
            const std::string subdetail_id = json_string_field(example, "subdetailsId");
            auto subdetail_it = subdetail_index_by_id.find(subdetail_id);
            if (subdetail_it == subdetail_index_by_id.end()) {
                continue;
            }

            transformed["subDetails"][subdetail_it->second]["examples"].push_back(json{
                {"id", json_string_field(example, "objectId")},
                {"title", json_string_field(example, "title")},
                {"trans", json_string_field(example, "trans")}
            });
        }
    }

    return transformed;
}

inline json transform_details_response(const json& response) {
    const json* results = find_nested(response, {"result", "result"});
    if (results == nullptr || !results->is_array()) {
        throw std::runtime_error("Cannot find 'result.result' in response object");
    }

    json words = json::array();
    for (const auto& entry : *results) {
        json word = transform_single_word(entry);
        if (!word["id"].get<std::string>().empty()) {
            words.push_back(std::move(word));
        }
    }

    return json{{"words", std::move(words)}};
}

template <typename EnsureReadyFn, typename RemotePostFn>
net::awaitable<LocalHttpResponse> dispatch_local_request(
    http::verb method,
    std::string_view target,
    std::string_view body,
    std::string_view version,
    EnsureReadyFn&& ensure_ready,
    RemotePostFn&& remote_post) {

    if (target == "/healthcheck") {
        printf("[http] /healthcheck\n");
        co_return make_text_response(http::status::ok, "version=" + std::string(version));
    }

    if (method != http::verb::post) {
        co_return make_text_response(http::status::method_not_allowed, "Only POST requests are supported");
    }

    const bool requires_remote_ready = target == "/search" || target == "/details";
    if (requires_remote_ready) {
        co_await ensure_ready();
    }

    json request_body;
    try {
        request_body = json::parse(body.empty() ? "{}" : std::string(body));
    } catch (const std::exception&) {
        co_return make_text_response(http::status::bad_request, "Invalid JSON format");
    }

    try {
        if (target == "/search") {
            printf("[http] /search %s\n", body.data());
            const std::string query = json_string_field(request_body, "query");
            json proxy_response = co_await remote_post(
                "https://api.mojidict.com/parse/functions/union-api",
                build_search_payload(query));

            const json* result = find_nested(proxy_response, {"result", "results", "search-all"});
            if (result == nullptr) {
                co_return make_text_response(http::status::bad_gateway, "Search response did not contain result.results.search-all");
            }

            co_return make_json_response(http::status::ok, *result);
        }

        if (target == "/details") {
            printf("[http] /details %s\n", body.data());
            std::vector<std::string> object_ids;
            auto object_ids_it = request_body.find("objectIds");
            if (object_ids_it != request_body.end() && object_ids_it->is_array()) {
                for (const auto& value : *object_ids_it) {
                    if (value.is_string()) {
                        object_ids.push_back(value.get<std::string>());
                    }
                }
            }

            json proxy_response = co_await remote_post(
                "https://api.mojidict.com/parse/functions/nlt-fetchManyLatestWords",
                build_details_payload(object_ids));

            co_return make_json_response(http::status::ok, transform_details_response(proxy_response));
        }
    } catch (const std::exception& error) {
        co_return make_text_response(http::status::bad_gateway, error.what());
    }

    co_return make_text_response(http::status::not_found, "Unknown route");
}

class MojiProxyService {
    std::string version_;
    std::string application_id_;
    std::string session_token_;
    std::string moji_version_;
    ssl::context ssl_context_;

    net::awaitable<std::string> fetch_text(std::string url) {
        ParsedUrl parsed = parse_url(url);
        auto executor = co_await net::this_coro::executor;
        tcp::resolver resolver(executor);
        auto const results = co_await resolver.async_resolve(parsed.host, parsed.port, net::use_awaitable);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;

        if (parsed.use_ssl) {
            beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_context_);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
                beast::error_code error{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
                throw beast::system_error(error);
            }
            stream.set_verify_callback(ssl::host_name_verification(parsed.host));

            co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);
            co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

            http::request<http::empty_body> request{http::verb::get, parsed.target, 11};
            request.set(http::field::host, parsed.host);
            co_await http::async_write(stream, request, net::use_awaitable);
            co_await http::async_read(stream, buffer, response, net::use_awaitable);

            beast::error_code shutdown_error;
            stream.shutdown(shutdown_error);
        } else {
            beast::tcp_stream stream(executor);
            co_await stream.async_connect(results, net::use_awaitable);

            http::request<http::empty_body> request{http::verb::get, parsed.target, 11};
            request.set(http::field::host, parsed.host);
            co_await http::async_write(stream, request, net::use_awaitable);
            co_await http::async_read(stream, buffer, response, net::use_awaitable);

            beast::error_code shutdown_error;
            stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_error);
        }

        if (response.result() != http::status::ok) {
            throw std::runtime_error("HTTP fetch failed with status " + std::to_string(static_cast<unsigned>(response.result_int())));
        }

        co_return response.body();
    }

public:
    explicit MojiProxyService(net::any_io_executor, std::string version)
        : version_(std::move(version)),
          ssl_context_(ssl::context::tls_client) {
                configure_ssl_context(ssl_context_);
    }

    const std::string& application_id() const {
        return application_id_;
    }

    net::awaitable<void> ensure_application_id() {
        if (!application_id_.empty()) {
            co_return;
        }

        co_await fetch_application_id();
    }

    net::awaitable<void> fetch_application_id() {
        if (!application_id_.empty()) {
            co_return;
        }

        printf("Fetching Mojidict ApplicationID\n");
        const std::string root_url = "https://www.mojidict.com/";
        const std::string html = co_await fetch_text(root_url);
        if (auto id = extract_application_id(html); id.has_value()) {
            application_id_ = *id;
            printf("Fetched Mojidict ApplicationID: %s\n", application_id_.c_str());
            co_return;
        }

        const auto script_urls = extract_preload_script_urls(html, root_url);
        for (const auto& script_url : script_urls) {
            try {
                const std::string script = co_await fetch_text(script_url);
                if (auto id = extract_application_id(script); id.has_value()) {
                    application_id_ = *id;
                    printf("Fetched Mojidict ApplicationID: %s\n", application_id_.c_str());
                    co_return;
                }
            } catch (const std::exception& error) {
                fprintf(stderr, "Skipping Mojidict asset %s: %s\n", script_url.c_str(), error.what());
            }
        }

        throw std::runtime_error("ApplicationID not found in Mojidict assets");
    }

    net::awaitable<json> post_json(std::string url, json payload) {
        co_await ensure_application_id();

        payload["_ApplicationId"] = application_id_;
        payload["g_os"] = "PCWeb";
        if (!moji_version_.empty()) {
            payload["g_ver"] = moji_version_;
        }
        if (!session_token_.empty()) {
            payload["_SessionToken"] = session_token_;
        }

        ParsedUrl parsed = parse_url(url);
        auto executor = co_await net::this_coro::executor;
        tcp::resolver resolver(executor);
        auto const results = co_await resolver.async_resolve(parsed.host, parsed.port, net::use_awaitable);

        const std::string body = payload.dump();
        beast::flat_buffer buffer;
        http::response<http::string_body> response;

        if (parsed.use_ssl) {
            beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_context_);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
                beast::error_code error{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
                throw beast::system_error(error);
            }
            stream.set_verify_callback(ssl::host_name_verification(parsed.host));

            co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);
            co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

            http::request<http::string_body> request{http::verb::post, parsed.target, 11};
            request.set(http::field::host, parsed.host);
            request.set(http::field::content_type, "application/json");
            request.body() = body;
            request.prepare_payload();

            co_await http::async_write(stream, request, net::use_awaitable);
            co_await http::async_read(stream, buffer, response, net::use_awaitable);

            beast::error_code shutdown_error;
            stream.shutdown(shutdown_error);
        } else {
            beast::tcp_stream stream(executor);
            co_await stream.async_connect(results, net::use_awaitable);

            http::request<http::string_body> request{http::verb::post, parsed.target, 11};
            request.set(http::field::host, parsed.host);
            request.set(http::field::content_type, "application/json");
            request.body() = body;
            request.prepare_payload();

            co_await http::async_write(stream, request, net::use_awaitable);
            co_await http::async_read(stream, buffer, response, net::use_awaitable);

            beast::error_code shutdown_error;
            stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_error);
        }

        if (response.result() != http::status::ok) {
            throw std::runtime_error("Mojidict request failed with status " + std::to_string(static_cast<unsigned>(response.result_int())));
        }

        try {
            co_return json::parse(response.body());
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Failed to parse Mojidict JSON response: ") + error.what());
        }
    }

    net::awaitable<LocalHttpResponse> handle_request(http::verb method, std::string_view target, std::string_view body) {
        co_return co_await dispatch_local_request(
            method,
            target,
            body,
            version_,
            [this]() -> net::awaitable<void> {
                co_await ensure_application_id();
            },
            [this](std::string url, json payload) -> net::awaitable<json> {
                co_return co_await post_json(std::move(url), std::move(payload));
            });
    }
};

inline net::awaitable<void> handle_http_session(tcp::socket socket, std::shared_ptr<MojiProxyService> service) {
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;

    try {
        http::request_parser<http::string_body> parser;
        parser.body_limit(1024 * 1024);
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto request = parser.release();

        LocalHttpResponse proxy_response = co_await service->handle_request(
            request.method(),
            std::string(request.target()),
            request.body());

        http::response<http::string_body> response{proxy_response.status, request.version()};
        response.set(http::field::server, "dltxt_bridge");
        response.set(http::field::content_type, proxy_response.content_type);
        response.keep_alive(false);
        response.body() = std::move(proxy_response.body);
        response.prepare_payload();

        co_await http::async_write(stream, response, net::use_awaitable);
    } catch (const std::exception& error) {
        fprintf(stderr, "Local HTTP session error: %s\n", error.what());
    }

    beast::error_code error;
    stream.socket().shutdown(tcp::socket::shutdown_both, error);
    stream.socket().close(error);
}

template <typename EnsureReadyFn>
inline net::awaitable<void> initialize_http_service(EnsureReadyFn&& ensure_ready) {
    try {
        co_await ensure_ready();
    } catch (const std::exception& error) {
        fprintf(stderr, "Failed to fetch Mojidict ApplicationID at HTTP listener startup: %s\n", error.what());
    }
}

inline net::awaitable<void> http_listener(unsigned short port, std::shared_ptr<MojiProxyService> service) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, tcp::endpoint(net::ip::address_v4::loopback(), port));

    printf("dltxt_bridge HTTP proxy on 127.0.0.1:%u\n", port);
    co_await initialize_http_service([service]() -> net::awaitable<void> {
        co_await service->ensure_application_id();
    });

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);
        net::co_spawn(executor, handle_http_session(std::move(socket), service), net::detached);
    }
}
}