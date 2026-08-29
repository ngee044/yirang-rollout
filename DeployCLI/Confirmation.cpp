#include "Confirmation.h"

#include <format>
#include <istream>
#include <ostream>

#ifdef _WIN32
#include <cstdio>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace DeployCli
{
	namespace
	{
		auto trimmed(const std::string& text) -> std::string
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				return {};
			}

			const auto last = text.find_last_not_of(" \t\r\n");

			return text.substr(first, last - first + 1);
		}
	}

	auto standard_input_is_interactive(void) -> bool
	{
#ifdef _WIN32
		return _isatty(_fileno(stdin)) != 0;
#else
		return isatty(STDIN_FILENO) != 0;
#endif
	}

	Confirmation::Confirmation(const std::string& provided_token, bool interactive, std::istream& input, std::ostream& output)
		: provided_token_(provided_token), interactive_(interactive), input_(input), output_(output)
	{
	}

	auto Confirmation::require(const std::string& expected_token, const std::string& scope) -> std::expected<void, std::string>
	{
		if (!provided_token_.empty())
		{
			if (provided_token_ == expected_token)
			{
				return {};
			}

			return std::unexpected(std::format("--confirm '{}' does not name the command being run — pass --confirm {}", provided_token_, expected_token));
		}

		if (!interactive_)
		{
			return std::unexpected(
				std::format("'{}' targets {} and was not confirmed — stdin is not a terminal, so pass --confirm {}", expected_token, scope, expected_token));
		}

		output_ << std::format("'{}' targets {}.\ntype '{}' to continue: ", expected_token, scope, expected_token);
		output_.flush();

		std::string answer;
		if (!std::getline(input_, answer))
		{
			return std::unexpected(std::format("'{}' was not confirmed — no answer on stdin", expected_token));
		}

		if (trimmed(answer) != expected_token)
		{
			return std::unexpected(std::format("'{}' was not confirmed", expected_token));
		}

		return {};
	}
}
