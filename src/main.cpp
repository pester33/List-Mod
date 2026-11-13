#include <Geode/Geode.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/LazySprite.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/random.hpp>

using namespace geode::prelude;

// The list site embeds its full level list as JSON on the Roulette page:
// <script id="roulette-data" type="application/json">[ ... ]</script>
static constexpr auto LIST_SITE = "http://gdgl.x10.mx/gdgaminglist/";
static constexpr auto SITE_ROOT = "http://gdgl.x10.mx";
static constexpr auto DATA_PAGE = "http://gdgl.x10.mx/gdgaminglist/roulette";
static constexpr auto DATA_MARKER = "id=\"roulette-data\"";
static constexpr auto DATA_END = "</script>";
// Backup copy of the list, refreshed by a GitHub Action every 30 minutes.
// Used when the site can't be reached or sends back a different page (some networks do that).
static constexpr auto MIRROR_URL = "https://raw.githubusercontent.com/pester33/List-Mod/main/data/levels.json";

// Each GD version has its own release, so the updater never installs a build for the wrong GD version
#if GEODE_COMP_GD_VERSION >= 22091
static constexpr auto RELEASE_BASE = "https://github.com/pester33/List-Mod/releases/download/latest-build-2.2091/";
#else
static constexpr auto RELEASE_BASE = "https://github.com/pester33/List-Mod/releases/download/latest-build/";
#endif

// Newest version seen on GitHub this session (filled by any update check)
static std::optional<VersionInfo> s_latestKnown;
// Set once an update was downloaded this session (it loads after a restart)
static bool s_updateInstalled = false;

static void checkForUpdate(bool manual, bool isRetry);
static void installUpdate(std::string version);

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

static constexpr ccColor3B COL_DARK = { 61, 34, 14 };      // header / bars
static constexpr ccColor3B COL_FIELD = { 42, 23, 8 };      // search box / info box
static constexpr ccColor3B COL_MAIN = { 122, 69, 32 };     // main list rows
static constexpr ccColor3B COL_EXT = { 59, 91, 150 };      // extended list rows
static constexpr ccColor3B COL_GOLD = { 255, 210, 63 };
static constexpr ccColor3B COL_SILVER = { 207, 214, 222 };
static constexpr ccColor3B COL_BRONZE = { 216, 137, 74 };
static constexpr ccColor3B COL_MUTED = { 217, 194, 163 };
static constexpr ccColor3B COL_LABEL = { 201, 176, 138 };

static ccColor4F toColor4F(ccColor3B c, float a = 1.f) {
    return { c.r / 255.f, c.g / 255.f, c.b / 255.f, a };
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct ListLevel {
    int siteID = 0;
    int position = 0;
    std::string name;
    std::string creator;
    std::string publishedBy;
    std::string verifiedBy;
    std::string byline;
    std::string bucket; // "main", "extended", ...
    std::string levelID;
    std::string videoUrl;
    std::string thumbUrl;
    std::string pageUrl;
    std::string score;
};

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// "40.00" -> "40", "12.50" -> "12.5"
static std::string tidyNumber(std::string n) {
    if (n.find('.') != std::string::npos) {
        while (!n.empty() && n.back() == '0') n.pop_back();
        if (!n.empty() && n.back() == '.') n.pop_back();
    }
    return n;
}

// "40.00 (68%) - 400.00 (100%) points" -> "400 max, 40 at 68%"
static std::string formatScore(std::string const& score) {
    auto dash = score.find(" - ");
    if (dash == std::string::npos) return score;
    auto minPart = score.substr(0, dash);
    auto maxPart = score.substr(dash + 3);
    auto minNum = tidyNumber(minPart.substr(0, minPart.find(' ')));
    auto maxNum = tidyNumber(maxPart.substr(0, maxPart.find(' ')));
    auto open = minPart.find('(');
    auto close = minPart.find(')');
    if (open == std::string::npos || close == std::string::npos || maxNum.empty()) return score;
    auto minPercent = minPart.substr(open + 1, close - open - 1);
    return fmt::format("{} max, {} at {}", maxNum, minNum, minPercent);
}

// "published by A, verified by B" -> A, B
static void splitByline(ListLevel& lvl) {
    auto const& b = lvl.byline;
    constexpr std::string_view PUB = "published by ";
    constexpr std::string_view VER = "verified by ";
    auto pub = b.find(PUB);
    auto ver = b.find(VER);
    if (pub != std::string::npos) {
        auto start = pub + PUB.size();
        auto end = ver != std::string::npos ? b.rfind(',', ver) : std::string::npos;
        lvl.publishedBy = b.substr(start, end == std::string::npos || end < start ? std::string::npos : end - start);
    }
    if (ver != std::string::npos) lvl.verifiedBy = b.substr(ver + VER.size());
    if (lvl.publishedBy.empty()) lvl.publishedBy = lvl.creator;
}

// Turns the list JSON (an array of levels) into levels
static Result<std::vector<ListLevel>> parseLevels(std::string_view text) {
    auto parsed = matjson::parse(text);
    if (!parsed) return Err("List data isn't valid JSON");
    auto json = std::move(parsed).unwrap();
    if (!json.isArray() || json.size() == 0) return Err("List data is empty");

    auto str = [](matjson::Value const& v) -> std::string {
        if (v.isString()) return v.asString().unwrapOr("");
        if (v.isNumber()) return std::to_string(v.asInt().unwrapOr(0));
        return "";
    };

    std::vector<ListLevel> levels;
    for (auto& entry : json) {
        if (!entry["shown"].asBool().unwrapOr(true)) continue;
        ListLevel lvl;
        lvl.siteID = static_cast<int>(entry["id"].asInt().unwrapOr(0));
        lvl.position = static_cast<int>(entry["position"].asInt().unwrapOr(0));
        lvl.name = str(entry["name"]);
        if (lvl.name.empty()) lvl.name = "Unknown";
        lvl.creator = str(entry["creator"]);
        lvl.byline = str(entry["byline"]);
        if (lvl.byline.empty() && !lvl.creator.empty()) lvl.byline = "by " + lvl.creator;
        lvl.bucket = str(entry["bucket"]);
        if (lvl.bucket.empty()) lvl.bucket = "main";
        lvl.levelID = str(entry["levelId"]);
        if (lvl.levelID == "0") lvl.levelID.clear();
        lvl.videoUrl = str(entry["videoUrl"]);
        lvl.thumbUrl = str(entry["thumb"]);
        lvl.pageUrl = str(entry["url"]);
        lvl.score = str(entry["score"]);
        splitByline(lvl);
        levels.push_back(std::move(lvl));
    }
    std::stable_sort(levels.begin(), levels.end(), [](auto const& a, auto const& b) {
        return a.position < b.position;
    });
    return Ok(std::move(levels));
}

// Finds the embedded list JSON on the site's Roulette page
static Result<std::vector<ListLevel>> parseListPage(std::string const& html) {
    auto marker = html.find(DATA_MARKER);
    if (marker == std::string::npos) return Err("List data not found on the page");
    auto start = html.find('>', marker);
    if (start == std::string::npos) return Err("List data is cut off");
    start += 1;
    auto end = html.find(DATA_END, start);
    if (end == std::string::npos) return Err("List data is cut off");
    return parseLevels(std::string_view(html).substr(start, end - start));
}

// ---------------------------------------------------------------------------
// Favourites (saved per level, by the site's own level id)
// ---------------------------------------------------------------------------

static std::string favKey(ListLevel const& lvl) {
    return lvl.siteID > 0 ? fmt::format("fav-{}", lvl.siteID) : "fav-name-" + lower(lvl.name);
}
static bool isFavorite(ListLevel const& lvl) {
    return Mod::get()->getSavedValue<bool>(favKey(lvl), false);
}
static void setFavorite(ListLevel const& lvl, bool fav) {
    Mod::get()->setSavedValue<bool>(favKey(lvl), fav);
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------

static CCScale9Sprite* panel(CCSize size, ccColor3B color, GLubyte opacity = 255) {
    auto spr = CCScale9Sprite::create("square02b_001.png");
    spr->setColor(color);
    spr->setOpacity(opacity);
    spr->setContentSize(size);
    return spr;
}

// Coloured circle with a number in it (gold / silver / bronze for the top 3)
static CCNode* rankBadge(ListLevel const& lvl, float radius) {
    // #1 gets the trophy
    if (lvl.bucket == "main" && lvl.position == 1) {
        if (auto trophy = CCSprite::create("gdgl_trophy.png"_spr)) {
            trophy->setScale(radius * 2.3f / trophy->getContentSize().width);
            return trophy;
        }
    }
    ccColor3B fill = lvl.bucket == "extended" ? ccColor3B{ 36, 64, 111 } : ccColor3B{ 74, 40, 16 };
    ccColor3B text = { 255, 255, 255 };
    if (lvl.bucket == "main" && lvl.position == 1) { fill = COL_GOLD; text = { 61, 34, 14 }; }
    if (lvl.bucket == "main" && lvl.position == 2) { fill = COL_SILVER; text = { 44, 44, 42 }; }
    if (lvl.bucket == "main" && lvl.position == 3) { fill = COL_BRONZE; text = { 61, 34, 14 }; }

    auto node = CCNode::create();
    node->setContentSize({ radius * 2, radius * 2 });
    node->setAnchorPoint({ 0.5f, 0.5f });

    auto dot = CCDrawNode::create();
    dot->drawDot({ radius, radius }, radius, toColor4F(fill));
    node->addChild(dot);

    auto label = CCLabelBMFont::create(std::to_string(lvl.position).c_str(), "bigFont.fnt");
    label->limitLabelWidth(radius * 1.5f, radius / 26.f, 0.1f);
    label->setColor(text);
    label->setPosition({ radius, radius });
    node->addChild(label);
    return node;
}

static CCNode* playSprite(float scale) {
    if (auto spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png")) {
        spr->setScale(scale);
        return spr;
    }
    auto btn = ButtonSprite::create("Play", "bigFont.fnt", "GJ_button_01.png", 0.8f);
    btn->setScale(0.45f);
    return btn;
}

// The GD Gaming List logo, scaled to `size` pixels wide (falls back to a gold "GL" badge)
static CCNode* logoSprite(float size) {
    if (auto spr = CCSprite::create("gdgl_logo.png"_spr)) {
        spr->setScale(size / spr->getContentSize().width);
        return spr;
    }
    auto node = CCNode::create();
    node->setContentSize({ size, size });
    node->setAnchorPoint({ 0.5f, 0.5f });
    auto bg = panel({ size, size }, COL_GOLD);
    bg->setPosition({ size / 2, size / 2 });
    node->addChild(bg);
    auto lbl = CCLabelBMFont::create("GL", "bigFont.fnt");
    lbl->setScale(size / 66.f);
    lbl->setColor(COL_DARK);
    lbl->setPosition({ size / 2, size / 2 });
    node->addChild(lbl);
    return node;
}

static void openLevel(std::string const& id) {
    auto search = GJSearchObject::create(SearchType::Search, id);
    auto scene = LevelBrowserLayer::scene(search);
    CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(0.5f, scene));
}

static CCMenuItemSpriteExtra* textButton(
    char const* text, char const* texture, float scale, std::function<void()> onClick, char const* font = "goldFont.fnt"
) {
    auto spr = ButtonSprite::create(text, font, texture, 0.8f);
    spr->setScale(scale);
    return CCMenuItemExt::createSpriteExtra(spr, [onClick](CCMenuItemSpriteExtra*) { onClick(); });
}

// ---------------------------------------------------------------------------
// Level details popup
// ---------------------------------------------------------------------------

class GdglLevelPopup : public Popup {
protected:
    static constexpr float W = 320.f;
    static constexpr float H = 262.f;

    ListLevel m_level;
    std::function<void()> m_onFavoriteChanged;
    CCMenuItemSpriteExtra* m_favBtn = nullptr;

    bool init(ListLevel level, std::function<void()> onFavoriteChanged) {
        if (!Popup::init(W, H)) return false;
        m_level = std::move(level);
        m_onFavoriteChanged = std::move(onFavoriteChanged);
        auto const& lvl = m_level;

        // Header bar
        auto header = panel({ W - 16.f, 26.f }, COL_DARK);
        header->setPosition({ W / 2, H - 21.f });
        m_mainLayer->addChild(header);
        auto headerLbl = CCLabelBMFont::create("Level details", "chatFont.fnt");
        headerLbl->setScale(0.6f);
        headerLbl->setColor(COL_MUTED);
        headerLbl->setPosition({ W / 2, H - 21.f });
        m_mainLayer->addChild(headerLbl);
        auto headerLogo = logoSprite(22.f);
        headerLogo->setPosition({ W / 2 - 52.f, H - 21.f });
        m_mainLayer->addChild(headerLogo);

        // Rank + name + section
        auto badge = rankBadge(lvl, 18.f);
        badge->setPosition({ 34.f, H - 64.f });
        m_mainLayer->addChild(badge);

        bool hasThumb = !lvl.thumbUrl.empty();
        float nameWidth = hasThumb ? 150.f : 240.f;

        auto name = CCLabelBMFont::create(lvl.name.c_str(), "goldFont.fnt");
        name->setAnchorPoint({ 0.f, 0.5f });
        name->limitLabelWidth(nameWidth, 0.75f, 0.2f);
        name->setPosition({ 60.f, H - 57.f });
        m_mainLayer->addChild(name);

        auto section = CCLabelBMFont::create(fmt::format("{} list  #{}", capitalize(lvl.bucket), lvl.position).c_str(), "chatFont.fnt");
        section->setAnchorPoint({ 0.f, 0.5f });
        section->setScale(0.55f);
        section->setColor(COL_MUTED);
        section->setPosition({ 61.f, H - 75.f });
        m_mainLayer->addChild(section);

        // Video thumbnail from YouTube
        if (hasThumb) {
            auto thumb = LazySprite::create({ 80.f, 45.f });
            thumb->setAutoResize(true);
            thumb->setPosition({ W - 54.f, H - 64.f });
            thumb->loadFromUrl(lvl.thumbUrl);
            m_mainLayer->addChild(thumb);
        }

        // Info box
        float boxH = 88.f;
        float boxTop = H - 92.f;
        auto box = panel({ W - 40.f, boxH }, COL_FIELD);
        box->setPosition({ W / 2, boxTop - boxH / 2 });
        m_mainLayer->addChild(box);

        std::vector<std::pair<std::string, std::string>> rows = {
            { "Published by", lvl.publishedBy.empty() ? "-" : lvl.publishedBy },
            { "Verified by", lvl.verifiedBy.empty() ? "-" : lvl.verifiedBy },
            { "Level ID", lvl.levelID.empty() ? "Not added yet" : lvl.levelID },
            { "Points", lvl.score.empty() ? "-" : formatScore(lvl.score) },
        };
        float rowY = boxTop - 13.f;
        for (auto& [label, value] : rows) {
            auto l = CCLabelBMFont::create(label.c_str(), "chatFont.fnt");
            l->setAnchorPoint({ 0.f, 0.5f });
            l->setScale(0.6f);
            l->setColor(COL_LABEL);
            l->setPosition({ 30.f, rowY });
            m_mainLayer->addChild(l);

            bool isID = label == "Level ID" && !lvl.levelID.empty();
            auto v = CCLabelBMFont::create(value.c_str(), "chatFont.fnt");
            v->setAnchorPoint({ 1.f, 0.5f });
            v->limitLabelWidth(170.f, 0.6f, 0.2f);
            v->setPosition({ W - (isID ? 62.f : 30.f), rowY });
            m_mainLayer->addChild(v);

            if (isID) {
                auto id = lvl.levelID;
                auto copy = textButton("Copy", "GJ_button_04.png", 0.35f, [id] {
                    if (clipboard::write(id)) Notification::create("Level ID copied", NotificationIcon::Success)->show();
                }, "bigFont.fnt");
                copy->setPosition({ W - 42.f, rowY });
                m_buttonMenu->addChild(copy);
            }
            rowY -= 20.f;
        }

        // Buttons
        if (!lvl.levelID.empty()) {
            auto id = lvl.levelID;
            auto play = textButton("Play level", "GJ_button_01.png", 0.7f, [id] { openLevel(id); });
            play->setPosition({ W / 2, 58.f });
            m_buttonMenu->addChild(play);
        } else {
            auto noId = CCLabelBMFont::create("No level ID on the list yet", "chatFont.fnt");
            noId->setScale(0.6f);
            noId->setColor(COL_MUTED);
            noId->setPosition({ W / 2, 58.f });
            m_mainLayer->addChild(noId);
        }

        float smallY = 24.f;
        if (!lvl.videoUrl.empty()) {
            auto url = lvl.videoUrl;
            auto video = textButton("Video", "GJ_button_05.png", 0.55f, [url] { web::openLinkInBrowser(url); }, "bigFont.fnt");
            video->setPosition({ W / 2 - 92.f, smallY });
            m_buttonMenu->addChild(video);
        }
        if (!lvl.pageUrl.empty()) {
            auto url = std::string(SITE_ROOT) + lvl.pageUrl;
            auto site = textButton("On site", "GJ_button_04.png", 0.55f, [url] { web::openLinkInBrowser(url); }, "bigFont.fnt");
            site->setPosition({ W / 2, smallY });
            m_buttonMenu->addChild(site);
        }
        this->updateFavButton();
        return true;
    }

    void updateFavButton() {
        if (m_favBtn) m_favBtn->removeFromParent();
        bool fav = isFavorite(m_level);
        m_favBtn = textButton(fav ? "Unfavorite" : "Favorite", fav ? "GJ_button_02.png" : "GJ_button_04.png", 0.55f, [this] {
            setFavorite(m_level, !isFavorite(m_level));
            this->updateFavButton();
            if (m_onFavoriteChanged) m_onFavoriteChanged();
        }, "bigFont.fnt");
        m_favBtn->setPosition({ W / 2 + 100.f, 24.f });
        m_buttonMenu->addChild(m_favBtn);
    }

public:
    static GdglLevelPopup* create(ListLevel level, std::function<void()> onFavoriteChanged) {
        auto ret = new GdglLevelPopup();
        if (ret->init(std::move(level), std::move(onFavoriteChanged))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Main list popup
// ---------------------------------------------------------------------------

class GdglListPopup : public Popup {
protected:
    static constexpr float W = 420.f;
    static constexpr float H = 310.f;
    static constexpr float LIST_W = 396.f;
    static constexpr float LIST_TOP = H - 104.f;
    static constexpr float LIST_BOTTOM = 40.f;
    static constexpr float LIST_H = LIST_TOP - LIST_BOTTOM;
    static constexpr float ROW_H = 36.f;

    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_subtitle = nullptr;
    CCLabelBMFont* m_sourceLbl = nullptr;
    CCDrawNode* m_sourceDot = nullptr;
    CCMenu* m_tabMenu = nullptr;
    CCMenuItemSpriteExtra* m_favToggle = nullptr;

    std::vector<ListLevel> m_levels;
    std::string m_filter = "all";
    std::string m_search;
    bool m_favOnly = false;
    bool m_fromBackup = false;
    async::TaskHolder<web::WebResponse> m_task;
    async::TaskHolder<web::WebResponse> m_updateCheck;
    CCMenuItemSpriteExtra* m_updateBtn = nullptr;

    bool init() {
        if (!Popup::init(W, H)) return false;

        // ----- Header
        auto header = panel({ W - 16.f, 40.f }, COL_DARK);
        header->setPosition({ W / 2, H - 28.f });
        m_mainLayer->addChild(header);

        auto logo = logoSprite(36.f);
        logo->setPosition({ 48.f, H - 28.f });
        m_mainLayer->addChild(logo);

        auto title = CCLabelBMFont::create("GD Gaming List", "goldFont.fnt");
        title->setAnchorPoint({ 0.f, 0.5f });
        title->setScale(0.65f);
        title->setPosition({ 70.f, H - 21.f });
        m_mainLayer->addChild(title);

        m_subtitle = CCLabelBMFont::create("Loading...", "chatFont.fnt");
        m_subtitle->setAnchorPoint({ 0.f, 0.5f });
        m_subtitle->setScale(0.55f);
        m_subtitle->setColor(COL_MUTED);
        m_subtitle->setPosition({ 71.f, H - 37.f });
        m_mainLayer->addChild(m_subtitle);

        CCNode* randomSpr = nullptr;
        if (auto eyes = CCSprite::create("gdgl_eyes.png"_spr)) {
            auto node = CCNode::create();
            node->setContentSize({ 50.f, 38.f });
            eyes->setScale(26.f / eyes->getContentSize().width);
            eyes->setPosition({ 25.f, 23.f });
            node->addChild(eyes);
            auto lbl = CCLabelBMFont::create("Random", "goldFont.fnt");
            lbl->setScale(0.38f);
            lbl->setPosition({ 25.f, 4.f });
            node->addChild(lbl);
            randomSpr = node;
        } else {
            auto b = ButtonSprite::create("Random", "bigFont.fnt", "GJ_button_04.png", 0.8f);
            b->setScale(0.5f);
            randomSpr = b;
        }
        auto random = CCMenuItemExt::createSpriteExtra(randomSpr, [this](CCMenuItemSpriteExtra*) { this->openRandom(); });
        random->setPosition({ W - 46.f, H - 28.f });
        m_buttonMenu->addChild(random);

        // ----- Search + favourites toggle
        auto search = TextInput::create(420.f, "Search levels or players", "bigFont.fnt");
        search->setScale(0.8f);
        search->setPosition({ W / 2 - 18.f, H - 64.f });
        search->setCallback([this](std::string const& text) {
            m_search = lower(text);
            this->rebuild();
        });
        m_mainLayer->addChild(search);
        this->updateFavToggle();

        // ----- Section tabs (built once the list loads)
        m_tabMenu = CCMenu::create();
        m_tabMenu->setContentSize({ LIST_W, 22.f });
        m_tabMenu->ignoreAnchorPointForPosition(false);
        m_tabMenu->setPosition({ W / 2, H - 90.f });
        m_tabMenu->setLayout(RowLayout::create()->setGap(6.f));
        m_mainLayer->addChild(m_tabMenu);

        // ----- List
        auto listBg = panel({ LIST_W + 6.f, LIST_H + 6.f }, { 0, 0, 0 }, 70);
        listBg->setPosition({ W / 2, LIST_BOTTOM + LIST_H / 2 });
        m_mainLayer->addChild(listBg);

        m_list = ScrollLayer::create({ LIST_W, LIST_H });
        m_list->setPosition({ W / 2 - LIST_W / 2, LIST_BOTTOM });
        m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
        m_mainLayer->addChild(m_list);

        m_status = CCLabelBMFont::create("Loading...", "bigFont.fnt");
        m_status->setScale(0.5f);
        m_status->setPosition({ W / 2, LIST_BOTTOM + LIST_H / 2 });
        m_mainLayer->addChild(m_status, 5);

        // ----- Bottom bar
        auto bar = panel({ W - 16.f, 26.f }, COL_DARK);
        bar->setPosition({ W / 2, 21.f });
        m_mainLayer->addChild(bar);

        m_sourceDot = CCDrawNode::create();
        m_sourceDot->setPosition({ 22.f, 21.f });
        m_mainLayer->addChild(m_sourceDot);

        m_sourceLbl = CCLabelBMFont::create("Connecting...", "chatFont.fnt");
        m_sourceLbl->setAnchorPoint({ 0.f, 0.5f });
        m_sourceLbl->setScale(0.55f);
        m_sourceLbl->setColor(COL_MUTED);
        m_sourceLbl->setPosition({ 32.f, 21.f });
        m_mainLayer->addChild(m_sourceLbl);
        this->setSource(false, false);

        auto site = textButton("Site", "GJ_button_04.png", 0.45f, [] { web::openLinkInBrowser(LIST_SITE); }, "bigFont.fnt");
        site->setPosition({ W - 108.f, 21.f });
        m_buttonMenu->addChild(site);

        auto refresh = textButton("Refresh", "GJ_button_04.png", 0.45f, [this] { this->reload(); }, "bigFont.fnt");
        refresh->setPosition({ W - 48.f, 21.f });
        m_buttonMenu->addChild(refresh);

        auto updates = textButton("Updates", "GJ_button_04.png", 0.45f, [] { checkForUpdate(true, false); }, "bigFont.fnt");
        updates->setPosition({ W - 172.f, 21.f });
        m_buttonMenu->addChild(updates);

        this->load();
        this->checkUpdateButton();
        return true;
    }

    // ----- Update button in the header (second way to update, besides the popup on game start)

    void checkUpdateButton() {
        if (s_updateInstalled) return;
        auto current = Mod::get()->getVersion();
        if (s_latestKnown) {
            if (*s_latestKnown > current) this->showUpdateButton(s_latestKnown->toVString());
            return;
        }
        // The check on game start hasn't finished (or failed): ask GitHub quietly
        web::WebRequest req;
        req.timeout(std::chrono::seconds(20));
        m_updateCheck.spawn(req.get(fmt::format("{}version.txt", RELEASE_BASE)), [this](web::WebResponse res) {
            if (!res.ok()) {
                log::info("[updater] Popup check failed: HTTP {}", res.code());
                return;
            }
            auto latest = VersionInfo::parse(utils::string::trim(res.string().unwrapOr("")));
            if (!latest) return;
            s_latestKnown = latest.unwrap();
            if (latest.unwrap() > Mod::get()->getVersion()) this->showUpdateButton(latest.unwrap().toVString());
        });
    }

    void showUpdateButton(std::string version) {
        if (m_updateBtn) return;
        auto spr = ButtonSprite::create(fmt::format("Update {}", version).c_str(), "goldFont.fnt", "GJ_button_01.png", 0.8f);
        spr->setScale(0.5f);
        m_updateBtn = CCMenuItemExt::createSpriteExtra(spr, [version](CCMenuItemSpriteExtra*) {
            createQuickPopup(
                "GD Gaming List",
                fmt::format("Update to <cg>{}</c> now? (you have {})", version, Mod::get()->getVersion().toVString()),
                "Later", "Update",
                [version](auto, bool update) {
                    if (update) installUpdate(version);
                }
            );
        });
        m_updateBtn->setPosition({ W - 128.f, H - 28.f });
        m_buttonMenu->addChild(m_updateBtn);
    }

    // ----- Loading

    void setSource(bool loaded, bool backup) {
        m_sourceDot->clear();
        ccColor3B color = !loaded ? ccColor3B{ 150, 150, 150 } : backup ? ccColor3B{ 239, 159, 39 } : ccColor3B{ 111, 227, 111 };
        m_sourceDot->drawDot({ 0.f, 0.f }, 4.f, toColor4F(color));
        m_sourceLbl->setString(!loaded ? "Connecting..." : backup ? "Using backup copy" : "Live from website");
    }

    void reload() {
        m_list->m_contentLayer->removeAllChildren();
        m_status->setString("Loading...");
        m_status->setVisible(true);
        m_subtitle->setString("Loading...");
        this->setSource(false, false);
        this->load();
    }

    void showLevels(std::vector<ListLevel> levels, bool backup) {
        m_levels = std::move(levels);
        m_fromBackup = backup;
        this->setSource(true, backup);
        m_subtitle->setString(fmt::format("{} levels  -  updated just now", m_levels.size()).c_str());
        this->buildTabs();
        this->rebuild();
    }

    // 1) Try the website itself
    void load() {
        web::WebRequest req;
        req.timeout(std::chrono::seconds(15));
        req.userAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) GDGamingListMod/1.3");

        m_task.spawn(req.get(DATA_PAGE), [this](web::WebResponse res) {
            if (res.ok()) {
                auto parsed = parseListPage(res.string().unwrapOr(""));
                if (parsed) return this->showLevels(std::move(parsed).unwrap(), false);
                log::warn("GDGL site page couldn't be read ({}), trying backup", parsed.unwrapErr());
            } else {
                log::warn("GDGL site failed: HTTP {}, trying backup", res.code());
            }
            this->loadMirror();
        });
    }

    // 2) Fall back to the copy on GitHub
    void loadMirror() {
        web::WebRequest req;
        req.timeout(std::chrono::seconds(15));

        m_task.spawn(req.get(MIRROR_URL), [this](web::WebResponse res) {
            if (!res.ok()) {
                log::warn("GDGL backup failed: HTTP {}", res.code());
                m_status->setString("Couldn't load the list.\nCheck your internet and press Refresh.");
                m_subtitle->setString("Not loaded");
                return;
            }
            auto parsed = parseLevels(res.string().unwrapOr(""));
            if (!parsed) {
                log::warn("GDGL backup couldn't be read: {}", parsed.unwrapErr());
                m_status->setString("Couldn't read the list.\nPress Refresh to try again.");
                m_subtitle->setString("Not loaded");
                return;
            }
            this->showLevels(std::move(parsed).unwrap(), true);
        });
    }

    // ----- Filters

    bool matches(ListLevel const& lvl) const {
        if (m_filter != "all" && lvl.bucket != m_filter) return false;
        if (m_favOnly && !isFavorite(lvl)) return false;
        if (m_search.empty()) return true;
        return lower(lvl.name).find(m_search) != std::string::npos
            || lower(lvl.byline).find(m_search) != std::string::npos
            || lower(lvl.creator).find(m_search) != std::string::npos
            || fmt::format("#{}", lvl.position) == m_search
            || std::to_string(lvl.position) == m_search
            || lvl.levelID == m_search;
    }

    void updateFavToggle() {
        if (m_favToggle) m_favToggle->removeFromParent();
        auto spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_starsIcon_001.png", 1.f,
            m_favOnly ? CircleBaseColor::Green : CircleBaseColor::Gray,
            CircleBaseSize::Small
        );
        spr->setScale(0.7f);
        m_favToggle = CCMenuItemExt::createSpriteExtra(spr, [this](CCMenuItemSpriteExtra*) {
            m_favOnly = !m_favOnly;
            this->updateFavToggle();
            this->rebuild();
            if (m_favOnly) Notification::create("Showing your favorites", NotificationIcon::Info, 1.f)->show();
        });
        m_favToggle->setPosition({ W - 28.f, H - 64.f });
        m_buttonMenu->addChild(m_favToggle);
    }

    void buildTabs() {
        std::vector<std::string> buckets = { "all" };
        for (auto& lvl : m_levels) {
            if (std::find(buckets.begin(), buckets.end(), lvl.bucket) == buckets.end()) buckets.push_back(lvl.bucket);
        }
        m_tabMenu->removeAllChildren();
        for (auto& bucket : buckets) {
            size_t count = bucket == "all"
                ? m_levels.size()
                : std::count_if(m_levels.begin(), m_levels.end(), [&](auto const& l) { return l.bucket == bucket; });
            auto text = fmt::format("{} {}", capitalize(bucket), count);
            auto spr = ButtonSprite::create(text.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.8f);
            spr->setScale(0.45f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, bucket](CCMenuItemSpriteExtra*) {
                m_filter = bucket;
                this->rebuild();
            });
            btn->setUserObject(CCString::create(bucket));
            m_tabMenu->addChild(btn);
        }
        m_tabMenu->updateLayout();
    }

    void updateTabs() {
        for (auto node : CCArrayExt<CCMenuItemSpriteExtra*>(m_tabMenu->getChildren())) {
            auto bucket = static_cast<CCString*>(node->getUserObject())->getCString();
            static_cast<ButtonSprite*>(node->getNormalImage())
                ->updateBGImage(m_filter == bucket ? "GJ_button_02.png" : "GJ_button_04.png");
        }
    }

    void rebuild() {
        this->updateTabs();
        auto content = m_list->m_contentLayer;
        content->removeAllChildren();

        int shown = 0;
        for (auto& lvl : m_levels) {
            if (!this->matches(lvl)) continue;
            content->addChild(this->createRow(lvl));
            shown++;
        }

        if (m_levels.empty()) {
            // still loading or failed; keep the current status text
        } else if (shown == 0 && m_favOnly) {
            m_status->setString("No favorites yet.\nOpen a level and press Favorite.");
        } else if (shown == 0 && !m_search.empty()) {
            m_status->setString("No levels match your search.");
        } else if (shown == 0) {
            m_status->setString("No levels here.");
        }
        m_status->setVisible(m_levels.empty() || shown == 0);

        content->updateLayout();
        m_list->scrollToTop();
    }

    void openDetails(ListLevel const& lvl) {
        auto popup = GdglLevelPopup::create(lvl, [this] {
            if (m_favOnly) this->rebuild();
        });
        if (popup) popup->show();
    }

    void openRandom() {
        std::vector<ListLevel const*> pool;
        for (auto& lvl : m_levels) if (this->matches(lvl)) pool.push_back(&lvl);
        if (pool.empty()) for (auto& lvl : m_levels) pool.push_back(&lvl);
        if (pool.empty()) {
            Notification::create("The list isn't loaded yet", NotificationIcon::Warning)->show();
            return;
        }
        auto index = static_cast<size_t>(random::generate<uint64_t>() % pool.size());
        this->openDetails(*pool[index]);
    }

    CCNode* createRow(ListLevel const& lvl) {
        bool ext = lvl.bucket == "extended";
        auto row = CCNode::create();
        row->setContentSize({ LIST_W, ROW_H });

        auto bg = panel({ LIST_W - 4.f, ROW_H }, ext ? COL_EXT : COL_MAIN);
        bg->setPosition({ LIST_W / 2, ROW_H / 2 });
        row->addChild(bg);

        // Coloured strip on the left: medal colour for the top 3
        ccColor3B strip = ext ? ccColor3B{ 133, 183, 235 } : ccColor3B{ 176, 112, 60 };
        if (!ext && lvl.position == 1) strip = COL_GOLD;
        if (!ext && lvl.position == 2) strip = COL_SILVER;
        if (!ext && lvl.position == 3) strip = COL_BRONZE;
        auto stripNode = CCLayerColor::create({ strip.r, strip.g, strip.b, 255 }, 4.f, ROW_H - 4.f);
        stripNode->setPosition({ 4.f, 2.f });
        row->addChild(stripNode);

        auto badge = rankBadge(lvl, 13.f);
        badge->setPosition({ 26.f, ROW_H / 2 });
        row->addChild(badge);

        auto name = CCLabelBMFont::create(lvl.name.c_str(), "bigFont.fnt");
        name->setAnchorPoint({ 0.f, 0.5f });
        name->limitLabelWidth(ext ? 200.f : 230.f, 0.48f, 0.2f);
        name->setPosition({ 46.f, ROW_H / 2 + 7.f });
        row->addChild(name);

        auto byline = CCLabelBMFont::create(lvl.byline.c_str(), "chatFont.fnt");
        byline->setAnchorPoint({ 0.f, 0.5f });
        byline->limitLabelWidth(240.f, 0.5f, 0.2f);
        byline->setColor(ext ? ccColor3B{ 219, 230, 251 } : ccColor3B{ 232, 207, 178 });
        byline->setPosition({ 46.f, ROW_H / 2 - 8.f });
        row->addChild(byline);

        if (ext) {
            auto tag = panel({ 30.f, 13.f }, { 133, 183, 235 });
            tag->setPosition({ LIST_W - 100.f, ROW_H / 2 });
            row->addChild(tag);
            auto tagLbl = CCLabelBMFont::create("EXT", "bigFont.fnt");
            tagLbl->setScale(0.3f);
            tagLbl->setColor({ 12, 68, 124 });
            tagLbl->setPosition(tag->getPosition());
            row->addChild(tagLbl);
        }

        if (isFavorite(lvl)) {
            auto star = CCSprite::createWithSpriteFrameName("GJ_starsIcon_001.png");
            if (star) {
                star->setScale(0.45f);
                star->setPosition({ LIST_W - (ext ? 128.f : 100.f), ROW_H / 2 });
                row->addChild(star);
            }
        }

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setContentSize(row->getContentSize());
        row->addChild(menu);

        // Small info button opens the details. (A full-row button would swallow drags and break scrolling.)
        CCNode* infoSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        if (infoSpr) infoSpr->setScale(0.7f);
        else {
            auto b = ButtonSprite::create("i", "bigFont.fnt", "GJ_button_04.png", 0.8f);
            b->setScale(0.45f);
            infoSpr = b;
        }
        auto info = CCMenuItemExt::createSpriteExtra(infoSpr, [this, lvl](CCMenuItemSpriteExtra*) { this->openDetails(lvl); });
        info->setPosition({ LIST_W - 64.f, ROW_H / 2 });
        menu->addChild(info);

        if (!lvl.levelID.empty()) {
            auto id = lvl.levelID;
            auto play = CCMenuItemExt::createSpriteExtra(playSprite(0.32f), [id](CCMenuItemSpriteExtra*) { openLevel(id); });
            play->setPosition({ LIST_W - 28.f, ROW_H / 2 });
            menu->addChild(play);
        } else {
            auto noId = CCLabelBMFont::create("No ID", "goldFont.fnt");
            noId->setScale(0.4f);
            noId->setOpacity(150);
            noId->setPosition({ LIST_W - 28.f, ROW_H / 2 });
            row->addChild(noId);
        }

        return row;
    }

public:
    static GdglListPopup* create() {
        auto ret = new GdglListPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(GdglSearchLayer, LevelSearchLayer) {
    bool init(int type) {
        if (!LevelSearchLayer::init(type)) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto spr = CCNode::create();
        spr->setContentSize({ 76.f, 60.f });
        auto logo = logoSprite(42.f);
        logo->setPosition({ 38.f, 37.f });
        spr->addChild(logo);
        auto label = CCLabelBMFont::create("GD Gaming List", "goldFont.fnt");
        label->limitLabelWidth(76.f, 0.45f, 0.2f);
        label->setPosition({ 38.f, 7.f });
        spr->addChild(label);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCMenuItemSpriteExtra*) {
            if (auto popup = GdglListPopup::create()) popup->show();
        });
        btn->setID("gdgl-list-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("gdgl-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        menu->addChild(btn);
        // Left side, a bit lower than the IPGDGLL List button so both mods fit together
        btn->setPosition({ 52.f, winSize.height / 2.f - 62.f });
        this->addChild(menu, 10);

        return true;
    }
};


// ---------------------------------------------------------------------------
// GDGL badge on level pages ("GDGL #12" next to the level when it's on the list)
// ---------------------------------------------------------------------------

// Shared copy of the list for badges (refreshed at most every 10 minutes)
static std::vector<ListLevel> s_badgeLevels;
static std::chrono::steady_clock::time_point s_badgeLoadedAt;
static bool s_badgeLoaded = false;
static bool s_badgeLoading = false;
static std::vector<std::function<void()>> s_badgeWaiting;
static async::TaskHolder<web::WebResponse> s_badgeTask;

static void badgeLoadDone(std::optional<std::vector<ListLevel>> levels) {
    s_badgeLoading = false;
    if (levels) {
        s_badgeLevels = std::move(*levels);
        s_badgeLoaded = true;
        s_badgeLoadedAt = std::chrono::steady_clock::now();
    }
    auto waiting = std::move(s_badgeWaiting);
    s_badgeWaiting.clear();
    for (auto& fn : waiting) fn();
}

static void withBadgeLevels(std::function<void()> fn) {
    bool fresh = s_badgeLoaded && std::chrono::steady_clock::now() - s_badgeLoadedAt < std::chrono::minutes(10);
    if (fresh) return fn();
    s_badgeWaiting.push_back(std::move(fn));
    if (s_badgeLoading) return;
    s_badgeLoading = true;

    web::WebRequest req;
    req.timeout(std::chrono::seconds(15));
    req.userAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) GDGamingListMod/1.5");
    s_badgeTask.spawn(req.get(DATA_PAGE), [](web::WebResponse res) {
        if (res.ok()) {
            if (auto parsed = parseListPage(res.string().unwrapOr(""))) return badgeLoadDone(std::move(parsed).unwrap());
        }
        // site failed: use the GitHub backup
        web::WebRequest backup;
        backup.timeout(std::chrono::seconds(15));
        s_badgeTask.spawn(backup.get(MIRROR_URL), [](web::WebResponse res) {
            if (res.ok()) {
                if (auto parsed = parseLevels(res.string().unwrapOr(""))) return badgeLoadDone(std::move(parsed).unwrap());
            }
            log::warn("[badge] Couldn't load the list for badges");
            badgeLoadDone(std::nullopt);
        });
    });
}

class $modify(GdglLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        if (!Mod::get()->getSettingValue<bool>("level-badge") || !level) return true;
        int id = level->m_levelID.value();
        if (id <= 0) return true;

        auto self = WeakRef<LevelInfoLayer>(this);
        withBadgeLevels([self, id] {
            auto layer = self.lock();
            if (!layer || !layer->getParent()) return;
            auto idStr = std::to_string(id);
            auto it = std::find_if(s_badgeLevels.begin(), s_badgeLevels.end(), [&](auto const& l) { return l.levelID == idStr; });
            if (it == s_badgeLevels.end()) return;
            static_cast<GdglLevelInfoLayer*>(layer.data())->addBadge(*it);
        });
        return true;
    }

    void addBadge(ListLevel const& lvl) {
        if (this->getChildByID("gdgl-badge-menu"_spr)) return;
        bool ext = lvl.bucket != "main";

        // logo + "GDGL #12" on a dark pill
        auto spr = CCNode::create();
        spr->setContentSize({ 112.f, 34.f });
        auto bg = panel({ 112.f, 34.f }, ext ? COL_EXT : COL_DARK, 220);
        bg->setPosition({ 56.f, 17.f });
        spr->addChild(bg);

        CCNode* icon = nullptr;
        if (!ext && lvl.position == 1) {
            if (auto trophy = CCSprite::create("gdgl_trophy.png"_spr)) {
                trophy->setScale(26.f / trophy->getContentSize().width);
                icon = trophy;
            }
        }
        if (!icon) icon = logoSprite(26.f);
        icon->setPosition({ 18.f, 17.f });
        spr->addChild(icon);

        auto rank = CCLabelBMFont::create(fmt::format("#{}", lvl.position).c_str(), "goldFont.fnt");
        rank->setAnchorPoint({ 0.f, 0.5f });
        rank->limitLabelWidth(72.f, 0.7f, 0.3f);
        ccColor3B rankColor = { 255, 255, 255 };
        if (!ext && lvl.position == 2) rankColor = COL_SILVER;
        if (!ext && lvl.position == 3) rankColor = COL_BRONZE;
        rank->setColor(rankColor);
        rank->setPosition({ 36.f, 21.f });
        spr->addChild(rank);

        auto section = CCLabelBMFont::create(ext ? "GDGL Extended" : "GD Gaming List", "chatFont.fnt");
        section->setAnchorPoint({ 0.f, 0.5f });
        section->limitLabelWidth(72.f, 0.5f, 0.2f);
        section->setColor(COL_MUTED);
        section->setPosition({ 37.f, 8.f });
        spr->addChild(section);

        // tap the badge to see the level's list details
        auto copy = lvl;
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [copy](CCMenuItemSpriteExtra*) {
            if (auto popup = GdglLevelPopup::create(copy, nullptr)) popup->show();
        });
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("gdgl-badge-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        btn->setPosition({ 72.f, winSize.height - 72.f });
        menu->addChild(btn);
        this->addChild(menu, 20);

        // small pop-in animation
        btn->setScale(0.f);
        btn->runAction(CCEaseBackOut::create(CCScaleTo::create(0.3f, 1.f)));
    }
};

// ---------------------------------------------------------------------------
// Self-updater: checks GitHub for a newer build when GD starts (or when asked)
// ---------------------------------------------------------------------------


static async::TaskHolder<web::WebResponse> s_updateTask;
static bool s_updateBusy = false;

// Runs `fn` once a normal screen is showing (not mid scene-transition), so popups don't get lost
static void whenScreenReady(std::function<void()> fn, int tries = 0) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    bool ready = scene && !typeinfo_cast<CCTransitionScene*>(scene);
    if (ready || tries > 600) {
        fn();
        return;
    }
    Loader::get()->queueInMainThread([fn = std::move(fn), tries]() mutable {
        whenScreenReady(std::move(fn), tries + 1);
    });
}

static void installUpdate(std::string version) {
    if (s_updateBusy) return;
    if (s_updateInstalled) {
        createQuickPopup("Update installed", "The update is already installed.\nRestart GD to use it.", "Later", "Restart",
            [](auto, bool restart) { if (restart) game::restart(true); });
        return;
    }
    s_updateBusy = true;

    auto url = fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID());
    log::info("[updater] Downloading {} from {}", version, url);
    auto loading = Notification::create("Downloading GD Gaming List update...", NotificationIcon::Loading, 0.f);
    loading->show();

    web::WebRequest req;
    req.timeout(std::chrono::seconds(90));
    s_updateTask.spawn(req.get(url), [loading = Ref(loading), version](web::WebResponse res) {
        s_updateBusy = false;
        loading->hide();
        auto const& data = res.data();
        log::info("[updater] Download finished: HTTP {}, {} bytes", res.code(), data.size());
        // A .geode file is a zip: must start with "PK"
        if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
            log::warn("[updater] Download failed or isn't a .geode file");
            Notification::create(fmt::format("Update failed to download (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        auto path = Mod::get()->getPackagePath();
        auto written = file::writeBinarySafe(path, data);
        if (!written) {
            log::warn("[updater] Couldn't write the mod file: {}", written.unwrapErr());
            Notification::create("Update failed to install", NotificationIcon::Error, 3.f)->show();
            return;
        }
        log::info("[updater] Installed {}", version);
        s_updateInstalled = true;
        whenScreenReady([version] {
            createQuickPopup(
                "Update installed",
                fmt::format("GD Gaming List <cg>{}</c> is installed.\nRestart GD to use it.", version),
                "Later", "Restart",
                [](auto, bool restart) {
                    if (restart) game::restart(true);
                }
            );
        });
    });
}

// manual = pressed the button (always tell the player the result);
// automatic = on game start (stay quiet unless there's an update, retry once on failure)
static void checkForUpdate(bool manual, bool isRetry) {
    if (s_updateBusy) {
        if (manual) Notification::create("Already checking for updates...", NotificationIcon::Info)->show();
        return;
    }
    s_updateBusy = true;

    auto url = fmt::format("{}version.txt", RELEASE_BASE);
    log::info("[updater] Checking {} (current {}){}", url, Mod::get()->getVersion().toVString(), isRetry ? " - retry" : "");
    if (manual) Notification::create("Checking for updates...", NotificationIcon::Loading, 1.f)->show();

    web::WebRequest req;
    req.timeout(std::chrono::seconds(20));
    s_updateTask.spawn(req.get(url), [manual, isRetry](web::WebResponse res) {
        s_updateBusy = false;
        auto body = utils::string::trim(res.string().unwrapOr(""));
        log::info("[updater] version.txt: HTTP {}, body '{}'", res.code(), body.substr(0, 40));

        if (!res.ok()) {
            if (!manual && !isRetry) {
                log::info("[updater] Retrying once");
                Loader::get()->queueInMainThread([] { checkForUpdate(false, true); });
                return;
            }
            if (manual) Notification::create(fmt::format("Couldn't check for updates (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        auto latest = VersionInfo::parse(body);
        if (!latest) {
            log::warn("[updater] Couldn't read version '{}': {}", body.substr(0, 40), latest.unwrapErr());
            if (manual) Notification::create("Couldn't read the latest version", NotificationIcon::Error, 3.f)->show();
            return;
        }
        s_latestKnown = latest.unwrap();
        auto current = Mod::get()->getVersion();
        auto version = latest.unwrap().toVString();
        if (!(latest.unwrap() > current)) {
            log::info("[updater] Up to date ({} >= {})", current.toVString(), version);
            if (manual) Notification::create(fmt::format("You're up to date ({})", current.toVString()), NotificationIcon::Success, 2.f)->show();
            return;
        }

        log::info("[updater] Update available: {} -> {}", current.toVString(), version);
        whenScreenReady([version, current] {
            createQuickPopup(
                "GD Gaming List",
                fmt::format("A new version is out: <cg>{}</c> (you have {}).\nUpdate now?", version, current.toVString()),
                "Later", "Update",
                [version](auto, bool update) {
                    if (update) installUpdate(version);
                }
            );
        });
    });
}

class $modify(GdglMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool checked = false;
        if (!checked) {
            checked = true;
            if (Mod::get()->getSettingValue<bool>("check-updates")) {
                checkForUpdate(false, false);
            } else {
                log::info("[updater] Automatic update check is turned off in settings");
            }
        }
        return true;
    }
};
