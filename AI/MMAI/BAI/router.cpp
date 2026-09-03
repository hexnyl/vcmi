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
#include "CRandomGenerator.h"
#include "callback/CBattleCallback.h"
#include "callback/CDynLibHandler.h"
#include "callback/IGameInfoCallback.h"
#include "filesystem/Filesystem.h"
#include "json/JsonUtils.h"

#include "BAI/base.h"
#include "BAI/model/NNModel.h"
#include "BAI/model/NNModelStochastic.h"
#include "BAI/model/ScriptedModel.h"
#include "BAI/router.h"

#include "common.h"

#include <utility>

namespace MMAI::BAI
{
using ModelStorage = std::map<std::string, std::unique_ptr<Schema::IModel>>;

namespace
{
	struct ModelRepository
	{
		ModelStorage models;
		float temperature = 1.0;
		uint64_t seed = 0;
		std::unique_ptr<ScriptedModel> fallbackModel;
		std::string fallbackName;
	};

	std::unique_ptr<ModelRepository> InitModelRepository()
	{
		auto repo = std::make_unique<ModelRepository>();
		auto json = JsonUtils::assembleFromFiles("MMAI/CONFIG/mmai-settings.json");
		if(!json.isStruct())
		{
			logAi->error("Could not load MMAI config. Is MMAI mod enabled?");
			std::string fallback = "BattleAI";
			logAi->debug("MMAI: preparing fallback model: %s", fallback);
			repo->fallbackModel = std::make_unique<ScriptedModel>(fallback);
			repo->fallbackName = fallback;
			return repo;
		}

		JsonUtils::validate(json, "vcmi:mmaiSettings", "mmai");
		repo->temperature = static_cast<float>(json["temperature"].Float());

		repo->seed = json["seed"].Integer();
		if(repo->seed == 0)
			repo->seed = CRandomGenerator::getDefault().nextInt();

		for(const std::string key : {"attacker", "defender"})
		{
			std::string path = "MMAI/models/" + json["models"][key].String();

			// Try loading stochastic and dynamic models with priority
			// (temporary code for a smooth migration path)
			std::string suffix;
			const auto pos = path.rfind(".onnx");
			if(pos != std::string::npos)
			{
				for(const std::string s : {"stochastic", "dynamic"})
				{
					std::string altpath = path;
					altpath.insert(pos, "-" + s); // insert right before ".onnx"
					const auto rpath = ResourcePath(altpath, EResType::AI_MODEL);
					const auto * rhandler = CResourceHandler::get();
					if(rhandler->existsResource(rpath))
					{
						path = altpath;
						suffix = s;
						break;
					}
				}
			}

			logAi->debug("MMAI: Loading NN %s model from: %s", key, path);
			try
			{
				// Only stochastic models use a separate class
				if(suffix == "stochastic")
					repo->models.try_emplace(key, std::make_unique<NNModelStochastic>(path, repo->temperature, repo->seed));
				else
					repo->models.try_emplace(key, std::make_unique<NNModel>(path, repo->temperature, repo->seed));
			}
			catch(std::exception & e)
			{
				logAi->error("MMAI: error loading " + key + ": " + std::string(e.what()));
			}
		}

		auto fallback = json["fallback"].isNull() ? "BattleAI" : json["fallback"].String();
		logAi->debug("MMAI: preparing fallback model: %s", fallback);
		repo->fallbackModel = std::make_unique<ScriptedModel>(fallback);
		repo->fallbackName = fallback;

		return repo;
	}

	Schema::IModel * GetModel(const std::string & key)
	{
		static const auto MODEL_REPO = InitModelRepository();
		auto it = MODEL_REPO->models.find(key);
		if(it == MODEL_REPO->models.end())
		{
			logAi->error("MMAI: no %s model loaded, trying fallback: %s", key, MODEL_REPO->fallbackName);
			ASSERT(MODEL_REPO->fallbackModel, "fallback failed: model is null");
			return MODEL_REPO->fallbackModel.get();
		}

		return it->second.get();
	}

	std::string MakeAddrStr(const CBattleGameInterface * bai)
	{
		std::ostringstream oss;
		oss << static_cast<const void *>(bai);
		return oss.str();
	}
}

Router::Router()
{
	std::ostringstream oss;
	// Store the memory address and include it in logging
	const auto * ptr = static_cast<const void *>(this);
	oss << ptr;
	addrstr = oss.str();
	info("+++ constructor +++"); // log after addrstr is set
}

Router::~Router()
{
	info("--- destructor ---");
	if(cb)
		cb->waitTillRealize = wasWaitingForRealize;
}

#ifdef ENABLE_MMAI_TEST
void Router::setTestBattleAIFactory(TestBattleAIFactory factory)
{
	testBattleAIFactory = std::move(factory);
}
#endif

void Router::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB)
{
	info("*** initBattleInterface ***");
	env = ENV;
	cb = CB;
	colorname = cb->getPlayerID()->toString();
	wasWaitingForRealize = cb->waitTillRealize;

	cb->waitTillRealize = false;
	battles.clear();
}

void Router::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB, AutocombatPreferences prefs)
{
	autocombatPreferences = prefs;
	initBattleInterface(ENV, CB);
}

CBattleGameInterface & Router::getBattleAI(const BattleID & bid)
{
	const auto it = battles.find(bid);
	if(it == battles.end())
	{
		const auto message = "MMAI Router: no BAI for BattleID " + std::to_string(bid.getNum()) + ", active battles: [" + formatActiveBattleIds() + "]";
		error(message);
		throw std::runtime_error(message);
	}

	if(!it->second.bai)
	{
		const auto message = "MMAI Router: null BAI for BattleID " + std::to_string(bid.getNum());
		error(message);
		throw std::runtime_error(message);
	}

	trace("BattleID " + std::to_string(bid.getNum()) + " -> BAI " + MakeAddrStr(it->second.bai.get()));
	return *it->second.bai;
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

std::shared_ptr<CBattleGameInterface> Router::createDelegatedBAI(BattleSide side)
{
	const std::string modelkey = side == BattleSide::ATTACKER ? "attacker" : "defender";
	Schema::IModel * model = GetModel(modelkey);

	auto modelside = model->getSide();
	auto realside = static_cast<Schema::Side>(EI(side));

	if(modelside != realside && modelside != Schema::Side::BOTH)
		logAi->warn("The loaded '%s' model was not trained to play as %s", modelkey, modelkey);

	switch(model->getType())
	{
		case Schema::ModelType::SCRIPTED:
			if(model->getName() == "StupidAI")
			{
				auto bai = CDynLibHandler::getNewBattleAI("StupidAI");
				bai->initBattleInterface(env, cb, autocombatPreferences);
				return bai;
			}
			if(model->getName() == "BattleAI")
			{
				auto bai = CDynLibHandler::getNewBattleAI("BattleAI");
				bai->initBattleInterface(env, cb, autocombatPreferences);
				return bai;
			}
			THROW_FORMAT("Unexpected scripted model name: %s", model->getName());
		case Schema::ModelType::NN:
			// XXX: must not call initBattleInterface here
			return Base::Create(model, env, cb, autocombatPreferences.enableSpellsUsage);
		default:
			THROW_FORMAT("Unexpected model type: %d", EI(model->getType()));
	}
}

/*
 * Delegated methods
 */

void Router::actionFinished(const BattleID & bid, const BattleAction & action)
{
	getBattleAI(bid).actionFinished(bid, action);
}

void Router::actionStarted(const BattleID & bid, const BattleAction & action)
{
	getBattleAI(bid).actionStarted(bid, action);
}

void Router::activeStack(const BattleID & bid, const CStack * astack)
{
	getBattleAI(bid).activeStack(bid, astack);
}

void Router::battleAttack(const BattleID & bid, const BattleAttack * ba)
{
	getBattleAI(bid).battleAttack(bid, ba);
}

void Router::battleCatapultAttacked(const BattleID & bid, const CatapultAttack & ca)
{
	getBattleAI(bid).battleCatapultAttacked(bid, ca);
}

void Router::battleEnd(const BattleID & bid, const BattleResult * br, QueryID queryID)
{
	const auto it = battles.find(bid);
	if(it == battles.end())
	{
		const auto message = "MMAI Router: battleEnd for unknown BattleID " + std::to_string(bid.getNum()) + ", active battles: [" + formatActiveBattleIds() + "]";
		error(message);
		throw std::runtime_error(message);
	}

	debug("battleEnd bid=" + std::to_string(bid.getNum()) + " -> BAI " + MakeAddrStr(it->second.bai.get()));
	auto bai = it->second.bai;
	battles.erase(it);
	bai->battleEnd(bid, br, queryID);
	debug("remaining battles: [" + formatActiveBattleIds() + "]");
}

void Router::battleGateStateChanged(const BattleID & bid, const EGateState state)
{
	getBattleAI(bid).battleGateStateChanged(bid, state);
};

void Router::battleLogMessage(const BattleID & bid, const std::vector<MetaString> & lines)
{
	getBattleAI(bid).battleLogMessage(bid, lines);
};

void Router::battleNewRound(const BattleID & bid)
{
	getBattleAI(bid).battleNewRound(bid);
}

void Router::battleNewRoundFirst(const BattleID & bid)
{
	getBattleAI(bid).battleNewRoundFirst(bid);
}

void Router::battleObstaclesChanged(const BattleID & bid, const std::vector<ObstacleChanges> & obstacles)
{
	getBattleAI(bid).battleObstaclesChanged(bid, obstacles);
};

void Router::battleSpellCast(const BattleID & bid, const BattleSpellCast * sc)
{
	getBattleAI(bid).battleSpellCast(bid, sc);
}

void Router::battleStackMoved(const BattleID & bid, const CStack * stack, const BattleHexArray & dest, int distance, bool teleport)
{
	getBattleAI(bid).battleStackMoved(bid, stack, dest, distance, teleport);
}

void Router::battleStacksAttacked(const BattleID & bid, const std::vector<BattleStackAttacked> & bsa, bool ranged)
{
	getBattleAI(bid).battleStacksAttacked(bid, bsa, ranged);
}

void Router::battleStacksEffectsSet(const BattleID & bid, const SetStackEffect & sse)
{
	getBattleAI(bid).battleStacksEffectsSet(bid, sse);
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
	if(battles.contains(bid))
	{
		const auto message = "MMAI Router: duplicate battleStart for BattleID " + std::to_string(bid.getNum()) + ", active battles: [" + formatActiveBattleIds() + "]";
		error(message);
		throw std::runtime_error(message);
	}

#ifdef ENABLE_MMAI_TEST
	std::shared_ptr<CBattleGameInterface> newBai;
	if(testBattleAIFactory)
		newBai = testBattleAIFactory(bid, side);
	else
		newBai = createDelegatedBAI(side);
#else
	auto newBai = createDelegatedBAI(side);
#endif

	debug("battleStart bid=" + std::to_string(bid.getNum()) + " -> BAI " + MakeAddrStr(newBai.get()));
	battles.emplace(bid, BattleContext{newBai});
	newBai->battleStart(bid, army1, army2, tile, hero1, hero2, side, replayAllowed);
}

void Router::battleTriggerEffect(const BattleID & bid, const BattleTriggerEffect & bte)
{
	getBattleAI(bid).battleTriggerEffect(bid, bte);
}

void Router::battleUnitsChanged(const BattleID & bid, const std::vector<UnitChanges> & changes)
{
	getBattleAI(bid).battleUnitsChanged(bid, changes);
}

void Router::yourTacticPhase(const BattleID & bid, int distance)
{
	getBattleAI(bid).yourTacticPhase(bid, distance);
}

/*
 * private
 */

void Router::error(const std::string & text) const
{
	log(ELogLevel::ERROR, text);
}
void Router::warn(const std::string & text) const
{
	log(ELogLevel::WARN, text);
}
void Router::info(const std::string & text) const
{
	log(ELogLevel::INFO, text);
}
void Router::debug(const std::string & text) const
{
	log(ELogLevel::DEBUG, text);
}
void Router::trace(const std::string & text) const
{
	log(ELogLevel::TRACE, text);
}
void Router::log(ELogLevel::ELogLevel level, const std::string & text) const
{
	if(logAi->getEffectiveLevel() <= level)
		logAi->debug("Router-%s [%s] %s", addrstr, colorname, text);
}
}
