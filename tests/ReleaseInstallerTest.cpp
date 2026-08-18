#include <gtest/gtest.h>

#include "ReleaseInstaller.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

using namespace Install;

namespace
{
	class TemporaryTree
	{
	public:
		TemporaryTree(void)
		{
			static int sequence = 0;
			static const auto token = std::to_string(std::random_device{}());

			root_ = std::filesystem::temp_directory_path() / ("yirang-install-test-" + token + "-" + std::to_string(++sequence));
			std::filesystem::create_directories(root_ / "cache");
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

		auto stage(const std::string& release_id, const std::string& relative, const std::string& content) const -> std::string
		{
			const auto path = root_ / "cache" / release_id / relative;
			std::filesystem::create_directories(path.parent_path());

			std::ofstream stream(path, std::ios::binary);
			stream << content;
			stream.close();

			return (root_ / "cache" / release_id).string();
		}

		auto cache_directory(const std::string& release_id) const -> std::string { return (root_ / "cache" / release_id).string(); }

	private:
		std::filesystem::path root_;
	};

	auto make_installer(const TemporaryTree& tree, int keep = 2) -> ReleaseInstaller
	{
		InstallOptions options;
		options.service_root = tree.service_root();
		options.keep_previous_releases = keep;

		return ReleaseInstaller(options);
	}

	auto read_file(const std::string& path) -> std::string
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	auto retired_directory(const ReleaseInstaller& installer, const std::string& release_id) -> std::filesystem::path
	{
		return std::filesystem::path(installer.release_directory(release_id)).parent_path() / (".retired-" + release_id);
	}

	auto deletion_is_enforced(void) -> bool
	{
		static const auto token = std::to_string(std::random_device{}());

		const auto probe = std::filesystem::temp_directory_path() / ("yirang-install-perm-probe-" + token);

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
}

TEST(ReleaseInstallerTest, InstallPlacesEveryFileUnderTheReleaseDirectory)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "binary");
	tree.stage("rel_1", "bin/helper.dll", "helper");
	auto installer = make_installer(tree);

	const auto installed = installer.install("rel_1", tree.cache_directory("rel_1"));
	ASSERT_TRUE(installed.has_value()) << (installed.has_value() ? "" : installed.error());

	const auto target = std::filesystem::path(installer.release_directory("rel_1"));
	EXPECT_EQ(read_file((target / "app.exe").string()), "binary");
	EXPECT_EQ(read_file((target / "bin" / "helper.dll").string()), "helper");
}

TEST(ReleaseInstallerTest, InstallLeavesNothingBehindWhenTheSourceIsMissing)
{
	TemporaryTree tree;
	auto installer = make_installer(tree);

	const auto installed = installer.install("rel_absent", tree.cache_directory("rel_absent"));

	ASSERT_FALSE(installed.has_value());
	EXPECT_FALSE(std::filesystem::exists(installer.release_directory("rel_absent")));

	for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(tree.service_root())))
	{
		EXPECT_EQ(entry.path().filename().string().rfind(".staging-", 0), std::string::npos) << entry.path().string();
	}
}

TEST(ReleaseInstallerTest, InstallRejectsPathReferenceReleaseId)
{
	TemporaryTree tree;
	auto installer = make_installer(tree);

	for (const auto& release_id : { "", ".", "..", "a/b", "a\\b" })
	{
		const auto installed = installer.install(release_id, tree.cache_directory("rel_1"));

		ASSERT_FALSE(installed.has_value()) << release_id;
	}
}

TEST(ReleaseInstallerTest, InstallRejectsAnEmptyRelease)
{
	TemporaryTree tree;
	std::filesystem::create_directories(tree.cache_directory("rel_empty"));
	auto installer = make_installer(tree);

	const auto installed = installer.install("rel_empty", tree.cache_directory("rel_empty"));

	ASSERT_FALSE(installed.has_value());
	EXPECT_NE(installed.error().find("no files"), std::string::npos);
}

TEST(ReleaseInstallerTest, ReinstallReplacesTheReleaseContent)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "first");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	tree.stage("rel_1", "app.exe", "second");
	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	EXPECT_EQ(read_file((std::filesystem::path(installer.release_directory("rel_1")) / "app.exe").string()), "second");
}

TEST(ReleaseInstallerTest, ActivateRecordsTheActiveAndPreviousRelease)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	tree.stage("rel_2", "app.exe", "two");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	ASSERT_TRUE(installer.install("rel_2", tree.cache_directory("rel_2")).has_value());

	ASSERT_TRUE(installer.activate("rel_1").has_value());
	auto first = installer.state();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first.value().active, "rel_1");
	EXPECT_TRUE(first.value().previous.empty());
	EXPECT_FALSE(first.value().updated_at.empty());

	ASSERT_TRUE(installer.activate("rel_2").has_value());
	auto second = installer.state();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second.value().active, "rel_2");
	EXPECT_EQ(second.value().previous, "rel_1");
}

TEST(ReleaseInstallerTest, ReactivatingTheSameReleaseKeepsTheRollbackTarget)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	tree.stage("rel_2", "app.exe", "two");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	ASSERT_TRUE(installer.install("rel_2", tree.cache_directory("rel_2")).has_value());
	ASSERT_TRUE(installer.activate("rel_1").has_value());
	ASSERT_TRUE(installer.activate("rel_2").has_value());
	ASSERT_TRUE(installer.activate("rel_2").has_value());

	auto state = installer.state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_2");
	EXPECT_EQ(state.value().previous, "rel_1");
}

TEST(ReleaseInstallerTest, ActivateRefusesAReleaseThatIsNotInstalled)
{
	TemporaryTree tree;
	auto installer = make_installer(tree);

	const auto activated = installer.activate("rel_absent");

	ASSERT_FALSE(activated.has_value());
	EXPECT_NE(activated.error().find("not installed"), std::string::npos);
}

TEST(ReleaseInstallerTest, RollbackReturnsToThePreviousReleaseAndCanGoBackAgain)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	tree.stage("rel_2", "app.exe", "two");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	ASSERT_TRUE(installer.install("rel_2", tree.cache_directory("rel_2")).has_value());
	ASSERT_TRUE(installer.activate("rel_1").has_value());
	ASSERT_TRUE(installer.activate("rel_2").has_value());

	auto reverted = installer.rollback();
	ASSERT_TRUE(reverted.has_value()) << (reverted.has_value() ? "" : reverted.error());
	EXPECT_EQ(reverted.value(), "rel_1");

	auto state = installer.state();
	ASSERT_TRUE(state.has_value());
	EXPECT_EQ(state.value().active, "rel_1");
	EXPECT_EQ(state.value().previous, "rel_2");

	auto again = installer.rollback();
	ASSERT_TRUE(again.has_value());
	EXPECT_EQ(again.value(), "rel_2");
}

TEST(ReleaseInstallerTest, RollbackFailsWhenThereIsNoPreviousRelease)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	ASSERT_TRUE(installer.activate("rel_1").has_value());

	const auto reverted = installer.rollback();

	ASSERT_FALSE(reverted.has_value());
	EXPECT_NE(reverted.error().find("no previous release"), std::string::npos);
}

TEST(ReleaseInstallerTest, PruneKeepsActiveAndPreviousBeyondTheKeepCount)
{
	TemporaryTree tree;
	auto installer = make_installer(tree, 1);

	for (const auto& release_id : { "rel_1", "rel_2", "rel_3", "rel_4" })
	{
		tree.stage(release_id, "app.exe", release_id);
		ASSERT_TRUE(installer.install(release_id, tree.cache_directory(release_id)).has_value()) << release_id;

		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	ASSERT_TRUE(installer.activate("rel_3").has_value());
	ASSERT_TRUE(installer.activate("rel_4").has_value());

	auto removed = installer.prune();
	ASSERT_TRUE(removed.has_value()) << (removed.has_value() ? "" : removed.error());
	EXPECT_EQ(removed.value(), 1u);

	EXPECT_TRUE(std::filesystem::exists(installer.release_directory("rel_4")));
	EXPECT_TRUE(std::filesystem::exists(installer.release_directory("rel_3")));
	EXPECT_TRUE(std::filesystem::exists(installer.release_directory("rel_2")));
	EXPECT_FALSE(std::filesystem::exists(installer.release_directory("rel_1")));
}

TEST(ReleaseInstallerTest, StateIsEmptyBeforeAnythingIsActivated)
{
	TemporaryTree tree;
	auto installer = make_installer(tree);

	auto state = installer.state();

	ASSERT_TRUE(state.has_value());
	EXPECT_TRUE(state.value().active.empty());
	EXPECT_TRUE(state.value().previous.empty());
}

TEST(ReleaseInstallerTest, InstalledIgnoresStagingDirectories)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	std::filesystem::create_directories(std::filesystem::path(tree.service_root()) / "releases" / ".staging-rel_9");

	auto releases = installer.installed();

	ASSERT_TRUE(releases.has_value());
	ASSERT_EQ(releases.value().size(), 1u);
	EXPECT_EQ(releases.value().at(0), "rel_1");
}

TEST(ReleaseInstallerTest, ReinstallReplacesAReleaseWhoseFilesCannotBeDeleted)
{
	if (!deletion_is_enforced())
	{
		GTEST_SKIP() << "this filesystem does not enforce directory write permission";
	}

	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "first");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	const auto target = std::filesystem::path(installer.release_directory("rel_1"));

	std::error_code ignored;
	std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, ignored);

	tree.stage("rel_1", "app.exe", "second");
	const auto reinstalled = installer.install("rel_1", tree.cache_directory("rel_1"));

	for (const auto& entry : std::filesystem::directory_iterator(target.parent_path()))
	{
		if (entry.path().filename().string().rfind(".retired-", 0) == 0)
		{
			std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);
		}
	}

	ASSERT_TRUE(reinstalled.has_value()) << (reinstalled.has_value() ? "" : reinstalled.error());
	EXPECT_EQ(read_file((target / "app.exe").string()), "second");

	auto releases = installer.installed();
	ASSERT_TRUE(releases.has_value());
	ASSERT_EQ(releases.value().size(), 1u);
	EXPECT_EQ(releases.value().at(0), "rel_1");
}

TEST(ReleaseInstallerTest, ReinstallKeepsTheInstalledReleaseWhenTheSwapCannotStart)
{
	if (!deletion_is_enforced())
	{
		GTEST_SKIP() << "this filesystem does not enforce directory write permission";
	}

	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "first");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	const auto retired = retired_directory(installer, "rel_1");
	std::filesystem::create_directories(retired);
	std::ofstream(retired / "leftover.txt") << "leftover";

	std::error_code ignored;
	std::filesystem::permissions(retired, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, ignored);

	tree.stage("rel_1", "app.exe", "second");
	const auto reinstalled = installer.install("rel_1", tree.cache_directory("rel_1"));

	std::filesystem::permissions(retired, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);

	ASSERT_FALSE(reinstalled.has_value());
	EXPECT_EQ(read_file((std::filesystem::path(installer.release_directory("rel_1")) / "app.exe").string()), "first");

	for (const auto& entry : std::filesystem::directory_iterator(retired.parent_path()))
	{
		EXPECT_NE(entry.path().filename().string().rfind(".staging-", 0), 0u) << entry.path().string();
	}
}

TEST(ReleaseInstallerTest, InstallRecoversTheReleaseSlotLeftBehindByAnInterruptedSwap)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "first");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	const auto target = std::filesystem::path(installer.release_directory("rel_1"));
	const auto retired = retired_directory(installer, "rel_1");

	std::error_code error;
	std::filesystem::rename(target, retired, error);
	ASSERT_FALSE(error) << error.message();
	ASSERT_FALSE(std::filesystem::exists(target));

	tree.stage("rel_1", "app.exe", "second");
	const auto reinstalled = installer.install("rel_1", tree.cache_directory("rel_1"));

	ASSERT_TRUE(reinstalled.has_value()) << (reinstalled.has_value() ? "" : reinstalled.error());
	EXPECT_EQ(read_file((target / "app.exe").string()), "second");
	EXPECT_FALSE(std::filesystem::exists(retired));
}

TEST(ReleaseInstallerTest, ReinstallLeavesNoRetiredDirectoryBehind)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "first");
	auto installer = make_installer(tree);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	tree.stage("rel_1", "app.exe", "second");
	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	EXPECT_FALSE(std::filesystem::exists(retired_directory(installer, "rel_1")));
}

TEST(ReleaseInstallerTest, InstallSweepsRetiredLeftoversFromEarlierSwaps)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	auto installer = make_installer(tree);

	auto leftover = retired_directory(installer, "rel_9");
	leftover += ".12345";
	std::filesystem::create_directories(leftover);
	std::ofstream(leftover / "app.exe") << "stale";

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());

	EXPECT_FALSE(std::filesystem::exists(leftover));
}

TEST(ReleaseInstallerTest, InstalledAndPruneIgnoreRetiredDirectories)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	auto installer = make_installer(tree, 0);

	ASSERT_TRUE(installer.install("rel_1", tree.cache_directory("rel_1")).has_value());
	ASSERT_TRUE(installer.activate("rel_1").has_value());

	const auto retired = retired_directory(installer, "rel_9");
	std::filesystem::create_directories(retired);

	auto releases = installer.installed();
	ASSERT_TRUE(releases.has_value());
	ASSERT_EQ(releases.value().size(), 1u);
	EXPECT_EQ(releases.value().at(0), "rel_1");

	auto removed = installer.prune();
	ASSERT_TRUE(removed.has_value()) << (removed.has_value() ? "" : removed.error());
	EXPECT_EQ(removed.value(), 0u);
	EXPECT_TRUE(std::filesystem::exists(retired));
}

TEST(ReleaseInstallerTest, InstallRejectsInternalDirectoryNamesAsReleaseId)
{
	TemporaryTree tree;
	tree.stage("rel_1", "app.exe", "one");
	auto installer = make_installer(tree);

	for (const auto& release_id : { ".staging-rel_1", ".retired-rel_1" })
	{
		const auto installed = installer.install(release_id, tree.cache_directory("rel_1"));

		ASSERT_FALSE(installed.has_value()) << release_id;
		EXPECT_NE(installed.error().find("internal name"), std::string::npos) << installed.error();
	}
}
