#include "Configurations.h"

#include "ArgumentParser.h"
#include "Logger.h"
#include "LogTypes.h"

#include <cstdint>
#include <format>
#include <memory>

using namespace Utilities;

auto main(int32_t argc, char* argv[]) -> int32_t
{
	auto configurations = std::make_shared<Configurations>(ArgumentParser(argc, argv));

	Logger::handle().file_mode(configurations->write_file_log());
	Logger::handle().console_mode(configurations->write_console_log());
	Logger::handle().write_interval(configurations->write_interval());
	Logger::handle().log_root(configurations->log_root_path());
	Logger::handle().start(configurations->main_title());

	auto required = configurations->validate_required();
	if (!required)
	{
		// Agent 제어 루프(MVP 1)가 구현되면 기동 실패(exit code != 0)로 전환한다
		Logger::handle().write(LogTypes::Warning, std::format("configuration incomplete: {}", required.error()));
	}

	Logger::handle().write(LogTypes::Information, std::format("{} started", configurations->main_title()));

	configurations.reset();

	Logger::handle().stop();
	Logger::destroy();

	return 0;
}
