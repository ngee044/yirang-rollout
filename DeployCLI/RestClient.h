#pragma once

#include <chrono>
#include <expected>
#include <string>

namespace DeployCli
{
	struct RestResponse
	{
		long status_code{ 0 };
		std::string body;

		auto ok(void) const -> bool { return status_code >= 200 && status_code < 300; }
	};

	class RestClient
	{
	public:
		RestClient(const std::string& base_url, const std::string& api_token, std::chrono::seconds timeout);

		auto post(const std::string& path, const std::string& json_body) const -> std::expected<RestResponse, std::string>;
		auto get(const std::string& path) const -> std::expected<RestResponse, std::string>;

		auto base_url(void) const -> std::string;

	private:
		auto send(const std::string& path, const std::string* json_body) const -> std::expected<RestResponse, std::string>;

		std::string base_url_;
		std::string api_token_;
		std::chrono::seconds timeout_;
	};
}
