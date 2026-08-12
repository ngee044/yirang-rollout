#include "ReleaseManifest.h"

#include <boost/json.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <set>

namespace Release
{
	namespace
	{
		constexpr size_t kSha256HexLength = 64;
		constexpr size_t kReadChunkSize = 64 * 1024;

		auto to_hex(const unsigned char* data, size_t length) -> std::string
		{
			static constexpr char kDigits[] = "0123456789abcdef";

			std::string hex;
			hex.reserve(length * 2);
			for (size_t index = 0; index < length; ++index)
			{
				hex.push_back(kDigits[data[index] >> 4]);
				hex.push_back(kDigits[data[index] & 0x0F]);
			}

			return hex;
		}

		auto is_sha256_hex(const std::string& value) -> bool
		{
			if (value.size() != kSha256HexLength)
			{
				return false;
			}

			return std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isxdigit(character) != 0; });
		}

		auto read_string(const boost::json::object& object, const char* key) -> std::expected<std::string, std::string>
		{
			if (!object.contains(key) || !object.at(key).is_string())
			{
				return std::unexpected(std::format("manifest field '{}' is missing or not a string", key));
			}

			return std::string(object.at(key).as_string().c_str());
		}
	}

	auto ReleaseManifest::file_sha256(const std::string& path) -> std::expected<std::string, std::string>
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			return std::unexpected(std::format("cannot open '{}' for hashing", path));
		}

		EVP_MD_CTX* context = EVP_MD_CTX_new();
		if (context == nullptr)
		{
			return std::unexpected("cannot allocate digest context");
		}

		if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
		{
			EVP_MD_CTX_free(context);

			return std::unexpected("cannot initialise sha256 digest");
		}

		std::vector<char> buffer(kReadChunkSize);
		while (stream)
		{
			stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

			const auto read_size = stream.gcount();
			if (read_size <= 0)
			{
				break;
			}

			if (EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(read_size)) != 1)
			{
				EVP_MD_CTX_free(context);

				return std::unexpected(std::format("cannot update sha256 digest for '{}'", path));
			}
		}

		std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
		unsigned int digest_length = 0;
		if (EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1)
		{
			EVP_MD_CTX_free(context);

			return std::unexpected(std::format("cannot finalise sha256 digest for '{}'", path));
		}

		EVP_MD_CTX_free(context);

		return to_hex(digest.data(), digest_length);
	}

	auto ReleaseManifest::make_release_id(const std::chrono::system_clock::time_point& when) -> std::string
	{
		const std::time_t time = std::chrono::system_clock::to_time_t(when);

		std::tm utc{};
#ifdef _WIN32
		gmtime_s(&utc, &time);
#else
		gmtime_r(&time, &utc);
#endif

		char buffer[32] = {};
		std::snprintf(buffer, sizeof(buffer), "rel_%04d%02d%02d_%02d%02d%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);

		return std::string(buffer);
	}

	auto ReleaseManifest::build(const std::vector<std::string>& upload_file_list, const std::string& release_id, const std::chrono::system_clock::time_point& created_at)
		-> std::expected<Manifest, std::string>
	{
		if (release_id.empty())
		{
			return std::unexpected("release_id is required");
		}

		if (upload_file_list.empty())
		{
			return std::unexpected("upload_file_list is empty");
		}

		Manifest manifest;
		manifest.release_id = release_id;

		{
			const std::time_t time = std::chrono::system_clock::to_time_t(created_at);

			std::tm utc{};
#ifdef _WIN32
			gmtime_s(&utc, &time);
#else
			gmtime_r(&time, &utc);
#endif

			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
			manifest.created_at = buffer;
		}

		std::set<std::string> claimed;

		for (const auto& source : upload_file_list)
		{
			std::error_code error;
			const std::filesystem::path path(source);

			if (!std::filesystem::exists(path, error))
			{
				return std::unexpected(std::format("upload file does not exist: {}", source));
			}

			if (!std::filesystem::is_regular_file(path, error))
			{
				return std::unexpected(std::format("upload file is not a regular file: {}", source));
			}

			const std::string install_path = path.filename().string();
			if (!claimed.insert(install_path).second)
			{
				return std::unexpected(std::format("duplicate install path '{}' — two upload files share the same file name", install_path));
			}

			auto hash = file_sha256(source);
			if (!hash)
			{
				return std::unexpected(hash.error());
			}

			ArtifactEntry entry;
			entry.install_path = install_path;
			entry.sha256 = hash.value();
			const auto size = std::filesystem::file_size(path, error);
			if (error)
			{
				return std::unexpected(std::format("cannot read size of '{}': {}", path.string(), error.message()));
			}

			entry.size_bytes = static_cast<uint64_t>(size);
			entry.source_path = source;

			manifest.artifacts.push_back(entry);
		}

		return manifest;
	}

	auto ReleaseManifest::serialize(const Manifest& manifest) -> std::string
	{
		boost::json::array artifacts;
		for (const auto& entry : manifest.artifacts)
		{
			boost::json::object item;
			item["install_path"] = entry.install_path;
			item["sha256"] = entry.sha256;
			item["size_bytes"] = entry.size_bytes;

			artifacts.push_back(item);
		}

		boost::json::object root;
		root["release_id"] = manifest.release_id;
		root["created_at"] = manifest.created_at;
		root["artifacts"] = artifacts;

		return boost::json::serialize(root);
	}

	auto ReleaseManifest::parse(const std::string& text) -> std::expected<Manifest, std::string>
	{
		boost::json::value parsed;
		try
		{
			parsed = boost::json::parse(text);
		}
		catch (const std::exception& exception)
		{
			return std::unexpected(std::format("cannot parse manifest: {}", exception.what()));
		}

		if (!parsed.is_object())
		{
			return std::unexpected("manifest root is not a JSON object");
		}

		const auto root = parsed.as_object();

		auto release_id = read_string(root, "release_id");
		if (!release_id)
		{
			return std::unexpected(release_id.error());
		}

		if (release_id.value().empty())
		{
			return std::unexpected("manifest field 'release_id' is empty");
		}

		auto created_at = read_string(root, "created_at");
		if (!created_at)
		{
			return std::unexpected(created_at.error());
		}

		if (!root.contains("artifacts") || !root.at("artifacts").is_array())
		{
			return std::unexpected("manifest field 'artifacts' is missing or not an array");
		}

		Manifest manifest;
		manifest.release_id = release_id.value();
		manifest.created_at = created_at.value();

		for (const auto& element : root.at("artifacts").as_array())
		{
			if (!element.is_object())
			{
				return std::unexpected("manifest artifact entry is not an object");
			}

			const auto item = element.as_object();

			auto install_path = read_string(item, "install_path");
			if (!install_path)
			{
				return std::unexpected(install_path.error());
			}

			if (install_path.value().empty())
			{
				return std::unexpected("manifest artifact 'install_path' is empty");
			}

			auto sha256 = read_string(item, "sha256");
			if (!sha256)
			{
				return std::unexpected(sha256.error());
			}

			if (!is_sha256_hex(sha256.value()))
			{
				return std::unexpected(std::format("manifest artifact '{}' has a malformed sha256", install_path.value()));
			}

			ArtifactEntry entry;
			entry.install_path = install_path.value();
			entry.sha256 = sha256.value();

			if (item.contains("size_bytes") && item.at("size_bytes").is_int64())
			{
				entry.size_bytes = static_cast<uint64_t>(item.at("size_bytes").as_int64());
			}

			manifest.artifacts.push_back(entry);
		}

		return manifest;
	}
}
