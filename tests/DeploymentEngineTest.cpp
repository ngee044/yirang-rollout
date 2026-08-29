#include <gtest/gtest.h>

#include "DeploymentEngine.h"

#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace Deploy;

namespace
{
	class FakeSupervisor : public Process::IProcessSupervisor
	{
	public:
		auto start(const Process::ProcessStartOptions& options) -> std::expected<Process::ProcessHandle, std::string> override
		{
			started_.push_back(options.executable_path);

			if (fail_start_)
			{
				return std::unexpected("start refused");
			}

			Process::ProcessHandle handle{ ++next_id_, static_cast<uint64_t>(next_id_) + 1000 };
			state_.insert({ handle.id, alive_after_start_ ? Process::ProcessState::Running : Process::ProcessState::Exited });
			token_.insert({ handle.id, handle.start_token });

			return handle;
		}

		auto adopt(const Process::ProcessHandle& handle) -> std::expected<Process::ProcessHandle, std::string> override
		{
			adopted_.push_back(handle.id);

			auto recorded = token_.find(handle.id);
			if (recorded == token_.end() || recorded->second != handle.start_token)
			{
				return std::unexpected("process is not the recorded instance");
			}

			auto living = state_.find(handle.id);
			if (living == state_.end() || living->second != Process::ProcessState::Running)
			{
				return std::unexpected("process does not exist");
			}

			return handle;
		}

		auto stop(const Process::ProcessHandle& handle, std::chrono::seconds) -> std::expected<void, std::string> override
		{
			stopped_.push_back(handle.id);

			if (fail_stop_)
			{
				return std::unexpected("stop refused");
			}

			state_[handle.id] = Process::ProcessState::Exited;

			return {};
		}

		auto status(const Process::ProcessHandle& handle) -> std::expected<Process::ProcessStatus, std::string> override
		{
			Process::ProcessStatus reported;

			auto iter = state_.find(handle.id);
			reported.state = (iter == state_.end()) ? Process::ProcessState::Unknown : iter->second;

			return reported;
		}

		auto started(void) const -> std::vector<std::string> { return started_; }
		auto stopped(void) const -> std::vector<int64_t> { return stopped_; }
		auto adopted(void) const -> std::vector<int64_t> { return adopted_; }

		auto fail_start(void) -> void { fail_start_ = true; }
		auto fail_stop(void) -> void { fail_stop_ = true; }

		auto exit_immediately(void) -> void { alive_after_start_ = false; }

		auto already_running(int64_t id, uint64_t token) -> void
		{
			state_[id] = Process::ProcessState::Running;
			token_[id] = token;

			if (id > next_id_)
			{
				next_id_ = id;
			}
		}

	private:
		std::map<int64_t, Process::ProcessState> state_;
		std::map<int64_t, uint64_t> token_;
		std::vector<std::string> started_;
		std::vector<int64_t> stopped_;
		std::vector<int64_t> adopted_;

		int64_t next_id_{ 0 };
		bool fail_start_{ false };
		bool fail_stop_{ false };
		bool alive_after_start_{ true };
	};

	class TemporaryTree
	{
	public:
		TemporaryTree(void)
		{
			static int sequence = 0;
			static const auto token = std::to_string(std::random_device{}());

			root_ = std::filesystem::temp_directory_path() / ("yirang-deploy-test-" + token + "-" + std::to_string(++sequence));
			std::filesystem::create_directories(root_ / "service");
		}

		~TemporaryTree(void)
		{
			std::error_code ignored;
			std::filesystem::remove_all(root_, ignored);
		}

		TemporaryTree(const TemporaryTree&) = delete;
		auto operator=(const TemporaryTree&) -> TemporaryTree& = delete;

		auto service_root(void) const -> std::string { return (root_ / "service").string(); }

		auto stage(const std::string& release_id, const std::string& executable = "app.exe") const -> std::string
		{
			const auto directory = root_ / "cache" / release_id;
			std::filesystem::create_directories(directory);

			std::ofstream stream(directory / executable, std::ios::binary);
			stream << "binary " << release_id;
			stream.close();

			return directory.string();
		}

	private:
		std::filesystem::path root_;
	};

	auto record_path(const TemporaryTree& tree) -> std::filesystem::path { return std::filesystem::path(tree.service_root()) / "runtime.json"; }

	auto read_record(const TemporaryTree& tree) -> boost::json::object
	{
		std::ifstream source(record_path(tree), std::ios::binary);

		std::ostringstream buffer;
		buffer << source.rdbuf();

		auto parsed = boost::json::parse(buffer.str());

		return parsed.as_object();
	}

	auto write_record(const TemporaryTree& tree, const boost::json::object& root) -> void
	{
		std::ofstream sink(record_path(tree), std::ios::binary | std::ios::trunc);
		sink << boost::json::serialize(root);
	}

	auto deletion_is_enforced(void) -> bool
	{
		static const auto token = std::to_string(std::random_device{}());

		const auto probe = std::filesystem::temp_directory_path() / ("yirang-deploy-perm-probe-" + token);

		std::error_code ignored;
		std::filesystem::remove_all(probe, ignored);
		std::filesystem::create_directories(probe);

		std::ofstream(probe / "locked.txt") << "locked";
		std::filesystem::permissions(probe, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, ignored);

		std::error_code error;
		std::filesystem::remove(probe / "locked.txt", error);

		const bool enforced = static_cast<bool>(error);

		std::filesystem::permissions(probe, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);
		std::filesystem::remove_all(probe, ignored);

		return enforced;
	}

	struct Fixture
	{
		std::shared_ptr<Install::ReleaseInstaller> installer;
		std::shared_ptr<FakeSupervisor> supervisor;
		std::shared_ptr<DeploymentEngine> engine;
	};

	auto make_fixture(const TemporaryTree& tree, const std::string& executable = "app.exe") -> Fixture
	{
		Install::InstallOptions install_options;
		install_options.service_root = tree.service_root();
		install_options.keep_previous_releases = 2;

		ServiceSpec service;
		service.executable = executable;
		service.stop_timeout = std::chrono::seconds(1);
		service.startup_timeout = std::chrono::seconds(2);

		Health::HealthCheckSpec health;
		health.kind = Health::CheckKind::Process;
		health.interval = std::chrono::milliseconds(10);
		health.timeout = std::chrono::milliseconds(50);
		health.success_threshold = 1;
		health.failure_threshold = 2;

		Fixture fixture;
		fixture.installer = std::make_shared<Install::ReleaseInstaller>(install_options);
		fixture.supervisor = std::make_shared<FakeSupervisor>();
		fixture.engine = std::make_shared<DeploymentEngine>(fixture.installer, fixture.supervisor, service, health);

		return fixture;
	}
}

TEST(DeploymentEngineTest, ApplyInstallsActivatesAndStartsTheRelease)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	const auto applied = fixture.engine->apply("rel_1", tree.stage("rel_1"));
	ASSERT_TRUE(applied.has_value()) << (applied.has_value() ? "" : applied.error());

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1");

	ASSERT_EQ(fixture.supervisor->started().size(), 1u);
	EXPECT_NE(fixture.supervisor->started().at(0).find("releases/rel_1/app.exe"), std::string::npos) << fixture.supervisor->started().at(0);
	EXPECT_TRUE(fixture.engine->running());
	EXPECT_NE(fixture.engine->last_detail().find("rel_1"), std::string::npos);
}

TEST(DeploymentEngineTest, ApplyStopsTheRunningReleaseBeforeSwitching)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(fixture.engine->apply("rel_2", tree.stage("rel_2")).has_value());

	EXPECT_EQ(fixture.supervisor->stopped().size(), 1u);
	ASSERT_EQ(fixture.supervisor->started().size(), 2u);

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_2");
	EXPECT_EQ(state.value().previous, "rel_1");
}

TEST(DeploymentEngineTest, ApplyRollsBackWhenTheNewReleaseIsNotReady)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	fixture.supervisor->exit_immediately();

	const auto applied = fixture.engine->apply("rel_2", tree.stage("rel_2"));
	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("rolled back to 'rel_1'"), std::string::npos) << applied.error();

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1");
}

TEST(DeploymentEngineTest, ApplyReportsWhenThereIsNoRollbackTarget)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);
	fixture.supervisor->exit_immediately();

	const auto applied = fixture.engine->apply("rel_1", tree.stage("rel_1"));

	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("no rollback target"), std::string::npos) << applied.error();
}

TEST(DeploymentEngineTest, ApplyRollsBackWhenTheProcessDoesNotStart)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	fixture.supervisor->fail_start();

	const auto applied = fixture.engine->apply("rel_2", tree.stage("rel_2"));

	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("cannot start"), std::string::npos) << applied.error();
}

TEST(DeploymentEngineTest, ApplyLeavesTheRunningServiceAloneWhenInstallFails)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	const auto stops_before = fixture.supervisor->stopped().size();

	const auto applied = fixture.engine->apply("rel_absent", (std::filesystem::temp_directory_path() / "yirang-absent-source").string());

	ASSERT_FALSE(applied.has_value());
	EXPECT_EQ(fixture.supervisor->stopped().size(), stops_before) << "설치 실패인데 서비스를 내렸다";

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1");
}

TEST(DeploymentEngineTest, ApplyFailsWhenTheExecutableIsMissingFromTheRelease)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree, "missing.exe");

	const auto applied = fixture.engine->apply("rel_1", tree.stage("rel_1"));

	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("missing.exe"), std::string::npos) << applied.error();
}

TEST(DeploymentEngineTest, ApplyRequiresAConfiguredExecutable)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree, "");

	const auto applied = fixture.engine->apply("rel_1", tree.stage("rel_1"));

	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("service.executable"), std::string::npos);
}

TEST(DeploymentEngineTest, RollbackToSwitchesAndRestarts)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(fixture.engine->apply("rel_2", tree.stage("rel_2")).has_value());

	const auto reverted = fixture.engine->rollback_to("rel_1");
	ASSERT_TRUE(reverted.has_value()) << (reverted.has_value() ? "" : reverted.error());

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1");
	EXPECT_EQ(fixture.supervisor->started().size(), 3u);
}

TEST(DeploymentEngineTest, RollbackToRefusesAReleaseThatIsNotInstalled)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	const auto reverted = fixture.engine->rollback_to("rel_absent");

	ASSERT_FALSE(reverted.has_value());
	EXPECT_NE(reverted.error().find("not installed"), std::string::npos);
}

TEST(DeploymentEngineTest, StartActiveRelaunchesFromRecordedState)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto restarted = make_fixture(tree);
	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);
	EXPECT_NE(restarted.supervisor->started().at(0).find("releases/rel_1/app.exe"), std::string::npos);
}

TEST(DeploymentEngineTest, StartActiveFailsWhenNothingIsActive)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	const auto resumed = fixture.engine->start_active();

	ASSERT_FALSE(resumed.has_value());
	EXPECT_NE(resumed.error().find("no active release"), std::string::npos);
}

TEST(DeploymentEngineTest, ApplyAbortsWhenTheRunningServiceWillNotStop)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	fixture.supervisor->fail_stop();

	const auto applied = fixture.engine->apply("rel_2", tree.stage("rel_2"));

	ASSERT_FALSE(applied.has_value());
	EXPECT_NE(applied.error().find("cannot stop"), std::string::npos) << applied.error();

	auto state = fixture.installer->state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1") << "중단 실패인데 포인터를 교체했다";
}

TEST(DeploymentEngineTest, ApplyMakesTheExecutableRunnable)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	const auto source = tree.stage("rel_1");

	std::error_code error;
	std::filesystem::permissions(std::filesystem::path(source) / "app.exe",
								 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read
									 | std::filesystem::perms::others_read,
								 std::filesystem::perm_options::replace, error);
	ASSERT_FALSE(error) << error.message();

	const auto applied = fixture.engine->apply("rel_1", source);
	ASSERT_TRUE(applied.has_value()) << (applied.has_value() ? "" : applied.error());

	const auto installed = std::filesystem::path(fixture.installer->release_directory("rel_1")) / "app.exe";
	const auto mode = std::filesystem::status(installed, error).permissions();

	ASSERT_FALSE(error) << error.message();
	EXPECT_NE((mode & std::filesystem::perms::owner_exec), std::filesystem::perms::none) << "설치본에 실행 권한이 없다";
}

TEST(DeploymentEngineTest, ApplyRecordsTheRunningInstance)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(std::filesystem::exists(record_path(tree)));

	const auto recorded = read_record(tree);

	EXPECT_EQ(recorded.at("release_id").as_string(), "rel_1");
	EXPECT_EQ(recorded.at("process_id").as_int64(), 1);
	EXPECT_EQ(recorded.at("start_token").as_int64(), 1001);
	EXPECT_FALSE(recorded.at("boot_identity").as_string().empty());
}

TEST(DeploymentEngineTest, StopRemovesTheRecord)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(fixture.engine->stop().has_value());

	EXPECT_FALSE(std::filesystem::exists(record_path(tree)));
}

TEST(DeploymentEngineTest, StartActiveAdoptsTheRecordedInstanceInsteadOfStartingASecond)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 1001);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	EXPECT_TRUE(restarted.supervisor->started().empty());
	EXPECT_EQ(restarted.supervisor->adopted(), std::vector<int64_t>{ 1 });
	EXPECT_TRUE(restarted.engine->running());
	EXPECT_NE(restarted.engine->last_detail().find("adopted"), std::string::npos) << restarted.engine->last_detail();
}

TEST(DeploymentEngineTest, StartActiveLaunchesWhenTheRecordedInstanceIsGone)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto restarted = make_fixture(tree);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);
	EXPECT_EQ(restarted.supervisor->adopted(), std::vector<int64_t>{ 1 });
	EXPECT_TRUE(restarted.supervisor->stopped().empty());
}

TEST(DeploymentEngineTest, StartActiveRefusesToAdoptWhenTheStartTokenDoesNotMatch)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 4242);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	EXPECT_TRUE(restarted.supervisor->stopped().empty());
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);

	const auto recorded = read_record(tree);
	EXPECT_EQ(recorded.at("process_id").as_int64(), 2);
}

TEST(DeploymentEngineTest, StartActiveRefusesToAdoptARecordFromAnEarlierBoot)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto stale = read_record(tree);
	stale["boot_identity"] = "0.000000";
	write_record(tree, stale);

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 1001);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	EXPECT_TRUE(restarted.supervisor->adopted().empty());
	EXPECT_TRUE(restarted.supervisor->stopped().empty());
	EXPECT_EQ(restarted.supervisor->started().size(), 1u);
}

TEST(DeploymentEngineTest, StartActiveStopsAnAdoptedInstanceOfAnotherRelease)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(fixture.installer->install("rel_2", tree.stage("rel_2")).has_value());
	ASSERT_TRUE(fixture.installer->activate("rel_2").has_value());

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 1001);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	EXPECT_EQ(restarted.supervisor->adopted(), std::vector<int64_t>{ 1 });
	EXPECT_EQ(restarted.supervisor->stopped(), std::vector<int64_t>{ 1 });
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);
	EXPECT_NE(restarted.supervisor->started().at(0).find("releases/rel_2/app.exe"), std::string::npos);

	const auto recorded = read_record(tree);
	EXPECT_EQ(recorded.at("release_id").as_string(), "rel_2");
}

TEST(DeploymentEngineTest, ApplyStopsTheAdoptedInstance)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 1001);
	ASSERT_TRUE(restarted.engine->start_active().has_value());

	const auto applied = restarted.engine->apply("rel_2", tree.stage("rel_2"));

	ASSERT_TRUE(applied.has_value()) << (applied.has_value() ? "" : applied.error());
	EXPECT_EQ(restarted.supervisor->stopped(), std::vector<int64_t>{ 1 });
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);
}

TEST(DeploymentEngineTest, StartActiveIgnoresACorruptedRecordAndStartsTheActiveRelease)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	{
		std::ofstream sink(record_path(tree), std::ios::binary | std::ios::trunc);
		sink << R"({"release_id":"rel_1","process_)";
	}

	auto restarted = make_fixture(tree);
	restarted.supervisor->already_running(1, 1001);

	const auto resumed = restarted.engine->start_active();

	ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error());
	EXPECT_TRUE(restarted.supervisor->adopted().empty());
	ASSERT_EQ(restarted.supervisor->started().size(), 1u);
	EXPECT_EQ(read_record(tree).at("process_id").as_int64(), 2);
}

TEST(DeploymentEngineTest, ApplyKeepsTheRunningInstanceWhenStopFailsAndDoesNotStartASecond)
{
	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.engine->apply("rel_1", tree.stage("rel_1")).has_value());

	fixture.supervisor->fail_stop();

	ASSERT_FALSE(fixture.engine->apply("rel_2", tree.stage("rel_2")).has_value());
	ASSERT_FALSE(fixture.engine->apply("rel_3", tree.stage("rel_3")).has_value());

	EXPECT_EQ(fixture.supervisor->started().size(), 1u) << "중단이 실패했는데 새 인스턴스를 띄웠다";
	EXPECT_EQ(read_record(tree).at("process_id").as_int64(), 1);
}

TEST(DeploymentEngineTest, LaunchReportsWhenTheInstanceCannotBeRecorded)
{
	if (!deletion_is_enforced())
	{
		GTEST_SKIP() << "this filesystem does not enforce directory write permission";
	}

	TemporaryTree tree;
	auto fixture = make_fixture(tree);

	ASSERT_TRUE(fixture.installer->install("rel_1", tree.stage("rel_1")).has_value());
	ASSERT_TRUE(fixture.installer->activate("rel_1").has_value());

	std::error_code ignored;
	std::filesystem::permissions(tree.service_root(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, ignored);

	const auto resumed = fixture.engine->start_active();

	std::filesystem::permissions(tree.service_root(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);

	ASSERT_FALSE(resumed.has_value());
	EXPECT_NE(resumed.error().find("cannot record"), std::string::npos) << resumed.error();
	EXPECT_EQ(fixture.supervisor->started().size(), 1u);
}
