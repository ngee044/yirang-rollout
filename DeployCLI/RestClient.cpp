#include "RestClient.h"

#include <curl/curl.h>

#include <format>

namespace DeployCli
{
	namespace
	{
		auto append_body(char* data, size_t size, size_t count, void* user_data) -> size_t
		{
			const auto total = size * count;
			static_cast<std::string*>(user_data)->append(data, total);

			return total;
		}

		auto trim_trailing_slash(const std::string& url) -> std::string
		{
			auto trimmed = url;
			while (!trimmed.empty() && trimmed.back() == '/')
			{
				trimmed.pop_back();
			}

			return trimmed;
		}

		class CurlHandle
		{
		public:
			CurlHandle(void) : handle_(curl_easy_init()), headers_(nullptr) {}

			~CurlHandle(void)
			{
				if (headers_ != nullptr)
				{
					curl_slist_free_all(headers_);
				}

				if (handle_ != nullptr)
				{
					curl_easy_cleanup(handle_);
				}
			}

			CurlHandle(const CurlHandle&) = delete;
			auto operator=(const CurlHandle&) -> CurlHandle& = delete;

			auto valid(void) const -> bool { return handle_ != nullptr; }
			auto get(void) const -> CURL* { return handle_; }

			auto add_header(const std::string& header) -> void { headers_ = curl_slist_append(headers_, header.c_str()); }
			auto headers(void) const -> curl_slist* { return headers_; }

		private:
			CURL* handle_;
			curl_slist* headers_;
		};
	}

	RestClient::RestClient(const std::string& base_url, const std::string& api_token, std::chrono::seconds timeout)
		: base_url_(trim_trailing_slash(base_url)), api_token_(api_token), timeout_(timeout)
	{
	}

	auto RestClient::base_url(void) const -> std::string { return base_url_; }

	auto RestClient::post(const std::string& path, const std::string& json_body) const -> std::expected<RestResponse, std::string> { return send(path, &json_body); }

	auto RestClient::get(const std::string& path) const -> std::expected<RestResponse, std::string> { return send(path, nullptr); }

	auto RestClient::send(const std::string& path, const std::string* json_body) const -> std::expected<RestResponse, std::string>
	{
		if (base_url_.empty())
		{
			return std::unexpected("control_plane_url is empty");
		}

		CurlHandle handle;
		if (!handle.valid())
		{
			return std::unexpected("cannot initialise the HTTP client");
		}

		const auto url = base_url_ + path;

		std::string response_body;
		curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
		curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_.count()));
		curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_body);
		curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response_body);

		curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);

		if (json_body != nullptr)
		{
			handle.add_header("Content-Type: application/json");
			curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
			curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, json_body->c_str());
			curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body->size()));
		}

		if (!api_token_.empty())
		{
			handle.add_header(std::format("Authorization: Bearer {}", api_token_));
		}

		if (handle.headers() != nullptr)
		{
			curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, handle.headers());
		}

		const auto performed = curl_easy_perform(handle.get());
		if (performed != CURLE_OK)
		{
			return std::unexpected(std::format("cannot reach '{}': {}", url, curl_easy_strerror(performed)));
		}

		RestResponse response;
		response.body = std::move(response_body);
		curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status_code);

		return response;
	}
}
