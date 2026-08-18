#pragma once

#include "Configurations.h"
#include "Confirmation.h"
#include "IArtifactStore.h"
#include "ReleaseTypes.h"
#include "RestClient.h"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DeployCli
{
	class Commands
	{
	public:
		Commands(const Configurations& configurations,
				 std::shared_ptr<Artifact::IArtifactStore> store,
				 std::shared_ptr<RestClient> client,
				 std::shared_ptr<Confirmation> confirmation = nullptr);

		auto run(const std::string& name, const std::vector<std::string>& arguments) -> std::expected<void, std::string>;

		auto names(void) const -> std::vector<std::string>;

		auto last_output(void) const -> std::string;

	private:
		auto deploy(const std::vector<std::string>& arguments) -> std::expected<void, std::string>;
		auto command(const std::vector<std::string>& arguments) -> std::expected<void, std::string>;
		auto results(const std::vector<std::string>& arguments) -> std::expected<void, std::string>;

		auto upload_release(const Release::Manifest& manifest) -> std::expected<void, std::string>;

		auto present(const std::string& title, const std::string& json_body) -> void;

		auto emit(const std::string& text) -> void;

		const Configurations& configurations_;
		std::shared_ptr<Artifact::IArtifactStore> store_;
		std::shared_ptr<RestClient> client_;
		std::shared_ptr<Confirmation> confirmation_;

		std::map<std::string, std::function<std::expected<void, std::string>(const std::vector<std::string>&)>> handlers_;

		std::string last_output_;
	};
}
