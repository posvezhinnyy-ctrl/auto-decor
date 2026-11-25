#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace geode::prelude;

namespace {

// ---------- palettes ----------

struct Palette {
	char const* name;
	ccColor3B bg, ground, a, b, dim, soft;
};

std::vector<Palette> const& palettes() {
	static std::vector<Palette> p = {
		{ "Neon",   {10, 8, 30},  {20, 10, 50}, {0, 255, 230},  {255, 40, 200}, {90, 60, 200},  {255, 255, 255} },
		{ "Sunset", {35, 10, 30}, {60, 15, 40}, {255, 150, 50}, {255, 60, 110}, {150, 40, 120}, {255, 230, 180} },
		{ "Ocean",  {5, 20, 45},  {5, 35, 70},  {60, 200, 255}, {80, 120, 255}, {30, 90, 160},  {200, 245, 255} },
		{ "Toxic",  {8, 25, 10},  {12, 40, 15}, {150, 255, 60}, {255, 235, 60}, {40, 120, 40},  {230, 255, 200} },
		{ "Ember",  {25, 8, 8},   {45, 12, 10}, {255, 90, 30},  {255, 200, 60}, {140, 40, 30},  {255, 230, 200} },
	};
	return p;
}

// Custom color channels used by the decoration (high numbers to avoid clashing with yours).
constexpr int CH_A    = 990;
constexpr int CH_B    = 991;
constexpr int CH_DIM  = 992;
constexpr int CH_BIG  = 993;
constexpr int CH_SOFT = 994;

// Z layers: B4 is furthest back.
constexpr int Z_B4 = -3;
constexpr int Z_B3 = -1;
constexpr int Z_B1 = 3;

// ---------- helpers ----------

std::vector<int> parseIDs(std::string const& text) {
	std::vector<int> ids;
	std::stringstream ss(text);
	std::string part;
	while (std::getline(ss, part, ',')) {
		auto res = utils::numFromString<int>(utils::string::trim(part));
		if (res.isOk() && res.unwrap() > 0) ids.push_back(res.unwrap());
	}
	return ids;
}

long long cellKey(int cx, int cy) {
	return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned int>(cy);
}

// One decoration object as a level-string entry.
// 121 = no touch, 134 = passable, so nothing placed here can affect gameplay.
std::string deco(int id, float x, float y, float rot, float scale, int ch, int z, int order, bool blend) {
	return fmt::format(
		"1,{},2,{:.1f},3,{:.1f},6,{:.1f},21,{},22,{},24,{},25,{},128,{:.2f},129,{:.2f},17,{},103,1,121,1,134,1;",
		id, x, y, rot, ch, ch, z, order, scale, scale, blend ? 1 : 0
	);
}

// Color trigger (object 899) that sets a channel to a color.
std::string colorTrigger(float x, int channel, ccColor3B c, float opacity, float duration) {
	return fmt::format(
		"1,899,2,{:.1f},3,-30,7,{},8,{},9,{},10,{:.2f},23,{},35,{:.2f};",
		x, static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b), duration, channel, opacity
	);
}

std::string paletteTriggers(float x, Palette const& p, float duration, bool recolorBg) {
	std::string s;
	s += colorTrigger(x, CH_A, p.a, 1.f, duration);
	s += colorTrigger(x, CH_B, p.b, 1.f, duration);
	s += colorTrigger(x, CH_DIM, p.dim, 0.7f, duration);
	s += colorTrigger(x, CH_BIG, p.b, 0.18f, duration);
	s += colorTrigger(x, CH_SOFT, p.soft, 0.9f, duration);
	if (recolorBg) {
		s += colorTrigger(x, 1000, p.bg, 1.f, duration);     // background
		s += colorTrigger(x, 1001, p.ground, 1.f, duration); // ground
		s += colorTrigger(x, 1002, p.a, 1.f, duration);      // line
	}
	return s;
}

} // namespace

class $modify(AutoDecorUI, EditorUI) {
	bool init(LevelEditorLayer* layer) {
		if (!EditorUI::init(layer)) return false;

		auto menu = CCMenu::create();
		menu->setID("auto-decor-menu"_spr);

		auto spr = ButtonSprite::create("Decor");
		auto btn = CCMenuItemSpriteExtra::create(
			spr, this, menu_selector(AutoDecorUI::onAutoDecor)
		);
		btn->setID("auto-decor-button"_spr);
		menu->addChild(btn);

		auto win = CCDirector::get()->getWinSize();
		menu->setPosition({ win.width - 60.f, win.height - 60.f });
		this->addChild(menu, 100);

		return true;
	}

	void onAutoDecor(CCObject*) {
		auto mod = Mod::get();

		auto shapes = parseIDs(mod->getSettingValue<std::string>("shape-ids"));
		if (shapes.empty()) {
			FLAlertLayer::create("Auto Decor", "No valid shape IDs in the mod settings.", "OK")->show();
			return;
		}

		int chainId     = static_cast<int>(mod->getSettingValue<int64_t>("chain-id"));
		float density   = mod->getSettingValue<int64_t>("density") / 100.f;
		float particles = mod->getSettingValue<int64_t>("particles") / 100.f;
		int maxObjects  = static_cast<int>(mod->getSettingValue<int64_t>("max-objects"));
		bool bigShapes  = mod->getSettingValue<bool>("big-shapes");
		bool shift      = mod->getSettingValue<bool>("color-shift");
		bool recolorBg  = mod->getSettingValue<bool>("recolor-bg");
		float interval  = static_cast<float>(mod->getSettingValue<int64_t>("color-shift-interval"));
		auto paletteSetting = mod->getSettingValue<std::string>("palette");

		// ---- read the level ----
		std::unordered_set<long long> occupied;
		std::vector<std::pair<int, int>> solids;
		float levelEnd = 0.f, topY = 0.f;

		for (auto obj : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
			float x = obj->getPositionX();
			float y = obj->getPositionY();
			int cx = static_cast<int>(std::floor(x / 30.f));
			int cy = static_cast<int>(std::floor(y / 30.f));
			occupied.insert(cellKey(cx, cy));
			if (obj->m_objectType == GameObjectType::Solid) solids.push_back({ cx, cy });
			levelEnd = std::max(levelEnd, x);
			topY = std::max(topY, y);
		}
		levelEnd = levelEnd < 300.f ? 3000.f : levelEnd + 300.f;
		topY = std::max(topY, 300.f);

		std::mt19937 rng{ std::random_device{}() };
		auto roll = [&] { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); };
		auto range = [&](float a, float b) { return a + (b - a) * roll(); };
		auto pickShape = [&] { return shapes[static_cast<size_t>(roll() * shapes.size()) % shapes.size()]; };
		auto pickColorCh = [&] { return roll() < 0.5f ? CH_A : CH_B; };

		std::string out;
		int n = 0;
		int order = 0;

		// ---- 1) decoration on the edges of existing blocks ----
		for (auto [cx, cy] : solids) {
			if (n >= maxObjects) break;
			float px = cx * 30.f + 15.f;
			float py = cy * 30.f + 15.f;

			// glowing studs on top
			if (!occupied.count(cellKey(cx, cy + 1)) && roll() < density) {
				float s = range(0.2f, 0.5f);
				out += deco(pickShape(), px + range(-9.f, 9.f), py + 15.f + s * 15.f,
					roll() < 0.5f ? 45.f : 0.f, s, pickColorCh(), Z_B1, order++ % 20, true);
				occupied.insert(cellKey(cx, cy + 1));
				n++;
			}

			// small sparks on the sides
			for (int dx : { -1, 1 }) {
				if (n >= maxObjects) break;
				if (!occupied.count(cellKey(cx + dx, cy)) && roll() < density * 0.3f) {
					float s = range(0.15f, 0.35f);
					out += deco(pickShape(), px + dx * (15.f + s * 15.f), py + range(-8.f, 8.f),
						45.f, s, CH_SOFT, Z_B1, order++ % 20, true);
					occupied.insert(cellKey(cx + dx, cy));
					n++;
				}
			}

			// hanging chains under platforms
			if (chainId > 0 && !occupied.count(cellKey(cx, cy - 1)) && roll() < density * 0.4f) {
				out += deco(chainId, px, py - 45.f, 0.f, range(0.8f, 1.1f),
					CH_DIM, Z_B3, order++ % 20, false);
				occupied.insert(cellKey(cx, cy - 1));
				occupied.insert(cellKey(cx, cy - 2));
				n++;
			}
		}

		// ---- 2) floating particles behind everything ----
		if (particles > 0.f) {
			for (float x = 0.f; x <= levelEnd && n < maxObjects; x += 30.f) {
				if (roll() > particles) continue;
				float s = range(0.12f, 0.35f);
				out += deco(pickShape(), x + range(0.f, 30.f), range(30.f, topY + 300.f),
					45.f, s, roll() < 0.6f ? CH_SOFT : CH_A, Z_B3, order++ % 20, true);
				n++;
			}
		}

		// ---- 3) big translucent shapes for depth ----
		if (bigShapes) {
			for (float x = 0.f; x <= levelEnd && n < maxObjects; x += range(250.f, 650.f)) {
				float s = range(3.f, 8.f);
				out += deco(shapes.front(), x, range(60.f, topY + 200.f),
					45.f, s, CH_BIG, Z_B4, order++ % 20, true);
				n++;
			}
		}

		// ---- 4) color triggers (palette now, and shifts along the level) ----
		auto const& pal = palettes();
		size_t idx = 0;
		bool random = paletteSetting == "Random";
		if (random) {
			idx = static_cast<size_t>(roll() * pal.size()) % pal.size();
		} else {
			for (size_t i = 0; i < pal.size(); i++) {
				if (paletteSetting == pal[i].name) idx = i;
			}
		}
		out += paletteTriggers(0.f, pal[idx], 0.f, recolorBg);

		if (shift) {
			for (float x = interval; x < levelEnd; x += interval) {
				if (random) {
					size_t next = idx;
					while (next == idx) next = static_cast<size_t>(roll() * pal.size()) % pal.size();
					idx = next;
				} else {
					idx = (idx + 1) % pal.size();
				}
				out += paletteTriggers(x, pal[idx], 2.f, recolorBg);
			}
		}

		// ---- place everything and select it ----
		auto created = m_editorLayer->createObjectsFromString(out, true, true);
		int total = created ? static_cast<int>(created->count()) : 0;
		if (total > 0) this->selectObjects(created, false);

		Notification::create(
			fmt::format("Auto Decor added {} objects and triggers", total),
			total > 0 ? NotificationIcon::Success : NotificationIcon::Warning
		)->show();
	}
};
