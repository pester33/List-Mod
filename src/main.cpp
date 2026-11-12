#include <Geode/Geode.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

// The list site embeds its full level list as JSON on the Roulette page:
// <script id="roulette-data" type="application/json">[ ... ]</script>
static constexpr auto LIST_SITE = "http://gdgl.x10.mx/gdgaminglist/";
static constexpr auto DATA_PAGE = "http://gdgl.x10.mx/gdgaminglist/roulette";
static constexpr auto DATA_MARKER = "id=\"roulette-data\"";
static constexpr auto DATA_END = "</script>";
// Backup copy of the list, refreshed by a GitHub Action every 30 minutes.
// Used when the site can't be reached or sends back a different page (some networks do that).
static constexpr auto MIRROR_URL = "https://raw.githubusercontent.com/pester33/List-Mod/main/data/levels.json";

struct ListLevel {
    int position = 0;
    std::string name;
    std::string byline;
    std::string bucket; // "main", "extended", ...
    std::string levelID;
};

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// Turns the list JSON (an array of levels) into levels
static Result<std::vector<ListLevel>> parseLevels(std::string_view text) {
    auto parsed = matjson::parse(text);
    if (!parsed) return Err("List data isn't valid JSON");
    auto json = std::move(parsed).unwrap();
    if (!json.isArray() || json.size() == 0) return Err("List data is empty");

    std::vector<ListLevel> levels;
    for (auto& entry : json) {
        if (!entry["shown"].asBool().unwrapOr(true)) continue;
        ListLevel lvl;
        lvl.position = static_cast<int>(entry["position"].asInt().unwrapOr(0));
        lvl.name = entry["name"].asString().unwrapOr("Unknown");
        lvl.byline = entry["byline"].asString().unwrapOr("");
        if (lvl.byline.empty()) {
            auto creator = entry["creator"].asString().unwrapOr("");
            if (!creator.empty()) lvl.byline = "by " + creator;
        }
        lvl.bucket = entry["bucket"].asString().unwrapOr("main");
        auto& id = entry["levelId"];
        if (id.isString()) lvl.levelID = id.asString().unwrapOr("");
        else if (id.isNumber()) lvl.levelID = std::to_string(id.asInt().unwrapOr(0));
        if (lvl.levelID == "0") lvl.levelID.clear();
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

class GdglListPopup : public Popup {
protected:
    static constexpr float WIDTH = 400.f;
    static constexpr float HEIGHT = 290.f;
    static constexpr float LIST_W = 360.f;
    static constexpr float LIST_H = 168.f;
    static constexpr float LIST_Y = 116.f; // centre of the list
    static constexpr float ROW_H = 34.f;

    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenu* m_tabMenu = nullptr;
    std::vector<ListLevel> m_levels;
    std::string m_filter = "all";
    std::string m_search;
    TextInput* m_searchInput = nullptr;
    async::TaskHolder<web::WebResponse> m_task;

    bool init() {
        if (!Popup::init(WIDTH, HEIGHT)) return false;
        this->setTitle("GD Gaming List");

        auto center = m_mainLayer->getContentSize() / 2;

        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(80);
        bg->setContentSize({ LIST_W + 6.f, LIST_H + 6.f });
        bg->setPosition({ center.width, LIST_Y });
        m_mainLayer->addChild(bg);

        m_list = ScrollLayer::create({ LIST_W, LIST_H });
        m_list->setPosition({ center.width - LIST_W / 2, LIST_Y - LIST_H / 2 });
        m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(3.f));
        m_mainLayer->addChild(m_list);

        m_status = CCLabelBMFont::create("Loading...", "bigFont.fnt");
        m_status->setScale(0.5f);
        m_status->setPosition({ center.width, LIST_Y });
        m_mainLayer->addChild(m_status, 5);

        // Tabs get built once we know which sections the list has (Main, Extended, Legacy...)
        m_tabMenu = CCMenu::create();
        m_tabMenu->setContentSize({ LIST_W, 24.f });
        m_tabMenu->setPosition({ center.width, HEIGHT - 46.f });
        m_tabMenu->ignoreAnchorPointForPosition(false);
        m_tabMenu->setLayout(RowLayout::create()->setGap(6.f));
        m_mainLayer->addChild(m_tabMenu);

        // Search bar: filters by level name, creator, publisher or verifier as you type
        m_searchInput = TextInput::create(LIST_W - 10.f, "Search levels...", "bigFont.fnt");
        m_searchInput->setPosition({ center.width, HEIGHT - 74.f });
        m_searchInput->setScale(0.85f);
        m_searchInput->setCallback([this](std::string const& text) {
            m_search = lower(text);
            this->rebuild();
        });
        m_mainLayer->addChild(m_searchInput);

        auto webSpr = ButtonSprite::create("Website", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        webSpr->setScale(0.55f);
        auto webBtn = CCMenuItemExt::createSpriteExtra(webSpr, [](CCMenuItemSpriteExtra*) {
            web::openLinkInBrowser(LIST_SITE);
        });
        webBtn->setPosition({ center.width - 45.f, 14.f });
        m_buttonMenu->addChild(webBtn);

        // Reload the list without closing the popup
        auto refreshSpr = ButtonSprite::create("Refresh", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        refreshSpr->setScale(0.55f);
        auto refreshBtn = CCMenuItemExt::createSpriteExtra(refreshSpr, [this](CCMenuItemSpriteExtra*) {
            m_list->m_contentLayer->removeAllChildren();
            m_status->setString("Loading...");
            m_status->setVisible(true);
            this->load();
        });
        refreshBtn->setPosition({ center.width + 45.f, 14.f });
        m_buttonMenu->addChild(refreshBtn);

        this->load();
        return true;
    }

    void showLevels(std::vector<ListLevel> levels) {
        m_levels = std::move(levels);
        this->buildTabs();
        this->rebuild();
    }

    // 1) Try the website itself
    void load() {
        web::WebRequest req;
        req.timeout(std::chrono::seconds(15));
        req.userAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) GDGamingListMod/1.2");

        m_task.spawn(req.get(DATA_PAGE), [this](web::WebResponse res) {
            if (res.ok()) {
                auto parsed = parseListPage(res.string().unwrapOr(""));
                if (parsed) return this->showLevels(std::move(parsed).unwrap());
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
                return;
            }
            auto parsed = parseLevels(res.string().unwrapOr(""));
            if (!parsed) {
                log::warn("GDGL backup couldn't be read: {}", parsed.unwrapErr());
                m_status->setString("Couldn't read the list.\nPress Refresh to try again.");
                return;
            }
            this->showLevels(std::move(parsed).unwrap());
        });
    }

    void buildTabs() {
        std::vector<std::string> buckets = { "all" };
        for (auto& lvl : m_levels) {
            if (std::find(buckets.begin(), buckets.end(), lvl.bucket) == buckets.end()) {
                buckets.push_back(lvl.bucket);
            }
        }
        m_tabMenu->removeAllChildren();
        for (auto& bucket : buckets) {
            auto spr = ButtonSprite::create(capitalize(bucket).c_str(), "bigFont.fnt", "GJ_button_04.png", 0.8f);
            spr->setScale(0.5f);
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
            if (m_filter != "all" && lvl.bucket != m_filter) continue;
            if (!m_search.empty()
                && lower(lvl.name).find(m_search) == std::string::npos
                && lower(lvl.byline).find(m_search) == std::string::npos
                && fmt::format("#{}", lvl.position) != m_search
                && std::to_string(lvl.position) != m_search) continue;
            content->addChild(this->createRow(lvl));
            shown++;
        }

        if (m_levels.empty()) m_status->setString("The list is empty.");
        else if (shown == 0 && !m_search.empty()) m_status->setString("No levels match your search.");
        else if (shown == 0) m_status->setString("No levels here.");
        m_status->setVisible(shown == 0);

        content->updateLayout();
        m_list->scrollToTop();
    }

    CCNode* createRow(ListLevel const& lvl) {
        auto row = CCNode::create();
        row->setContentSize({ LIST_W, ROW_H });

        // Main = brown, Extended = blue-ish, others = grey (like section colours on demon lists)
        ccColor3B color = lvl.bucket == "main"
            ? (lvl.position % 2 ? ccColor3B{ 161, 88, 44 } : ccColor3B{ 194, 114, 62 })
            : lvl.bucket == "extended"
                ? (lvl.position % 2 ? ccColor3B{ 52, 86, 150 } : ccColor3B{ 70, 108, 176 })
                : (lvl.position % 2 ? ccColor3B{ 80, 80, 80 } : ccColor3B{ 105, 105, 105 });

        auto rowBg = CCScale9Sprite::create("square02b_001.png");
        rowBg->setColor(color);
        rowBg->setContentSize({ LIST_W - 4.f, ROW_H });
        rowBg->setPosition({ LIST_W / 2, ROW_H / 2 });
        row->addChild(rowBg);

        auto rankLbl = CCLabelBMFont::create(fmt::format("#{}", lvl.position).c_str(), "goldFont.fnt");
        rankLbl->limitLabelWidth(40.f, 0.65f, 0.3f);
        rankLbl->setPosition({ 26.f, ROW_H / 2 });
        row->addChild(rankLbl);

        auto nameLbl = CCLabelBMFont::create(lvl.name.c_str(), "bigFont.fnt");
        nameLbl->setAnchorPoint({ 0.f, 0.5f });
        nameLbl->limitLabelWidth(200.f, 0.5f, 0.2f);
        nameLbl->setPosition({ 52.f, ROW_H / 2 + 6.f });
        row->addChild(nameLbl);

        auto infoLbl = CCLabelBMFont::create(lvl.byline.c_str(), "chatFont.fnt");
        infoLbl->setAnchorPoint({ 0.f, 0.5f });
        infoLbl->limitLabelWidth(220.f, 0.55f, 0.2f);
        infoLbl->setPosition({ 52.f, ROW_H / 2 - 7.f });
        row->addChild(infoLbl);

        if (!lvl.levelID.empty()) {
            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            menu->setContentSize(row->getContentSize());
            row->addChild(menu);

            auto spr = ButtonSprite::create("View", "bigFont.fnt", "GJ_button_01.png", 0.8f);
            spr->setScale(0.5f);
            auto id = lvl.levelID;
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [id](CCMenuItemSpriteExtra*) {
                auto search = GJSearchObject::create(SearchType::Search, id);
                auto scene = LevelBrowserLayer::scene(search);
                CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(0.5f, scene));
            });
            btn->setPosition({ LIST_W - 38.f, ROW_H / 2 });
            menu->addChild(btn);
        } else {
            auto noId = CCLabelBMFont::create("No ID", "goldFont.fnt");
            noId->setScale(0.4f);
            noId->setOpacity(150);
            noId->setPosition({ LIST_W - 38.f, ROW_H / 2 });
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

        auto spr = ButtonSprite::create("GD Gaming List", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        spr->setScale(0.55f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCMenuItemSpriteExtra*) {
            if (auto popup = GdglListPopup::create()) popup->show();
        });
        btn->setID("gdgl-list-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("gdgl-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        menu->addChild(btn);
        // Left side, a bit lower than the IPGDGLL List button so both mods fit together
        btn->setPosition({ 52.f, winSize.height / 2.f - 52.f });
        this->addChild(menu, 10);

        return true;
    }
};

// ---------------------------------------------------------------------------
// Self-updater: checks GitHub for a newer build when GD starts
// ---------------------------------------------------------------------------

// Builds for GD 2.2091 live on their own release, so old 2.2081 copies never download them
static constexpr auto RELEASE_BASE = "https://github.com/pester33/List-Mod/releases/download/latest-build-2.2091/";

static async::TaskHolder<web::WebResponse> s_updateTask;

static void installUpdate(std::string version) {
    auto loading = Notification::create("Downloading GD Gaming List update...", NotificationIcon::Loading, 0.f);
    loading->show();

    web::WebRequest req;
    req.timeout(std::chrono::seconds(60));
    s_updateTask.spawn(
        req.get(fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID())),
        [loading = Ref(loading), version](web::WebResponse res) {
            loading->hide();
            auto const& data = res.data();
            // A .geode file is a zip: must start with "PK"
            if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
                log::warn("Update download failed: HTTP {}", res.code());
                Notification::create("Update failed to download", NotificationIcon::Error)->show();
                return;
            }
            // Replace our own .geode file; Geode loads the new one on the next start
            auto written = file::writeBinarySafe(Mod::get()->getPackagePath(), data);
            if (!written) {
                log::warn("Update install failed: {}", written.unwrapErr());
                Notification::create("Update failed to install", NotificationIcon::Error)->show();
                return;
            }
            createQuickPopup(
                "Update installed",
                fmt::format("GD Gaming List <cg>{}</c> is installed. Restart GD to use it.", version),
                "Later", "Restart",
                [](auto, bool restart) {
                    if (restart) game::restart(true);
                }
            );
        }
    );
}

static void checkForUpdate() {
    web::WebRequest req;
    req.timeout(std::chrono::seconds(15));
    s_updateTask.spawn(req.get(fmt::format("{}version.txt", RELEASE_BASE)), [](web::WebResponse res) {
        if (!res.ok()) return;
        auto text = utils::string::trim(res.string().unwrapOr(""));
        auto latest = VersionInfo::parse(text);
        if (!latest) return;
        auto current = Mod::get()->getVersion();
        if (!(latest.unwrap() > current)) return;

        auto version = latest.unwrap().toVString();
        createQuickPopup(
            "GD Gaming List",
            fmt::format("A new version is out: <cg>{}</c> (you have {}).\nUpdate now?", version, current.toVString()),
            "Later", "Update",
            [version](auto, bool update) {
                if (update) installUpdate(version);
            }
        );
    });
}

class $modify(GdglMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool checked = false;
        if (!checked && Mod::get()->getSettingValue<bool>("check-updates")) {
            checked = true;
            checkForUpdate();
        }
        return true;
    }
};
