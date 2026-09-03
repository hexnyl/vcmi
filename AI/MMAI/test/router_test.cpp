/*
 * router_test.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "BAI/router.h"
#include "networkPacks/PacksForClientBattle.h"
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
class RecordingBattleAI : public CBattleGameInterface
{
public:
	explicit RecordingBattleAI(int instanceId)
		: instanceId(instanceId)
	{
	}

	int getInstanceId() const
	{
		return instanceId;
	}

	const std::vector<std::string> & getEvents() const
	{
		return events;
	}

	void activeStack(const BattleID & bid, const CStack * stack) override
	{
		(void)stack;
		record("activeStack", bid);
	}

	void yourTacticPhase(const BattleID & bid, int distance) override
	{
		(void)distance;
		record("yourTacticPhase", bid);
	}

	void actionFinished(const BattleID & bid, const BattleAction & action) override
	{
		(void)action;
		record("actionFinished", bid);
	}

	void actionStarted(const BattleID & bid, const BattleAction & action) override
	{
		(void)action;
		record("actionStarted", bid);
	}

	void battleStacksAttacked(const BattleID & bid, const std::vector<BattleStackAttacked> & bsa, bool ranged) override
	{
		(void)bsa;
		(void)ranged;
		record("battleStacksAttacked", bid);
	}

	void battleEnd(const BattleID & bid, const BattleResult * br, QueryID queryID) override
	{
		(void)br;
		(void)queryID;
		record("battleEnd", bid);
	}

	void battleStart(
		const BattleID & bid,
		const CCreatureSet * army1,
		const CCreatureSet * army2,
		int3 tile,
		const CGHeroInstance * hero1,
		const CGHeroInstance * hero2,
		BattleSide side,
		bool replayAllowed
	) override
	{
		(void)army1;
		(void)army2;
		(void)tile;
		(void)hero1;
		(void)hero2;
		(void)side;
		(void)replayAllowed;
		record("battleStart", bid);
	}

private:
	int instanceId = 0;
	std::vector<std::string> events;

	void record(const std::string & callback, const BattleID & bid)
	{
		events.push_back(callback + ":" + std::to_string(bid.getNum()) + ":id=" + std::to_string(instanceId));
	}
};

BattleID makeBattleId(int32_t num)
{
	BattleID bid;
	bid.setNum(num);
	return bid;
}

class RouterRoutingTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		instances.clear();
		nextInstanceId = 0;
		router = std::make_unique<MMAI::BAI::Router>();
		router->setTestBattleAIFactory(
			[this](const BattleID & bid, BattleSide side) -> std::shared_ptr<CBattleGameInterface>
			{
				(void)side;
				auto ai = std::make_shared<RecordingBattleAI>(++nextInstanceId);
				instances[bid.getNum()] = ai;
				return ai;
			}
		);
	}

	RecordingBattleAI & ai(int32_t battleNum)
	{
		return *instances.at(battleNum);
	}

	std::unique_ptr<MMAI::BAI::Router> router;
	std::map<int32_t, std::shared_ptr<RecordingBattleAI>> instances;
	int nextInstanceId = 0;
};

TEST_F(RouterRoutingTest, SequentialBattles)
{
	const BattleID battleA = makeBattleId(17);
	const BattleID battleB = makeBattleId(18);

	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->activeStack(battleA, nullptr);
	router->battleStacksAttacked(battleA, {}, false);
	router->battleEnd(battleA, nullptr, QueryID(-1));
	ASSERT_EQ(router->activeBattleCount(), 0u);

	router->battleStart(battleB, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->activeStack(battleB, nullptr);
	router->battleEnd(battleB, nullptr, QueryID(-1));

	ASSERT_NE(ai(17).getInstanceId(), ai(18).getInstanceId());
	EXPECT_EQ(
		ai(17).getEvents(),
		(std::vector<std::string>{"battleStart:17:id=1", "activeStack:17:id=1", "battleStacksAttacked:17:id=1", "battleEnd:17:id=1"})
	);
	EXPECT_EQ(
		ai(18).getEvents(),
		(std::vector<std::string>{"battleStart:18:id=2", "activeStack:18:id=2", "battleEnd:18:id=2"})
	);
	ASSERT_EQ(router->activeBattleCount(), 0u);
}

TEST_F(RouterRoutingTest, ConcurrentBattlesRouteToMatchingInstance)
{
	const BattleID battleA = makeBattleId(17);
	const BattleID battleB = makeBattleId(18);

	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->battleStart(battleB, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	ASSERT_EQ(router->activeBattleCount(), 2u);
	ASSERT_NE(ai(17).getInstanceId(), ai(18).getInstanceId());

	router->activeStack(battleA, nullptr);
	router->battleStacksAttacked(battleB, {}, false);
	router->actionStarted(battleA, BattleAction());

	EXPECT_EQ(
		ai(17).getEvents(),
		(std::vector<std::string>{"battleStart:17:id=1", "activeStack:17:id=1", "actionStarted:17:id=1"})
	);
	EXPECT_EQ(
		ai(18).getEvents(),
		(std::vector<std::string>{"battleStart:18:id=2", "battleStacksAttacked:18:id=2"})
	);
}

TEST_F(RouterRoutingTest, EndFirstBattleLeavesSecondActive)
{
	const BattleID battleA = makeBattleId(17);
	const BattleID battleB = makeBattleId(18);

	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->battleStart(battleB, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);

	router->battleEnd(battleA, nullptr, QueryID(-1));
	ASSERT_EQ(router->activeBattleCount(), 1u);
	router->activeStack(battleB, nullptr);

	EXPECT_EQ(
		ai(17).getEvents(),
		(std::vector<std::string>{"battleStart:17:id=1", "battleEnd:17:id=1"})
	);
	EXPECT_EQ(
		ai(18).getEvents(),
		(std::vector<std::string>{"battleStart:18:id=2", "activeStack:18:id=2"})
	);
}

TEST_F(RouterRoutingTest, EndSecondBattleLeavesFirstActive)
{
	const BattleID battleA = makeBattleId(17);
	const BattleID battleB = makeBattleId(18);

	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->battleStart(battleB, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);

	router->battleEnd(battleB, nullptr, QueryID(-1));
	ASSERT_EQ(router->activeBattleCount(), 1u);
	router->activeStack(battleA, nullptr);

	EXPECT_EQ(
		ai(18).getEvents(),
		(std::vector<std::string>{"battleStart:18:id=2", "battleEnd:18:id=2"})
	);
	EXPECT_EQ(
		ai(17).getEvents(),
		(std::vector<std::string>{"battleStart:17:id=1", "activeStack:17:id=1"})
	);
}

TEST_F(RouterRoutingTest, UnknownBattleIdDoesNotAffectOtherBattles)
{
	const BattleID battleA = makeBattleId(17);
	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	const auto before = ai(17).getEvents();

	EXPECT_NO_THROW(router->activeStack(makeBattleId(999), nullptr));
	EXPECT_NO_THROW(router->battleStacksAttacked(makeBattleId(999), {}, false));
	EXPECT_EQ(ai(17).getEvents(), before);
	ASSERT_EQ(router->activeBattleCount(), 1u);
}

TEST_F(RouterRoutingTest, CallbackAfterEndDoesNotReachOtherBattle)
{
	const BattleID battleA = makeBattleId(17);
	const BattleID battleB = makeBattleId(18);

	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->battleStart(battleB, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	router->battleEnd(battleA, nullptr, QueryID(-1));

	EXPECT_NO_THROW(router->activeStack(battleA, nullptr));
	EXPECT_NO_THROW(router->battleEnd(battleA, nullptr, QueryID(-1)));
	EXPECT_EQ(
		ai(17).getEvents(),
		(std::vector<std::string>{"battleStart:17:id=1", "battleEnd:17:id=1"})
	);
	EXPECT_EQ(ai(18).getEvents(), (std::vector<std::string>{"battleStart:18:id=2"}));

	router->activeStack(battleB, nullptr);
	EXPECT_EQ(
		ai(18).getEvents(),
		(std::vector<std::string>{"battleStart:18:id=2", "activeStack:18:id=2"})
	);
	router->battleEnd(battleB, nullptr, QueryID(-1));
	ASSERT_EQ(router->activeBattleCount(), 0u);
}

TEST_F(RouterRoutingTest, DuplicateBattleStartFailsClearly)
{
	const BattleID battleA = makeBattleId(17);
	router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false);
	EXPECT_THROW(
		router->battleStart(battleA, nullptr, nullptr, int3(), nullptr, nullptr, BattleSide::DEFENDER, false),
		std::runtime_error
	);
	ASSERT_EQ(router->activeBattleCount(), 1u);
	EXPECT_EQ(ai(17).getEvents(), (std::vector<std::string>{"battleStart:17:id=1"}));
}
}
