// BuildFilter: a loot filter for The Slormancer built around your build - an Aurie module that needs SlormAPI.
//
// In the inventory the Filter-o-tron 3000 Mk.II button is cut to half width and "Build Filter" sits next to it; Build
// Filter opens its own page in Friedrich's panel (tabs Rules / Gear / Extras / Pickup). Everything is drawn INTO the
// game's own inventory bake and registered in obj_ui_next_gen.mba (the game's list of clickable UI elements) through
// SlormAPI draw programs, so the game itself slides it with the menu, draws hover frames and plays the sounds. Our
// elements call the "sink" script with "bf:<action>"; SlormAPI swallows those calls (skip override) and logs them, and
// this module reads the log. Filtered items are converted with the game's own Filter-o-tron conversion.
//
// Settings: mods/aurie/BuildFilter/rules.json (written by the in-game panel). Log: mods/aurie/BuildFilter/loot_log.txt.
#include <YYToolkit/YYTK_Shared.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include "Json.hpp"
#include "SlormAPI_Interface.hpp"
#include "GameData.hpp"

using namespace Aurie;
using namespace YYTK;
using sj::Json;
using bf::Num;
using bf::Str;

static YYTKInterface* g_Yytk = nullptr;
static SlormAPIInterface* g_Api = nullptr;
static std::filesystem::path g_Dir;           // mods/aurie/BuildFilter
static bf::GameData g_Data;

static constexpr const char* kVersion = "1.0.0";
static constexpr const char* OWNER = "BuildFilter";      // our layer of SlormAPI's hooks and our hook log
static constexpr const char* SINK = "scr_blacksmith_open_filter_from_inventory";
static constexpr double BTN_X = 1537, BTN_Y = 904;      // the Filter-o-tron button when its element is not listed
static constexpr int VALUE_LEVEL = 100;
static constexpr unsigned BOX = 0x030303;

// ============================================================================
// JSON helpers
// ============================================================================

static Json O(std::initializer_list<std::pair<const char*, Json>> Kv)
{
	Json j = Json::Object();
	for (auto& p : Kv) j.Add(p.first, p.second);
	return j;
}
static Json A(std::initializer_list<Json> Xs)
{
	Json j = Json::Array();
	for (auto& x : Xs) j.Push(x);
	return j;
}
static Json AV(const std::vector<Json>& Xs)
{
	Json j = Json::Array();
	for (auto& x : Xs) j.Push(x);
	return j;
}
static Json Op(const char* OpName, std::initializer_list<Json> Args) { return O({ { "$op", OpName }, { "args", A(Args) } }); }
static Json V(const char* Name) { return O({ { "$v", Name } }); }
static Json Asset(const char* Name) { return O({ { "$asset", Name } }); }
static Json SelfVar(const char* Name) { return O({ { "$self", Name } }); }
static Json Inst(double Id) { return O({ { "$inst", Id } }); }

// ============================================================================
// SlormAPI calls (in-process, on the game thread)
// ============================================================================

static Json Api(const Json& Req)
{
	const char* out = g_Api->Request(sj::Dump(Req).c_str());
	Json r = sj::Parse(out ? out : "");
	if (!r.GetBool("ok", false)) throw std::runtime_error(r.GetStr("error", "SlormAPI error"));
	const Json* res = r.Find("result");
	return res ? *res : Json();
}
static Json ApiGet(const std::string& Path, const Json& Root = Json(), int Depth = 1)
{
	Json q = O({ { "cmd", "get" }, { "path", Path }, { "depth", Depth } });
	if (!Root.IsNull()) q.Add("root", Root);
	return Api(q);
}
static void ApiSet(const std::string& Path, const Json& Value, const Json& Root = Json())
{
	Json q = O({ { "cmd", "set" }, { "path", Path }, { "value", Value } });
	if (!Root.IsNull()) q.Add("root", Root);
	Api(q);
}
static Json ApiCall(const std::string& Fn, const std::vector<Json>& Args, const Json& Self = Json(), int Depth = 1)
{
	Json q = O({ { "cmd", "call" }, { "fn", Fn }, { "args", AV(Args) }, { "depth", Depth } });
	if (!Self.IsNull()) q.Add("self", Self);
	return Api(q);
}
static Json ApiInstances(const std::string& Object, const std::vector<std::string>& Vars = {})
{
	Json q = O({ { "cmd", "instances" }, { "object", Object } });
	if (!Vars.empty())
	{
		Json v = Json::Array();
		for (auto& s : Vars) v.Push(Json(s));
		q.Add("vars", v);
	}
	return Api(q);
}

// ============================================================================
// Settings (rules.json)
// ============================================================================

static Json g_Rules = Json::Object();
static const std::vector<std::string> CONVERT_TO = { "fragments", "goldus", "hide" };

static const Json* RF(const char* K) { return g_Rules.Find(K); }
static bool RB(const char* K, bool D) { const Json* v = RF(K); return v && v->type == Json::Bool ? v->b : D; }
static double RN(const char* K, double D) { const Json* v = RF(K); return v ? Num(*v, D) : D; }
static std::string RS(const char* K, const std::string& D) { const Json* v = RF(K); return v && v->type == Json::Str ? v->s : D; }
static Json RJ(const char* K) { const Json* v = RF(K); return v ? *v : Json(); }
static std::vector<int> RInts(const char* K)
{
	std::vector<int> out;
	if (const Json* v = RF(K); v && v->type == Json::Arr)
		for (auto& x : v->a) out.push_back(static_cast<int>(Num(x)));
	return out;
}
static Json Ints(const std::vector<int>& Xs)
{
	Json j = Json::Array();
	for (int x : Xs) j.Push(Json(x));
	return j;
}
static Json SlotMap(const Json& Value)
{
	Json j = Json::Object();
	for (auto& s : bf::Slots()) j.Add(s.Key, Value);
	return j;
}

static void EnsureDefaults()
{
	auto def = [](const char* k, Json v) { if (!g_Rules.Find(k)) g_Rules.Add(k, std::move(v)); };
	def("enabled", Json(false));                         // a fresh install starts off: set your stats first
	def("build", Json("your build"));
	def("min_gold", Json(3));
	def("convert_to", Json("fragments"));
	def("auto_collect", Json(true));
	def("gold_pickup", Json(true));
	def("tags", Json(true));
	def("keep_all_legendaries", Json(true));
	def("keep_pure", Json(false));
	def("keep_extras", Json(false));
	def("min_level", Json(0));
	def("extras_affinity", Json(true));
	def("extras_attrs", Json::Array());
	def("reapers", Json::Object());
	Json empty = A({ A({ Json(), Json(0) }), A({ Json(), Json(0) }), A({ Json(), Json(0) }) });
	def("gear_default", SlotMap(empty));
	def("gear", RJ("gear_default"));
	def("leg_default", SlotMap(Json()));
	def("leg_wanted", RJ("leg_default"));
	def("leg_min_roll", SlotMap(Json(0)));
	Json gear = RJ("gear");                              // every slot present
	for (auto& s : bf::Slots())
		if (!gear.Find(s.Key)) gear.Set(s.Key, empty);
	g_Rules.Set("gear", gear);
}

static std::filesystem::path RulesPath() { return g_Dir / "rules.json"; }

static void LoadRules()
{
	std::ifstream f(RulesPath(), std::ios::binary);
	if (f)
	{
		std::stringstream ss;
		ss << f.rdbuf();
		try { g_Rules = sj::Parse(ss.str()); }
		catch (...) { g_Rules = Json::Object(); }
	}
	if (g_Rules.type != Json::Obj) g_Rules = Json::Object();
	EnsureDefaults();
}

static int g_RulesVer = 0;                    // bumped on every settings change (the UI state key)

static void SaveRules()
{
	++g_RulesVer;
	std::error_code ec;
	std::filesystem::create_directories(g_Dir, ec);
	std::ofstream f(RulesPath(), std::ios::binary | std::ios::trunc);
	f << sj::Dump(g_Rules);
}

static std::vector<int> GoldRolls()
{
	std::vector<int> v = { 0 };
	for (int r = 15; r <= 40; ++r) v.push_back(r);
	return v;
}
static std::vector<int> LegRolls()
{
	std::vector<int> v = { 0 };
	for (int r = 75; r <= 100; ++r) v.push_back(r);
	return v;
}
static std::vector<int> Levels()
{
	std::vector<int> v = { 0 };
	for (int r = 5; r <= 100; r += 5) v.push_back(r);
	return v;
}
static int StepIn(const std::vector<int>& Values, int Cur, int Step)
{
	auto it = std::find(Values.begin(), Values.end(), Cur);
	int i = it == Values.end() ? 0 : static_cast<int>(it - Values.begin());
	i = (std::max)(0, (std::min)(static_cast<int>(Values.size()) - 1, i + Step));
	return Values[i];
}

// ============================================================================
// Items: parse, classify, convert
// ============================================================================

struct Item
{
	std::string Kind, Rarity, Slot;
	int Level = 0;
	int Legendary = -1;
	double LegRoll = 0;
	struct Line { std::string Tier, Stat; double Roll; };
	std::vector<Line> Lines;
	bool Pure = false;
	struct Extra { std::string Tag; int Index, N; };
	std::vector<Extra> Extras;
	Json Raw;
};

static bool ParseItem(const Json& Ar, Item& It)
{
	if (Ar.type != Json::Arr || Ar.a.empty() || Ar.a[0].type != Json::Arr || Ar.a[0].a.size() < 4) return false;
	const auto& h = Ar.a[0].a;
	It.Rarity = Str(h[0]);
	It.Slot = Str(h[1]);
	It.Level = static_cast<int>(Num(h[2]));
	It.Kind = Str(h[3]);
	It.Raw = Ar;
	for (size_t k = 1; k < Ar.a.size(); ++k)
	{
		const Json& l = Ar.a[k];
		if (l.type != Json::Arr || l.a.empty()) continue;
		std::string tag = Str(l.a[0]);
		if (tag == "L" && l.a.size() > 1)
		{
			It.Legendary = static_cast<int>(Num(l.a[1]));
			It.LegRoll = l.a.size() > 2 ? Num(l.a[2]) : 0;
		}
		else if ((tag == "RP" || tag == "MA" || tag == "AT") && l.a.size() > 2)
			It.Extras.push_back({ tag, static_cast<int>(Num(l.a[1])), static_cast<int>(Num(l.a[2])) });
		else if (l.a.size() > 2 && l.a[1].type == Json::Str)
		{
			It.Lines.push_back({ tag, l.a[1].s, Num(l.a[2]) });
			if (l.a.size() > 3 && Num(l.a[3]) > 0) It.Pure = true;
		}
	}
	return true;
}

static int GoldMatches(const std::vector<std::pair<std::string, double>>& Gold, const Json& Rows)
{
	std::vector<std::pair<std::string, double>> free;
	for (auto& r : Rows.a)
		if (r.type == Json::Arr && r.a.size() >= 2 && r.a[0].type == Json::Str) free.push_back({ r.a[0].s, Num(r.a[1]) });
	int n = 0;
	for (auto& [st, roll] : Gold)
		for (size_t i = 0; i < free.size(); ++i)
			if (free[i].first == st && roll >= free[i].second) { free.erase(free.begin() + i); ++n; break; }
	return n;
}

static int g_Cls = 0;                         // the current hero's class

static std::string ExtrasMatch(const Item& It)
{
	if (!RB("keep_extras", false)) return "";
	auto skills = RInts("extras_skills"), attrs = RInts("extras_attrs");
	for (auto& e : It.Extras)
	{
		if (e.Tag == "MA" && std::find(skills.begin(), skills.end(), e.Index) != skills.end())
			return "+" + std::to_string(e.N) + " " + g_Data.SkillName(g_Cls, e.Index);
		if (e.Tag == "AT" && e.Index >= 0 && e.Index < 8 && std::find(attrs.begin(), attrs.end(), e.Index) != attrs.end())
			return "+" + std::to_string(e.N) + " " + bf::kTraits[e.Index];
		if (e.Tag == "RP" && RB("extras_affinity", true) && RF("smith") && e.Index == static_cast<int>(RN("smith", -1)))
			return "+" + std::to_string(e.N) + " Affinity";
	}
	return "";
}

// verdict: BUILD / GOLD / PURE / EXTRA (kept, tagged), CONVERT, COLLECT, or "" (left alone)
static std::pair<std::string, std::string> Classify(const Item& It)
{
	if (It.Kind != "equipment")
	{
		if (It.Kind == "weapon" || It.Kind == "reaper")
		{
			std::string rid = It.Raw.a[0].a.size() > 4 ? Str(It.Raw.a[0].a[4]) : "";
			Json reapers = RJ("reapers");
			if (const Json* n = reapers.Find(rid)) return { "BUILD", "BUILD " + Str(*n) };
			return { "", "" };
		}
		return RB("auto_collect", true) ? std::make_pair(std::string("COLLECT"), std::string()) : std::make_pair(std::string(), std::string());
	}
	if (It.Rarity == "legendary" || It.Rarity == "nether" || It.Legendary >= 0)
	{
		if (It.Legendary >= 0)
		{
			Json wanted = RJ("leg_wanted"), minRoll = RJ("leg_min_roll");
			for (auto& [slot, v] : wanted.o)
				if (v.type == Json::Num && static_cast<int>(v.n) == It.Legendary)
				{
					const Json* mr = minRoll.Find(slot);
					if (It.LegRoll >= (mr ? Num(*mr) : 0)) return { "BUILD", "BUILD " + g_Data.LegName(It.Legendary) };
				}
			if (It.Rarity != "nether" && !RB("keep_all_legendaries", true)) return { "CONVERT", "" };
		}
		return { "", "" };
	}
	if (It.Level < static_cast<int>(RN("min_level", 0))) return { "CONVERT", "" };
	std::vector<std::pair<std::string, double>> gold;
	for (auto& l : It.Lines)
		if (l.Tier == "E") gold.push_back({ l.Stat, l.Roll });
	Json gear = RJ("gear");
	const Json* rows = gear.Find(It.Slot);
	int n = rows ? GoldMatches(gold, *rows) : 0;
	int need = (std::min)(static_cast<int>(RN("min_gold", 3)), static_cast<int>(gold.size()));
	if (!gold.empty() && n >= need && n >= 1) return { "GOLD", "GOLD " + std::to_string(n) + "/" + std::to_string(gold.size()) };
	std::string extra = ExtrasMatch(It);
	if (!extra.empty()) return { "EXTRA", extra };
	if (RB("keep_pure", false) && It.Pure) return { "PURE", "PURE" };
	return { "CONVERT", "" };
}

static const std::map<std::string, int> RARITY_ROW = { { "normal", 0 }, { "magic", 1 }, { "rare", 2 }, { "epic", 3 }, { "legendary", 4 } };

// The game's own Filter-o-tron conversion for this one item: the hero's filter row of its rarity set to "hide, no
// exceptions" and its conversion to Fragments / Goldus for one scr_blacksmith_filter_mla call, then put back.
static Json g_SavedFilter;                    // the hero's own filter settings, read once per scan pass

static bool Convert(double Hero, double LootInst, const Item& It)
{
	std::string to = RS("convert_to", "fragments");
	if (to != "fragments" && to != "goldus") return false;
	auto row = RARITY_ROW.find(It.Rarity);
	if (row == RARITY_ROW.end()) return false;
	std::string path = "loot_filter[" + std::to_string(g_Cls) + "]";
	if (g_SavedFilter.IsNull()) g_SavedFilter = ApiGet(path, Json(), 3);
	auto set = [&](const std::string& Path, Json Value)
	{
		return O({ { "cmd", "set" }, { "path", Path }, { "value", Value } });
	};
	// one frame-consistent batch; the put-back runs even if the conversion call fails
	Json r = Api(O({ { "cmd", "batch" }, { "cmds", A({
		set(path + "[0]", Json(1)),
		set(path + "[1][" + std::to_string(row->second) + "]", A({ 1, 0, 0, 0, 0, 0, 0 })),
		set(path + "[2]", Json(to == "fragments" ? 1 : 2)),
		O({ { "cmd", "call" }, { "fn", "scr_blacksmith_filter_mla" }, { "args", A({ It.Raw }) }, { "self", Inst(Hero) } }),
		set(path, g_SavedFilter),
		}) } }));
	const Json* res = r.a.size() > 3 ? r.a[3].Find("result") : nullptr;
	if (res && res->type == Json::Num && res->n == 0)
	{
		ApiCall("instance_destroy", { Inst(LootInst) });
		return true;
	}
	return false;
}

// ============================================================================
// Scanning the ground
// ============================================================================

static std::unordered_set<int64_t> g_Seen;
static std::map<std::string, int64_t> g_Counts = { { "BUILD", 0 }, { "GOLD", 0 }, { "PURE", 0 }, { "EXTRA", 0 },
	{ "CONVERT", 0 }, { "COLLECT", 0 }, { "COINS", 0 } };
static bool IsKeep(const std::string& V) { return V == "BUILD" || V == "GOLD" || V == "PURE" || V == "EXTRA"; }

static bool g_LootLog = true;    // setting "loot_log"

static void LogLine(const std::string& Line)
{
	if (!g_LootLog) return;
	std::ofstream f(g_Dir / "loot_log.txt", std::ios::app);
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_s(&tmv, &t);
	char ts[16];
	std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
	f << ts << " " << Line << "\n";
}

static double Now();
static constexpr double SCAN_BUDGET_S = 0.002;   // per frame; the rest of a burst waits for the next frames

// returns true when drops are left for the next frame
static bool Scan(double Hero)
{
	bool backlog = false;
	g_SavedFilter = Json();
	Json loots = ApiInstances("obj_loot");
	double t0 = Now();                            // the budget is for the drops: one always gets its turn
	int handled = 0;
	for (auto& l : loots.a)
	{
		double inst = Num(l.At("$inst"));
		int64_t key = static_cast<int64_t>(inst);
		if (g_Seen.count(key)) continue;
		if (handled++ > 0 && Now() - t0 > SCAN_BUDGET_S) { backlog = true; break; }
		g_Seen.insert(key);
		try
		{
			Item it;
			if (!ParseItem(ApiGet("ar", Inst(inst), 3), it)) continue;
			auto [verdict, label] = Classify(it);
			if (verdict == "COLLECT")
			{
				Json ok = ApiCall("scr_pick_up_item", { it.Raw, Json(true), Json(false), Json(true) }, Inst(Hero));
				if (ok.type == Json::Bool ? ok.b : Num(ok) != 0) ApiCall("instance_destroy", { Inst(inst) });
				else { g_Seen.erase(key); continue; }                      // inventory full: try again later
			}
			else if (verdict == "CONVERT")
			{
				if (!Convert(Hero, inst, it)) ApiSet("visible", Json(false), Inst(inst));
			}
			else if (IsKeep(verdict) && RB("tags", true))
			{
				std::string name = Str(ApiGet("loot_name", Inst(inst)));
				Api(O({ { "cmd", "batch" }, { "cmds", A({
					O({ { "cmd", "set" }, { "path", "loot_name" }, { "value", Json(label + "  " + name) }, { "root", Inst(inst) } }),
					O({ { "cmd", "set" }, { "path", "baked" }, { "value", false }, { "root", Inst(inst) } }) }) } }));
			}
			if (!verdict.empty())
			{
				g_Counts[verdict]++;
				std::string gold;
				for (auto& ln : it.Lines)
					if (ln.Tier == "E") gold += (gold.empty() ? "" : ", ") + ln.Stat + " " + std::to_string(static_cast<int>(ln.Roll));
				LogLine(verdict + " " + (it.Kind == "equipment" ? it.Rarity + " " + it.Slot + " leg=" + std::to_string(it.Legendary)
					+ " gold=[" + gold + "]" + (label.empty() ? "" : " " + label) : it.Kind));
			}
		}
		catch (const std::exception&) {}                                     // picked up / despawned meanwhile
	}
	if (g_Seen.size() > 5000) g_Seen.clear();
	if (RB("gold_pickup", true) && !backlog)
	{
		// gold coins: the coin's own pick-up flag, so it flies to the hero and the game adds it (gold find, sound)
		Json coins = ApiInstances("obj_gold_coin", { "pick_up_animation", "deny_loot", "value" });
		Json cmds = Json::Array();
		for (auto& c : coins.a)
		{
			const Json* pa = c.Find("pick_up_animation");
			const Json* dl = c.Find("deny_loot");
			if ((pa && Num(*pa) != 0) || (dl && Num(*dl) != 0)) continue;
			cmds.Push(O({ { "cmd", "set" }, { "path", "pick_up_animation" }, { "value", true }, { "root", c.At("$inst") } }));
			const Json* v = c.Find("value");
			g_Counts["COINS"] += v ? static_cast<int64_t>(Num(*v)) : 0;
		}
		if (!cmds.a.empty()) Api(O({ { "cmd", "batch" }, { "cmds", cmds } }));
	}
	return backlog;
}

// ============================================================================
// In-game UI: draw programs (see SlormAPI's draw programs)
// ============================================================================

static Json Text(Json X, Json Y, Json S, const char* Font = "ft_standard_12", int Ha = 1, int Va = 1,
	const char* Color = "white", int A8 = 0, int A9 = 1, const char* Mark = nullptr)
{
	Json args = A({ X, Y, S, 1, 1, Font, Ha, Va, A8, A9, Color, "black", 1, Json(Mark != nullptr) });
	if (Mark) args.Push(Json(Mark));
	return O({ { "fn", "scr_draw_text" }, { "args", args } });
}
// The game lays its menus out for 1920x1080 and multiplies positions and sizes by self.ui_scale (from the resolution)
// as it draws; its mba entries and the panel's position arrive already scaled. "sc" is that scale (a program var).
static Json Sc() { return V("sc"); }
static Json S(Json N) { return Op("*", { std::move(N), Sc() }); }
static Json ScaleVar() { return A({ "sc", Op("??", { SelfVar("ui_scale"), 1 }) }); }
static Json Spr(const char* Name, Json Frame, Json X, Json Y)
{
	return O({ { "fn", "draw_sprite_ext" }, { "args", A({ Asset(Name), Frame, X, Y, Sc(), Sc(), 0, 0xFFFFFF, 1 }) } });
}
static Json Push(Json Entry, Json When = Json())
{
	Json c = O({ { "fn", "$mba_push" }, { "args", A({ Entry }) } });
	if (!When.IsNull()) c.Add("when", When);
	return c;
}

struct Calls
{
	Json List = Json::Array();
	void operator+=(Json C) { List.Push(std::move(C)); }
};

static double g_Sink = 0;

static Json InventoryProgram(bool On, bool Ours)
{
	Json shown = Op("==", { SelfVar("menu_citizen"), "blacksmith" });
	Json oursShown = Op("and", { shown, Json(Ours) });
	Json x = V("bx"), y = V("by");
	const char* right = On ? "spr_menu_red_button" : "spr_menu_grey_button";
	Calls c;
	c += O({ { "fn", "$mba_set" }, { "args", A({ "Filter Active", 4, S(168) }) } });
	c += O({ { "fn", "$mba_set" }, { "args", A({ "Filter Active", 1, Asset("spr_menu_grey_button") }) } });
	c += O({ { "fn", "$mba_set" }, { "args", A({ "Filter Active", 6, "Filter-o-tron" }) } });
	c += Spr("spr_menu_grey_button", Op("?", { Op("and", { shown, Json(!Ours) }), 3, 0 }), x, y);
	c += Text(Op("+", { x, S(84) }), Op("+", { y, S(30) }), "Filter-o-tron");
	c += Spr(right, Op("?", { oursShown, 3, 0 }), Op("+", { x, S(168) }), y);
	c += Text(Op("+", { x, S(252) }), Op("+", { y, S(30) }), "Build Filter");
	c += Push(A({ "Build Filter", Asset(right), Op("+", { x, S(168) }), y, S(168), S(52), "Build Filter", g_Sink, "bf:open" }),
		Op("not", { oursShown }));
	c += Push(A({ "Filter-o-tron", Asset("spr_menu_grey_button"), x, y, S(168), S(52), "Filter-o-tron", g_Sink, "bf:game" }), oursShown);
	return O({
		{ "vars", A({ ScaleVar(), A({ "bx", Op("??", { Op("mba", { "Filter Active", 2 }), S(BTN_X) }) }),
			A({ "by", Op("??", { Op("mba", { "Filter Active", 3 }), S(BTN_Y) }) }) }) },
		{ "when", Op("or", { Op("def", { Op("mba", { "Filter Active", 0 }) }),
			Op("and", { shown, Op("==", { SelfVar("menu_show"), "inventory" }) }) }) },
		{ "calls", c.List } });
}

struct Panel
{
	Calls c;
	static Json X(double N) { return Op("+", { S(N), V("dx") }); }
	static Json Y(double N) { return Op("+", { S(N), V("dy") }); }

	void Radio(double Yp, const std::string& Label, const char* Mark, bool On, const std::string& Action = "",
		bool Big = false, double Xp = 184, const char* Font = "ft_standard_12")
	{
		std::string spr;
		Json extra = Json("");
		if (Big)
		{
			c += Spr("spr_radio_btn_v3", 0, X(Xp), Y(Yp));
			if (On) c += Spr("spr_radio_cross_v2", 0, X(Xp + 18), Y(Yp + 18));
			c += Text(X(Xp + 42), Y(Yp + 10), Json(Label), "ft_standard_16", 0, 1, "white", 0, 1, Mark);
			spr = "spr_radio_btn_v3";
			if (On) extra = Json(788);                     // spr_radio_cross_v2: the game's "on" fill for hover
		}
		else
		{
			spr = On ? "spr_radio_small_btn_filled" : "spr_radio_small_btn";
			c += Spr(spr.c_str(), 0, X(Xp), Y(Yp));
			c += Text(X(Xp + 42), Y(Yp + 18), Json(Label), Font, 0, 1, "white", 0, 1, Mark);
		}
		if (!Action.empty())
			c += Push(A({ "Filter", Asset(spr.c_str()), X(Xp), Y(Yp), S(36), S(36), extra, g_Sink, Json("bf:" + Action), "" }));
	}
	void Arrow(bool Left, double Xp, double Yp, const std::string& Action)
	{
		const char* spr = Left ? "spr_selector_btn_l_v2" : "spr_selector_btn_r_v2";
		c += Spr(spr, 0, X(Xp), Y(Yp));
		c += Push(A({ "sel", Asset(spr), X(Xp), Y(Yp), S(24), S(36), "", g_Sink, Json("bf:" + Action) }));
	}
	void Block(double Xp, double Yp, double W, const std::string& Value, const char* Color = "grey",
		const char* Font = "ft_standard_12")
	{
		bool square = W <= 72;
		const char* name = square ? "spr_selector_block_square" : "spr_selector_block_v2";
		double native = square ? 36 : 256;
		c += O({ { "fn", "draw_sprite_ext" }, { "args", A({ Asset(name), 0, X(Xp), Y(Yp), S(W / native), Sc(), 0, 0xFFFFFF, 1 }) } });
		c += Text(X(Xp + W / 2), Y(Yp + 18), Json(Value), Font, 1, 1, Color, 1, 0);
	}
	void Selector(double Yp, const std::string& Value, const std::string& L, const std::string& R, double XL = 284,
		double XR = 352, bool Wide = false)
	{
		if (Wide) Block(202, Yp, 256, Value); else Block(312, Yp, 36, Value);
		if (!L.empty()) Arrow(true, XL, Yp, L);
		if (!R.empty()) Arrow(false, XR, Yp, R);
	}
	void Steps(const std::vector<int>& Values, int Cur, double XL, double XR, double Yp, const std::string& Minus,
		const std::string& Plus)
	{
		auto it = std::find(Values.begin(), Values.end(), Cur);
		size_t i = it == Values.end() ? 0 : static_cast<size_t>(it - Values.begin());
		if (i > 0) Arrow(true, XL, Yp, Minus);
		if (i + 1 < Values.size()) Arrow(false, XR, Yp, Plus);
	}
	void Button(double Xp, double Yp, const std::string& Label, const std::string& Action, bool Selected = false,
		bool Disabled = false)
	{
		c += Spr("spr_menu_grey_button", Selected ? 3 : 0, X(Xp), Y(Yp));
		c += Text(X(Xp + 84), Y(Yp + 30), Json(Label), "ft_standard_12", 1, 1, Disabled ? "grey" : "white");
		if (!(Selected || Disabled))
			c += Push(A({ Json("BF " + Label), Asset("spr_menu_grey_button"), X(Xp), Y(Yp), S(168), S(52), Json(Label), g_Sink,
				Json("bf:" + Action) }));
	}
	void Tab(double Xp, double Yp, const std::string& Label, const std::string& Action, bool Selected)
	{
		const char* spr = "spr_menu_floor_grey_button_done";     // 88x52: 0 normal, 1 hover, 2 gold (selected)
		c += Spr(spr, Selected ? 2 : 0, X(Xp), Y(Yp));
		c += Text(X(Xp + 44), Y(Yp + 28), Json(Label), "ft_standard_11");
		if (!Selected)
			c += Push(A({ Json("BF " + Label), Asset(spr), X(Xp), Y(Yp), S(88), S(52), Json(Label), g_Sink, Json("bf:" + Action) }));
	}
	void Box(double X0, double Y0, double X1, double Y1)
	{
		c += O({ { "fn", "draw_set_colour" }, { "args", A({ Json(static_cast<double>(BOX)) }) } });
		c += O({ { "fn", "draw_rectangle" }, { "args", A({ X(X0), Y(Y0), X(X1), Y(Y1), false }) } });
		c += O({ { "fn", "draw_set_colour" }, { "args", A({ Json(static_cast<double>(0xFFFFFF)) }) } });
	}
	void Wrapped(double Xp, double Yp, const std::string& S, double Width, const char* Color = "white")
	{
		c += Text(X(Xp), Y(Yp), O({ { "$fn", "scr_cut_text" }, { "args", A({ Json(S), ::S(Width), "ft_standard_12" }) } }),
			"ft_standard_12", 1, 1, Color);
	}
};

static std::string Fmt(double V)
{
	char b[32];
	snprintf(b, sizeof(b), "%g", V);
	return b;
}

static std::string ValueLabel(const std::string& Stat, int MinRoll)
{
	if (Stat.empty() || MinRoll <= 0) return "any";
	double v = g_Data.AffixValue(Stat, MinRoll, VALUE_LEVEL);
	bool pct = (Stat.size() > 8 && Stat.compare(Stat.size() - 8, 8, "_percent") == 0)
		|| (Stat.size() > 5 && Stat.compare(Stat.size() - 5, 5, "_mult") == 0);
	std::string s = Fmt(v) + (pct ? "%" : "");
	return MinRoll >= 40 ? s + " max" : ">= " + s;
}

static std::string Thousands(int64_t N)
{
	std::string s = std::to_string(N), out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (i && (s.size() - i) % 3 == 0) out += ' ';
		out += s[i];
	}
	return out;
}

static std::vector<int> BuildSkills()
{
	auto v = RInts("build_skills");
	if (!v.empty()) return v;
	try                                      // the class's equipped skills (global.skill_equipped[class][id] 2..4)
	{
		Json eq = ApiGet("skill_equipped[" + std::to_string(g_Cls) + "]", Json(), 1);
		for (size_t i = 0; i < eq.a.size() && i < 64; ++i)
		{
			double x = Num(eq.a[i], -1);
			if (x >= 2 && x <= 4) v.push_back(static_cast<int>(i));
		}
	}
	catch (...) {}
	return v;
}

static const std::vector<std::pair<std::string, std::string>> PAGES = {
	{ "rules", "Rules" }, { "gear", "Gear" }, { "extras", "Extras" }, { "pickup", "Pickup" } };
static const std::map<std::string, std::string> CONVERT_LABEL = {
	{ "fragments", "Fragments" }, { "goldus", "Goldus" }, { "hide", "Nothing (hide them)" } };

static Json PanelProgram(bool On, const std::string& Page, int SlotI)
{
	Panel p;
	int g = static_cast<int>(RN("min_gold", 3));
	int64_t kept = g_Counts["BUILD"] + g_Counts["GOLD"] + g_Counts["PURE"] + g_Counts["EXTRA"];
	std::string done = "Kept " + std::to_string(kept) + "   Converted " + std::to_string(g_Counts["CONVERT"])
		+ "   Picked up " + std::to_string(g_Counts["COLLECT"]) + "   Gold " + Thousands(g_Counts["COINS"]);
	p.Box(136, 348, 523, 467);
	std::string build = RS("build", "your build");
	if (build.size() > 5 && build.compare(build.size() - 5, 5, ".json") == 0) build.resize(build.size() - 5);
	p.c += Text(Panel::X(330), Panel::Y(370), Json("Loot Filter for your Build (" + build + ")"),
		"ft_standard_12", 1, 1, "grey");
	p.Wrapped(330, 406, "Keeps your build's legendaries and gold stats; every other drop becomes materials.", 350);
	p.c += Text(Panel::X(330), Panel::Y(446), Json(done), "ft_standard_12", 1, 1, "grey");
	for (size_t i = 0; i < PAGES.size(); ++i)
		p.Tab(136 + 100.0 * i, 478, PAGES[i].second, "page:" + PAGES[i].first, Page == PAGES[i].first);
	p.c += Spr("spr_filter_slot_decoration", 0, Panel::X(330), Panel::Y(545));

	if (Page == "rules")
	{
		p.Radio(576, On ? "BUILD FILTER {ON}" : "BUILD FILTER {OFF}", On ? "pure" : "grey", On, "toggle", true);
		p.c += Text(Panel::X(226), Panel::Y(618), "Keep a dropped item when...", "ft_standard_12", 0, 1, "white", 0, 1, "normal");
		bool all = RB("keep_all_legendaries", true);
		p.Radio(626, all ? "... it is any {Legendary}" : "... it is a {Legendary} picked in Gear", "item_highlight", true, "legall");
		p.Radio(662, "... it has {Gold Stats} the build wants:", "item_highlight", true);
		p.Selector(698, std::to_string(g), g > 1 ? "gold-" : "", g < 3 ? "gold+" : "");
		p.c += Text(Panel::X(390), Panel::Y(716), "of 3", "ft_standard_12", 0, 1, "grey");
		p.Radio(740, "... it has a {Pure Stat}", "pure", RB("keep_pure", false), "pure");
		p.Radio(774, "... it has an {Extra} ticked in Extras", "item_highlight", RB("keep_extras", false), "extras");
		int lvl = static_cast<int>(RN("min_level", 0));
		p.Box(136, 854, 523, 930);
		p.c += Text(Panel::X(330), Panel::Y(878), "Convert equipment below item level:", "ft_standard_12", 1, 2, "grey");
		p.Block(202, 890, 256, lvl ? "level " + std::to_string(lvl) : "any level");
		p.Steps(Levels(), lvl, 170, 466, 890, "lvl-", "lvl+");
	}
	else if (Page == "gear")
	{
		const auto& slot = bf::Slots()[SlotI];
		Json gear = RJ("gear");
		Json rows = gear.Find(slot.Key) ? *gear.Find(slot.Key) : Json::Array();
		p.Selector(560, slot.Label, "slot-", "slot+", 170, 466, true);
		p.c += Text(Panel::X(330), Panel::Y(610), Json("Gold stats to keep, minimum value (level " + std::to_string(VALUE_LEVEL) + "):"),
			"ft_standard_12", 1, 1, "grey");
		for (size_t k = 0; k < 3; ++k)
		{
			std::string stat;
			int minRoll = 0;
			if (k < rows.a.size() && rows.a[k].type == Json::Arr && rows.a[k].a.size() >= 2)
			{
				if (rows.a[k].a[0].type == Json::Str) stat = rows.a[k].a[0].s;
				minRoll = static_cast<int>(Num(rows.a[k].a[1]));
			}
			double y = 624 + 40.0 * k;
			std::string ks = std::to_string(k);
			p.Arrow(true, 142, y, "stat" + ks + "-");
			p.Arrow(false, 358, y, "stat" + ks + "+");
			p.Block(166, y, 190, g_Data.StatLabel(stat, slot.Key), stat.empty() ? "grey" : "white", "ft_standard_11");
			p.Block(426, y, 64, ValueLabel(stat, minRoll), (!stat.empty() && minRoll) ? "white" : "grey", "ft_standard_11");
			if (!stat.empty()) p.Steps(GoldRolls(), minRoll, 400, 492, y, "min" + ks + "-", "min" + ks + "+");
		}
		Json lw = RJ("leg_wanted"), lm = RJ("leg_min_roll");
		const Json* legJ = lw.Find(slot.Key);
		int leg = legJ && legJ->type == Json::Num ? static_cast<int>(legJ->n) : -1;
		int lroll = lm.Find(slot.Key) ? static_cast<int>(Num(*lm.Find(slot.Key))) : 0;
		p.c += Text(Panel::X(330), Panel::Y(750), "Legendary to keep, minimum legendary roll:", "ft_standard_12", 1, 1, "grey");
		p.Arrow(true, 142, 764, "leg-");
		p.Arrow(false, 388, 764, "leg+");
		p.Block(166, 764, 220, leg >= 0 ? g_Data.LegName(leg) : "(no legendary)", leg >= 0 ? "item_highlight" : "grey", "ft_standard_11");
		p.Block(442, 764, 48, lroll ? ">= " + std::to_string(lroll) + "%" : "any", (leg >= 0 && lroll) ? "white" : "grey", "ft_standard_11");
		if (leg >= 0) p.Steps(LegRolls(), lroll, 416, 492, 764, "lroll-", "lroll+");
		Json gd = RJ("gear_default"), ld = RJ("leg_default");
		bool isDefault = gd.Find(slot.Key) && sj::Dump(*gd.Find(slot.Key)) == sj::Dump(rows)
			&& sj::Dump(ld.Find(slot.Key) ? *ld.Find(slot.Key) : Json()) == sj::Dump(legJ ? *legJ : Json()) && !lroll;
		p.Box(136, 854, 523, 930);
		p.Button(246, 866, "Reset slot", "reset", false, isDefault);
	}
	else if (Page == "extras")
	{
		bool on = RB("keep_extras", false);
		p.Radio(576, on ? "KEEP {EXTRAS}" : "KEEP {EXTRAS} (off)", on ? "item_highlight" : "grey", on, "extras", true);
		p.c += Text(Panel::X(330), Panel::Y(622), "Skill levels (+Mastery) and reaper affinity:", "ft_standard_12", 1, 1, "grey");
		auto chosen = RInts("extras_skills");
		std::vector<std::tuple<std::string, bool, std::string>> cells;
		for (int sid : BuildSkills())
			cells.push_back({ g_Data.SkillName(g_Cls, sid), std::find(chosen.begin(), chosen.end(), sid) != chosen.end(),
				"xs:" + std::to_string(sid) });
		cells.push_back({ "Reaper Affinity", RB("extras_affinity", true), "xrp" });
		for (size_t i = 0; i < cells.size(); ++i)
			p.Radio(636 + 32.0 * (i / 2), std::get<0>(cells[i]), nullptr, std::get<1>(cells[i]), std::get<2>(cells[i]), false,
				190 + 160.0 * (i % 2), "ft_standard_11");
		double y0 = 636 + 32.0 * ((cells.size() + 1) / 2) + 12;
		p.c += Text(Panel::X(330), Panel::Y(y0), "Attribute points:", "ft_standard_12", 1, 1, "grey");
		auto attrs = RInts("extras_attrs");
		for (int i = 0; i < 8; ++i)
			p.Radio(y0 + 12 + 30.0 * (i / 2), bf::kTraits[i], nullptr, std::find(attrs.begin(), attrs.end(), i) != attrs.end(),
				"xa:" + std::to_string(i), false, 190 + 160.0 * (i % 2), "ft_standard_11");
		p.Box(136, 854, 523, 930);
		p.Wrapped(330, 892, "An item is kept when it has any ticked extra (and \"... it has an Extra\" is on in Rules).", 360, "grey");
	}
	else
	{
		p.Radio(580, "Pick up {Materials} from anywhere", "pure", RB("auto_collect", true), "collect");
		p.Radio(620, "Pick up {Gold} from anywhere", "item_highlight", RB("gold_pickup", true), "goldpick");
		p.Radio(660, "Show {BUILD / GOLD} tags on kept drops", "item_highlight", RB("tags", true), "tags");
		p.Wrapped(330, 740, "Materials and gold fly to you like normal pickups, so gold find and sounds are the game's own.", 340, "grey");
		p.Box(136, 854, 523, 930);
		p.c += Text(Panel::X(330), Panel::Y(878), "Turn filtered items into:", "ft_standard_12", 1, 2, "grey");
		auto lab = CONVERT_LABEL.find(RS("convert_to", "fragments"));
		p.Selector(890, lab != CONVERT_LABEL.end() ? lab->second : "Fragments", "conv-", "conv+", 170, 466, true);
	}
	// the panel's position arrives scaled (330, 156 at 1920x1080): our layout's offsets from it scale too
	return O({ { "vars", A({ ScaleVar(), A({ "dx", Op("-", { O({ { "$arg", 0 } }), S(330) }) }),
		A({ "dy", Op("-", { O({ { "$arg", 1 } }), S(156) }) }) }) },
		{ "calls", p.c.List } });
}

// ============================================================================
// Hooks and clicks
// ============================================================================

static bool g_PanelHooked = false;

static void Guard()
{
	Api(O({ { "cmd", "hook" }, { "owner", OWNER }, { "script", SINK }, { "capture", true }, { "depth", 1 },
		{ "capture_args", A({ "bf:*" }) },
		{ "overrides", A({ O({ { "args", A({ "bf:*" }) }, { "skip", true } }) }) } }));
}

static void ApplyUi(bool On, bool Ours, const std::string& Page, int SlotI)
{
	Guard();
	Api(O({ { "cmd", "hook" }, { "owner", OWNER }, { "script", "scr_ui_menus_inventory" }, { "capture", false },
		{ "after", InventoryProgram(On, Ours) } }));
	Json texts = A({ O({ { "args", A({ Json(), Json(), "Filter-o-tron 3000 Mk.II", Json(), Json(), "ft_standard_12" }) }, { "skip", true } }) });
	if (Ours)
		texts.Push(O({ { "args", A({ Json(), Json(), "Filter-o-tron 3000 Mk.II", Json(), Json(), "ft_standard_14" }) },
			{ "set_args", O({ { "2", "Build Filter" } }) } }));
	Api(O({ { "cmd", "hook" }, { "owner", OWNER }, { "script", "scr_draw_text" }, { "capture", false }, { "overrides", texts } }));
	if (Ours)
	{
		Api(O({ { "cmd", "hook" }, { "owner", OWNER }, { "script", "scr_blacksmith_panel_filter" }, { "capture", false },
			{ "overrides", A({ O({ { "args", Json::Array() }, { "skip", true } }) }) }, { "after", PanelProgram(On, Page, SlotI) } }));
		g_PanelHooked = true;
	}
	else if (g_PanelHooked)
	{
		Api(O({ { "cmd", "unhook" }, { "owner", OWNER }, { "script", "scr_blacksmith_panel_filter" } }));
		g_PanelHooked = false;
	}
}

// ============================================================================
// Actions
// ============================================================================

static void GearClick(const std::string& Slot, const std::string& Act)
{
	Json gear = RJ("gear");
	Json rows = gear.Find(Slot) ? *gear.Find(Slot) : Json::Array();
	while (rows.a.size() < 3) rows.Push(A({ Json(), Json(0) }));
	size_t k = static_cast<size_t>(Act[Act.size() - 2] - '0');
	if (k > 2) return;
	int step = Act.back() == '+' ? 1 : -1;
	if (Act.rfind("stat", 0) == 0)
	{
		std::vector<std::string> pool = { "" };
		auto it = g_Data.GoldPool.find(Slot);
		std::vector<std::string> stats = it != g_Data.GoldPool.end() ? it->second : std::vector<std::string>();
		// the build's stats for this slot first, then the build's other stats, then the rest by name
		std::set<std::string> here, build;
		Json gd = RJ("gear_default");
		if (const Json* r = gd.Find(Slot))
			for (auto& x : r->a) if (x.a.size() && x.a[0].type == Json::Str) here.insert(x.a[0].s);
		if (const Json* b = RF("build_stats")) for (auto& x : b->a) build.insert(Str(x));
		std::sort(stats.begin(), stats.end(), [&](const std::string& a, const std::string& b)
		{
			int ra = here.count(a) ? 0 : build.count(a) ? 1 : 2, rb = here.count(b) ? 0 : build.count(b) ? 1 : 2;
			return ra != rb ? ra < rb : g_Data.StatName(a) < g_Data.StatName(b);
		});
		pool.insert(pool.end(), stats.begin(), stats.end());
		std::set<std::string> taken;
		for (size_t i = 0; i < rows.a.size(); ++i)
			if (i != k && rows.a[i].a.size() && rows.a[i].a[0].type == Json::Str) taken.insert(rows.a[i].a[0].s);
		std::string cur = rows.a[k].a.size() && rows.a[k].a[0].type == Json::Str ? rows.a[k].a[0].s : "";
		auto pos = std::find(pool.begin(), pool.end(), cur);
		long long i = pos == pool.end() ? 0 : pos - pool.begin();
		long long n = static_cast<long long>(pool.size());
		for (long long t = 0; t < n; ++t)
		{
			i = ((i + step) % n + n) % n;
			if (!taken.count(pool[static_cast<size_t>(i)])) break;
		}
		const std::string& s = pool[static_cast<size_t>(i)];
		double keep = s.empty() ? 0 : Num(rows.a[k].a.size() > 1 ? rows.a[k].a[1] : Json(0));
		rows.a[k] = A({ s.empty() ? Json() : Json(s), keep });
	}
	else
	{
		int cur = static_cast<int>(Num(rows.a[k].a.size() > 1 ? rows.a[k].a[1] : Json(0)));
		rows.a[k] = A({ rows.a[k].a.size() ? rows.a[k].a[0] : Json(), StepIn(GoldRolls(), cur, step) });
	}
	gear.Set(Slot, rows);
	g_Rules.Set("gear", gear);
}

static void ToggleInList(const char* Key, int Value)
{
	auto v = RInts(Key);
	auto it = std::find(v.begin(), v.end(), Value);
	if (it == v.end()) v.push_back(Value); else v.erase(it);
	std::sort(v.begin(), v.end());
	g_Rules.Set(Key, Ints(v));
}

// ============================================================================
// Main loop (once per frame, from obj_ui_next_gen's Step)
// ============================================================================

struct State
{
	bool Ready = false, Failed = false;
	double Ui = 0;
	bool Ours = false;
	std::string Page = "rules";
	int SlotI = 0;
	std::string Applied;
	double NextPoll = 0, CheckHooksAt = 0, NextScan = 0, RetryAt = 0;
	bool InventoryOpen = false;
	std::string Show, Citizen;
};
static State g_S;

static double Now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static bool GameFocused()
{
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

static uint64_t g_LootSub = 0;   // SlormAPI event: a drop is checked the moment it lands

static void OnLoot(const char*, const char*, void*) { g_S.NextScan = 0; }

// Settings in Options > Mods (SlormAPI): the on/off hotkey and the loot log
static int g_Hotkey = 0;
static uint64_t g_SettingSub = 0;

static int HotkeyVk(const std::string& Name)   // "F5".."F12", "Off" = 0
{
	if (Name.size() < 2 || Name[0] != 'F') return 0;
	int n = atoi(Name.c_str() + 1);
	return n >= 1 && n <= 12 ? VK_F1 + n - 1 : 0;
}

static void ApplySetting(const std::string& Key, const Json& Value)
{
	if (Key == "hotkey" && Value.type == Json::Str)
	{
		g_Hotkey = HotkeyVk(Value.s);
		if (g_Hotkey) GetAsyncKeyState(g_Hotkey);              // forget a press from before
	}
	else if (Key == "loot_log" && Value.type == Json::Bool) g_LootLog = Value.b;
}

static void OnSetting(const char*, const char* DataJson, void*)
{
	try
	{
		Json d = sj::Parse(DataJson ? DataJson : "");
		if (d.GetStr("mod") == "BuildFilter")
			if (const Json* v = d.Find("value")) ApplySetting(d.GetStr("key"), *v);
	}
	catch (const std::exception&) {}
}

static void Init()
{
	AurieInterfaceBase* base = nullptr;
	if (!AurieSuccess(ObGetInterface(SLORMAPI_INTERFACE_NAME, base)) || !base)
		throw std::runtime_error("SlormAPI is not loaded (BuildFilter needs it in mods/aurie)");
	g_Api = static_cast<SlormAPIInterface*>(base);
	if (!g_Data.Loaded) g_Data.Load();
	LoadRules();
	Json ui = ApiInstances("obj_ui_next_gen");
	if (ui.a.empty()) throw std::runtime_error("no UI yet");
	g_S.Ui = Num(ui.a[0].At("$inst"));
	g_Sink = Num(ApiCall("asset_get_index", { Json(SINK) }));
	if (Str(ApiCall("script_get_name", { Json(g_Sink) })) != SINK) throw std::runtime_error("click sink script not found");
	Api(O({ { "cmd", "hooklog" }, { "owner", OWNER }, { "max", 1000000 } }));   // drop stale entries
	// our line on SlormAPI's Mods page, drops the moment they land, and our settings in Options > Mods
	Api(O({ { "cmd", "register_mod" }, { "name", "BuildFilter" }, { "version", kVersion }, { "author", "thothas" },
		{ "file", "BuildFilter.dll" }, { "description", "Loot filter for your build: inventory > Build Filter" },
		{ "requires", O({ { "SlormAPI", "1.0.0" } }) }, { "game", "1.0.2c" } }));
	if (!g_LootSub) g_LootSub = g_Api->Subscribe("loot_dropped", OnLoot, nullptr);
	{
		Json v = Api(O({ { "cmd", "register_settings" }, { "mod", "BuildFilter" }, { "settings", A({
			O({ { "key", "hotkey" }, { "label", "Hotkey: Build Filter on / off" }, { "help", "Works while the game window has the focus" },
				{ "type", "choice" }, { "options", A({ "Off", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" }) }, { "default", "Off" } }),
			O({ { "key", "loot_log" }, { "label", "Loot log" }, { "help", "Write every kept / converted drop to loot_log.txt" },
				{ "type", "bool" }, { "default", true } }) }) } }));
		for (auto& [k, val] : v.o) ApplySetting(k, val);
		if (!g_SettingSub) g_SettingSub = g_Api->Subscribe("setting_changed", OnSetting, nullptr);
	}
	if (g_Hotkey) GetAsyncKeyState(g_Hotkey);
	g_S.Ready = true;
	g_S.Applied.clear();
	DbgPrintEx(LOG_SEVERITY_INFO, "[BuildFilter] %s ready (settings: %s)", kVersion, RulesPath().string().c_str());
}

static void HandleAction(const std::string& Act)
{
	const auto& slot = bf::Slots()[g_S.SlotI].Key;
	if (Act == "open")
	{
		g_S.Ours = true;
		ApplyUi(RB("enabled", false), true, g_S.Page, g_S.SlotI);      // our page first, then the panel opens
		if (Str(ApiGet("menu_citizen", Inst(g_S.Ui))) != "blacksmith") ApiCall(SINK, {}, Inst(g_S.Ui));
	}
	else if (Act == "game") g_S.Ours = false;
	else if (Act == "toggle") g_Rules.Set("enabled", Json(!RB("enabled", false)));
	else if (Act == "gold-" || Act == "gold+")
		g_Rules.Set("min_gold", Json((std::max)(1, (std::min)(3, static_cast<int>(RN("min_gold", 3)) + (Act == "gold+" ? 1 : -1)))));
	else if (Act == "collect") g_Rules.Set("auto_collect", Json(!RB("auto_collect", true)));
	else if (Act == "goldpick") g_Rules.Set("gold_pickup", Json(!RB("gold_pickup", true)));
	else if (Act == "tags") g_Rules.Set("tags", Json(!RB("tags", true)));
	else if (Act == "pure") g_Rules.Set("keep_pure", Json(!RB("keep_pure", false)));
	else if (Act == "extras") g_Rules.Set("keep_extras", Json(!RB("keep_extras", false)));
	else if (Act == "legall") g_Rules.Set("keep_all_legendaries", Json(!RB("keep_all_legendaries", true)));
	else if (Act == "xrp") g_Rules.Set("extras_affinity", Json(!RB("extras_affinity", true)));
	else if (Act.rfind("xs:", 0) == 0) ToggleInList("extras_skills", std::stoi(Act.substr(3)));
	else if (Act.rfind("xa:", 0) == 0) ToggleInList("extras_attrs", std::stoi(Act.substr(3)));
	else if (Act == "conv-" || Act == "conv+")
	{
		auto it = std::find(CONVERT_TO.begin(), CONVERT_TO.end(), RS("convert_to", "fragments"));
		long long i = it == CONVERT_TO.end() ? 0 : it - CONVERT_TO.begin(), n = static_cast<long long>(CONVERT_TO.size());
		i = ((i + (Act == "conv+" ? 1 : -1)) % n + n) % n;
		g_Rules.Set("convert_to", Json(CONVERT_TO[static_cast<size_t>(i)]));
	}
	else if (Act.rfind("page:", 0) == 0) g_S.Page = Act.substr(5);
	else if (Act == "slot-" || Act == "slot+")
	{
		int n = static_cast<int>(bf::Slots().size());
		g_S.SlotI = ((g_S.SlotI + (Act == "slot+" ? 1 : -1)) % n + n) % n;
	}
	else if (Act.rfind("stat", 0) == 0 || Act.rfind("min", 0) == 0) GearClick(slot, Act);
	else if (Act == "leg-" || Act == "leg+")
	{
		std::vector<int> pool = { -1 };
		for (int id : g_Data.LegendaryPool(slot, g_Cls)) pool.push_back(id);
		Json lw = RJ("leg_wanted");
		const Json* cur = lw.Find(slot);
		int c = cur && cur->type == Json::Num ? static_cast<int>(cur->n) : -1;
		auto it = std::find(pool.begin(), pool.end(), c);
		long long i = it == pool.end() ? 0 : it - pool.begin(), n = static_cast<long long>(pool.size());
		i = ((i + (Act == "leg+" ? 1 : -1)) % n + n) % n;
		lw.Set(slot, pool[static_cast<size_t>(i)] < 0 ? Json() : Json(pool[static_cast<size_t>(i)]));
		g_Rules.Set("leg_wanted", lw);
	}
	else if (Act == "lroll-" || Act == "lroll+")
	{
		Json lm = RJ("leg_min_roll");
		int cur = lm.Find(slot) ? static_cast<int>(Num(*lm.Find(slot))) : 0;
		lm.Set(slot, Json(StepIn(LegRolls(), cur, Act == "lroll+" ? 1 : -1)));
		g_Rules.Set("leg_min_roll", lm);
	}
	else if (Act == "lvl-" || Act == "lvl+")
		g_Rules.Set("min_level", Json(StepIn(Levels(), static_cast<int>(RN("min_level", 0)), Act == "lvl+" ? 1 : -1)));
	else if (Act == "reset")
	{
		Json gear = RJ("gear"), gd = RJ("gear_default"), lw = RJ("leg_wanted"), ld = RJ("leg_default"), lm = RJ("leg_min_roll");
		gear.Set(slot, gd.Find(slot) ? *gd.Find(slot) : Json::Array());
		lw.Set(slot, ld.Find(slot) ? *ld.Find(slot) : Json());
		lm.Set(slot, Json(0));
		g_Rules.Set("gear", gear);
		g_Rules.Set("leg_wanted", lw);
		g_Rules.Set("leg_min_roll", lm);
	}
	SaveRules();
}

static bool OurHooksPresent()
{
	std::set<std::string> have;
	for (auto& h : Api(O({ { "cmd", "hooks" } })).a)
	{
		bool ours = false;                                       // one layer per owner on each hooked script
		if (const Json* layers = h.Find("layers"))
			for (auto& l : layers->a) ours |= l.GetStr("owner") == OWNER;
		if (ours) have.insert(h.GetStr("script"));
	}
	for (const char* s : { SINK, "scr_ui_menus_inventory", "scr_draw_text" })
		if (!have.count(std::string("gml_Script_") + s)) return false;
	return !g_S.Ours || have.count("gml_Script_scr_blacksmith_panel_filter");
}

static void Tick()
{
	double now = Now();
	if (!g_S.Ready)
	{
		if (now < g_S.RetryAt) return;
		try { Init(); }
		catch (const std::exception& e)
		{
			if (!g_S.Failed) DbgPrintEx(LOG_SEVERITY_WARNING, "[BuildFilter] waiting: %s", e.what());
			g_S.Failed = true;
			g_S.RetryAt = now + 3;
			return;
		}
	}
	try
	{
		bool f8 = g_Hotkey && (GetAsyncKeyState(g_Hotkey) & 1) && GameFocused();
		// poll every frame while the inventory is open (clicks), else 10 times a second: one batched request
		if (g_S.InventoryOpen || now >= g_S.NextPoll || f8)
		{
			g_S.NextPoll = now + 0.1;
			Json r = Api(O({ { "cmd", "batch" }, { "cmds", A({
				O({ { "cmd", "get" }, { "path", "current_hero" } }),
				O({ { "cmd", "get" }, { "path", "menu_show" }, { "root", Inst(g_S.Ui) } }),
				O({ { "cmd", "get" }, { "path", "menu_citizen" }, { "root", Inst(g_S.Ui) } }),
				O({ { "cmd", "hooklog" }, { "owner", OWNER }, { "max", 1000 } }) }) } }));
			for (auto& sub : r.a)
				if (!sub.GetBool("ok", false)) throw std::runtime_error(sub.GetStr("error", "SlormAPI error"));
			g_Cls = static_cast<int>(Num(r.a[0].At("result")));
			g_S.Show = Str(r.a[1].At("result"));
			g_S.Citizen = Str(r.a[2].At("result"));
			g_S.InventoryOpen = g_S.Show == "inventory";
			bool acted = false;
			if (const Json* e = r.a[3].At("result").Find("entries"))
				for (auto& x : e->a)
					if (const Json* args = x.Find("args"); args && !args->a.empty() && args->a[0].type == Json::Str
						&& args->a[0].s.rfind("bf:", 0) == 0)
					{
						HandleAction(args->a[0].s.substr(3));
						acted = true;
					}
			if (f8)
			{
				g_Rules.Set("enabled", Json(!RB("enabled", false)));
				SaveRules();
			}
			// (the menu state was read before this frame's clicks: an "open" is re-checked on the next frame)
			if (!acted && g_S.Ours && (g_S.Show != "inventory" || g_S.Citizen != "blacksmith")) g_S.Ours = false;
			bool on = RB("enabled", false);
			std::string counts;
			if (g_S.Ours)
				for (auto& [k, v] : g_Counts) counts += std::to_string(v) + ",";
			std::string state = std::string(on ? "1" : "0") + (g_S.Ours ? "1" : "0") + g_S.Page + "," + std::to_string(g_S.SlotI)
				+ "," + std::to_string(g_RulesVer) + "," + counts;
			bool missing = now >= g_S.CheckHooksAt && (g_S.CheckHooksAt = now + 2, !OurHooksPresent());
			if (state != g_S.Applied || missing)
			{
				ApplyUi(on, g_S.Ours, g_S.Page, g_S.SlotI);
				if (g_S.InventoryOpen) ApiSet("rebake_menu", Json(true), Inst(g_S.Ui));
				g_S.Applied = state;
			}
		}
		if (RB("enabled", false) && now >= g_S.NextScan)
		{
			g_S.NextScan = now + 0.25;
			Json heroes = ApiInstances("obj_parent_hero");
			if (!heroes.a.empty() && Scan(Num(heroes.a[0].At("$inst")))) g_S.NextScan = now;   // backlog: next frame
		}
	}
	catch (const std::exception& e)
	{
		DbgPrintEx(LOG_SEVERITY_WARNING, "[BuildFilter] %s", e.what());
		g_S.Ready = false;                                // re-init (e.g. the UI object was recreated)
		g_S.RetryAt = now + 2;
	}
}

// optional profiler: an empty file mods/aurie/BuildFilter/profile -> the tick's cost in the Aurie log every 10 s
static bool g_Profile = false;
static double g_ProfSum = 0, g_ProfMax = 0, g_ProfAt = 0;
static int g_ProfN = 0;

static void CodeEventCallback(FWCodeEvent& Context)
{
	auto& args = Context.Arguments();
	CCode* code = std::get<2>(args);
	if (!code || !code->GetName() || strcmp(code->GetName(), "gml_Object_obj_ui_next_gen_Step_0") != 0) return;
	if (!g_Profile) { Tick(); return; }
	double t0 = Now();
	Tick();
	double us = (Now() - t0) * 1e6;
	g_ProfSum += us;
	g_ProfMax = (std::max)(g_ProfMax, us);
	++g_ProfN;
	if (t0 >= g_ProfAt)
	{
		if (g_ProfN > 1)
			DbgPrintEx(LOG_SEVERITY_INFO, "[BuildFilter] profile: %d ticks, avg %.0f us, max %.0f us", g_ProfN,
				g_ProfSum / g_ProfN, g_ProfMax);
		g_ProfSum = g_ProfMax = 0;
		g_ProfN = 0;
		g_ProfAt = t0 + 10;
	}
}

EXPORTED AurieStatus ModuleInitialize(IN AurieModule* Module, IN const fs::path& ModulePath)
{
	g_Yytk = YYTK::GetInterface();
	if (!g_Yytk) return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;
	g_Dir = ModulePath.parent_path() / "BuildFilter";
	std::error_code ec;
	std::filesystem::create_directories(g_Dir, ec);
	g_Profile = std::filesystem::exists(g_Dir / "profile", ec);
	AurieStatus st = g_Yytk->CreateCallback(Module, EVENT_OBJECT_CALL, reinterpret_cast<PVOID>(CodeEventCallback), 0);
	if (!AurieSuccess(st)) return st;
	DbgPrintEx(LOG_SEVERITY_INFO, "[BuildFilter] %s loaded", kVersion);
	return AURIE_SUCCESS;
}

EXPORTED AurieStatus ModuleUnload(IN AurieModule* Module, IN const fs::path& ModulePath)
{
	UNREFERENCED_PARAMETER(ModulePath);
	if (g_Yytk) g_Yytk->RemoveCallback(Module, reinterpret_cast<PVOID>(CodeEventCallback));
	return AURIE_SUCCESS;
}
