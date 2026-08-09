#include "Configurations.h"

#include "ArgumentParser.h"
#include "Logger.h"
#include "LogTypes.h"

#include <cstdint>
#include <format>

#ifndef YIRANG_DEPLOY_CLI_VERSION
#define YIRANG_DEPLOY_CLI_VERSION "0.0.0"
#endif

using namespace Utilities;
using namespace DeployCli;

namespace
{
	auto print_usage(void) -> void
	{
		Logger::handle().write(LogTypes::Information, "usage:\n"
													  "  yirang [command] [options]\n"
													  "\n"
													  "connection:\n"
													  "  --control_plane_url <url>        Control Plane REST API 주소 (필수)\n"
													  "  --api_token <token>              인증 토큰\n"
													  "  --request_timeout_seconds <n>    요청 타임아웃 (기본 30)\n"
													  "  --output_format <table|json>     출력 형식 (기본 table)\n"
													  "\n"
													  "common:\n"
													  "  --config_path <path>             설정 파일 경로 (기본 yirang_deploy_configurations.json)\n"
													  "  --log_root_path <dir>            로그 디렉터리\n"
													  "  --write_console_log <0-8>        콘솔 로그 레벨 (0=None 2=Error 3=Warning 4=Information)\n"
													  "  --write_file_log <0-8>           파일 로그 레벨\n"
													  "  --write_interval <ms>            로그 기록 주기\n"
													  "  --help                           도움말\n"
													  "  --version                        버전");
	}
}

auto main(int32_t argc, char* argv[]) -> int32_t
{
	Configurations configurations(ArgumentParser(argc, argv));

	Logger::handle().file_mode(configurations.write_file_log());
	Logger::handle().console_mode(configurations.write_console_log());
	Logger::handle().write_interval(configurations.write_interval());
	Logger::handle().log_root(configurations.log_root_path());
	Logger::handle().start(configurations.app_title());

	auto exit_code = 0;

	if (configurations.show_version())
	{
		Logger::handle().write(LogTypes::Information, std::format("yirang {}", YIRANG_DEPLOY_CLI_VERSION));
	}
	else if (configurations.show_help())
	{
		print_usage();
	}
	else
	{
		// 설정 파일이 없어도 CLI 인자만으로 동작할 수 있으므로 경고로만 알린다.
		auto warning = configurations.load_warning();
		if (warning != std::nullopt)
		{
			Logger::handle().write(LogTypes::Warning, warning.value());
		}

		auto required = configurations.validate_required();
		if (!required)
		{
			Logger::handle().write(LogTypes::Error, required.error());
			exit_code = 1;
		}
		else
		{
			// 서브커맨드는 Control Plane REST API(T-101) 착수와 함께 추가한다.
			Logger::handle().write(LogTypes::Warning, "no command given");
			print_usage();
			exit_code = 1;
		}
	}

	Logger::handle().stop();
	Logger::destroy();

	return exit_code;
}
