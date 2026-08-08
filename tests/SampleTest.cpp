#include <gtest/gtest.h>

#include <string>

// 빌드·테스트 파이프라인이 동작하는지 확인하는 스켈레톤 테스트.
// 실제 테스트 케이스가 추가되면 이 파일은 삭제한다.
TEST(YirangRolloutTest, BuildPipelineWorks)
{
	const std::string name = "yirang-rollout";

	EXPECT_FALSE(name.empty());
}
