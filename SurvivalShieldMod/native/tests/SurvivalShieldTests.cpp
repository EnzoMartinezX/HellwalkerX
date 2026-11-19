#include "SettingsStore.hpp"
#include "SurvivalShieldCore.hpp"
#include "BurnEngine.hpp"
#include "ModLibrary.hpp"
#include "DeclReader.hpp"
#include "ModsMenu.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++g_failures;
    }
}

class ScopedLocalAppDataOverride {
public:
    explicit ScopedLocalAppDataOverride(const std::filesystem::path& path) {
        const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (needed > 0) {
            std::vector<wchar_t> previous(needed);
            const DWORD written = GetEnvironmentVariableW(
                L"LOCALAPPDATA", previous.data(), static_cast<DWORD>(previous.size()));
            if (written > 0 && written < previous.size()) {
                previousValue_.assign(previous.data(), written);
                hadPreviousValue_ = true;
            }
        }
        changed_ = SetEnvironmentVariableW(L"LOCALAPPDATA", path.c_str()) != FALSE;
    }

    ~ScopedLocalAppDataOverride() { Restore(); }

    void Restore() noexcept {
        if (!changed_) {
            return;
        }
        SetEnvironmentVariableW(L"LOCALAPPDATA",
                                hadPreviousValue_ ? previousValue_.c_str() : nullptr);
        changed_ = false;
    }

    bool Changed() const noexcept { return changed_; }

private:
    std::wstring previousValue_;
    bool hadPreviousValue_ = false;
    bool changed_ = false;
};

struct EntityState {
    bool enemy = true;
    bool alive = true;
    survival_shield::Vec3 center{};
};

class MockCombatWorld final : public survival_shield::ICombatWorld {
public:
    bool IsEnemy(survival_shield::EntityKey entity) const override {
        const auto found = entities.find(entity);
        return found != entities.end() && found->second.enemy;
    }

    bool IsAlive(survival_shield::EntityKey entity) const override {
        const auto found = entities.find(entity);
        return found != entities.end() && found->second.alive;
    }

    survival_shield::Vec3 Center(survival_shield::EntityKey entity) const override {
        const auto found = entities.find(entity);
        return found == entities.end() ? survival_shield::Vec3{} : found->second.center;
    }

    std::vector<survival_shield::EnemyCandidate> QueryEnemies(
        const survival_shield::Vec3&, float) const override {
        return candidates;
    }

    bool HasLineOfSight(const survival_shield::Vec3&,
                        const survival_shield::Vec3& end) const override {
        for (const auto& [entity, state] : entities) {
            if (state.center.x == end.x && state.center.y == end.y && state.center.z == end.z) {
                return !blockedLineOfSight.contains(entity);
            }
        }
        return false;
    }

    void ApplyOrRefreshBurn(survival_shield::EntityKey target,
                            survival_shield::EntityKey,
                            std::uint64_t) override {
        burned.push_back(target);
    }

    std::unordered_map<survival_shield::EntityKey, EntityState,
                       survival_shield::EntityKeyHash> entities;
    std::vector<survival_shield::EnemyCandidate> candidates;
    std::unordered_set<survival_shield::EntityKey,
                       survival_shield::EntityKeyHash> blockedLineOfSight;
    std::vector<survival_shield::EntityKey> burned;
};

bool Contains(const std::vector<survival_shield::EntityKey>& values,
              survival_shield::EntityKey target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

void TestScoreRules() {
    using namespace survival_shield;

    ModConfig config{};
    config.targetScore = 500;
    config.pctFodder = 50;
    config.pctHeavy = 50;
    config.pctSuperheavy = 50;
    SurvivalShieldCore core(config);

    Check(!core.RecordDemonKill({{1, 1}, false, true, DemonCategory::Fodder}),
          "uncredited kill does not score");
    Check(!core.RecordDemonKill({{2, 1}, true, false, DemonCategory::Fodder}),
          "non-demon kill does not score");
    Check(!core.RecordDemonKill({{3, 1}, true, true, DemonCategory::Other}),
          "uncategorized demon does not score");

    Check(core.RecordDemonKill({{4, 1}, true, true, DemonCategory::Fodder}),
          "credited fodder kill is accepted");
    Check(core.ScorePoints() == 50, "fodder percentage scales its base score");
    Check(!core.RecordDemonKill({{4, 1}, true, true, DemonCategory::Fodder}),
          "same victim generation is counted only once");
    Check(core.RecordDemonKill({{4, 2}, true, true, DemonCategory::Fodder}),
          "reused entity id with a new spawn generation can score");
    Check(core.RecordDemonKill({{5, 1}, true, true, DemonCategory::Heavy}),
          "heavy kill is accepted");
    Check(core.RecordDemonKill({{6, 1}, true, true, DemonCategory::Superheavy}),
          "superheavy kill is accepted");
    Check(core.ScorePoints() == 750, "category totals retain their configured scaling");
    Check(core.GetScoreSnapshot().supercharged,
          "snapshot becomes ready once centipoints reach the threshold");

    config = core.GetConfig();
    config.enabled = false;
    config.targetScore = 550;
    config.poweredDashDamage = 825;
    config.pctFodder = 130;
    core.SetConfig(config);
    const ModConfig normalized = core.GetConfig();
    Check(!normalized.enabled, "enabled option can be disabled");
    Check(normalized.targetScore == 600, "target score is rounded to 100-point steps");
    Check(normalized.poweredDashDamage == 850, "dash damage is rounded to 50-point steps");
    Check(normalized.pctFodder == 100, "score percentage is clamped to 100 percent");
    Check(!core.IsSupercharged(), "disabled mod cannot remain supercharged");
}

void TestSurvivalBurnPropagation() {
    using namespace survival_shield;

    ModConfig cfg{};
    cfg.targetScore = 100;
    cfg.pctFodder = 100;
    SurvivalShieldCore core(cfg);
    Check(core.RecordDemonKill({{10, 1}, true, true, DemonCategory::Fodder}),
          "threshold setup kill is counted");

    const EntityKey primary{100, 1};
    const EntityKey forward{101, 1};
    const EntityKey side{102, 1};
    const EntityKey blocked{103, 1};
    const EntityKey dead{104, 1};
    const EntityKey behind{105, 1};

    MockCombatWorld world;
    world.entities.emplace(primary, EntityState{true, true, {0.0F, 0.0F, 0.0F}});
    world.entities.emplace(forward, EntityState{true, true, {0.0F, 3.0F, 0.0F}});
    world.entities.emplace(side, EntityState{true, true, {4.0F, 1.0F, 0.0F}});
    world.entities.emplace(blocked, EntityState{true, true, {0.0F, 2.0F, 0.0F}});
    world.entities.emplace(dead, EntityState{true, false, {0.0F, 1.0F, 0.0F}});
    world.entities.emplace(behind, EntityState{true, true, {0.0F, -2.0F, 0.0F}});
    world.blockedLineOfSight.insert(blocked);
    world.candidates = {{primary, {0.0F, 0.0F, 0.0F}},
                        {forward, {0.0F, 3.0F, 0.0F}},
                        {side, {4.0F, 1.0F, 0.0F}},
                        {blocked, {0.0F, 2.0F, 0.0F}},
                        {dead, {0.0F, 1.0F, 0.0F}},
                        {behind, {0.0F, -2.0F, 0.0F}}};

    const bool applied = core.OnShieldDashImpact(
        world, primary, {200, 1}, {0.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 1);
    Check(applied, "powered dash impact is accepted while the primary survives");
    Check(world.burned.size() == 2, "Burn reaches only surviving enemies in the clear cone");
    Check(Contains(world.burned, primary), "surviving primary receives Burn");
    Check(Contains(world.burned, forward), "eligible forward enemy receives propagated Burn");
    Check(!Contains(world.burned, side), "enemy outside the cone is not burned");
    Check(!Contains(world.burned, blocked), "enemy behind blocked line of sight is not burned");
    Check(!Contains(world.burned, dead), "dead enemy is not burned");
    Check(!Contains(world.burned, behind), "enemy behind the impact is not burned");

    world.entities.at(primary).alive = false;
    const std::size_t burnedBeforeDeadPrimary = world.burned.size();
    Check(!core.OnShieldDashImpact(
              world, primary, {200, 1}, {0.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 2),
          "dash does not propagate Burn when the primary died to the impact");
    Check(world.burned.size() == burnedBeforeDeadPrimary,
          "dead primary adds no Burn applications");

    SurvivalShieldCore uncharged;
    world.entities.at(primary).alive = true;
    Check(!uncharged.OnShieldDashImpact(
              world, primary, {200, 1}, {0.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 3),
          "uncharged shield cannot start the powered Burn chain");
}

void TestSettingsStore() {
    using namespace survival_shield;

    const auto tempRoot = std::filesystem::temp_directory_path() /
                          (L"HellwalkerSettingsTest-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(tempRoot, error);
    Check(!error, "temporary settings directory is created");
    if (error) {
        return;
    }

    ScopedLocalAppDataOverride localAppData(tempRoot);
    Check(localAppData.Changed(), "settings test redirects LOCALAPPDATA");
    if (!localAppData.Changed()) {
        std::filesystem::remove(tempRoot, error);
        return;
    }

    SettingsStore store;
    const auto filePath = store.FilePath();
    std::filesystem::create_directories(filePath.parent_path(), error);
    Check(!error, "settings file directory is created");
    if (error) {
        return;
    }

    {
        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        file << "{\n"
             << "  \"bEnableSurvivalShieldMod\": false-junk,\n"
             << "  \"iTargetSurvivalScore\": 950tail,\n"
             << "  \"iHellwalkerPoweredDashDamage\": 1800,\n"
             << "  \"iHellwalkerPctFodder\": 0,\n"
             << "  \"note\": \"\\\"iHellwalkerPctHeavy\\\": 0\"\n"
             << "}\n";
        Check(static_cast<bool>(file), "malformed-value fixture is written");
    }

    const ModConfig parsed = store.LoadOrDefault();
    Check(parsed.enabled, "invalid boolean token leaves the default intact");
    Check(parsed.targetScore == 300, "integer prefix with trailing junk is rejected (default 300)");
    Check(parsed.poweredDashDamage == 1800, "valid powered dash setting is loaded");
    Check(parsed.pctFodder == 0, "valid zero percentage is loaded");
    Check(parsed.pctHeavy == 10, "text nested in another field cannot override a setting (default 10)");
    Check(parsed.parryPct == 10, "default parryPct is 10 percent");

    ModConfig toSave{};
    toSave.enabled = false;
    toSave.targetScore = 257;
    toSave.poweredDashDamage = 879;
    toSave.pctFodder = 33;
    toSave.pctHeavy = 66;
    toSave.pctSuperheavy = 140;
    toSave.parryPct = 23;
    Check(store.Save(toSave), "normalized settings are saved");
    ModConfig roundTrip = store.LoadOrDefault();
    Check(!roundTrip.enabled, "saved enabled state round-trips");
    Check(roundTrip.targetScore == 300, "saved target uses the normalized 100-point step");
    Check(roundTrip.poweredDashDamage == 900, "saved dash damage uses the normalized 50-point step");
    Check(roundTrip.pctFodder == 33 && roundTrip.pctHeavy == 66 && roundTrip.pctSuperheavy == 100,
          "saved category percentages round-trip with clamping");
    Check(roundTrip.parryPct == 20, "saved parryPct normalizes to 10-percent steps (23 -> 20)");

    toSave.parryPct = 75;
    Check(store.Save(toSave) && store.LoadOrDefault().parryPct == 50,
          "parryPct clamps to max 50 percent");
    toSave.parryPct = -5;
    Check(store.Save(toSave) && store.LoadOrDefault().parryPct == 0,
          "parryPct clamps to min 0 percent");

    Check(SurvivalShieldCore::ParryPointsFor(100, 10) == 10.0,
          "parry 10% of 100 target is 10 points");
    Check(SurvivalShieldCore::ParryPointsFor(1800, 50) == 900.0,
          "parry 50% of 1800 target is 900 points");
    Check(SurvivalShieldCore::ParryPointsFor(2000, 0) == 0.0,
          "parry 0% disables parry credit");

    Check(BurnEngine::kTickDamagePct == 0.03,
          "burn tick deals 3% of max HP per tick");
    Check(BurnEngine::kMaxTicks == 8,
          "burn window runs 8 ticks (24% total over 4s)");

    Check(SurvivalShieldCore::SuperchargedBashDamageFor(2000, 1000.0F, DemonCategory::Fodder) == 2000.0F,
          "fodder bash respects configured floor");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(2000, 12000.0F, DemonCategory::Heavy) == 4200.0F,
          "heavy bash deals 35% at default 2000 slider");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(2000, 25000.0F, DemonCategory::Superheavy) == 5500.0F,
          "superheavy bash deals 22% at default 2000 slider");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(2000, 130000.0F, DemonCategory::Superheavy) == 15600.0F,
          "boss bash deals 12% at default 2000 slider");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(1000, 12000.0F, DemonCategory::Heavy) == 2100.0F,
          "slider 1000 halves heavy bash scaling");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(5000, 12000.0F, DemonCategory::Heavy) == 9000.0F,
          "slider 5000 caps heavy bash scaling at 75%");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(5000, 130000.0F, DemonCategory::Superheavy) == 39000.0F,
          "boss bash deals 30% (12% * 2.5x, capped at 35%) at max 5000 slider");
    Check(SurvivalShieldCore::SuperchargedBashDamageFor(1800, 0.0F, DemonCategory::Other) == 1800.0F,
          "unreadable max health falls back to configured slider damage");

    std::filesystem::remove(filePath, error);
    std::filesystem::remove(filePath.parent_path(), error);
    std::filesystem::remove(filePath.parent_path().parent_path(), error);
    localAppData.Restore();
    std::filesystem::remove(tempRoot, error);
}

void TestModLibrary() {
    using namespace survival_shield;

    // --- ParseManifest: formato real de darkagesmod.txt ---
    const std::string manifest =
        "modinfo = {\n"
        "    name = \"Hellwalker Intro Skip\"\n"
        "    game = \"Doom The Dark Ages\"\n"
        "    author = \"Enzo\"\n"
        "    description = \"Skips the startup screens\"\n"
        "    version = \"1.0\"\n"
        "    loadPriority = 2\n"
        "    requiredVersion = 1\n"
        "}\n";
    const ModLibraryEntry parsed = ModLibrary::ParseManifest(manifest, "HellwalkerIntroSkip");
    Check(parsed.name == "Hellwalker Intro Skip", "manifest name is parsed");
    Check(parsed.author == "Enzo", "manifest author is parsed");
    Check(parsed.version == "1.0", "manifest version is parsed");
    Check(parsed.loadPriority == 2, "manifest loadPriority is parsed");
    Check(parsed.requiredVersion == 1, "manifest requiredVersion is parsed");

    // --- Manifest mínimo: sin modinfo el nombre cae a la carpeta ---
    const ModLibraryEntry minimal = ModLibrary::ParseManifest("{\n}\n", "SoloCarpeta");
    Check(minimal.name == "SoloCarpeta", "missing name falls back to the folder name");
    Check(minimal.loadPriority == 0, "missing loadPriority stays zero");

    // --- Diagnóstico: manifest roto se registra, no revienta el escaneo ---
    const ModLibraryEntry broken =
        ModLibrary::ParseManifest("modinfo = {\n    loadPriority = \"alto\"\n}\n", "Roto");
    Check(broken.error.find("loadPriority") != std::string::npos,
          "non-numeric loadPriority is reported in the diagnostic field");
    Check(broken.loadPriority == 0, "non-numeric loadPriority falls back to zero");
    const ModLibraryEntry vacio =
        ModLibrary::ParseManifest("// solo comentarios\n", "Vacio");
    Check(vacio.error.find("sin campos") != std::string::npos,
          "manifest without modinfo fields is flagged");
    Check(vacio.name == "Vacio", "flagged manifest still resolves a usable name");

    // --- Escaneo + registro persistente ---
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          (L"HellwalkerModLibTest-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(tempRoot, error);
    Check(!error, "temporary mod library directory is created");
    if (error) {
        return;
    }

    const auto writeText = [](const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };

    const std::string modA =
        "modinfo = {\n"
        "    name = \"Mod A\"\n"
        "    author = \" Tester \"\n"
        "    version = \"0.1\"\n"
        "    loadPriority = 5\n"
        "}\n";
    writeText(tempRoot / L"mods" / L"ModA" / L"darkagesmod.txt", modA);
    writeText(tempRoot / L"mods" / L"ModA" / L"decls" / L"uiwidget" / L"a.decl", "{}\n");
    writeText(tempRoot / L"mods" / L"ModA" / L"decls" / L"uiwidget" / L"sub" / L"b.decl", "{}\n");
    writeText(tempRoot / L"mods" / L"ModB.off" / L"darkagesmod.txt",
              "modinfo = {\n    name = \"Mod B\"\n}\n");
    // Carpeta sin manifest: no es un mod para la biblioteca.
    writeText(tempRoot / L"mods" / L"NotAMod" / L"readme.txt", "hola\n");

    ModLibrary library;
    const auto registryPath = tempRoot / L"registry" / L"modlibrary.json";
    Check(library.Scan(tempRoot / L"mods", registryPath), "first scan persists the registry");

    const auto& entries = library.Entries();
    Check(entries.size() == 2, "scan registers exactly the folders with a manifest");
    Check(!entries.empty() && entries[0].folder == "ModA",
          "higher loadPriority sorts first");
    Check(!entries.empty() && entries[0].declFiles == 2,
          "decl file count recurses into decls/");
    Check(!entries.empty() && entries[0].declTypes.size() == 1 &&
              entries[0].declTypes.at("uiwidget") == 2,
          "decl type inventory groups files by first-level folder");
    Check(entries[0].error.empty(), "valid manifest carries no diagnostic");
    Check(!entries.empty() && entries[0].enabled, "folder without .off suffix is enabled");
    const auto entryB = std::find_if(entries.begin(), entries.end(), [](const ModLibraryEntry& e) {
        return e.folder == "ModB.off";
    });
    Check(entryB != entries.end() && !entryB->enabled, ".off folder is registered as disabled");
    Check(entryB != entries.end() && entryB->present, ".off folder is still present on disk");
    Check(entryB != entries.end() && !entryB->firstSeen.empty(),
          "firstSeen timestamp is recorded");

    // --- Manifest roto: queda registrado con diagnóstico, sin caer ---
    writeText(tempRoot / L"mods" / L"ModC" / L"darkagesmod.txt",
              "modinfo = {\n    name = \"Mod C\"\n    loadPriority = alto\n}\n");
    ModLibrary withBroken;
    Check(withBroken.Scan(tempRoot / L"mods", registryPath),
          "scan survives a broken manifest");
    const auto entryC = std::find_if(withBroken.Entries().begin(),
                                     withBroken.Entries().end(),
                                     [](const ModLibraryEntry& e) { return e.folder == "ModC"; });
    Check(entryC != withBroken.Entries().end() && entryC->name == "Mod C",
          "broken manifest keeps the mod registered");
    Check(entryC != withBroken.Entries().end() &&
              entryC->error.find("loadPriority") != std::string::npos,
          "broken field is reported in the diagnostic");

    // --- Renombrar ModC -> ModC.off reutiliza su entrada (sin duplicados .off) ---
    std::filesystem::rename(tempRoot / L"mods" / L"ModC", tempRoot / L"mods" / L"ModC.off", error);
    Check(!error, "renaming ModC to ModC.off succeeds");
    ModLibrary afterDisable;
    Check(afterDisable.Scan(tempRoot / L"mods", registryPath),
          "scan after .off rename persists the registry");
    Check(afterDisable.Entries().size() == 3,
          ".off rename updates existing entry instead of creating a duplicate");
    const auto entryCOff = std::find_if(afterDisable.Entries().begin(),
                                        afterDisable.Entries().end(),
                                        [](const ModLibraryEntry& e) { return e.folder == "ModC.off"; });
    Check(entryCOff != afterDisable.Entries().end() && !entryCOff->enabled && entryCOff->present,
          "renamed .off mod is marked disabled and present");

    // --- Rescan tras borrar ModA: sigue en la biblioteca como dado de baja ---
    std::filesystem::remove_all(tempRoot / L"mods" / L"ModA", error);
    ModLibrary rescanned;
    Check(rescanned.Scan(tempRoot / L"mods", registryPath), "second scan persists the registry");
    const auto& kept = rescanned.Entries();
    Check(kept.size() == 3, "removed mod is retained by the library");
    const auto keptA = std::find_if(kept.begin(), kept.end(), [](const ModLibraryEntry& e) {
        return e.folder == "ModA";
    });
    Check(keptA != kept.end() && !keptA->present, "removed mod is marked as not present");
    Check(keptA != kept.end() && keptA->name == "Mod A", "removed mod keeps its manifest data");

    // --- El JSON del registro se puede releer con las mismas entradas ---
    std::string registryJson;
    {
        std::ifstream registryFile(registryPath, std::ios::binary);
        registryJson.assign((std::istreambuf_iterator<char>(registryFile)),
                            std::istreambuf_iterator<char>());
    }
    Check(registryJson.find("\"folder\": \"ModB.off\"") != std::string::npos,
          "registry JSON contains the disabled mod");
    ModLibrary reloaded;
    Check(reloaded.Scan(tempRoot / L"mods" / L"nonexistent", registryPath),
          "registry re-scan tolerates a missing mods directory");
    bool reloadedSeesModB = false;
    for (const auto& entry : reloaded.Entries()) {
        if (entry.folder == "ModB.off" && entry.name == "Mod B" && !entry.enabled) {
            reloadedSeesModB = true;
        }
    }
    Check(reloadedSeesModB, "registry round-trips through modlibrary.json");

    std::filesystem::remove_all(tempRoot, error);
}

void TestDeclReader() {
    using namespace survival_shield;

    // Formato real de un uiwidget de pantalla de arranque.
    const std::string declText =
        "{\n"
        "\tinherit = \"ui/base/menu_screen\";\n"
        "\tedit = {\n"
        "\t\tclass = \"idUIWidget_Menu_Fixed_Time_Start_Screen\";\n"
        "\t\tmodel = {\n"
        "\t\t\tclassName = \"idUIWidgetModel_Menu_Fixed_Time_Start_Screen\";\n"
        "\t\t\tobject = {\n"
        "\t\t\t\tduration = 1;\n"
        "\t\t\t\t// comentario ignorado\n"
        "\t\t\t\tgeneralLaunchTargetScreen = \"start\";\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tchildWidgets = {\n"
        "\t\t\tnum = 1;\n"
        "\t\t\titem[0] = {\n"
        "\t\t\t\tmodelBindings = { \"icon\" = \"displayMedia\"; }\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}\n";
    DeclInfo parsed;
    Check(DeclReader::ParseText(declText, &parsed), "well-formed decl parses");
    Check(parsed.inherit == "ui/base/menu_screen", "root inherit is captured");
    Check(parsed.className == "idUIWidget_Menu_Fixed_Time_Start_Screen",
          "edit.class is captured");
    bool hasDuration = false;
    for (const auto& prop : parsed.props) {
        if (prop == "edit.model.object.duration") {
            hasDuration = true;
        }
    }
    Check(hasDuration, "nested property path under edit is flattened");
    Check(parsed.props.size() >= 6, "all scalar properties are listed");

    DeclInfo garbage;
    Check(!DeclReader::ParseText("esto no es un decl", &garbage),
          "text without a root block is rejected");

    // --- Decl "plana" (sin llaves raíz), formato real de entityDef syncmelee ---
    const std::string flatDecl =
        "inherit = \"entityDef/syncmelee/base\";\n"
        "expandInheritance = true;\n"
        "systemVars = {\n"
        "\tentityType = \"idSyncEntity\";\n"
        "}\n"
        "edit = {\n"
        "\tsyncAnim = \"animweb/syncmelee/playervsai/tyrant/null/null\";\n"
        "\tparticipants = {\n"
        "\t\tnum = 10;\n"
        "\t\titem[0] = {\n"
        "\t\t\tattachJointName = \"attach00\";\n"
        "\t\t}\n"
        "\t}\n"
        "}\n";
    DeclInfo flat;
    Check(DeclReader::ParseText(flatDecl, &flat), "flat decl without root braces parses");
    Check(flat.inherit == "entityDef/syncmelee/base", "flat decl inherit is captured");
    bool hasSyncAnim = false;
    for (const auto& prop : flat.props) {
        if (prop == "edit.syncAnim") {
            hasSyncAnim = true;
        }
    }
    Check(hasSyncAnim, "flat decl edit properties are flattened");
}

void TestModLibraryDeclFichas() {
    using namespace survival_shield;

    const auto tempRoot = std::filesystem::temp_directory_path() /
                          (L"HellwalkerDeclFichasTest-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(tempRoot, error);
    if (error) {
        return;
    }

    const auto writeText = [](const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };

    const std::string bethesdaDecl =
        "{\n"
        "\tinherit = \"ui/base/menu_screen\";\n"
        "\tedit = {\n"
        "\t\tclass = \"idUIWidget_Menu_Fixed_Time_Start_Screen\";\n"
        "\t\tmodel = { className = \"X\"; object = { duration = 1; } }\n"
        "\t}\n"
        "}\n";

    // Dos mods activos que tocan el MISMO decl: solape.
    writeText(tempRoot / L"mods" / L"ModA" / L"darkagesmod.txt",
              "modinfo = {\n    name = \"Mod A\"\n    loadPriority = 2\n}\n");
    writeText(tempRoot / L"mods" / L"ModA" / L"decls" / L"uiwidget" / L"shared.decl",
              bethesdaDecl);
    writeText(tempRoot / L"mods" / L"ModB" / L"darkagesmod.txt",
              "modinfo = {\n    name = \"Mod B\"\n    loadPriority = 1\n}\n");
    writeText(tempRoot / L"mods" / L"ModB" / L"decls" / L"uiwidget" / L"shared.decl",
              bethesdaDecl);
    // Y un decl propio de ModB sin solape.
    writeText(tempRoot / L"mods" / L"ModB" / L"decls" / L"setting" / L"own.decl",
              "{ edit = { propertyName = \"ownProp\"; } }\n");

    ModLibrary library;
    const auto registryPath = tempRoot / L"modlibrary.json";
    Check(library.Scan(tempRoot / L"mods", registryPath), "decl ficha scan persists");

    const auto& conflicts = library.Conflicts();
    Check(conflicts.size() == 1, "exactly one decl overlap is detected");
    if (!conflicts.empty()) {
        Check(conflicts[0].decl == "uiwidget/shared",
              "overlap key is decl type plus path without extension");
        // Orden por prioridad: ModA (2) antes que ModB (1).
        Check(conflicts[0].mods.size() == 2 && conflicts[0].mods[0] == "ModA" &&
                  conflicts[0].mods[1] == "ModB",
              "overlap lists mods ordered by load priority");
        // Ambos tocan edit.model.object.duration en el mismo decl.
        const auto sharedDuration = std::find(conflicts[0].props.begin(),
                                              conflicts[0].props.end(),
                                              "edit.model.object.duration");
        Check(sharedDuration != conflicts[0].props.end(),
              "property intersection reports the shared edited path");
    }

    const auto entryB = std::find_if(library.Entries().begin(), library.Entries().end(),
                                     [](const ModLibraryEntry& e) { return e.folder == "ModB"; });
    Check(entryB != library.Entries().end() && entryB->decls.size() == 2,
          "mod fichas include every decl file");
    const auto own = std::find_if(entryB->decls.begin(), entryB->decls.end(),
                                  [](const DeclInfo& d) { return d.type == "setting"; });
    Check(own != entryB->decls.end() && own->name == "own" &&
              own->className.empty(),
          "setting decl ficha has no class and its path has no extension");
    bool hasDuration = false;
    for (const auto& decl : entryB->decls) {
        for (const auto& prop : decl.props) {
            if (prop == "edit.model.object.duration") {
                hasDuration = true;
            }
        }
    }
    Check(hasDuration, "ficha props survive the scan round-trip");

    // El JSON del registro trae decls y conflictos legibles.
    std::ifstream registryFile(registryPath, std::ios::binary);
    const std::string registryJson((std::istreambuf_iterator<char>(registryFile)),
                                   std::istreambuf_iterator<char>());
    registryFile.close();
    Check(registryJson.find("\"decls\": [") != std::string::npos &&
              registryJson.find("\"conflicts\": [") != std::string::npos,
          "registry JSON contains decl fichas and conflicts sections");

    // Re-lectura del registro: desaparecido ModA, el solape se poda.
    std::filesystem::remove_all(tempRoot / L"mods" / L"ModA", error);
    ModLibrary rescanned;
    rescanned.Scan(tempRoot / L"mods", registryPath);
    Check(rescanned.Conflicts().empty(),
          "overlap is dropped once only one side remains");

    std::filesystem::remove_all(tempRoot, error);
}

void TestModsMenu() {
    using namespace survival_shield;

    // --- menuId: regla compartida con generate_mods_menu.ps1 (sin prefijo;
    // el prefijo game/modslist_ lo agregan el generador y el bridge) ---
    Check(MenuIdForFolder("HellwalkerIntroSkip") == "hellwalkerintroskip",
          "folder name sanitizes to lowercase id");
    Check(MenuIdForFolder("Mod  B--2") == "mod_b_2",
          "separators collapse and borders trim");
    Check(MenuIdForFolder("...") == "mod",
          "empty after sanitize falls back to mod");
    Check(MenuIdForFolder("ModB.off") == "modb",
          "disabled suffix is stripped so the row id is toggle-stable");
    Check(MenuIdForFolder("ModB.DISABLED") == "modb",
          "disabled suffix strip is case-insensitive");

    // --- registro de filas (menuId -> carpeta + estado) ---
    std::map<std::string, std::pair<std::string, bool>> rows;
    rows["modb"] = {"ModB", true};
    ModsMenuSetRows(rows);
    std::string folder;
    bool enabledNow = false;
    Check(ModsMenuLookupRow(L"modb", &folder, &enabledNow) && folder == "ModB" &&
              enabledNow,
          "row registry resolves menuId to folder and state");
    bool missed = ModsMenuLookupRow(L"modslist_unknown", nullptr, nullptr);
    Check(!missed, "unknown menuId does not resolve");

    // --- estado deseado persistente ---
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          (L"HellwalkerModsMenuTest-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(tempRoot, error);
    if (error) {
        return;
    }
    const auto statePath = tempRoot / L"mods_menu_state.json";

    bool out = true;
    ModsMenuGetDesired("ModB", false, &out);
    Check(!out, "without a stored desired state the scan fallback applies");
    Check(ModsMenuSetDesired("ModB", false, statePath), "desired state persists to disk");
    ModsMenuGetDesired("ModB", true, &out);
    Check(!out, "stored desired state overrides the fallback");
    ModsMenuGetDesired("ModC", true, &out);
    Check(out, "unrelated folder keeps its fallback");

    const auto loaded = ModsMenuLoadDesired(statePath);
    Check(loaded.size() == 1 && loaded.count("ModB") == 1 && !loaded.at("ModB"),
          "state file round-trips");

    Check(ModsMenuSetDesired("ModB.off", false, statePath),
          "setting with .off suffix normalizes to base key");
    Check(ModsMenuLookupRow(L"modb", nullptr, nullptr) &&
              (ModsMenuGetDesired("ModB", true, &out), !out),
          "lookup by base folder sees state written with .off suffix");

    const auto preloadedPath = tempRoot / L"mods_menu_state_init.json";
    {
        std::ofstream file(preloadedPath, std::ios::binary | std::ios::trunc);
        file << "{\n  \"ModX.off\": false,\n  \"ModY\": true\n}\n";
    }
    ModsMenuInitializeDesired(preloadedPath);
    ModsMenuGetDesired("ModX", true, &out);
    Check(!out, "initialized desired state preserves false entry");
    ModsMenuGetDesired("ModY", false, &out);
    Check(out, "initialized desired state preserves true entry");
    Check(ModsMenuSetDesired("ModZ", false, preloadedPath),
          "adding new entry persists alongside initialized entries");
    const auto reloadedInit = ModsMenuLoadDesired(preloadedPath);
    Check(reloadedInit.count("ModX") == 1 && !reloadedInit.at("ModX") &&
              reloadedInit.count("ModY") == 1 && reloadedInit.at("ModY") &&
              reloadedInit.count("ModZ") == 1 && !reloadedInit.at("ModZ"),
          "initialized entries survive saving a new toggle");

    std::filesystem::remove_all(tempRoot, error);
}

} // namespace

int main() {
    TestScoreRules();
    TestSurvivalBurnPropagation();
    TestSettingsStore();
    TestModLibrary();
    TestDeclReader();
    TestModLibraryDeclFichas();
    TestModsMenu();

    if (g_failures != 0) {
        std::cerr << g_failures << " SurvivalShield check(s) failed.\n";
        return 1;
    }
    std::cout << "All SurvivalShield core/settings checks passed.\n";
    return 0;
}
