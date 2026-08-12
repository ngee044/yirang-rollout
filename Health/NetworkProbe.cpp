#include "NetworkProbe.h"

#include <curl/curl.h>

#include <format>
#include <mutex>
#include <string>

namespace Health
{
	namespace
	{
		auto ensure_global_initialised(void) -> void
		{
			static std::once_flag once;
			std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
		}

		auto append_body(char* data, size_t size, size_t count, void* user) -> size_t
		{
			const size_t length = size * count;
			static_cast<std::string*>(user)->append(data, length);

			return length;
		}

		auto normalise_path(const std::string& path) -> std::string
		{
			if (path.empty())
			{
				return "/";
			}

			return (path.front() == '/') ? path : ("/" + path);
		}
	}

	NetworkProbe::NetworkProbe(const HealthCheckSpec& spec) : spec_(spec) { ensure_global_initialised(); }

	auto NetworkProbe::probe(void) -> std::expected<void, std::string>
	{
		if (spec_.port == 0)
		{
			return std::unexpected("port is required for tcp/http health check");
		}

		if (spec_.kind == CheckKind::Tcp)
		{
			return probe_tcp();
		}

		if (spec_.kind == CheckKind::Http)
		{
			return probe_http();
		}

		return std::unexpected("NetworkProbe supports tcp and http checks only");
	}

	auto NetworkProbe::probe_tcp(void) -> std::expected<void, std::string>
	{
		CURL* handle = curl_easy_init();
		if (handle == nullptr)
		{
			return std::unexpected("cannot initialise curl handle");
		}

		const std::string url = std::format("http://{}:{}", spec_.host, spec_.port);

		curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
		curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 1L);
		curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(spec_.timeout.count()));
		curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(handle, CURLOPT_PROXY, "");

		const CURLcode code = curl_easy_perform(handle);
		curl_easy_cleanup(handle);

		if (code != CURLE_OK)
		{
			return std::unexpected(std::format("cannot connect to {}:{} — {}", spec_.host, spec_.port, curl_easy_strerror(code)));
		}

		return {};
	}

	auto NetworkProbe::probe_http(void) -> std::expected<void, std::string>
	{
		CURL* handle = curl_easy_init();
		if (handle == nullptr)
		{
			return std::unexpected("cannot initialise curl handle");
		}

		const std::string url = std::format("http://{}:{}{}", spec_.host, spec_.port, normalise_path(spec_.path));
		std::string body;

		curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
		curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(spec_.timeout.count()));
		curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(spec_.timeout.count()));
		curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(handle, CURLOPT_PROXY, "");
		curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
		curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);

		const CURLcode code = curl_easy_perform(handle);
		if (code != CURLE_OK)
		{
			const std::string reason = curl_easy_strerror(code);
			curl_easy_cleanup(handle);

			return std::unexpected(std::format("cannot request {} — {}", url, reason));
		}

		long response_code = 0;
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
		curl_easy_cleanup(handle);

		if (response_code != static_cast<long>(spec_.expected_status))
		{
			return std::unexpected(std::format("unexpected status from {} — got {}, expected {}", url, response_code, spec_.expected_status));
		}

		if (!spec_.expected_body.empty() && body.find(spec_.expected_body) == std::string::npos)
		{
			return std::unexpected(std::format("response body from {} does not contain '{}'", url, spec_.expected_body));
		}

		return {};
	}
}
