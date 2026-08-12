#include <gtest/gtest.h>

#include "HealthChecker.h"
#include "NetworkProbe.h"
#include "PosixProcessSupervisor.h"
#include "ProcessProbe.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace Health;
using namespace Process;

namespace
{
	class ScriptedProbe : public IHealthProbe
	{
	public:
		explicit ScriptedProbe(std::vector<bool> results) : results_(std::move(results)) {}

		auto probe(void) -> std::expected<void, std::string> override
		{
			if (index_ >= results_.size())
			{
				return std::unexpected("scripted probe exhausted");
			}

			const bool succeed = results_[index_++];

			return succeed ? std::expected<void, std::string>{} : std::unexpected("scripted failure");
		}

	private:
		std::vector<bool> results_;
		size_t index_{ 0 };
	};

	class LocalServer
	{
	public:
		explicit LocalServer(std::optional<std::string> http_response = std::nullopt)
		{
			listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

			int reuse = 1;
			::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;

			::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			::listen(listen_fd_, 8);

			sockaddr_in bound{};
			socklen_t length = sizeof(bound);
			::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length);
			port_ = ntohs(bound.sin_port);

			if (http_response != std::nullopt)
			{
				worker_ = std::thread(&LocalServer::serve, this, http_response.value());
			}
		}

		~LocalServer(void) { close(); }

		auto port(void) const -> uint16_t { return port_; }

		auto close(void) -> void
		{
			if (listen_fd_ < 0)
			{
				return;
			}

			running_ = false;
			::shutdown(listen_fd_, SHUT_RDWR);
			::close(listen_fd_);
			listen_fd_ = -1;

			if (worker_.joinable())
			{
				worker_.join();
			}
		}

	private:
		auto serve(const std::string& response) -> void
		{
			while (running_)
			{
				pollfd descriptor{ listen_fd_, POLLIN, 0 };
				if (::poll(&descriptor, 1, 50) <= 0)
				{
					continue;
				}

				const int client = ::accept(listen_fd_, nullptr, nullptr);
				if (client < 0)
				{
					continue;
				}

				char discard[1024] = {};
				(void)::recv(client, discard, sizeof(discard), 0);
				(void)::send(client, response.data(), response.size(), 0);

				::close(client);
			}
		}

		int listen_fd_{ -1 };
		uint16_t port_{ 0 };
		std::atomic<bool> running_{ true };
		std::thread worker_;
	};

	auto http_response(int status, const std::string& reason, const std::string& body) -> std::string
	{
		return std::format("HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", status, reason, body.size(), body);
	}
}

TEST(HealthCheckerTest, HealthyOnlyAfterSuccessThreshold)
{
	HealthCheckSpec spec;
	spec.success_threshold = 3;
	spec.failure_threshold = 2;

	HealthChecker checker(spec, std::make_shared<ScriptedProbe>(std::vector<bool>{ true, true, true }));

	EXPECT_EQ(checker.evaluate(), HealthState::Unknown);
	EXPECT_EQ(checker.evaluate(), HealthState::Unknown);
	EXPECT_EQ(checker.evaluate(), HealthState::Healthy);
	EXPECT_EQ(checker.consecutive_successes(), 3);
}

TEST(HealthCheckerTest, UnhealthyOnlyAfterFailureThreshold)
{
	HealthCheckSpec spec;
	spec.success_threshold = 1;
	spec.failure_threshold = 3;

	HealthChecker checker(spec, std::make_shared<ScriptedProbe>(std::vector<bool>{ false, false, false }));

	EXPECT_EQ(checker.evaluate(), HealthState::Unknown);
	EXPECT_EQ(checker.evaluate(), HealthState::Unknown);
	EXPECT_EQ(checker.evaluate(), HealthState::Unhealthy);
	ASSERT_TRUE(checker.last_error().has_value());
	EXPECT_EQ(checker.last_error().value(), "scripted failure");
}

TEST(HealthCheckerTest, StreaksResetOnOppositeResult)
{
	HealthCheckSpec spec;
	spec.success_threshold = 3;
	spec.failure_threshold = 3;

	HealthChecker checker(spec, std::make_shared<ScriptedProbe>(std::vector<bool>{ true, true, false, true }));

	checker.evaluate();
	checker.evaluate();
	EXPECT_EQ(checker.consecutive_successes(), 2);

	checker.evaluate();
	EXPECT_EQ(checker.consecutive_successes(), 0);
	EXPECT_EQ(checker.consecutive_failures(), 1);

	checker.evaluate();
	EXPECT_EQ(checker.consecutive_successes(), 1);
	EXPECT_EQ(checker.consecutive_failures(), 0);
	EXPECT_EQ(checker.state(), HealthState::Unknown);
}

TEST(HealthCheckerTest, NonPositiveThresholdsAreClampedToOne)
{
	HealthCheckSpec spec;
	spec.success_threshold = 0;
	spec.failure_threshold = -2;

	HealthChecker checker(spec, std::make_shared<ScriptedProbe>(std::vector<bool>{ true }));

	EXPECT_EQ(checker.spec().success_threshold, 1);
	EXPECT_EQ(checker.spec().failure_threshold, 1);
	EXPECT_EQ(checker.evaluate(), HealthState::Healthy);
}

TEST(HealthCheckerTest, ResetClearsAccumulatedState)
{
	HealthCheckSpec spec;
	spec.success_threshold = 1;

	HealthChecker checker(spec, std::make_shared<ScriptedProbe>(std::vector<bool>{ true }));
	checker.evaluate();
	ASSERT_EQ(checker.state(), HealthState::Healthy);

	checker.reset();

	EXPECT_EQ(checker.state(), HealthState::Unknown);
	EXPECT_EQ(checker.consecutive_successes(), 0);
	EXPECT_FALSE(checker.last_error().has_value());
}

TEST(HealthCheckerTest, ProcessProbeFollowsProcessLifetime)
{
	auto supervisor = std::make_shared<PosixProcessSupervisor>();

	const auto handle = supervisor->start({ "/bin/sh", { "-c", "sleep 30" }, "", {} });
	ASSERT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());

	ProcessProbe probe(supervisor, handle.value());
	EXPECT_TRUE(probe.probe().has_value());

	ASSERT_TRUE(supervisor->stop(handle.value(), std::chrono::seconds(2)).has_value());

	const auto after_stop = probe.probe();
	EXPECT_FALSE(after_stop.has_value());
}

TEST(HealthCheckerTest, TcpProbeDetectsOpenAndClosedPort)
{
	HealthCheckSpec spec;
	spec.kind = CheckKind::Tcp;
	spec.timeout = std::chrono::milliseconds(1000);

	LocalServer server;
	spec.port = server.port();

	NetworkProbe open_probe(spec);
	EXPECT_TRUE(open_probe.probe().has_value());

	server.close();

	NetworkProbe closed_probe(spec);
	EXPECT_FALSE(closed_probe.probe().has_value());
}

TEST(HealthCheckerTest, HttpProbeChecksStatusCode)
{
	LocalServer server(http_response(200, "OK", "ready"));

	HealthCheckSpec spec;
	spec.kind = CheckKind::Http;
	spec.port = server.port();
	spec.path = "/healthz";
	spec.expected_status = 200;
	spec.timeout = std::chrono::milliseconds(2000);

	NetworkProbe probe(spec);
	EXPECT_TRUE(probe.probe().has_value());

	spec.expected_status = 204;
	NetworkProbe mismatched(spec);
	const auto result = mismatched.probe();
	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("unexpected status"), std::string::npos);
}

TEST(HealthCheckerTest, HttpProbeChecksBodyPattern)
{
	LocalServer server(http_response(200, "OK", "status: ready"));

	HealthCheckSpec spec;
	spec.kind = CheckKind::Http;
	spec.port = server.port();
	spec.timeout = std::chrono::milliseconds(2000);

	spec.expected_body = "ready";
	NetworkProbe matching(spec);
	EXPECT_TRUE(matching.probe().has_value());

	spec.expected_body = "degraded";
	NetworkProbe mismatched(spec);
	EXPECT_FALSE(mismatched.probe().has_value());
}

TEST(HealthCheckerTest, NetworkProbeRequiresPort)
{
	HealthCheckSpec spec;
	spec.kind = CheckKind::Tcp;
	spec.port = 0;

	NetworkProbe probe(spec);
	const auto result = probe.probe();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error().find("port is required"), std::string::npos);
}
