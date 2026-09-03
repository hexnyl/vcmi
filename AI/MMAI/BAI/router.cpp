/*
 * router.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "callback/CBattleCallback.h"
#include "callback/IGameInfoCallback.h"
#include "filesystem/Filesystem.h"
#include "lib/CRandomGenerator.h"
#include "json/JsonUtils.h"

#include "../../BattleAI/BattleAI.h"
#include "../../StupidAI/StupidAI.h"

#include "BAI/factory.h"
#include "BAI/fallback/scripted_model.h"
#include "BAI/router.h"

#include "common.h"

namespace MMAI::BAI
{
// key => {model, path}
using ModelStorage = std::map<std::string, std::pair<std::shared_ptr<MMAI::Schema::IModel>, std::string>>;

namespace
{
	struct ModelRepository
	{
		mutable std::mutex mutex;
		mutable ModelStorage models;
		float temperature = 1.0;
		uint64_t seed = 0;
		std::map<std::string, std::string> paths;
		std::shared_ptr<ScriptedModel> fallbackModel;
		std::string fallbackName;
	};

	std::shared_ptr<ModelRepository> InitModelRepository()
	{
		auto repo = std::make_unique<ModelRepository>();
		auto json = JsonUtils::assembleFromFiles("MMAI/CONFIG/mmai-settings.json");
		std::string fallback;

		if(json.isStruct())
		{
			JsonUtils::validate(json, "vcmi:mmaiSettings", "mmai");
			repo->temperature = static_cast<float>(json["temperature"].Float());

			repo->seed = json["seed"].Integer();
			if(repo->seed == 0)
				repo->seed = CRandomGenerator::getDefault().nextInt();

			for(const auto & [key, node] : json["models"].Struct())
				repo->paths.try_emplace(key, "MMAI/models/" + node.String());

			fallback = json["fallback"].isNull() ? "BattleAI" : json["fallback"].String();
		}
		else
		{
			logAi->error("Could not load MMAI config. Is MMAI mod enabled?");
			fallback = "BattleAI";
		}

		logAi->debug("MMAI: repo initialized with fallback: %s", fallback);
		repo->fallbackModel = std::make_unique<ScriptedModel>(fallback);
		repo->fallbackName = fallback;

		return repo;
	}

	const ModelRepository * GetModelRepository()
	{
		static const auto repo = InitModelRepository();
		return repo.get();
	}

	Schema::IModel * GetModel(const std::string & key)
	{
		const auto * repo = GetModelRepository();
		auto lock = std::lock_guard<std::mutex>(repo->mutex);

		std::shared_ptr<Schema::IModel> model = nullptr;

		// Search for "foo.bar.baz" -> "foo.bar" -> "foo"
		auto currentKey = key;
		auto keysToAdd = std::vector<std::string>{};

		while(true)
		{
			logAi->debug("MMAI: trying %s", currentKey);
			auto it = repo->models.find(currentKey);

			if(it != repo->models.end())
			{
				model = it->second.first;
				logAi->debug("MMAI: cache hit: %s (%s)", currentKey, it->second.second);
				break;
			}

			logAi->debug("MMAI: cache miss: %s", currentKey);
			keysToAdd.push_back(currentKey);

			auto itPath = repo->paths.find(currentKey);
			if(itPath != repo->paths.end())
			{
				const auto & path = itPath->second;
				logAi->debug("MMAI: Loading %s model from: %s", currentKey, path);
				try
				{
					model = CreateNNModel(path, repo->temperature, repo->seed);
					for(const auto & k : keysToAdd)
					{
						logAi->debug("MMAI: cache write: %s (%s)", k, path);
						repo->models.try_emplace(k, model, path);
					}
					break;
				}
				catch(std::exception & e)
				{
					logAi->error("MMAI: load error: %s: %s", currentKey, std::string(e.what()));
				}
			}
			else
			{
				logAi->warn("MMAI: load error: %s: no path configured", currentKey);
			}

			// Try next key (if any)
			auto pos = currentKey.rfind('.');
			if(pos == std::string::npos)
				break;

			currentKey = currentKey.substr(0, pos);
		}

		if(!model)
		{
			logAi->error("MMAI: %s: falling back to %s", key, repo->fallbackName);
			ASSERT(repo->fallbackModel, "fallback error: model is null");
			model = repo->fallbackModel;
		}

		return model.get();
	}

	// Convert a memory address for logging purposes
	std::string MakeAddrStr(const void * p)
	{
		std::ostringstream oss;
		oss << p;
		return oss.str();
	}

}

#define MMAI_LOG_TAG LogTag _(basetag + "." + __func__)

Router::Router() : addrstr(MakeAddrStr(this)), basetag(addrstr + ":MMAI") {}

Router::~Router()
{
	if(cb)
		cb->waitTillRealize = wasWaitingForRealize;
}

#ifdef ENABLE_MMAI_TEST
void Router::setTestBattleAIFactory(TestBattleAIFactory factory)
{
	testBattleAIFactory = std::move(factory);
}

size_t Router::activeBattleCount() const
{
	return battles.size();
}
#endif

void Router::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB)
{
	env = ENV;
	cb = CB;
	colorname = cb->getPlayerID()->toString();
	wasWaitingForRealize = cb->waitTillRealize;

	cb->waitTillRealize = false;
	battles.clear();
}

void Router::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB, AutocombatPreferences prefs)
{
	MMAI_LOG_TAG;
	autocombatPreferences = prefs;
	initBattleInterface(ENV, CB);
}

const Router::BattleContext * Router::findBattle(const BattleID & bid) const
{
	const auto it = battles.find(bid);
	if(it == battles.end())
		return nullptr;
	return &it->second;
}

void Router::logMissingBattle(const BattleID & bid, const char * func) const
{
	logAi->error("MMAI Router: no BAI for BattleID %d in %s, active battles: [%s]", bid.getNum(), func, formatActiveBattleIds());
}

std::string Router::battleLogTag(const BattleContext & ctx) const
{
	return ctx.logtag.empty() ? basetag : ctx.logtag;
}

template<typename Fn>
void Router::withBattle(const BattleID & bid, const char * func, Fn && fn)
{
	const auto * ctx = findBattle(bid);
	if(!ctx || !ctx->bai)
	{
		logMissingBattle(bid, func);
		return;
	}

	LogTag _(battleLogTag(*ctx) + "." + func);
	logAi->trace("MMAI Router: %s BattleID %d -> BAI %s", func, bid.getNum(), MakeAddrStr(ctx->bai.get()));
	fn(*ctx->bai);
}

std::string Router::formatActiveBattleIds() const
{
	std::string result;
	for(const auto & [battleId, context] : battles)
	{
		if(!result.empty())
			result += ", ";
		result += std::to_string(battleId.getNum());
		if(context.bai)
			result += "@" + MakeAddrStr(context.bai.get());
	}
	if(result.empty())
		return "none";
	return result;
}

Router::BattleContext Router::createDelegatedBAI(const BattleID & bid, BattleSide side)
{
	std::string modelkey = side == BattleSide::ATTACKER ? "attacker" : "defender";

	if(cb->getBattle(bid)->battleGetWallState(EWallPart::GATE) != EWallState::NONE)
		modelkey += ".siege";

	Schema::IModel * model = GetModel(modelkey);

	BattleContext ctx;
	ctx.logtag = basetag + ".v" + std::to_string(model->getVersion());

	auto modelside = model->getSide();
	auto realside = static_cast<Schema::Side>(EI(side));

	if(modelside != realside && modelside != Schema::Side::BOTH)
		logAi->warn("The loaded '%s' model was not trained to play as %s", modelkey, modelkey);

	switch(model->getType())
	{
		case Schema::ModelType::SCRIPTED:
			if(model->getName() == "StupidAI")
			{
				ctx.bai = std::make_shared<CStupidAI>();
				ctx.bai->initBattleInterface(env, cb, autocombatPreferences);
			}
			else if(model->getName() == "BattleAI")
			{
				ctx.bai = std::make_shared<CBattleAI>();
				ctx.bai->initBattleInterface(env, cb, autocombatPreferences);
			}
			else
			{
				THROW_FORMAT("Unexpected scripted model name: %s", model->getName());
			}
			break;
		case Schema::ModelType::NN:
			// XXX: must not call initBattleInterface here
			ctx.bai = CreateBAI(model, env, cb, autocombatPreferences.enableSpellsUsage);
			break;

		default:
			THROW_FORMAT("Unexpected model type: %d", EI(model->getType()));
	}

	return ctx;
}

/*
 * Delegated methods
 */

void Router::actionFinished(const BattleID & bid, const BattleAction & action)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.actionFinished(bid, action);
	});
}

void Router::actionStarted(const BattleID & bid, const BattleAction & action)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.actionStarted(bid, action);
	});
}

void Router::activeStack(const BattleID & bid, const CStack * astack)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.activeStack(bid, astack);
	});
}

void Router::battleAttack(const BattleID & bid, const BattleAttack * ba)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleAttack(bid, ba);
	});
}

void Router::battleCatapultAttacked(const BattleID & bid, const CatapultAttack & ca)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleCatapultAttacked(bid, ca);
	});
}

void Router::battleEnd(const BattleID & bid, const BattleResult * br, QueryID queryID)
{
	const auto it = battles.find(bid);
	if(it == battles.end() || !it->second.bai)
	{
		logMissingBattle(bid, __func__);
		return;
	}

	LogTag _(battleLogTag(it->second) + "." + __func__);
	logAi->debug("MMAI Router: battleEnd bid=%d -> BAI %s", bid.getNum(), MakeAddrStr(it->second.bai.get()));
	auto bai = it->second.bai;
	battles.erase(it);
	bai->battleEnd(bid, br, queryID);
	logAi->debug("MMAI Router: remaining battles: [%s]", formatActiveBattleIds());
}

void Router::battleGateStateChanged(const BattleID & bid, const EGateState state)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleGateStateChanged(bid, state);
	});
};

void Router::battleLogMessage(const BattleID & bid, const std::vector<MetaString> & lines)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleLogMessage(bid, lines);
	});
};

void Router::battleNewRound(const BattleID & bid)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleNewRound(bid);
	});
}

void Router::battleNewRoundFirst(const BattleID & bid)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleNewRoundFirst(bid);
	});
}

void Router::battleObstaclesChanged(const BattleID & bid, const ObstacleChanges & obstacle)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleObstaclesChanged(bid, obstacle);
	});
};

void Router::battleSpellCast(const BattleID & bid, const BattleSpellCast * sc)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleSpellCast(bid, sc);
	});
}

void Router::battleStackMoved(const BattleID & bid, const CStack * stack, const BattleHexArray & dest, int distance, bool teleport)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleStackMoved(bid, stack, dest, distance, teleport);
	});
}

void Router::battleStacksAttacked(const BattleID & bid, const std::vector<BattleStackAttacked> & bsa, bool ranged)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleStacksAttacked(bid, bsa, ranged);
	});
}

void Router::battleStacksEffectsSet(const BattleID & bid, const SetStackEffect & sse)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleStacksEffectsSet(bid, sse);
	});
}

void Router::battleStart(
	const BattleID & bid,
	const CCreatureSet * army1,
	const CCreatureSet * army2,
	int3 tile,
	const CGHeroInstance * hero1,
	const CGHeroInstance * hero2,
	BattleSide side,
	bool replayAllowed
)
{
	MMAI_LOG_TAG;
	if(battles.contains(bid))
	{
		const auto message = "MMAI Router: duplicate battleStart for BattleID " + std::to_string(bid.getNum()) + ", active battles: [" + formatActiveBattleIds() + "]";
		logAi->error("%s", message);
		throw std::runtime_error(message);
	}

	BattleContext ctx;
#ifdef ENABLE_MMAI_TEST
	if(testBattleAIFactory)
	{
		ctx.bai = testBattleAIFactory(bid, side);
		ctx.logtag = basetag;
	}
	else
		ctx = createDelegatedBAI(bid, side);
#else
	ctx = createDelegatedBAI(bid, side);
#endif

	LogTag _2(battleLogTag(ctx) + "." + __func__);
	logAi->debug("MMAI Router: battleStart bid=%d -> BAI %s", bid.getNum(), MakeAddrStr(ctx.bai.get()));
	battles.emplace(bid, ctx);
	ctx.bai->battleStart(bid, army1, army2, tile, hero1, hero2, side, replayAllowed);
}

void Router::battleTriggerEffect(const BattleID & bid, const BattleTriggerEffect & bte)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleTriggerEffect(bid, bte);
	});
}

void Router::battleUnitsChanged(const BattleID & bid, const std::vector<UnitChanges> & changes)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.battleUnitsChanged(bid, changes);
	});
}

void Router::yourTacticPhase(const BattleID & bid, int distance)
{
	withBattle(bid, __func__, [&](CBattleGameInterface & ai)
	{
		ai.yourTacticPhase(bid, distance);
	});
}
}
