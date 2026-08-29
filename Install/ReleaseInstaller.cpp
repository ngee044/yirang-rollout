#include "ReleaseInstaller.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace Install
{
	namespace
	{
		constexpr auto kStagingPrefix = ".staging-";
		constexpr auto kRetiredPrefix = ".retired-";

		auto is_internal_name(const std::string& name) -> bool { return name.rfind(kStagingPrefix, 0) == 0 || name.rfind(kRetiredPrefix, 0) == 0; }

		auto validate_release_id(const std::string& release_id) -> std::expected<void, std::string>
		{
			if (release_id.empty())
			{
				return std::unexpected("release_id is required");
			}

			if (release_id.find('/') != std::string::npos || release_id.find('\\') != std::string::npos)
			{
				return std::unexpected(std::format("release_id must not contain a path separator: '{}'", release_id));
			}

			if (release_id == "." || release_id == ".." || is_internal_name(release_id))
			{
				return std::unexpected(std::format("release_id must not be a path reference or an internal name: '{}'", release_id));
			}

			if (release_id.size() >= 2 && release_id[1] == ':')
			{
				return std::unexpected(std::format("release_id must not be a drive path: '{}'", release_id));
			}

			return {};
		}

		auto utc_now(void) -> std::string
		{
			const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

			std::tm utc{};
#ifdef _WIN32
			gmtime_s(&utc, &time);
#else
			gmtime_r(&time, &utc);
#endif

			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);

			return buffer;
		}

		auto has_parent_reference(const std::filesystem::path& relative) -> bool
		{
			for (const auto& element : relative)
			{
				if (element == "..")
				{
					return true;
				}
			}

			return false;
		}
	}

	ReleaseInstaller::ReleaseInstaller(const InstallOptions& options) : options_(options) {}

	auto ReleaseInstaller::options(void) const -> InstallOptions { return options_; }

	auto ReleaseInstaller::releases_root(void) const -> std::string { return (std::filesystem::path(options_.service_root) / "releases").string(); }

	auto ReleaseInstaller::state_path(void) const -> std::string { return (std::filesystem::path(options_.service_root) / "state.json").string(); }

	auto ReleaseInstaller::release_directory(const std::string& release_id) const -> std::string
	{
		return (std::filesystem::path(releases_root()) / release_id).string();
	}

	auto ReleaseInstaller::retired_directory(const std::string& release_id) const -> std::string
	{
		return (std::filesystem::path(releases_root()) / (std::string(kRetiredPrefix) + release_id)).string();
	}

	auto ReleaseInstaller::install(const std::string& release_id, const std::string& source_directory) -> std::expected<void, std::string>
	{
		if (options_.service_root.empty())
		{
			return std::unexpected("service_root is not configured");
		}

		auto valid = validate_release_id(release_id);
		if (!valid)
		{
			return std::unexpected(valid.error());
		}

		std::error_code error;
		if (!std::filesystem::is_directory(source_directory, error))
		{
			return std::unexpected(std::format("release '{}' is not downloaded (expected '{}')", release_id, source_directory));
		}

		const auto staging = (std::filesystem::path(releases_root()) / (std::string(kStagingPrefix) + release_id)).string();

		std::filesystem::remove_all(staging, error);

		std::filesystem::create_directories(staging, error);
		if (error)
		{
			return std::unexpected(std::format("cannot create staging directory '{}': {}", staging, error.message()));
		}

		auto discard = [&staging](void) -> void
		{
			std::error_code ignored;
			std::filesystem::remove_all(staging, ignored);
		};

		uint32_t copied = 0;
		const std::filesystem::path source(source_directory);

		std::filesystem::recursive_directory_iterator entry(source, std::filesystem::directory_options::none, error);
		if (error)
		{
			discard();

			return std::unexpected(std::format("cannot list '{}': {}", source_directory, error.message()));
		}

		const std::filesystem::recursive_directory_iterator last;
		while (entry != last)
		{
			const auto relative = std::filesystem::relative(entry->path(), source, error);
			if (error)
			{
				discard();

				return std::unexpected(std::format("cannot resolve '{}': {}", entry->path().string(), error.message()));
			}

			if (has_parent_reference(relative))
			{
				discard();

				return std::unexpected(std::format("release file escapes the install root: '{}'", relative.string()));
			}

			const auto destination = std::filesystem::path(staging) / relative;

			if (entry->is_directory(error))
			{
				std::filesystem::create_directories(destination, error);
			}
			else
			{
				std::filesystem::create_directories(destination.parent_path(), error);
				if (!error)
				{
					std::filesystem::copy_file(entry->path(), destination, std::filesystem::copy_options::overwrite_existing, error);
					++copied;
				}
			}

			if (error)
			{
				discard();

				return std::unexpected(std::format("cannot place '{}': {}", relative.string(), error.message()));
			}

			entry.increment(error);
			if (error)
			{
				discard();

				return std::unexpected(std::format("cannot walk '{}': {}", source_directory, error.message()));
			}
		}

		if (copied == 0)
		{
			discard();

			return std::unexpected(std::format("release '{}' has no files to install", release_id));
		}

		auto published = publish_staging(staging, release_id);
		if (!published)
		{
			discard();

			return std::unexpected(published.error());
		}

		return {};
	}

	auto ReleaseInstaller::sweep_retired(void) const -> void
	{
		std::error_code error;
		std::filesystem::directory_iterator entries(releases_root(), error);
		if (error)
		{
			return;
		}

		std::vector<std::filesystem::path> stale;
		for (const auto& entry : entries)
		{
			if (entry.path().filename().string().rfind(kRetiredPrefix, 0) == 0)
			{
				stale.push_back(entry.path());
			}
		}

		for (const auto& path : stale)
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	}

	auto ReleaseInstaller::publish_staging(const std::string& staging, const std::string& release_id) const -> std::expected<void, std::string>
	{
		sweep_retired();

		const auto target = release_directory(release_id);
		const auto retired = retired_directory(release_id);

		std::error_code error;
		const bool replacing = std::filesystem::exists(target, error);

		if (replacing)
		{
			std::filesystem::remove_all(retired, error);
			if (error)
			{
				return std::unexpected(std::format("cannot clear '{}': {}", retired, error.message()));
			}

			std::filesystem::rename(target, retired, error);
			if (error)
			{
				return std::unexpected(std::format("cannot set '{}' aside: {}", target, error.message()));
			}
		}

		std::filesystem::rename(staging, target, error);
		if (error)
		{
			const auto reason = error.message();

			std::error_code restore;
			if (replacing)
			{
				std::filesystem::rename(retired, target, restore);
			}

			if (replacing && restore)
			{
				return std::unexpected(std::format("cannot publish '{}': {} (the release it replaces is left at '{}')", target, reason, retired));
			}

			return std::unexpected(std::format("cannot publish '{}': {}", target, reason));
		}

		std::error_code cleanup;
		std::filesystem::remove_all(retired, cleanup);
		if (cleanup)
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

			std::error_code ignored;
			std::filesystem::rename(retired, std::format("{}.{}", retired, stamp), ignored);
		}

		return {};
	}

	auto ReleaseInstaller::activate(const std::string& release_id) -> std::expected<void, std::string>
	{
		auto valid = validate_release_id(release_id);
		if (!valid)
		{
			return std::unexpected(valid.error());
		}

		std::error_code error;
		if (!std::filesystem::is_directory(release_directory(release_id), error))
		{
			return std::unexpected(std::format("release '{}' is not installed (expected '{}')", release_id, release_directory(release_id)));
		}

		auto current = state();
		if (!current)
		{
			return std::unexpected(current.error());
		}

		ActivationState next;
		next.active = release_id;
		next.previous = (current.value().active == release_id) ? current.value().previous : current.value().active;
		next.updated_at = utc_now();

		auto written = write_state(next);
		if (!written)
		{
			return std::unexpected(written.error());
		}

		refresh_current_link(release_id);

		return {};
	}

	auto ReleaseInstaller::rollback(void) -> std::expected<std::string, std::string>
	{
		auto current = state();
		if (!current)
		{
			return std::unexpected(current.error());
		}

		if (current.value().previous.empty())
		{
			return std::unexpected("no previous release to roll back to");
		}

		const auto target = current.value().previous;

		std::error_code error;
		if (!std::filesystem::is_directory(release_directory(target), error))
		{
			return std::unexpected(std::format("previous release '{}' is no longer installed", target));
		}

		ActivationState next;
		next.active = target;
		next.previous = current.value().active;
		next.updated_at = utc_now();

		auto written = write_state(next);
		if (!written)
		{
			return std::unexpected(written.error());
		}

		refresh_current_link(target);

		return target;
	}

	auto ReleaseInstaller::installed(void) const -> std::expected<std::vector<std::string>, std::string>
	{
		std::error_code error;
		if (!std::filesystem::is_directory(releases_root(), error))
		{
			return std::vector<std::string>{};
		}

		std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;

		std::filesystem::directory_iterator entry(releases_root(), error);
		if (error)
		{
			return std::unexpected(std::format("cannot list '{}': {}", releases_root(), error.message()));
		}

		const std::filesystem::directory_iterator last;
		while (entry != last)
		{
			const auto name = entry->path().filename().string();

			if (entry->is_directory(error) && !is_internal_name(name))
			{
				const auto written = std::filesystem::last_write_time(entry->path(), error);
				found.push_back({ error ? std::filesystem::file_time_type{} : written, name });
			}

			entry.increment(error);
			if (error)
			{
				return std::unexpected(std::format("cannot walk '{}': {}", releases_root(), error.message()));
			}
		}

		std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first > right.first; });

		std::vector<std::string> names;
		names.reserve(found.size());
		for (const auto& [written, name] : found)
		{
			names.push_back(name);
		}

		return names;
	}

	auto ReleaseInstaller::prune(void) -> std::expected<uint32_t, std::string>
	{
		auto current = state();
		if (!current)
		{
			return std::unexpected(current.error());
		}

		auto releases = installed();
		if (!releases)
		{
			return std::unexpected(releases.error());
		}

		const auto keep = (options_.keep_previous_releases < 0) ? 0 : options_.keep_previous_releases;

		uint32_t removed = 0;
		int retained = 0;

		for (const auto& name : releases.value())
		{
			if (name == current.value().active || name == current.value().previous)
			{
				continue;
			}

			if (retained < keep)
			{
				++retained;
				continue;
			}

			std::error_code error;
			std::filesystem::remove_all(release_directory(name), error);
			if (error)
			{
				return std::unexpected(std::format("cannot remove '{}': {}", release_directory(name), error.message()));
			}

			++removed;
		}

		return removed;
	}

	auto ReleaseInstaller::state(void) const -> std::expected<ActivationState, std::string>
	{
		std::error_code error;
		if (!std::filesystem::exists(state_path(), error))
		{
			return ActivationState{};
		}

		std::ifstream source(state_path(), std::ios::binary);
		if (!source.is_open())
		{
			return std::unexpected(std::format("cannot open '{}'", state_path()));
		}

		std::ostringstream buffer;
		buffer << source.rdbuf();
		source.close();

		boost::json::value parsed;
		try
		{
			parsed = boost::json::parse(buffer.str());
		}
		catch (const std::exception& exception)
		{
			return std::unexpected(std::format("cannot parse '{}': {}", state_path(), exception.what()));
		}

		if (!parsed.is_object())
		{
			return std::unexpected(std::format("'{}' root is not a JSON object", state_path()));
		}

		const auto root = parsed.as_object();

		ActivationState loaded;
		if (root.contains("active") && root.at("active").is_string())
		{
			loaded.active = root.at("active").as_string().c_str();
		}
		if (root.contains("previous") && root.at("previous").is_string())
		{
			loaded.previous = root.at("previous").as_string().c_str();
		}
		if (root.contains("updated_at") && root.at("updated_at").is_string())
		{
			loaded.updated_at = root.at("updated_at").as_string().c_str();
		}

		return loaded;
	}

	auto ReleaseInstaller::write_state(const ActivationState& next) const -> std::expected<void, std::string>
	{
		std::error_code error;
		std::filesystem::create_directories(options_.service_root, error);
		if (error)
		{
			return std::unexpected(std::format("cannot create '{}': {}", options_.service_root, error.message()));
		}

		boost::json::object root;
		root["active"] = next.active;
		root["previous"] = next.previous;
		root["updated_at"] = next.updated_at;

		const auto temporary = state_path() + ".tmp";
		{
			std::ofstream sink(temporary, std::ios::binary | std::ios::trunc);
			if (!sink.is_open())
			{
				return std::unexpected(std::format("cannot write '{}'", temporary));
			}

			sink << boost::json::serialize(root);
			sink.flush();
			if (!sink.good())
			{
				return std::unexpected(std::format("cannot write '{}'", temporary));
			}
		}

		std::filesystem::rename(temporary, state_path(), error);
		if (error)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);

			return std::unexpected(std::format("cannot publish '{}': {}", state_path(), error.message()));
		}

		return {};
	}

	auto ReleaseInstaller::refresh_current_link(const std::string& release_id) const -> void
	{
		const auto link = (std::filesystem::path(options_.service_root) / "current").string();

		std::error_code ignored;
		std::filesystem::remove(link, ignored);

		std::filesystem::create_directory_symlink(std::filesystem::path("releases") / release_id, link, ignored);
	}
}
