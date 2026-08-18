#pragma once

#include <expected>
#include <iosfwd>
#include <string>

namespace DeployCli
{
	auto standard_input_is_interactive(void) -> bool;

	class Confirmation
	{
	public:
		Confirmation(const std::string& provided_token, bool interactive, std::istream& input, std::ostream& output);

		auto require(const std::string& expected_token, const std::string& scope) -> std::expected<void, std::string>;

	private:
		std::string provided_token_;
		bool interactive_;
		std::istream& input_;
		std::ostream& output_;
	};
}
