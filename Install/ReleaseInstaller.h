#pragma once

#include "InstallTypes.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Install
{
	class ReleaseInstaller
	{
	public:
		explicit ReleaseInstaller(const InstallOptions& options);

		auto install(const std::string& release_id, const std::string& source_directory) -> std::expected<void, std::string>;

		auto activate(const std::string& release_id) -> std::expected<void, std::string>;

		auto rollback(void) -> std::expected<std::string, std::string>;

		auto prune(void) -> std::expected<uint32_t, std::string>;

		auto state(void) const -> std::expected<ActivationState, std::string>;

		auto release_directory(const std::string& release_id) const -> std::string;

		auto installed(void) const -> std::expected<std::vector<std::string>, std::string>;

		auto options(void) const -> InstallOptions;

	private:
		auto releases_root(void) const -> std::string;
		auto state_path(void) const -> std::string;
		auto write_state(const ActivationState& next) const -> std::expected<void, std::string>;

		auto refresh_current_link(const std::string& release_id) const -> void;

		InstallOptions options_;
	};
}
