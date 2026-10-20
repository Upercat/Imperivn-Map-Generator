// W7 window: drop or open a replay or saved game, see what map it is, save it as a .BFHP or install it into the
// game's Scenarios folder. Plain Win32; all game work runs on one worker thread at a time.
//
// Hidden test options (not documented to users): --game <folder>, --lang es|en, --screenshot <out.bmp> (captures
// the window once the file given on the command line is processed, then exits), --export <out.BFHP> (writes the
// packed map without a dialog, with the screenshot run).
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <atomic>
#include <random>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "app_settings.hpp"
#include "safe_output.hpp"
#include "gallery.hpp"
#include "extract.hpp"
#include "input_reader.hpp"
#include "map_assembly.hpp"
#include "minimap.hpp"
#include "ui_strings.hpp"

namespace {

namespace fs = std::filesystem;
using mapx::ui::Lang;
using mapx::ui::S;

constexpr UINT kMsgInstall = WM_APP + 1, kMsgProgress = WM_APP + 2, kMsgResult = WM_APP + 3, kMsgGalleryItem = WM_APP + 4, kMsgGalleryDone = WM_APP + 5;
constexpr UINT_PTR kTimerScreenshot = 1;
enum Control { IdOpen = 101, IdSave, IdInstall, IdChange, IdLang, IdTabs, IdTemplate, IdSize, IdPlayers,
    IdRace, IdSeed, IdRandomSeed, IdGenerate, IdNext, IdCancel, IdBack, IdCard = 200 };
enum class Tone { Neutral, Busy, Good, Warn, Bad };

// Layout in 96-dpi units.
constexpr int kW = 1040, kH = 784, kM = 16, kMap = 288, kBar = 30, kTabOffset = 48;

struct PlayerRow {
    int slot = 0;
    std::wstring name;
    int race = 8;
    int team = 0;
    bool human = false;
};

// Everything shown about one input, built on the worker.
struct Result {
    fs::path path;
    mapx::Kind kind = mapx::Kind::Unreadable;
    S message = S::InternalError;
    Tone tone = Tone::Bad;
    std::wstring template_name;
    std::uint32_t map_size = 0, seed = 0;
    std::vector<PlayerRow> players;
    std::vector<std::uint8_t> volume;  // the packed .BFHP, empty when there is nothing to save
    bool confirmed = false;            // verified, or a replay (nothing to verify against)
    mapx::Image minimap;
    std::array<int, static_cast<std::size_t>(mapx::Feature::Count)> counts{};  // markers per feature
};

struct InstallResult {
    std::unique_ptr<mapx::Install> install;
    std::optional<S> error;
    std::string text_language;
    std::vector<mapx::GalleryTemplate> templates;
};

struct GalleryItem {
    std::size_t index = 0;
    std::unique_ptr<Result> result;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND open = nullptr, save = nullptr, install = nullptr, change = nullptr, lang_button = nullptr;
    HWND tabs = nullptr, template_combo = nullptr, size_combo = nullptr, players_combo = nullptr, race_combo = nullptr;
    HWND back = nullptr;
    bool enlarged = false, test_enlarged = false;
    HWND seed_edit = nullptr, random_seed = nullptr, generate = nullptr, next = nullptr, cancel_button = nullptr;
    std::array<HWND, 9> cards{};
    std::array<std::unique_ptr<Result>, 9> gallery;
    std::vector<mapx::GalleryTemplate> templates;
    mapx::GalleryRequest gallery_request;
    int tab = 0, selected = -1;
    std::size_t gallery_finished = 0;
    bool generating_gallery = false, automatic_gallery = false, gallery_cancelled = false;
    bool test_next_batch = false, test_maximized = false;
    int test_cancel_after = 0;
    UINT test_dpi = 0;
    std::atomic<bool> cancel_requested{false};
    std::wstring initial_seed = L"12345678";
    std::string initial_template = "CONTINENT";
    int initial_size = 1, initial_players = 4, test_selected = 0;
    fs::path ui_report;
    Lang lang = Lang::Es;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, big = nullptr;
    fs::path game;
    std::unique_ptr<mapx::Install> installation;
    std::string text_language = "english";
    bool loading_game = false;
    std::optional<S> game_error;
    std::thread worker;
    bool busy = false;
    std::size_t progress_i = 0, progress_n = 0;
    std::unique_ptr<Result> result;
    fs::path pending;
    std::optional<std::wstring> notice;  // after saving or installing
    Tone notice_tone = Tone::Good;
    fs::path screenshot, export_path;
} g;

void layout();
void start_gallery(bool next_batch = false);
void refresh_gallery_settings(bool reset = false);
void switch_tab(int tab);
void select_gallery(int index);
void open_detail(int index);
void close_detail();

Result* current_result() {
    if (g.tab == 0) return g.result.get();
    return g.selected >= 0 ? g.gallery[static_cast<std::size_t>(g.selected)].get() : nullptr;
}

const wchar_t* tr(S id) { return mapx::ui::tr(g.lang, id); }
int px(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }
int client_width() { RECT r{}; GetClientRect(g.hwnd, &r); return MulDiv(r.right, 96, g.dpi); }
int client_height() { RECT r{}; GetClientRect(g.hwnd, &r); return MulDiv(r.bottom, 96, g.dpi); }
int sidebar_x() { return client_width() - kM - 300; }
int gallery_cell_width() { return (sidebar_x() - 32) / 3; }
int gallery_cell_height() { return (client_height() - 224) / 3; }
int gallery_image_size(int width, int height) { return std::max(1, std::min(width - px(12), height - px(36))); }

std::wstring widen_latin1(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(1252, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(1252, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring format1(const wchar_t* pattern, const std::wstring& arg) {
    std::wstring p = pattern;
    if (const auto at = p.find(L"%s"); at != std::wstring::npos) p.replace(at, 2, arg);
    return p;
}

// ---------------------------------------------------------------------------------------------- worker side

S problem_message(mapx::Problem p) {
    switch (p) {
    case mapx::Problem::NotGameFile: return S::NotGameFile;
    case mapx::Problem::Damaged: return S::Damaged;
    case mapx::Problem::GameVersion: return S::GameVersion;
    case mapx::Problem::UnknownLayout: return S::UnknownLayout;
    case mapx::Problem::ModdedGame: return S::ModdedGame;
    case mapx::Problem::None: break;
    }
    return S::InternalError;
}

S install_message(mapx::InstallProblem p) {
    switch (p) {
    case mapx::InstallProblem::NotAGameFolder: return S::NotAGameFolder;
    case mapx::InstallProblem::ModdedConstants: return S::ModdedConstants;
    case mapx::InstallProblem::ModdedData: return S::ModdedData;
    case mapx::InstallProblem::TemplateMissing: return S::TemplateMissing;
    case mapx::InstallProblem::GenerationFailed: return S::GenerationFailed;
    case mapx::InstallProblem::None: break;
    }
    return S::InternalError;
}

// `map_px`: preview raster resolution, independent of the resizable window.
std::unique_ptr<Result> process(mapx::Install& install, const std::string& text_language, const fs::path& path, HWND hwnd,
                                int map_px) {
    auto r = std::make_unique<Result>();
    r->path = path;
    const auto in = mapx::read_input(path);
    r->kind = in.kind;
    switch (in.kind) {
    case mapx::Kind::Unreadable:
    case mapx::Kind::Unsupported:
        r->message = problem_message(in.problem);
        return r;
    case mapx::Kind::PredefinedMap:
        r->message = S::Predefined, r->tone = Tone::Warn;
        return r;
    case mapx::Kind::Campaign:
        r->message = S::Campaign, r->tone = Tone::Warn;
        return r;
    default:
        break;
    }
    const auto& rm = in.settings.random_map;
    r->map_size = rm.map_size;
    r->seed = in.settings.sync_seed;
    r->template_name = widen_latin1(mapx::template_name(rm.template_path));
    try {
        static const std::regex name_re(R"re(<map-template\s+name\s*=\s*"([^"]*)")re");
        const auto xml = install.fs().read_text(rm.template_path);
        std::smatch m;
        if (std::regex_search(xml, m, name_re) && !m[1].str().empty()) r->template_name = widen_latin1(m[1].str());
    } catch (const std::exception&) {
        // a missing template is reported by the generation below
    }
    for (const int k : mapx::active_slots(rm)) {
        const auto& p = in.settings.players[static_cast<std::size_t>(k)];
        r->players.push_back({k, widen_latin1(p.name), rm.races[static_cast<std::size_t>(k)], rm.teams[static_cast<std::size_t>(k)], p.control == 0});
    }
    try {
        const auto ex = mapx::extract_map(install, in, [&](std::size_t i, std::size_t n) {
            PostMessageW(hwnd, kMsgProgress, i, n);
        });
        // resolved races, and the AI names a replay's session would generate
        for (auto& row : r->players) {
            row.race = ex.pre.races[static_cast<std::size_t>(row.slot)];
            const auto idx = ex.pre.name_index[static_cast<std::size_t>(row.slot)];
            if (row.name.empty() && in.kind == mapx::Kind::RandomMapReplay && idx >= 0 && row.race >= 0 && row.race < 8) {
                const auto& table = install.names().names[static_cast<std::size_t>(row.race)];
                if (static_cast<std::size_t>(idx) < table.size()) row.name = widen_latin1(table[static_cast<std::size_t>(idx)]);
            }
        }
        const auto files = mapx::assemble_map(install, ex.map, in, ex.pre, text_language);
        r->volume = mapx::pack_map(install, files);
        const auto markers = mapx::map_markers(install, ex.map);
        r->counts = mapx::feature_counts(markers);
        r->minimap = mapx::render_minimap(install, ex.map, in, map_px, &markers);
        switch (ex.verification) {
        case mapx::Verification::Verified: r->message = S::Verified, r->tone = Tone::Good, r->confirmed = true; break;
        case mapx::Verification::NotVerifiable: r->message = S::NotVerifiable, r->tone = Tone::Neutral, r->confirmed = true; break;
        case mapx::Verification::Mismatch: r->message = S::Mismatch, r->tone = Tone::Warn; break;
        }
    } catch (const mapx::InstallError& e) {
        r->message = install_message(e.problem);
    } catch (const mapx::NoCandidateError&) {
        r->message = S::NoCandidate;
    } catch (const std::exception&) {
        r->message = S::InternalError;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------- UI side

void update_controls() {
    const bool idle = !g.busy && !g.loading_game;
    const auto* result = current_result();
    const bool can_save = idle && result && !result->volume.empty();
    EnableWindow(g.tabs, idle);
    for (HWND control : {g.template_combo, g.size_combo, g.players_combo, g.race_combo, g.seed_edit, g.random_seed})
        EnableWindow(control, idle);
    EnableWindow(g.generate, idle && g.installation && !g.templates.empty());
    EnableWindow(g.next, idle && g.installation && !g.templates.empty());
    EnableWindow(g.cancel_button, g.busy && g.generating_gallery && !g.cancel_requested);
    for (std::size_t i = 0; i < g.cards.size(); ++i)
        EnableWindow(g.cards[i], g.gallery[i] && !g.gallery[i]->volume.empty());
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(tr(S::TabRecover));
    TabCtrl_SetItem(g.tabs, 0, &item);
    item.pszText = const_cast<wchar_t*>(tr(S::TabGenerate));
    TabCtrl_SetItem(g.tabs, 1, &item);
    SetWindowTextW(g.back, tr(S::GalleryBack));
    SetWindowTextW(g.generate, tr(S::GalleryGenerate));
    SetWindowTextW(g.next, tr(S::GalleryNext));
    SetWindowTextW(g.cancel_button, tr(S::GalleryCancel));
    SetWindowTextW(g.random_seed, tr(S::GalleryRandomSeed));
    EnableWindow(g.open, idle);
    EnableWindow(g.change, idle);
    EnableWindow(g.save, can_save);
    EnableWindow(g.install, can_save && !g.game.empty());
    SetWindowTextW(g.hwnd, tr(S::AppTitle));
    SetWindowTextW(g.open, tr(S::OpenFile));
    SetWindowTextW(g.save, tr(S::SaveBfhp));
    SetWindowTextW(g.install, tr(S::InstallGame));
    SetWindowTextW(g.change, tr(S::ChangeFolder));
    SetWindowTextW(g.lang_button, tr(S::OtherLanguage));
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void join_worker() {
    if (g.worker.joinable()) g.worker.join();
}

void load_game(const fs::path& folder) {
    join_worker();
    g.game = folder;
    g.installation.reset();
    g.result.reset();
    for (auto& item : g.gallery) item.reset();
    g.selected = -1;
    g.gallery_finished = 0;
    g.gallery_request = {};
    g.gallery_cancelled = false;
    g.templates.clear();
    refresh_gallery_settings();
    g.game_error.reset();
    g.loading_game = true;
    update_controls();
    const HWND hwnd = g.hwnd;
    g.worker = std::thread([folder, hwnd] {
        auto r = std::make_unique<InstallResult>();
        try {
            r->install = std::make_unique<mapx::Install>(folder);
            r->text_language = mapx::app::game_text_language(folder);
            r->templates = mapx::gallery_templates(r->install->fs());
        } catch (const mapx::InstallError& e) {
            r->error = install_message(e.problem);
        } catch (const std::exception&) {
            r->error = S::NotAGameFolder;
        }
        if (PostMessageW(hwnd, kMsgInstall, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
    });
}

void start_file(const fs::path& path) {
    if (g.busy) return;
    switch_tab(0);
    g.notice.reset();
    if (!g.installation) {
        if (g.loading_game) g.pending = path;
        else g.notice = tr(S::NeedGameFolder), g.notice_tone = Tone::Warn;
        update_controls();
        return;
    }
    join_worker();
    g.busy = true;
    g.progress_i = g.progress_n = 0;
    g.result.reset();
    update_controls();
    mapx::Install* install = g.installation.get();
    const std::string text_language = g.text_language;
    const int map_px = 1024;
    const HWND hwnd = g.hwnd;
    g.worker = std::thread([install, text_language, path, hwnd, map_px] {
        std::unique_ptr<Result> r;
        try {
            r = process(*install, text_language, path, hwnd, map_px);
        } catch (...) {
            r = std::make_unique<Result>();
            r->path = path;
        }
        if (PostMessageW(hwnd, kMsgResult, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
    });
}

std::wstring filter(S id) {
    std::wstring f = tr(id);
    std::replace(f.begin(), f.end(), L'|', L'\0');
    return f;
}

void choose_file() {
    std::wstring buf(32768, L'\0');
    const auto flt = filter(S::OpenFilter);
    const std::wstring initial = g.game.empty() ? std::wstring() : (g.game / L"Profiles").wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = flt.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) start_file(fs::path(buf.c_str()));
}

void choose_game_folder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(tr(S::ChooseGameFolder));
    fs::path chosen;
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                chosen = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (chosen.empty()) return;
    g.notice.reset();
    if (!mapx::app::looks_like_game(chosen)) {
        g.notice = tr(S::NotAGameFolder), g.notice_tone = Tone::Bad;
        update_controls();
        return;
    }
    mapx::app::ini_write(L"GameFolder", chosen.wstring());
    load_game(chosen);
}

// A file name from the input's name: letters, digits, spaces, '-' and '_' only (the game lists maps by file name).
std::wstring suggested_name() {
    std::wstring out;
    for (const wchar_t c : current_result()->path.stem().wstring())
        out += (c < 128 && (iswalnum(c) || c == L' ' || c == L'-' || c == L'_')) ? c : L'_';
    while (!out.empty() && (out.back() == L' ' || out.back() == L'_')) out.pop_back();
    return out.empty() ? std::wstring(L"mapa") : out;
}

bool confirm_unverified() {
    if (current_result()->confirmed) return true;
    return MessageBoxW(g.hwnd, tr(S::ConfirmMismatch), tr(S::AppTitle), MB_YESNO | MB_ICONWARNING) == IDYES;
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    return mapx::app::write_file_atomic(path, bytes);
}

void save_as() {
    if (!current_result() || current_result()->volume.empty() || !confirm_unverified()) return;
    std::wstring buf = suggested_name() + L".BFHP";
    buf.resize(32768, L'\0');
    const auto flt = filter(S::SaveFilter);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = flt.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrDefExt = L"BFHP";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    const fs::path target(buf.c_str());
    if (write_file(target, current_result()->volume)) g.notice = format1(tr(S::SavedAs), target.wstring()), g.notice_tone = Tone::Good;
    else g.notice = tr(S::WriteFailed), g.notice_tone = Tone::Bad;
    update_controls();
}

void install_in_game() {
    if (!current_result() || current_result()->volume.empty() || g.game.empty() || !confirm_unverified()) return;
    std::error_code ec;
    const auto dir = g.game / L"Scenarios";
    fs::create_directories(dir, ec);
    const auto base = suggested_name();
    std::wstring name = base;
    bool written = false;
    for (int n = 1; !ec && n <= 10000; ++n) {
        name = n == 1 ? base : base + L" (" + std::to_wstring(n) + L")";
        if (fs::exists(dir / (name + L".BFHP"), ec)) continue;
        if (ec) break;
        written = mapx::app::write_file_atomic(dir / (name + L".BFHP"), current_result()->volume, false);
        if (written || (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)) break;
    }
    if (written)
        g.notice = format1(tr(S::InstalledAs), name) + L"\n" + tr(S::InstallHint), g.notice_tone = Tone::Good;
    else
        g.notice = tr(S::WriteFailed), g.notice_tone = Tone::Bad;
    update_controls();
}


std::wstring seed_text(std::uint32_t seed) {
    wchar_t value[16];
    std::swprintf(value, 16, L"%08X", seed);
    return value;
}

void refresh_gallery_settings(bool reset) {
    if (!g.template_combo) return;
    int template_index = static_cast<int>(SendMessageW(g.template_combo, CB_GETCURSEL, 0, 0));
    if (reset || template_index < 0 || template_index >= static_cast<int>(g.templates.size())) {
        SendMessageW(g.template_combo, CB_RESETCONTENT, 0, 0);
        template_index = 0;
        for (std::size_t i = 0; i < g.templates.size(); ++i) {
            SendMessageW(g.template_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen_latin1(g.templates[i].name).c_str()));
            if (mapx::template_name(g.templates[i].path) == g.initial_template) template_index = static_cast<int>(i);
        }
        SendMessageW(g.template_combo, CB_SETCURSEL, template_index, 0);
    }
    int wanted_size = reset ? g.initial_size : static_cast<int>(SendMessageW(g.size_combo, CB_GETITEMDATA,
        SendMessageW(g.size_combo, CB_GETCURSEL, 0, 0), 0));
    int wanted_players = reset ? g.initial_players : static_cast<int>(SendMessageW(g.players_combo, CB_GETCURSEL, 0, 0)) + 2;
    const int wanted_race = reset ? 0 : std::max(0, static_cast<int>(SendMessageW(g.race_combo, CB_GETCURSEL, 0, 0)));
    SendMessageW(g.race_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.race_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(S::RaceRandom)));
    for (int i = 0; i < 8; ++i)
        SendMessageW(g.race_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(static_cast<S>(static_cast<int>(S::Race0) + i))));
    SendMessageW(g.race_combo, CB_SETCURSEL, wanted_race, 0);
    SendMessageW(g.size_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.players_combo, CB_RESETCONTENT, 0, 0);
    if (g.templates.empty()) return;
    const auto& item = g.templates[static_cast<std::size_t>(template_index)];
    int selected = 0;
    for (int size = 0; size < 3; ++size) {
        if (item.max_players[static_cast<std::size_t>(size)] < 2) continue;
        const auto index = SendMessageW(g.size_combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(tr(static_cast<S>(static_cast<int>(S::SizeSmall) + size))));
        SendMessageW(g.size_combo, CB_SETITEMDATA, index, size);
        if (size == wanted_size) selected = static_cast<int>(index);
    }
    SendMessageW(g.size_combo, CB_SETCURSEL, selected, 0);
    wanted_size = static_cast<int>(SendMessageW(g.size_combo, CB_GETITEMDATA, selected, 0));
    const int max_players = item.max_players[static_cast<std::size_t>(wanted_size)];
    for (int n = 2; n <= max_players; ++n)
        SendMessageW(g.players_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(n).c_str()));
    SendMessageW(g.players_combo, CB_SETCURSEL, std::clamp(wanted_players, 2, max_players) - 2, 0);
}

void switch_tab(int tab) {
    g.enlarged = false;
    g.tab = tab;
    TabCtrl_SetCurSel(g.tabs, tab);
    g.notice.reset();
    layout();
    update_controls();
}

void select_gallery(int index) {
    if (index < 0 || index >= 9 || !g.gallery[static_cast<std::size_t>(index)] ||
        g.gallery[static_cast<std::size_t>(index)]->volume.empty()) return;
    g.selected = index;
    g.notice.reset();
    for (HWND card : g.cards) InvalidateRect(card, nullptr, FALSE);
    update_controls();
}

void open_detail(int index) {
    if (g.tab != 1 || index < 0 || index >= 9 || !g.gallery[index] ||
        g.gallery[index]->minimap.width <= 0 || g.gallery[index]->volume.empty()) return;
    select_gallery(index);
    g.enlarged = true;
    layout();
    SetFocus(g.back);
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void close_detail() {
    if (!g.enlarged) return;
    g.enlarged = false;
    layout();
    if (g.selected >= 0) SetFocus(g.cards[g.selected]);
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void start_gallery(bool next_batch) {
    if (g.busy || g.loading_game) return;
    if (!g.installation || g.templates.empty()) {
        g.notice = tr(g.installation ? S::GalleryNoTemplates : S::NeedGameFolder);
        g.notice_tone = Tone::Warn;
        update_controls();
        return;
    }
    wchar_t buffer[64]{};
    GetWindowTextW(g.seed_edit, buffer, 64);
    std::string seed;
    for (const wchar_t c : std::wstring(buffer)) { if (c > 127) { seed = "?"; break; } seed += static_cast<char>(c); }
    const auto parsed = mapx::gallery_seed(seed);
    if (!parsed) {
        g.notice = tr(S::GalleryInvalidSeed);
        g.notice_tone = Tone::Warn;
        update_controls();
        SetFocus(g.seed_edit);
        return;
    }
    const auto template_index = SendMessageW(g.template_combo, CB_GETCURSEL, 0, 0);
    if (template_index < 0 || template_index >= static_cast<LRESULT>(g.templates.size())) return;
    mapx::GalleryRequest request;
    request.map_template = g.templates[static_cast<std::size_t>(template_index)];
    request.size_index = static_cast<int>(SendMessageW(g.size_combo, CB_GETITEMDATA,
        SendMessageW(g.size_combo, CB_GETCURSEL, 0, 0), 0));
    request.players = static_cast<int>(SendMessageW(g.players_combo, CB_GETCURSEL, 0, 0)) + 2;
    const int race = static_cast<int>(SendMessageW(g.race_combo, CB_GETCURSEL, 0, 0));
    request.race = race <= 0 ? 8 : race - 1;
    request.base_seed = *parsed + (next_batch ? 9u : 0u);
    SetWindowTextW(g.seed_edit, seed_text(request.base_seed).c_str());
    switch_tab(1);
    join_worker();
    g.busy = g.generating_gallery = true;
    g.cancel_requested = false;
    g.gallery_request = request;
    g.gallery_finished = 0;
    g.gallery_cancelled = false;
    g.selected = -1;
    for (auto& item : g.gallery) item.reset();
    for (std::size_t i = 0; i < g.cards.size(); ++i) {
        const auto label = std::wstring(tr(S::GalleryMap)) + L" " + std::to_wstring(i + 1) + L" · " +
            seed_text(mapx::gallery_card_seed(request.base_seed, i));
        SetWindowTextW(g.cards[i], label.c_str());
    }
    update_controls();
    const auto language = g.text_language;
    auto* install = g.installation.get();
    const HWND hwnd = g.hwnd;
    const int thumbnail_size = 1024;  // Keep previews sharp when the window grows; resizing never regenerates a map.
    g.worker = std::thread([request, language, install, hwnd, thumbnail_size] {
        bool stopped = false;
        for (std::size_t i = 0; i < 9; ++i) {
            if (g.cancel_requested) { stopped = true; break; }
            auto message = std::make_unique<GalleryItem>();
            message->index = i;
            auto result = std::make_unique<Result>();
            result->seed = mapx::gallery_card_seed(request.base_seed, i);
            result->path = fs::path(L"Imperivm_" + seed_text(result->seed) + L".BFHP");
            result->template_name = widen_latin1(request.map_template.name);
            result->map_size = 8192u << request.size_index;
            try {
                auto map = mapx::generate_gallery_map(*install, request, i, language, thumbnail_size,
                    [] { return g.cancel_requested.load(); });
                if (!map) { stopped = true; break; }
                result->minimap = std::move(map->minimap);
                result->volume = std::move(map->volume);
                result->counts = map->counts;
                result->message = S::GalleryGenerated;
                result->tone = Tone::Good;
                result->confirmed = true;
                for (int k = 0; k < request.players; ++k)
                    result->players.push_back({k, L"", map->pre.races[static_cast<std::size_t>(k)], k, k == 0});
            } catch (const mapx::InstallError& e) { result->message = install_message(e.problem); }
              catch (const std::exception&) { result->message = S::GenerationFailed; }
            message->result = std::move(result);
            if (PostMessageW(hwnd, kMsgGalleryItem, 0, reinterpret_cast<LPARAM>(message.get()))) message.release();
        }
        PostMessageW(hwnd, kMsgGalleryDone, stopped ? 1 : 0, 0);
    });
}

LRESULT CALLBACK card_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR index) {
    if (message == WM_LBUTTONDBLCLK) { open_detail(static_cast<int>(index)); return 0; }
    if (message == WM_KEYDOWN && wparam == VK_RETURN) { open_detail(static_cast<int>(index)); return 0; }
    if (message == WM_GETDLGCODE && lparam && reinterpret_cast<MSG*>(lparam)->wParam == VK_RETURN)
        return DLGC_WANTMESSAGE;
    if (message == WM_GETDLGCODE) return DefSubclassProc(hwnd, message, wparam, lparam) | DLGC_WANTARROWS;
    if (message == WM_KEYDOWN && (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN)) {
        const int delta = wparam == VK_LEFT ? -1 : wparam == VK_RIGHT ? 1 : wparam == VK_UP ? -3 : 3;
        const int target = std::clamp(static_cast<int>(index) + delta, 0, 8);
        select_gallery(target);
        if (IsWindowEnabled(g.cards[static_cast<std::size_t>(target)])) SetFocus(g.cards[static_cast<std::size_t>(target)]);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------------------------- painting

RECT rc(int l, int t, int r, int b) { return RECT{px(l), px(t), px(r), px(b)}; }
RECT drop_rect() { return rc(kM, kM + kTabOffset, client_width() - kM, kM + 72 + kTabOffset); }
RECT map_rect() {
    const int size = std::max(kMap, std::min(client_width() - 480, client_height() - 364));
    return rc(client_width() - kM - size, 152, client_width() - kM, 152 + size);
}
RECT info_rect() { const auto map = map_rect(); return RECT{px(kM), px(152), map.left - px(kM), px(client_height() - 212)}; }
RECT legend_rect() { return rc(kM, client_height() - 204, client_width() - kM, client_height() - 124); }
RECT status_rect() { return rc(kM, client_height() - 100, client_width() - kM, client_height() - 52); }

COLORREF tone_fill(Tone t) {
    switch (t) {
    case Tone::Good: return RGB(226, 243, 222);
    case Tone::Warn: return RGB(252, 240, 210);
    case Tone::Bad: return RGB(250, 225, 222);
    case Tone::Busy: return RGB(225, 236, 250);
    case Tone::Neutral: break;
    }
    return RGB(236, 236, 236);
}

void text(HDC dc, const std::wstring& s, RECT r, UINT flags, HFONT font, COLORREF colour = GetSysColor(COLOR_WINDOWTEXT)) {
    SelectObject(dc, font);
    SetTextColor(dc, colour);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX);
}

std::wstring race_name(int race) {
    return race >= 0 && race < 8 ? tr(static_cast<S>(static_cast<int>(S::Race0) + race)) : tr(S::RaceRandom);
}

void paint_info(HDC dc) {
    const RECT area = info_rect();
    if (!g.result) return;
    const auto& r = *g.result;
    int y = area.top;
    const int label_w = px(96), line = px(22);
    auto row = [&](S label, const std::wstring& value) {
        text(dc, tr(label), RECT{area.left, y, area.left + label_w, y + line}, DT_SINGLELINE | DT_END_ELLIPSIS, g.bold);
        text(dc, value, RECT{area.left + label_w, y, area.right, y + line}, DT_SINGLELINE | DT_PATH_ELLIPSIS, g.font);
        y += line;
    };
    row(S::LabelFile, r.path.filename().wstring());
    S kind = S::KindOther;
    if (r.kind == mapx::Kind::RandomMapReplay) kind = S::KindReplay;
    else if (r.kind == mapx::Kind::RandomMapSave) kind = S::KindSave;
    else if (r.kind == mapx::Kind::PredefinedMap) kind = S::KindPredefined;
    else if (r.kind == mapx::Kind::Campaign) kind = S::KindCampaign;
    row(S::LabelType, tr(kind));
    if (r.map_size == 0) return;
    row(S::LabelTemplate, r.template_name);
    const S size = r.map_size == 0x2000 ? S::SizeSmall : r.map_size == 0x4000 ? S::SizeMedium : S::SizeLarge;
    row(S::LabelSize, tr(size));
    wchar_t seed[16];
    std::swprintf(seed, 16, L"%08X", r.seed);
    row(S::LabelSeed, seed);
    text(dc, tr(S::LabelPlayers), RECT{area.left, y, area.left + label_w, y + line}, DT_SINGLELINE, g.bold);
    for (const auto& p : r.players) {
        std::wstring s = std::to_wstring(p.slot + 1) + L". " + (p.name.empty() ? L"—" : p.name) + L" · " + race_name(p.race) + L" · " +
            tr(S::Team) + L" " + std::to_wstring(p.team + 1) + L" · " + tr(p.human ? S::Human : S::Computer);
        text(dc, s, RECT{area.left + label_w, y, area.right, y + line}, DT_SINGLELINE | DT_END_ELLIPSIS, g.font);
        y += line;
        if (y > area.bottom - line) break;
    }
}

void paint_status(HDC dc) {
    std::wstring message;
    Tone tone = Tone::Neutral;
    if (g.loading_game) message = tr(S::LoadingGame), tone = Tone::Busy;
    else if (g.busy && g.generating_gallery) {
        wchar_t buffer[160];
        std::swprintf(buffer, 160, tr(S::GalleryProgress), g.gallery_finished);
        message = buffer;
        tone = Tone::Busy;
    } else if (g.busy) {
        tone = Tone::Busy;
        if (g.progress_n > 1) {
            wchar_t buf[160];
            std::swprintf(buf, 160, tr(S::GeneratingCandidate), g.progress_i, g.progress_n);
            message = buf;
        } else {
            message = tr(S::Generating);
        }
    } else if (g.notice) message = *g.notice, tone = g.notice_tone;
    else if (g.tab == 1 && g.installation) message = tr(g.templates.empty() ? S::GalleryNoTemplates : (g.gallery_cancelled ? S::GalleryStopped : (g.gallery_finished ? S::GalleryReady : S::GalleryInitial)));
    else if (g.result) message = tr(g.result->message), tone = g.result->tone;
    else if (g.game_error) message = tr(*g.game_error), tone = Tone::Bad;
    else if (g.game.empty()) message = tr(S::NeedGameFolder), tone = Tone::Warn;
    if (message.empty()) return;
    RECT box = status_rect();
    HBRUSH brush = CreateSolidBrush(tone_fill(tone));
    FillRect(dc, &box, brush);
    DeleteObject(brush);
    InflateRect(&box, -px(10), -px(g.tab == 1 ? 4 : 8));
    text(dc, message, box, DT_WORDBREAK, g.font, RGB(30, 30, 30));
}

// The legend symbol of a feature, in the shape and colour the minimap uses, centred in a 12-unit box at (x, y).
void swatch(HDC dc, int x, int y, mapx::Feature f) {
    const auto s = mapx::feature_style(f);
    HBRUSH brush = CreateSolidBrush(RGB(s.colour.r, s.colour.g, s.colour.b));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
    auto old_brush = SelectObject(dc, brush);
    auto old_pen = SelectObject(dc, pen);
    const double c = px(6), r = px(f == mapx::Feature::PlayerStart || s.sides == 0 ? 5 : 6);
    if (s.sides == 0) {
        Ellipse(dc, x + static_cast<int>(c - r), y + static_cast<int>(c - r), x + static_cast<int>(c + r), y + static_cast<int>(c + r));
    } else {
        std::vector<POINT> pts;
        for (const auto& [px_, py_] : mapx::polygon_points(s, x + c, y + c, r))
            pts.push_back(POINT{static_cast<LONG>(std::lround(px_)), static_cast<LONG>(std::lround(py_))});
        Polygon(dc, pts.data(), static_cast<int>(pts.size()));
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// Three columns: features worth taking early, late-game forts, everything else; counts once a map is shown.
void paint_legend(HDC dc) {
    using F = mapx::Feature;
    struct Item {
        F feature;
        S label;
    };
    const Item columns[3][4] = {
        {{F::PlayerStart, S::LegendStart}, {F::GoldFort, S::LegendGoldFort}, {F::CarthaginianFort, S::LegendCarthaginianFort},
         {F::TeutonCamp, S::LegendCamp}},
        {{F::IberianFort, S::LegendIberianFort}, {F::BritonFort, S::LegendBritonFort}, {F::GermanFort, S::LegendGermanFort},
         {F::GallicFort, S::LegendGallicFort}},
        {{F::Village, S::LegendVillage}, {F::Teleport, S::LegendTeleport}, {F::Site, S::LegendSite}, {F::OtherFort, S::LegendOtherFort}},
    };
    const RECT area = legend_rect();
    const bool counted = g.result && g.result->minimap.width > 0;
    const int col_w = (area.right - area.left) / 3, line = px(20);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 4; ++r) {
            const auto& it = columns[c][r];
            const int n = counted ? g.result->counts[static_cast<std::size_t>(it.feature)] : 0;
            if (it.feature == F::OtherFort && n == 0) continue;
            const int x = area.left + c * col_w, y = area.top + r * line;
            swatch(dc, x, y + px(4), it.feature);
            std::wstring label = tr(it.label);
            if (counted) label += L" · " + std::to_wstring(n);
            text(dc, label, RECT{x + px(18), y, x + col_w - px(4), y + line}, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, g.font);
        }
}

void paint_map(HDC dc) {
    const RECT r = map_rect();
    if (g.result && g.result->minimap.width > 0) {
        const auto& img = g.result->minimap;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = img.width;
        bi.bmiHeader.biHeight = -img.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, img.width, img.height, img.pixels.data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        HBRUSH brush = CreateSolidBrush(RGB(232, 232, 232));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
}


void draw_gallery_card(HDC dc, std::size_t index, RECT card, bool focused) {
    const auto* result = g.gallery[index].get();
    const bool selected = g.selected == static_cast<int>(index);
    HBRUSH fill = CreateSolidBrush(selected ? RGB(227, 239, 252) : RGB(246, 248, 250));
    FillRect(dc, &card, fill);
    DeleteObject(fill);
    HPEN pen = CreatePen(PS_SOLID, px(selected ? 2 : 1), selected ? RGB(38, 104, 173) : RGB(214, 220, 226));
    auto old_pen = SelectObject(dc, pen);
    auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, card.left, card.top, card.right, card.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    const int side = gallery_image_size(card.right - card.left, card.bottom - card.top);
    const int top = card.top + px(6) + (card.bottom - card.top - px(36) - side) / 2;
    RECT image{card.left + (card.right - card.left - side) / 2, top, 0, top + side};
    image.right = image.left + side;
    if (result && result->minimap.width > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = result->minimap.width;
        bi.bmiHeader.biHeight = -result->minimap.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, image.left, image.top, image.right - image.left, image.bottom - image.top,
            0, 0, result->minimap.width, result->minimap.height, result->minimap.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        const auto label = tr(result ? S::GalleryFailed : (g.busy && index == g.gallery_finished ? S::GalleryWorking : S::GalleryWaiting));
        text(dc, label, image, DT_CENTER | DT_VCENTER | DT_SINGLELINE, g.font, RGB(110, 117, 125));
    }
    const std::wstring title = g.gallery_request.map_template.path.empty()
        ? std::wstring(tr(S::GalleryMap)) + L" " + std::to_wstring(index + 1)
        : std::to_wstring(index + 1) + L" · " + seed_text(mapx::gallery_card_seed(g.gallery_request.base_seed, index)) +
            (selected ? L"  ✓" : L"");
    text(dc, title, RECT{card.left + px(4), card.bottom - px(28), card.right - px(4), card.bottom - px(4)},
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, selected ? g.bold : g.font, RGB(35, 48, 62));
    if (focused) { InflateRect(&card, -px(4), -px(4)); DrawFocusRect(dc, &card); }
}

void paint_gallery(HDC dc) {
    const int x = sidebar_x();
    text(dc, tr(S::GalleryTitle), rc(16, 64, x - 16, 90), DT_SINGLELINE, g.big, RGB(30, 45, 60));
    text(dc, tr(S::GalleryHint), rc(16, 94, x - 16, 116), DT_SINGLELINE, g.font, RGB(95, 104, 114));
    const std::pair<S, int> labels[] = {{S::LabelTemplate, 124}, {S::LabelSize, 198}, {S::LabelPlayers, 272},
        {S::GalleryRace, 346}, {S::GallerySeed, 420}};
    for (const auto& [label, y] : labels)
        text(dc, tr(label), rc(x, y, x + 300, y + 20), DT_SINGLELINE, g.bold, RGB(45, 56, 68));
    const auto* selected = current_result();
    if (selected) {
        const auto title = std::wstring(tr(S::GallerySelected)) + L": " + tr(S::GalleryMap) + L" " + std::to_wstring(g.selected + 1);
        text(dc, title, rc(x, 580, x + 300, 604), DT_SINGLELINE | DT_END_ELLIPSIS, g.bold, RGB(38, 104, 173));
        text(dc, std::wstring(tr(S::LabelSeed)) + L": " + seed_text(selected->seed), rc(x, 606, x + 300, 628), DT_SINGLELINE, g.font);
        const auto selected_size = selected->map_size == 8192 ? S::SizeSmall : selected->map_size == 16384 ? S::SizeMedium : S::SizeLarge;
        text(dc, selected->template_name + L" · " + tr(selected_size), rc(x, 630, x + 300, 652), DT_SINGLELINE | DT_END_ELLIPSIS, g.font);
        text(dc, std::to_wstring(selected->players.size()) + L" " + tr(S::LabelPlayers) + L" · " + tr(S::GalleryAllAgainstAll),
            rc(x, 654, x + 300, 674), DT_SINGLELINE | DT_END_ELLIPSIS, g.font);
    } else {
        text(dc, tr(S::GalleryChoose), rc(x, 586, x + 300, 644), DT_WORDBREAK, g.font, RGB(110, 117, 125));
    }
}

RECT detail_map_rect() {
    RECT client{}; GetClientRect(g.hwnd, &client);
    const int side = std::max(1L, std::min(client.right - px(32), client.bottom - px(72)));
    const int left = (client.right - side) / 2, top = px(56) + (client.bottom - px(72) - side) / 2;
    return RECT{left, top, left + side, top + side};
}

void paint_detail(HDC dc) {
    RECT client{}; GetClientRect(g.hwnd, &client);
    HBRUSH background = CreateSolidBrush(RGB(24, 29, 35));
    FillRect(dc, &client, background);
    DeleteObject(background);
    const auto* result = current_result();
    if (!result || result->minimap.width <= 0) return;
    const auto title = std::wstring(tr(S::GalleryMap)) + L" " + std::to_wstring(g.selected + 1) +
        L" · " + tr(S::LabelSeed) + L": " + seed_text(result->seed);
    text(dc, title, rc(208, 12, client_width() - 16, 44), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        g.bold, RGB(240, 244, 248));
    const auto& image = result->minimap;
    const RECT area = detail_map_rect();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = image.width;
    bi.bmiHeader.biHeight = -image.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, area.left, area.top, area.right - area.left, area.bottom - area.top,
        0, 0, image.width, image.height, image.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void paint(HDC dc) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (g.enlarged) { paint_detail(dc); return; }
    if (g.tab == 1) { paint_gallery(dc); paint_status(dc); } else {
    // drop zone
    const RECT drop = drop_rect();
    HPEN pen = CreatePen(PS_DASH, 1, GetSysColor(COLOR_GRAYTEXT));
    auto old_pen = SelectObject(dc, pen);
    auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, drop.left, drop.top, drop.right, drop.bottom, px(10), px(10));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    RECT t1 = drop, t2 = drop;
    t1.bottom = (drop.top + drop.bottom) / 2 + px(2);
    t2.top = t1.bottom;
    text(dc, tr(S::DropHint), t1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE, g.big);
    text(dc, tr(S::DropHint2), t2, DT_CENTER | DT_TOP | DT_SINGLELINE, g.font, GetSysColor(COLOR_GRAYTEXT));
    paint_info(dc);
    paint_status(dc);
    paint_map(dc);
    paint_legend(dc);
    }
    // game folder
    const int bar = px(client_height() - kM - kBar);
    RECT label{px(kM + 100), bar, px(client_width() - kM - 170 - 8 - 130 - 8 - 96 - 8), bar + px(kBar)};
    const std::wstring folder = std::wstring(tr(S::GameLabel)) + L" " + (g.game.empty() ? tr(S::NoGameFolder) : g.game.wstring());
    text(dc, folder, label, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS, g.font);
}

// ---------------------------------------------------------------------------------------------- window

void make_fonts() {
    for (HFONT* f : {&g.font, &g.bold, &g.big})
        if (*f) DeleteObject(*f), *f = nullptr;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, static_cast<int>(g.dpi), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 5 / 4;
    g.big = CreateFontIndirectW(&lf);
}

void layout() {
    const int width = client_width(), height = client_height(), x = sidebar_x();
    const int bar = height - kM - kBar;
    auto place = [](HWND h, int x, int y, int w, int hgt) {
        SetWindowPos(h, nullptr, px(x), px(y), px(w), px(hgt), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    };
    place(g.back, 16, 12, 176, 32);
    ShowWindow(g.back, g.enlarged ? SW_SHOW : SW_HIDE);
    ShowWindow(g.tabs, g.enlarged ? SW_HIDE : SW_SHOW);
    for (HWND control : {g.lang_button, g.change, g.save, g.install})
        ShowWindow(control, g.enlarged ? SW_HIDE : SW_SHOW);
    place(g.tabs, 16, 12, width - 32, 36);
    place(g.template_combo, x, 146, 300, 220);
    place(g.size_combo, x, 220, 300, 140);
    place(g.players_combo, x, 294, 300, 210);
    place(g.race_combo, x, 368, 300, 260);
    place(g.seed_edit, x, 442, 178, 28);
    place(g.random_seed, x + 186, 442, 114, 28);
    place(g.generate, x, 490, 300, 36);
    place(g.next, x, 536, 194, 32);
    place(g.cancel_button, x + 202, 536, 98, 32);
    for (std::size_t i = 0; i < g.cards.size(); ++i)
        place(g.cards[i], 16 + static_cast<int>(i % 3) * gallery_cell_width(),
            124 + static_cast<int>(i / 3) * gallery_cell_height(), gallery_cell_width() - 8, gallery_cell_height() - 4);
    for (HWND control : {g.template_combo, g.size_combo, g.players_combo, g.race_combo, g.seed_edit,
                         g.random_seed, g.generate, g.next, g.cancel_button}) ShowWindow(control, g.tab == 1 && !g.enlarged ? SW_SHOW : SW_HIDE);
    for (HWND card : g.cards) ShowWindow(card, g.tab == 1 && !g.enlarged ? SW_SHOW : SW_HIDE);
    place(g.lang_button, kM, bar, 90, kBar);
    place(g.change, width - kM - 170 - 8 - 130 - 8 - 96, bar, 96, kBar);
    place(g.save, width - kM - 170 - 8 - 130, bar, 130, kBar);
    place(g.install, width - kM - 170, bar, 170, kBar);
    place(g.open, width - kM - 140, kM + 88 + kTabOffset + 250 - 4, 0, 0);  // the drop zone opens files; the button stays hidden
    ShowWindow(g.open, SW_HIDE);
}

UINT fitted_dpi(UINT requested) {
    if (g.test_dpi) requested = g.test_dpi;
    MONITORINFO monitor{sizeof monitor};
    if (!GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return requested;
    const UINT width = static_cast<UINT>(std::max<LONG>(1, monitor.rcWork.right - monitor.rcWork.left - 32));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, monitor.rcWork.bottom - monitor.rcWork.top - 64));
    return std::max<UINT>(48, std::min({requested, width * 96 / kW, height * 96 / kH}));
}

void resize_for_dpi() {
    RECT r{0, 0, px(kW), px(kH)};
    AdjustWindowRectExForDpi(&r, GetWindowLongW(g.hwnd, GWL_STYLE), FALSE, 0, g.dpi);
    RECT current{};
    GetWindowRect(g.hwnd, &current);
    MONITORINFO monitor{sizeof monitor};
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        current.left = std::clamp(current.left, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        current.top = std::clamp(current.top, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    }
    SetWindowPos(g.hwnd, nullptr, current.left, current.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void save_screenshot() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int w = client.right, h = client.bottom;
    HDC wdc = GetDC(g.hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    auto old = SelectObject(mem, bmp);
    // the current state, not the last composed frame: the window paints itself, then each button at its place
    paint(mem);
    std::vector<HWND> children{g.back, g.tabs, g.lang_button, g.change, g.save, g.install, g.template_combo, g.size_combo,
        g.players_combo, g.race_combo, g.seed_edit, g.random_seed, g.generate, g.next, g.cancel_button};
    children.insert(children.end(), g.cards.begin(), g.cards.end());
    for (HWND child : children) {
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT cr;
        GetWindowRect(child, &cr);
        MapWindowPoints(nullptr, g.hwnd, reinterpret_cast<POINT*>(&cr), 2);
        POINT origin;
        SetViewportOrgEx(mem, cr.left, cr.top, &origin);
        const auto card = std::find(g.cards.begin(), g.cards.end(), child);
        if (card != g.cards.end()) draw_gallery_card(mem, static_cast<std::size_t>(card - g.cards.begin()),
            RECT{0, 0, cr.right - cr.left, cr.bottom - cr.top}, false);
        else {
            SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
            // Hidden native drop-down lists omit their selection text from WM_PRINT. Render the actual
            // selected item into the captured control, retaining the native frame and arrow.
            if (child == g.template_combo || child == g.size_combo || child == g.players_combo || child == g.race_combo) {
                const auto selected = SendMessageW(child, CB_GETCURSEL, 0, 0);
                const auto length = selected >= 0 ? SendMessageW(child, CB_GETLBTEXTLEN, selected, 0) : CB_ERR;
                if (length >= 0) {
                    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
                    SendMessageW(child, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(value.data()));
                    value.resize(static_cast<std::size_t>(length));
                    COMBOBOXINFO info{sizeof info};
                    GetComboBoxInfo(child, &info);
                    RECT item = info.rcItem;
                    FillRect(mem, &item, GetSysColorBrush(COLOR_WINDOW));
                    SetBkMode(mem, TRANSPARENT);
                    item.left += px(3);
                    text(mem, value, item, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, g.font);
                }
            }
        }
        SetViewportOrgEx(mem, origin.x, origin.y, nullptr);
    }
    SelectObject(mem, old);
    mapx::Image img{w, h, std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h)};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, static_cast<UINT>(h), img.pixels.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g.hwnd, wdc);
    for (auto& p : img.pixels) p &= 0x00FFFFFF;
    write_file(g.screenshot, mapx::to_bmp(img));
}

void finish_screenshot_if_requested() {
    if (g.screenshot.empty() || g.busy || g.loading_game) return;
    bool native_tabs = true, keyboard = true, selection = true;
    if (g.tab == 1 && g.test_selected >= 0) {
        SendMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IdCard + g.test_selected, BN_CLICKED),
            reinterpret_cast<LPARAM>(g.cards[static_cast<std::size_t>(g.test_selected)]));
        if (!g.ui_report.empty()) {
            const auto original = g.result.get();
            NMHDR header{g.tabs, IdTabs, TCN_SELCHANGE};
            TabCtrl_SetCurSel(g.tabs, 0);
            SendMessageW(g.hwnd, WM_NOTIFY, IdTabs, reinterpret_cast<LPARAM>(&header));
            native_tabs = g.tab == 0 && current_result() == original;
            TabCtrl_SetCurSel(g.tabs, 1);
            SendMessageW(g.hwnd, WM_NOTIFY, IdTabs, reinterpret_cast<LPARAM>(&header));
            native_tabs = native_tabs && g.tab == 1;
            if (g.gallery[1] && !g.gallery[1]->volume.empty()) {
                SendMessageW(g.cards[0], WM_KEYDOWN, VK_RIGHT, 0);
                keyboard = g.selected == 1;
            }
            SendMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IdCard + g.test_selected, BN_CLICKED),
                reinterpret_cast<LPARAM>(g.cards[static_cast<std::size_t>(g.test_selected)]));
            selection = current_result() == g.gallery[static_cast<std::size_t>(g.test_selected)].get();
        }
    }
    bool detail_round_trip = true;
    if (g.tab == 1 && current_result()) {
        const auto selected_index = g.selected;
        const auto original = current_result();
        const auto volume_digest = iberivm::sha256_hex(original->volume);
        const auto raster = original->minimap.pixels.data();
        std::array<Result*, 9> retained{};
        for (std::size_t i = 0; i < retained.size(); ++i) retained[i] = g.gallery[i].get();
        auto double_click = [&] { SendMessageW(g.cards[selected_index], WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(20, 20)); };
        double_click();
        detail_round_trip = g.enlarged && current_result() == original &&
            !(GetWindowLongW(g.cards[selected_index], GWL_STYLE) & WS_VISIBLE) &&
            (GetWindowLongW(g.back, GWL_STYLE) & WS_VISIBLE);
        SendMessageW(g.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        detail_round_trip = detail_round_trip && !g.enlarged && g.selected == selected_index &&
            (GetWindowLongW(g.cards[selected_index], GWL_STYLE) & WS_VISIBLE);
        double_click();
        SendMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IdBack, BN_CLICKED), reinterpret_cast<LPARAM>(g.back));
        detail_round_trip = detail_round_trip && !g.enlarged && current_result() == original;
        for (std::size_t i = 0; i < retained.size(); ++i)
            detail_round_trip = detail_round_trip && retained[i] == g.gallery[i].get();
        detail_round_trip = detail_round_trip && original->minimap.pixels.data() == raster &&
            volume_digest == iberivm::sha256_hex(original->volume);
        if (g.test_enlarged) double_click();
    }
    if (!g.export_path.empty() && current_result() && !current_result()->volume.empty()) write_file(g.export_path, current_result()->volume);
    if (!g.ui_report.empty()) {
        int ready = 0;
        for (const auto& item : g.gallery) if (item && !item->volume.empty()) ++ready;
        const auto* selected = current_result();
        const bool recovery_retained = g.result && !g.result->volume.empty();
        const auto digest = selected ? iberivm::sha256_hex(selected->volume) : std::string();
        RECT bounds{};
        MONITORINFO monitor{sizeof monitor};
        GetClientRect(g.hwnd, &bounds);
        MapWindowPoints(g.hwnd, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
        const bool fits = GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
            bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top &&
            bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom;
        std::ofstream report(g.ui_report);
        report << "{\"ready\":" << ready << ",\"selected\":" << g.selected << ",\"seed\":"
               << (selected ? selected->seed : 0) << ",\"recovery_retained\":" << (recovery_retained ? "true" : "false")
               << ",\"native_tabs\":" << (native_tabs ? "true" : "false")
               << ",\"keyboard_selection\":" << (keyboard ? "true" : "false")
               << ",\"selection_matches\":" << (selection ? "true" : "false")
               << ",\"can_save\":" << (IsWindowEnabled(g.save) ? "true" : "false")
               << ",\"can_generate\":" << (IsWindowEnabled(g.generate) ? "true" : "false")
               << ",\"has_error\":" << ((g.game_error || (g.result && g.result->tone == Tone::Bad)) ? "true" : "false")
               << ",\"detail_round_trip\":" << (detail_round_trip ? "true" : "false")
               << ",\"enlarged\":" << (g.enlarged ? "true" : "false")
               << ",\"detail_pixels\":" << (detail_map_rect().right - detail_map_rect().left)
               << ",\"cancelled\":" << (g.gallery_cancelled ? "true" : "false")
               << ",\"fits_work_area\":" << (fits ? "true" : "false")
               << ",\"client_width\":" << client_width() << ",\"client_height\":" << client_height()
               << ",\"maximized\":" << (IsZoomed(g.hwnd) ? "true" : "false")
               << ",\"image_pixels\":" << gallery_image_size(px(gallery_cell_width() - 8), px(gallery_cell_height() - 4))
               << ",\"dpi\":" << g.dpi << ",\"text_language\":\"" << g.text_language
               << "\",\"base_seed\":" << g.gallery_request.base_seed
               << ",\"export_sha256\":\"" << digest << "\"}";
    }
    UpdateWindow(g.hwnd);
    save_screenshot();
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        g.dpi = fitted_dpi(GetDpiForWindow(hwnd));
        make_fonts();
        auto button = [&](int id) {
            return CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
        };
        g.back = button(IdBack);
        g.open = button(IdOpen);
        g.save = button(IdSave);
        g.install = button(IdInstall);
        g.change = button(IdChange);
        g.lang_button = button(IdLang);
        g.tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(IdTabs), g.instance, nullptr);
        TCITEMW tab{}; tab.mask = TCIF_TEXT; tab.pszText = const_cast<wchar_t*>(tr(S::TabRecover));
        TabCtrl_InsertItem(g.tabs, 0, &tab);
        tab.pszText = const_cast<wchar_t*>(tr(S::TabGenerate)); TabCtrl_InsertItem(g.tabs, 1, &tab);
        TabCtrl_SetCurSel(g.tabs, g.tab);
        auto combo = [&](int id) {
            return CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 10, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
        };
        g.template_combo = combo(IdTemplate); g.size_combo = combo(IdSize);
        g.players_combo = combo(IdPlayers); g.race_combo = combo(IdRace);
        g.seed_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g.initial_seed.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 10, 28, hwnd, reinterpret_cast<HMENU>(IdSeed), g.instance, nullptr);
        SendMessageW(g.seed_edit, EM_SETLIMITTEXT, 10, 0);
        g.random_seed = button(IdRandomSeed); g.generate = button(IdGenerate); g.next = button(IdNext); g.cancel_button = button(IdCancel);
        for (std::size_t i = 0; i < g.cards.size(); ++i) {
            g.cards[i] = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCard + i)), g.instance, nullptr);
            SetWindowSubclass(g.cards[i], card_proc, 1, i);
        }
        refresh_gallery_settings(true);
        layout();
        if (!g.test_maximized) resize_for_dpi();
        DragAcceptFiles(hwnd, TRUE);
        update_controls();
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = fitted_dpi(HIWORD(wparam));
        make_fonts();
        const auto* r = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        if (g.tabs && wparam != SIZE_MINIMIZED) {
            layout();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (g.hwnd) {
            RECT minimum{0, 0, px(kW), px(kH)};
            AdjustWindowRectExForDpi(&minimum, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, g.dpi);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = POINT{minimum.right - minimum.left, minimum.bottom - minimum.top};
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        auto old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        const RECT drop = drop_rect();
        if (g.tab == 0 && PtInRect(&drop, p) && !g.busy && !g.loading_game) choose_file();
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        const RECT drop = drop_rect();
        if (g.tab == 0 && LOWORD(lparam) == HTCLIENT && PtInRect(&drop, p)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        std::wstring buf(32768, L'\0');
        if (DragQueryFileW(drop, 0, buf.data(), static_cast<UINT>(buf.size()))) start_file(fs::path(buf.c_str()));
        DragFinish(drop);
        return 0;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == IdTabs && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE)
            switch_tab(TabCtrl_GetCurSel(g.tabs));
        return 0;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID >= IdCard && item->CtlID < IdCard + 9) {
            SetBkMode(item->hDC, TRANSPARENT);
            draw_gallery_card(item->hDC, item->CtlID - IdCard, item->rcItem, (item->itemState & ODS_FOCUS) != 0);
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && g.enlarged) { close_detail(); return 0; }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) >= IdCard && LOWORD(wparam) < IdCard + 9) { select_gallery(LOWORD(wparam) - IdCard); return 0; }
        switch (LOWORD(wparam)) {
        case IdTemplate:
        case IdSize:
            if (HIWORD(wparam) == CBN_SELCHANGE) { refresh_gallery_settings(); update_controls(); }
            break;
        case IdBack: close_detail(); break;
        case IdGenerate: start_gallery(); break;
        case IdNext: start_gallery(true); break;
        case IdCancel: g.cancel_requested = true; update_controls(); break;
        case IdRandomSeed: {
            std::random_device random;
            SetWindowTextW(g.seed_edit, seed_text(static_cast<std::uint32_t>(random())).c_str());
            break;
        }
        case IdOpen: choose_file(); break;
        case IdSave: save_as(); break;
        case IdInstall: install_in_game(); break;
        case IdChange: choose_game_folder(); break;
        case IdLang:
            g.lang = g.lang == Lang::Es ? Lang::En : Lang::Es;
            mapx::app::ini_write(L"Language", g.lang == Lang::Es ? L"es" : L"en");
            refresh_gallery_settings();
            update_controls();
            break;
        }
        return 0;
    case kMsgInstall: {
        std::unique_ptr<InstallResult> r(reinterpret_cast<InstallResult*>(lparam));
        join_worker();
        g.loading_game = false;
        g.installation = std::move(r->install);
        g.game_error = r->error;
        if (g.installation) {
            g.text_language = r->text_language;
            g.templates = std::move(r->templates);
            refresh_gallery_settings(true);
            if (g.screenshot.empty()) mapx::app::ini_write(L"GameFolder", g.game.wstring());
        }
        update_controls();
        if (!g.pending.empty()) {
            const auto p = g.pending;
            g.pending.clear();
            start_file(p);
        }
        if (!g.busy && g.automatic_gallery && g.installation) {
            g.automatic_gallery = false;
            start_gallery();
        }
        finish_screenshot_if_requested();
        return 0;
    }
    case kMsgProgress:
        g.progress_i = wparam, g.progress_n = static_cast<std::size_t>(lparam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kMsgResult: {
        g.result.reset(reinterpret_cast<Result*>(lparam));
        join_worker();
        g.busy = false;
        update_controls();
        if (g.automatic_gallery && g.installation) { g.automatic_gallery = false; start_gallery(); }
        finish_screenshot_if_requested();
        return 0;
    }
    case kMsgGalleryItem: {
        std::unique_ptr<GalleryItem> item(reinterpret_cast<GalleryItem*>(lparam));
        g.gallery[item->index] = std::move(item->result);
        ++g.gallery_finished;
        if (g.test_cancel_after > 0 && g.gallery_finished >= static_cast<std::size_t>(g.test_cancel_after))
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IdCancel, BN_CLICKED), reinterpret_cast<LPARAM>(g.cancel_button));
        if (g.selected < 0 && !g.gallery[item->index]->volume.empty()) g.selected = static_cast<int>(item->index);
        InvalidateRect(g.cards[item->index], nullptr, FALSE);
        update_controls();
        return 0;
    }
    case kMsgGalleryDone: {
        join_worker();
        g.busy = g.generating_gallery = false;
        g.gallery_cancelled = wparam != 0;
        const bool any_ready = std::any_of(g.gallery.begin(), g.gallery.end(), [](const auto& item) { return item && !item->volume.empty(); });
        g.notice = tr(wparam ? S::GalleryStopped : (any_ready ? S::GalleryReady : S::GenerationFailed));
        g.notice_tone = wparam ? Tone::Neutral : (any_ready ? Tone::Good : Tone::Bad);
        update_controls();
        if (g.test_next_batch) {
            g.test_next_batch = false;
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IdNext, BN_CLICKED), reinterpret_cast<LPARAM>(g.next));
            return 0;
        }
        finish_screenshot_if_requested();
        return 0;
    }
    case WM_TIMER:
        if (wparam == kTimerScreenshot) {
            KillTimer(hwnd, kTimerScreenshot);
            if (!g.screenshot.empty()) {
                save_screenshot();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    case WM_CLOSE:
        g.cancel_requested = true;
        join_worker();
        {
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, kMsgInstall, kMsgGalleryDone, PM_REMOVE)) {
                if (pending.message == kMsgGalleryItem) delete reinterpret_cast<GalleryItem*>(pending.lParam);
                else if (pending.message == kMsgResult) delete reinterpret_cast<Result*>(pending.lParam);
                else if (pending.message == kMsgInstall) delete reinterpret_cast<InstallResult*>(pending.lParam);
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        join_worker();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g.instance = instance;
    g.initial_seed = seed_text(static_cast<std::uint32_t>(std::random_device{}()));
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // language: saved choice, else Spanish for a Spanish Windows, else English
    const auto saved_lang = mapx::app::ini_read(L"Language");
    if (saved_lang == L"es") g.lang = Lang::Es;
    else if (saved_lang == L"en") g.lang = Lang::En;
    else g.lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? Lang::Es : Lang::En;

    fs::path file, game_override;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--game" && i + 1 < argc) game_override = argv[++i];
            else if (a == L"--screenshot" && i + 1 < argc) g.screenshot = argv[++i];
            else if (a == L"--export" && i + 1 < argc) g.export_path = argv[++i];
            else if (a == L"--gallery") { g.tab = 1; g.automatic_gallery = true; }
            else if (a == L"--ui-enlarge") g.test_enlarged = true;
            else if (a == L"--ui-maximize") g.test_maximized = true;
            else if (a == L"--next-batch") g.test_next_batch = true;
            else if (a == L"--cancel-after" && i + 1 < argc) g.test_cancel_after = _wtoi(argv[++i]);
            else if (a == L"--ui-dpi" && i + 1 < argc) g.test_dpi = static_cast<UINT>(_wtoi(argv[++i]));
            else if (a == L"--seed" && i + 1 < argc) g.initial_seed = argv[++i];
            else if (a == L"--players" && i + 1 < argc) g.initial_players = _wtoi(argv[++i]);
            else if (a == L"--size" && i + 1 < argc) {
                const std::wstring value = argv[++i]; g.initial_size = value == L"small" ? 0 : value == L"large" ? 2 : 1;
            }
            else if (a == L"--select" && i + 1 < argc) g.test_selected = std::clamp(_wtoi(argv[++i]), 0, 8);
            else if (a == L"--ui-report" && i + 1 < argc) g.ui_report = argv[++i];
            else if (a == L"--lang" && i + 1 < argc) g.lang = std::wstring(argv[++i]) == L"en" ? Lang::En : Lang::Es;
            else file = a;
        }
        LocalFree(argv);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ImperivmMapExtractor";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPEDWINDOW | (g.test_maximized ? WS_MAXIMIZE : 0);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", style, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, g.screenshot.empty() ? show : SW_HIDE);
    UpdateWindow(hwnd);

    const fs::path game = !game_override.empty() ? game_override : mapx::app::detect_game_folder();
    if (!file.empty()) g.pending = file;
    if (!game.empty()) load_game(game);
    else if (!g.pending.empty()) {
        g.pending.clear();
        update_controls();
    }
    if (!g.screenshot.empty()) {
        SetTimer(hwnd, kTimerScreenshot, 60000, nullptr);
        if (game.empty()) finish_screenshot_if_requested();
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g.enlarged && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { close_detail(); continue; }
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    CoUninitialize();
    return static_cast<int>(m.wParam);
}
