#include <Geode/Geode.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

// The list site embeds its full level list as JSON on the Roulette page:
// <script id="roulette-data" type="application/json">[ ... ]</script>
static constexpr auto LIST_SITE = "http://gdgl.x10.mx/gdgaminglist/";
static constexpr auto DATA_PAGE = "http://gdgl.x10.mx/gdgaminglist/roulette";
static constexpr auto DATA_START = "<script id=\"roulette-data\" type=\"application/json\">";
static constexpr auto DATA_END = "</script>";

struct ListLevel {
    int position = 0;
    std::string name;
    std::string byline;
    std::string bucket; // "main", "extended", ...
    std::string levelID;
};

static std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// Pulls the embedded JSON out of the page and turns it into levels
static Result<std::vector<ListLevel>> parseListPage(std::string const& html) {
    auto start = html.find(DATA_START);
    if (start == std::string::npos) return Err("List data not found on the page");
    start += std::string_view(DATA_START).size();
    auto end = html.find(DATA_END, start);
    if (end == std::string::npos) return Err("List data is cut off");

    GEODE_UNWRAP_INTO(auto json, matjson::parse(std::string_view(html).substr(start, end - start)));
    if (!json.isArray()) return Err("List data isn't a list");

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

class GdglListPopup : public Popup {
protected:
    static constexpr float WIDTH = 400.f;
    static constexpr float HEIGHT = 270.f;
    static constexpr float LIST_W = 360.f;
    static constexpr float LIST_H = 195.f;
    static constexpr float ROW_H = 34.f;

    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenu* m_tabMenu = nullptr;
    std::vector<ListLevel> m_levels;
    std::string m_filter = "all";
    async::TaskHolder<web::WebResponse> m_task;

    bool init() {
        if (!Popup::init(WIDTH, HEIGHT)) return false;
        this->setTitle("GD Gaming List");

        auto center = m_mainLayer->getContentSize() / 2;

        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(80);
        bg->setContentSize({ LIST_W + 6.f, LIST_H + 6.f });
        bg->setPosition({ center.width, center.height - 16.f });
        m_mainLayer->addChild(bg);

        m_list = ScrollLayer::create({ LIST_W, LIST_H });
        m_list->setPosition({ center.width - LIST_W / 2, center.height - 16.f - LIST_H / 2 });
        m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(3.f));
        m_mainLayer->addChild(m_list);

        m_status = CCLabelBMFont::create("Loading...", "bigFont.fnt");
        m_status->setScale(0.5f);
        m_status->setPosition({ center.width, center.height - 16.f });
        m_mainLayer->addChild(m_status, 5);

        // Tabs get built once we know which sections the list has (Main, Extended, Legacy...)
        m_tabMenu = CCMenu::create();
        m_tabMenu->setContentSize({ LIST_W, 24.f });
        m_tabMenu->setPosition({ center.width, HEIGHT - 46.f });
        m_tabMenu->ignoreAnchorPointForPosition(false);
        m_tabMenu->setLayout(RowLayout::create()->setGap(6.f));
        m_mainLayer->addChild(m_tabMenu);

        auto webSpr = ButtonSprite::create("Website", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        webSpr->setScale(0.55f);
        auto webBtn = CCMenuItemExt::createSpriteExtra(webSpr, [](CCMenuItemSpriteExtra*) {
            web::openLinkInBrowser(LIST_SITE);
        });
        webBtn->setPosition({ center.width, 14.f });
        m_buttonMenu->addChild(webBtn);

        this->load();
        return true;
    }

    void load() {
        web::WebRequest req;
        req.timeout(std::chrono::seconds(15));
        req.userAgent("Mozilla/5.0 (Geode; GD Gaming List mod)");

        m_task.spawn(req.get(DATA_PAGE), [this](web::WebResponse res) {
            if (!res.ok()) {
                log::warn("GDGL list failed: HTTP {}", res.code());
                m_status->setString("Couldn't load the list.\nCheck your internet.");
                return;
            }
            auto parsed = parseListPage(res.string().unwrapOr(""));
            if (!parsed) {
                log::warn("GDGL list parse failed: {}", parsed.unwrapErr());
                m_status->setString("Couldn't read the list.\nThe site may have changed.");
                return;
            }
            m_levels = std::move(parsed).unwrap();
            this->buildTabs();
            this->rebuild();
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
            content->addChild(this->createRow(lvl));
            shown++;
        }

        if (m_levels.empty()) m_status->setString("The list is empty.");
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
