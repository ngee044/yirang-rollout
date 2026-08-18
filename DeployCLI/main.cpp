#include "Commands.h"
#include "Configurations.h"
#include "Confirmation.h"
#include "RestClient.h"

#include "ArgumentParser.h"
#include "ArtifactTypes.h"
#include "Logger.h"
#include "LogTypes.h"
#include "S3ArtifactStore.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef YIRANG_DEPLOY_CLI_VERSION
#define YIRANG_DEPLOY_CLI_VERSION "0.0.0"
#endif

using namespace Utilities;
using namespace DeployCli;

namespace
{
	auto print_usage(void) -> void
	{
		std::cout << "usage:\n"
					 "  yirang <command> [options]\n"
					 "\n"
					 "commands:\n"
					 "  deploy                           upload_file_list 를 S3 에 올리고 배포를 요청한다\n"
					 "  command <name> [release_id]      Agent 명령을 발행한다\n"
					 "                                   download_version | apply_version | current_status\n"
					 "                                   clean_old_version | rollback_version\n"
					 "                                   clean_old_version 은 확인이 필요하다 (--confirm clean_old_version)\n"
					 "  results                          디바이스가 보고한 결과를 조회한다\n"
					 "\n"
					 "connection:\n"
					 "  --control_plane_url <url>        REST API 주소 (필수)\n"
					 "  --api_token <token>              인증 토큰\n"
					 "  --request_timeout_seconds <n>    요청 타임아웃 (기본 30)\n"
					 "  --output_format <table|json>     출력 형식 (기본 table)\n"
					 "\n"
					 "deploy:\n"
					 "  --upload_file_list <a,b,c>       업로드 대상 (쉼표 구분, 설정 파일 목록을 대체)\n"
					 "  --target_group <name>            대상 그룹 (비우면 등록된 전체 큐)\n"
					 "  --s3_bucket <name>               업로드 버킷 (deploy 필수)\n"
					 "  --s3_region <region>             버킷 리전 (기본 us-east-1)\n"
					 "  --s3_endpoint <url>              MinIO·LocalStack 등 호환 저장소 (스킴 생략 시 https)\n"
					 "  --allow_insecure_tls <bool>      저장소 TLS 검증 해제 (기본 false, 신뢰된 사설망 전용)\n"
					 "\n"
					 "common:\n"
					 "  --config_path <path>             설정 파일 경로 (기본 yirang_deploy_configurations.json)\n"
					 "  --log_root_path <dir>            로그 디렉터리\n"
					 "  --write_console_log <0-8>        콘솔 로그 레벨 (0=None 2=Error 3=Warning 4=Information)\n"
					 "  --write_file_log <0-8>           파일 로그 레벨\n"
					 "  --write_interval <ms>            로그 기록 주기\n"
					 "  --confirm <command>              파괴적 명령 승인 (비대화형에서 필수, 대화형이면 확인 입력으로 대체)\n"
					 "  --help                           도움말\n"
					 "  --version                        버전\n";
		std::cout.flush();
	}

	auto positional_arguments(int32_t argc, char* argv[]) -> std::vector<std::string>
	{
		std::vector<std::string> positional;

		for (int32_t index = 1; index < argc; ++index)
		{
			const std::string token(argv[index]);

			if (token.rfind("--", 0) == 0)
			{
				if (token != "--help" && token != "--version" && index + 1 < argc && std::string(argv[index + 1]).rfind("--", 0) != 0)
				{
					++index;
				}

				continue;
			}

			positional.push_back(token);
		}

		return positional;
	}

	auto make_store_options(const Configurations& configurations) -> Artifact::StoreOptions
	{
		Artifact::StoreOptions options;
		options.bucket = configurations.s3_bucket();
		options.region = configurations.s3_region();
		options.endpoint = configurations.s3_endpoint();
		options.allow_insecure_tls = configurations.allow_insecure_tls();

		return options;
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
		std::cout << std::format("yirang {}\n", YIRANG_DEPLOY_CLI_VERSION);
		std::cout.flush();
	}
	else if (configurations.show_help())
	{
		print_usage();
	}
	else
	{
		auto warning = configurations.load_warning();
		if (warning != std::nullopt)
		{
			Logger::handle().write(LogTypes::Warning, warning.value());
		}

		const auto positional = positional_arguments(argc, argv);
		if (positional.empty())
		{
			Logger::handle().write(LogTypes::Error, "no command given");
			print_usage();
			exit_code = 1;
		}
		else
		{
			auto store = std::make_shared<Artifact::S3ArtifactStore>(make_store_options(configurations));
			auto client = std::make_shared<RestClient>(configurations.control_plane_url(), configurations.api_token(),
													   std::chrono::seconds(configurations.request_timeout_seconds()));

			auto confirmation = std::make_shared<Confirmation>(configurations.confirm_token(), standard_input_is_interactive(), std::cin, std::cout);

			Commands commands(configurations, store, client, confirmation);

			const auto name = positional.front();
			const std::vector<std::string> arguments(positional.begin() + 1, positional.end());

			auto handled = commands.run(name, arguments);
			if (!handled)
			{
				Logger::handle().write(LogTypes::Error, handled.error());
				exit_code = 1;
			}
		}
	}

	Logger::handle().stop();
	Logger::destroy();

	return exit_code;
}
