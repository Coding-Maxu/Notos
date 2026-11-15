// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

#include "yuzu/notos/notos_home.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <set>
#include <thread>

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDirIterator>
#include <QFontMetricsF>
#include <QFontDatabase>
#include <QImage>
#include <QSettings>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QGuiApplication>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTime>
#include <QTimer>
#include <QWindow>

#include <SDL3/SDL.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "hid_core/frontend/emulated_controller.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/qt_common.h"
#include "yuzu/fritchii/fritchii_ui.h"

namespace Notos {
namespace {

// The launcher's colour choices: Spanish name, colour.
constexpr std::array<std::pair<const char*, const char*>, 7> Accents{{
    {"Azul", "#2F9BFF"},
    {"Rojo", "#FF4D4D"},
    {"Verde", "#3DDC84"},
    {"Amarillo", "#FFC83D"},
    {"Morado", "#A66BFF"},
    {"Naranja", "#FF8A3D"},
    {"Arcoíris", "rainbow"}, // every colour, turning slowly
}};

// The opening sequence, in milliseconds: the emblem alone, then it flies to its corner while the
// home screen comes in.
constexpr double IntroMerge = 1700, IntroMergeTime = 700, IntroUi = 2050, IntroTotal = 3300;
// Choosing a game, in milliseconds: its cover's answer, then the loading screen.
constexpr int LaunchTime = 520;

double Clamp01(double x) {
    return std::clamp(x, 0.0, 1.0);
}
double EaseOut(double x) {
    x = Clamp01(x);
    return 1 - (1 - x) * (1 - x) * (1 - x);
}
double EaseInOut(double x) {
    x = Clamp01(x);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

// Sizes in pixels at 1920x1080 (the Recomp Launcher's layout as the player tuned it).
constexpr double TopBar = 96, Tile = 320, TileGap = 6, TileBorder = 5, TilePad = 4, TitleSize = 27,
                 IconSize = 92, IconRow = 130, BottomBar = 64, HintSize = 22, ClockSize = 40,
                 SideMargin = 56, MenuWidth = 640, MenuFont = 18, BadgeSize = 48;

struct Button {
    const char* icon;
    const char* label;
};
constexpr std::array<Button, 3> Buttons{{
    {"plus", "Agregar"},
    {"settings", "Ajustes"},
    {"power", "Salir"},
}};
/// Next to the Notos name, above the covers.
constexpr std::array<Button, 2> TopButtons{{
    {"user", "Editor de Mii"},
    {"community", "Multijugador"},
}};

// Gamepad bits of NpadButtonState. Switch positions with the default mapping: B is the bottom
// face button (accept, ✕ on a PlayStation pad), A the right one (back, ○), X the top one (△).
constexpr u64 PadA = 1ULL << 0, PadB = 1ULL << 1, PadX = 1ULL << 2;
constexpr u64 PadLeft = (1ULL << 12) | (1ULL << 16), PadUp = (1ULL << 13) | (1ULL << 17),
              PadRight = (1ULL << 14) | (1ULL << 18), PadDown = (1ULL << 15) | (1ULL << 19);

QString ConfigPath() {
    return QString::fromStdString(Common::FS::PathToUTF8String(
               Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir))) +
           QStringLiteral("/notos.json");
}

QColor Hsv(double hue, double saturation, double value) {
    return QColor::fromHsvF(std::fmod(hue, 360.0) / 360.0, saturation, value);
}

/// A loose ExeFS dump (<folder>/[base|update|<id>]/exefs) whose folder also holds packaged
/// games: the game list names it after that folder, which is a library, not a game.
bool IsStrayDump(const QString& path) {
    // The game list keeps the exefs folder as the path (the main file elsewhere).
    const QFileInfo info(path);
    QDir dir = info.fileName() == QStringLiteral("main") ? info.dir() : QDir(path);
    if (dir.dirName().compare(QStringLiteral("exefs"), Qt::CaseInsensitive) != 0 || !dir.cdUp()) {
        return false;
    }
    static const QRegularExpression storage(QStringLiteral("^(base|update|updates|[0-9a-fA-F]+)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    while (storage.match(dir.dirName()).hasMatch() && dir.cdUp()) {
    }
    QStringList packaged;
    for (const QString& extension : QtCommon::supported_file_extensions) {
        packaged << QStringLiteral("*.") + extension;
    }
    return !dir.entryList(packaged, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
}

QPixmap LoadCover(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > 32 * 1024 * 1024) {
        return {};
    }
    QPixmap pixmap;
    if (!pixmap.load(path)) {
        return {};
    }
    if (pixmap.width() > 720 || pixmap.height() > 720) {
        pixmap = pixmap.scaled(720, 720, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    return pixmap;
}

/// True for a missing or fully transparent icon (some dumps have none).
bool IsBlank(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return true;
    }
    const QImage image = pixmap.toImage();
    const int step_x = std::max(1, image.width() / 16);
    const int step_y = std::max(1, image.height() / 16);
    for (int y = 0; y < image.height(); y += step_y) {
        for (int x = 0; x < image.width(); x += step_x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return false;
            }
        }
    }
    return true;
}

/// A cover the player put next to the game file: "<file name>.png" or "icon.png".
QPixmap LocalCover(const QString& game_path) {
    const QFileInfo info(game_path);
    const QDir dir = info.absoluteDir();
    for (const QString& base : {info.completeBaseName(), QStringLiteral("icon")}) {
        for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
            QPixmap pixmap = LoadCover(dir.filePath(base + QLatin1String(ext)));
            if (!pixmap.isNull()) {
                return pixmap;
            }
        }
    }
    return {};
}

#ifdef _WIN32
/// A window Windows keeps off screen although it is "visible": games create theirs cloaked and
/// show it once their first frame is ready (DK Bananza after its shaders).
bool IsCloaked(HWND hwnd) {
    using GetAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const auto get_attribute = [] {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<GetAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute"))
                   : nullptr;
    }();
    constexpr DWORD Cloaked = 14; // DWMWA_CLOAKED
    DWORD cloaked = 0;
    return get_attribute && SUCCEEDED(get_attribute(hwnd, Cloaked, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

/// Hides or shows one of Notos's own windows without changing its state (DWMWA_CLOAK).
void SetCloaked(HWND hwnd, bool cloaked) {
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto set_attribute = [] {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"))
                   : nullptr;
    }();
    constexpr DWORD Cloak = 13; // DWMWA_CLOAK
    const BOOL value = cloaked ? TRUE : FALSE;
    if (set_attribute) {
        set_attribute(hwnd, Cloak, &value, sizeof(value));
    }
}

/// The native game's main window: its largest visible top-level window.
HWND NativeWindow(void* process) {
    if (!process) {
        return nullptr;
    }
    struct Find {
        DWORD pid;
        HWND best = nullptr;
        long long area = 0;
    } find{GetProcessId(process)};
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            auto& f = *reinterpret_cast<Find*>(data);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != f.pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
                IsCloaked(hwnd)) {
                return TRUE;
            }
            RECT r{};
            GetWindowRect(hwnd, &r);
            const long long area =
                static_cast<long long>(r.right - r.left) * static_cast<long long>(r.bottom - r.top);
            if (area > f.area) {
                f.area = area;
                f.best = hwnd;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.best;
}

/// The game's window without its frame, over the whole screen it is on.
void MakeBorderless(HWND hwnd) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    const RECT& m = monitor.rcMonitor;
    constexpr LONG_PTR Frame =
        WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT r{};
    GetWindowRect(hwnd, &r);
    if ((style & Frame) == 0 && r.left == m.left && r.top == m.top && r.right == m.right &&
        r.bottom == m.bottom) {
        return; // already
    }
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~Frame);
    SetWindowPos(hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
}

/// MakeBorderless off Notos's thread: changing another program's window waits for that program,
/// and a game busy closing or loading would freeze Notos with it (and a game waiting on Notos
/// would never end).
void MakeBorderlessAsync(HWND hwnd) {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) {
        return;
    }
    std::thread([hwnd] {
        if (IsWindow(hwnd)) {
            MakeBorderless(hwnd);
        }
        busy = false;
    }).detach();
}
#endif

QFont Font(const QFont& base, double px, QFont::Weight weight = QFont::Normal) {
    QFont font = base;
    font.setPixelSize(std::max(8, static_cast<int>(std::lround(px))));
    font.setWeight(weight);
    return font;
}

} // namespace

Home::Home(QMainWindow* window_, QBoxLayout* central, QWidget* game_list_, QWidget* placeholder_,
           HomeHooks hooks_)
    : QWidget(window_), window{window_}, game_list{game_list_}, placeholder{placeholder_},
      hooks{std::move(hooks_)} {
    setObjectName(QStringLiteral("NotosHome"));
    // The launcher's own type (the emulator's dialogs keep Inter).
    for (const char* weight : {"Regular", "Medium", "SemiBold", "Bold"}) {
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/fritchii/fonts/Montserrat-%1.ttf").arg(QLatin1String(weight)));
    }
    setFont(QFont(QStringLiteral("Montserrat")));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    central->addWidget(this, 1);

    LoadConfig();
    // Next to Notos.exe (the sounds come with each version), or user/notos_music from before.
    const QString music = QCoreApplication::applicationDirPath() + QStringLiteral("/notos_music");
    sounds.Load(QDir(music).exists()
                    ? music
                    : QDir::cleanPath(QFileInfo(ConfigPath()).absolutePath() +
                                      QStringLiteral("/../notos_music")));
    ApplySound();

    // The classic menus stay in a zero-height bar (their shortcuts keep working); the classic game
    // list shows them, with "Inicio" to come back.
    home_action = window->menuBar()->addAction(QStringLiteral("◀ Inicio"));
    connect(home_action, &QAction::triggered, this, &Home::ShowHome);
    window->menuBar()->setFixedHeight(0); // also when Notos starts straight into a game
    home_action->setVisible(false);

    game_list->installEventFilter(this);
    placeholder->installEventFilter(this);

    refresh_timer = new QTimer(this);
    refresh_timer->setSingleShot(true);
    refresh_timer->setInterval(150);
    connect(refresh_timer, &QTimer::timeout, this, &Home::Refresh);
    if (hooks.model) {
        const auto later = [this] { refresh_timer->start(); };
        connect(hooks.model, &QAbstractItemModel::rowsInserted, this, later);
        connect(hooks.model, &QAbstractItemModel::rowsRemoved, this, later);
        connect(hooks.model, &QAbstractItemModel::modelReset, this, later);
        connect(hooks.model, &QAbstractItemModel::dataChanged, this, later);
    }

    clock_timer = new QTimer(this);
    clock_timer->setInterval(1000);
    connect(clock_timer, &QTimer::timeout, this, [this] {
        pad_name = PadName();
        if (isVisible()) {
            update();
        }
    });
    pad_name = PadName();
    clock_timer->start();

    pad_timer = new QTimer(this);
    pad_timer->setInterval(16);
    connect(pad_timer, &QTimer::timeout, this, &Home::PollPad);
    pad_timer->start();

    overlay = new Overlay(this);
    overlay_timer = new QTimer(this);
    overlay_timer->setInterval(16);
    connect(overlay_timer, &QTimer::timeout, this, [this] {
        if (overlay->isVisible()) {
            PlaceOverlay(); // follows fullscreen changes and window moves
            overlay->update();
        }
    });
    // The keyboard's Home key opens the Home menu too.
    auto* home_key = new QShortcut(QKeySequence(Qt::Key_Home), window);
    connect(home_key, &QShortcut::activated, this, &Home::ToggleGameMenu);

    native_timer = new QTimer(this);
    native_timer->setInterval(500);
    connect(native_timer, &QTimer::timeout, this, [this] {
#ifdef _WIN32
        if (native_process && WaitForSingleObject(native_process, 0) == WAIT_OBJECT_0) {
            // A store-started game may hand over from a launcher to the game itself: another
            // program from its folder takes over.
            // The launcher may close a moment before the game shows: Notos waits for it.
            if (!native_dir.isEmpty() && !closing) {
                CloseHandle(native_process);
                native_process = nullptr;
                native_window = nullptr;
                if (!FindNativeProcess()) {
                    native_handover.start();
                }
                return;
            }
            NativeEnded();
            return;
        }
        if (!native_process && !native_dir.isEmpty()) {
            // Steam starts it: Notos waits for it to run from its folder.
            if (FindNativeProcess()) {
                native_handover.invalidate();
                return;
            }
            if (native_handover.isValid()) {
                if (native_handover.elapsed() > 12000) {
                    NativeEnded(); // nothing came after: the game is over
                }
                return;
            }
            if (!native_stepped_aside && native_wait.elapsed() > 8000) {
                // Steam is starting, updating or asking something (sign-in, a licence): its
                // windows show instead of the loading screen.
                native_stepped_aside = true;
                window->showMinimized();
            }
            if (native_wait.elapsed() > 180000) {
                const QString name = native_name;
                NativeEnded();
                Message(QStringLiteral("%1 no llegó a abrirse.").arg(name));
            }
            return;
        }
        // Games size their window again while they start (DK Bananza once its shaders are
        // ready): it is made borderless again for a while after it shows.
        if (native_window && !native_suspended && !closing && GamesFullscreen() &&
            native_window_seen.isValid() && native_window_seen.elapsed() < 15000) {
            if (HWND game_window = NativeWindow(native_process);
                game_window && !IsHungAppWindow(game_window)) {
                MakeBorderlessAsync(game_window);
            }
        }
#endif
    });

    anim_clock.start();
    anim_timer = new QTimer(this);
    anim_timer->setTimerType(Qt::PreciseTimer);
    anim_timer->setInterval(8); // a frame at up to 120 Hz
    connect(anim_timer, &QTimer::timeout, this, &Home::AnimTick);
    anim_timer->start();

    loading_timer = new QTimer(this);
    loading_timer->setInterval(33);
    connect(loading_timer, &QTimer::timeout, this, &Home::LoadingTick);

    if (hooks.set_game_fullscreen) {
        hooks.set_game_fullscreen(GamesFullscreen()); // also for a game Notos starts with
    }
    // The main window is shown after this, framed, and goes fullscreen a moment later: it stays
    // invisible until then, so the framed window never flashes.
    const bool start_fullscreen = Prefs().value(QStringLiteral("fullscreen")).toBool();
#ifdef _WIN32
    if (start_fullscreen) {
        SetCloaked(reinterpret_cast<HWND>(window->winId()), true);
    }
#endif

    QTimer::singleShot(0, this, [this, start_fullscreen] {
        MoveToLastScreen();
        Restore();
        if (Prefs().value(QStringLiteral("fullscreen")).toBool()) {
            ApplyFullscreen();
        }
#ifdef _WIN32
        if (start_fullscreen) {
            // Visible once Windows has drawn it at its fullscreen size.
            QTimer::singleShot(120, this, [this] {
                SetCloaked(reinterpret_cast<HWND>(window->winId()), false);
            });
        }
#else
        (void)start_fullscreen;
#endif
    });
}

// ---- data

void Home::MoveToLastScreen() {
    // The monitor Notos was on last (Qt names it by its model): the same one where it was, else
    // that model elsewhere, else whichever monitor is there now.
    const QJsonObject saved = config.value(QStringLiteral("screen")).toObject();
    const QString name = saved.value(QStringLiteral("name")).toString();
    const QPoint origin{saved.value(QStringLiteral("x")).toInt(),
                        saved.value(QStringLiteral("y")).toInt()};
    QScreen* target = nullptr;
    int best = 0;
    if (!name.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            const int score = (screen->name() == name ? 2 : 0) +
                              (screen->geometry().topLeft() == origin ? 1 : 0);
            if (score > best) {
                best = score;
                target = screen;
            }
        }
    }
    QWindow* handle = window->windowHandle();
    if (!handle) {
        window->winId();
        handle = window->windowHandle();
    }
    if (target && handle && handle->screen() != target) {
        // The window keeps its size, centred on that monitor; fullscreen then fills it.
        const QRect area = target->availableGeometry();
        QRect frame = window->geometry();
        frame.setSize(frame.size().boundedTo(area.size()));
        frame.moveCenter(area.center());
        handle->setScreen(target);
        window->setGeometry(frame);
    }
    if (handle) {
        // From now on, remembered whenever Notos changes monitor (dragged, or Win+Shift+arrow).
        connect(handle, &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (!screen) {
                return;
            }
            config.insert(QStringLiteral("screen"),
                          QJsonObject{{QStringLiteral("name"), screen->name()},
                                      {QStringLiteral("x"), screen->geometry().x()},
                                      {QStringLiteral("y"), screen->geometry().y()}});
            SaveConfig();
        });
    }
}

void Home::LoadConfig() {
    QFile file(ConfigPath());
    if (file.open(QFile::ReadOnly)) {
        config = QJsonDocument::fromJson(file.readAll()).object();
    }
}

void Home::SaveConfig() {
    QDir().mkpath(QFileInfo(ConfigPath()).absolutePath());
    QFile file(ConfigPath());
    if (file.open(QFile::WriteOnly | QFile::Truncate)) {
        file.write(QJsonDocument(config).toJson());
    }
}

QJsonObject Home::Prefs() const {
    QJsonObject settings = config.value(QStringLiteral("settings")).toObject();
    const auto fallback = [&](const char* key, const QJsonValue& value) {
        if (!settings.contains(QLatin1String(key))) {
            settings.insert(QLatin1String(key), value);
        }
    };
    fallback("theme", QStringLiteral("dark"));
    fallback("accent", QStringLiteral("rainbow"));
    fallback("fullscreen", true);
    fallback("gamesFullscreen", true);
    fallback("clock24", true);
    fallback("sound", true);
    fallback("volume", 70);
    return settings;
}

void Home::ApplySound() {
    sounds.SetEnabled(Prefs().value(QStringLiteral("sound")).toBool());
    sounds.SetVolume(Prefs().value(QStringLiteral("volume")).toInt());
}

bool Home::GamesFullscreen() const {
    return Prefs().value(QStringLiteral("gamesFullscreen")).toBool();
}

void Home::SetSetting(const QString& key, const QJsonValue& value) {
    QJsonObject settings = config.value(QStringLiteral("settings")).toObject();
    settings.insert(key, value);
    config.insert(QStringLiteral("settings"), settings);
    SaveConfig();
    update();
}

QString Home::PadName() const {
    // The emulator's input already started SDL's gamepad support; its first gamepad is named
    // like the launcher showed it ("DualSense Wireless Controller", "Xbox Series X Controller").
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        return {};
    }
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    QString name;
    if (pads && count > 0) {
        if (const char* raw = SDL_GetGamepadNameForID(pads[0])) {
            name = QString::fromUtf8(raw);
        }
    }
    SDL_free(pads);
    return name;
}

bool Home::Rainbow() const {
    return Prefs().value(QStringLiteral("accent")).toString() == QStringLiteral("rainbow");
}

QColor Home::Accent() const {
    if (Rainbow()) {
        // One turn of the colour wheel every 9 seconds.
        return Hsv(anim_clock.elapsed() * 0.04, 0.62, 1.0);
    }
    const QColor color(Prefs().value(QStringLiteral("accent")).toString());
    return color.isValid() ? color : QColor(Accents[0].second);
}

QBrush Home::AccentBrush(const QRectF& rect) const {
    if (!Rainbow()) {
        return Accent();
    }
    QConicalGradient gradient(rect.center(), std::fmod(anim_clock.elapsed() * 0.09, 360.0));
    for (int i = 0; i <= 6; ++i) {
        gradient.setColorAt(i / 6.0, Hsv(i * 60.0, 0.62, 1.0));
    }
    return gradient;
}

void Home::AnimTick() {
    const qint64 now = anim_clock.elapsed();
    const double dt = static_cast<double>(std::min<qint64>(now - anim_last, 100));
    anim_last = now;
    ++anim_ticks;
    // The open menu comes in and its highlight glides (also over a game, where Notos is hidden).
    if (menu.empty()) {
        menu_show = 0;
    } else {
        bool changed = false;
        if (menu_show <= 0) {
            menu_cursor = menu_index;
        }
        if (menu_show < 1) {
            menu_show = std::min(1.0, menu_show + dt / 200.0);
            changed = true;
        }
        const double gap = menu_index - menu_cursor;
        if (std::abs(gap) > 0.01) {
            menu_cursor += gap * (1 - std::exp(-dt / 50.0));
            changed = true;
        } else if (menu_cursor != menu_index) {
            menu_cursor = menu_index;
            changed = true;
        }
        if (changed) {
            if (overlay && overlay->isVisible()) {
                overlay->update();
            } else {
                update();
            }
        }
    }
    if (closing && closing_emulated) {
        // Kept on top while the window changes under it.
        PlaceOverlay();
        if (!overlay->isVisible()) {
            overlay->show();
        }
        overlay->raise();
        overlay->update();
    }
    if (!isVisible()) {
        return;
    }
    // The emulator shows its game list again when a game ends: never next to the home screen.
    if (!list_mode && (game_list->isVisible() || placeholder->isVisible())) {
        game_list->hide();
        placeholder->hide();
    }
    // The opening plays on frames actually shown: Notos is busy for a few seconds when it starts,
    // and a sequence timed on the clock would be over before anyone saw it.
    if (intro_pending) {
        intro_calm = dt < 30 ? intro_calm + 1 : 0;
        if (intro_calm >= 20) {
            intro_pending = false;
            intro_start = 0;
            sounds.Play(QStringLiteral("startup"));
        }
    } else if (intro_start >= 0) {
        intro_start += std::min(dt, 34.0);
        if (intro_start >= IntroTotal) {
            intro_start = -1;
            intro_reveal_only = false;
        }
    }
    bool moving = intro_pending || intro_start >= 0;
    // Each frame covers a share of what is left: quick at first, soft at the end.
    const double glide = 1 - std::exp(-dt / 85.0);
    if (std::abs(scroll_target - scroll) > 0.5) {
        scroll += (scroll_target - scroll) * glide;
        moving = true;
    } else if (scroll != scroll_target) {
        scroll = scroll_target;
        moving = true;
    }
    if (focus.size() != games.size()) {
        focus.assign(games.size(), 0.0);
    }
    const double grow = 1 - std::exp(-dt / 75.0);
    for (int i = 0; i < static_cast<int>(focus.size()); ++i) {
        const double target = row == 0 && i == game_index ? 1.0 : 0.0;
        if (std::abs(target - focus[i]) > 0.004) {
            focus[i] += (target - focus[i]) * grow;
            moving = true;
        } else if (focus[i] != target) {
            focus[i] = target;
            moving = true;
        }
    }
    // The rainbow alone turns slowly: a few frames a second are enough.
    if (moving || loading || launching || closing || (Rainbow() && anim_ticks % 3 == 0)) {
        update();
    }
}

bool Home::Dark() const {
    return Prefs().value(QStringLiteral("theme")).toString() != QStringLiteral("light");
}

QString Home::CoverFor(const QString& key) const {
    return config.value(QStringLiteral("covers")).toObject().value(key).toString();
}

void Home::Refresh() {
    QString selected_key;
    if (game_index >= 0 && game_index < static_cast<int>(games.size())) {
        const Game& game = games[game_index];
        selected_key = game.native ? game.exe : game.path;
    }
    const bool was_empty = games.empty();
    games.clear();

    // Native games first, in the player's order.
    for (const QJsonValue& value : config.value(QStringLiteral("native")).toArray()) {
        const QJsonObject object = value.toObject();
        Game game;
        game.native = true;
        game.name = object.value(QStringLiteral("name")).toString();
        game.exe = object.value(QStringLiteral("exe")).toString();
        game.args = object.value(QStringLiteral("args")).toString();
        game.show_console = object.value(QStringLiteral("showConsole")).toBool();
        game.dir = object.value(QStringLiteral("dir")).toString();
        game.cover = LoadCover(object.value(QStringLiteral("cover")).toString());
        if (game.name.isEmpty()) {
            game.name = QFileInfo(game.exe).completeBaseName();
        }
        games.push_back(std::move(game));
    }

    // Then the Switch games added as files, and those of the game list's folders.
    std::set<QString> seen;
    for (const QJsonValue& value : config.value(QStringLiteral("files")).toArray()) {
        const QJsonObject object = value.toObject();
        Game game;
        game.picked = true;
        game.path = object.value(QStringLiteral("path")).toString();
        if (game.path.isEmpty() || !seen.insert(game.path).second) {
            continue;
        }
        game.program_id = object.value(QStringLiteral("programId")).toString().toULongLong(nullptr, 16);
        game.name = object.value(QStringLiteral("name")).toString();
        if (game.name.isEmpty()) {
            game.name = QFileInfo(game.path).completeBaseName();
        }
        game.cover = LoadCover(CoverFor(game.path));
        if (game.cover.isNull()) {
            game.cover = LoadCover(object.value(QStringLiteral("icon")).toString());
        }
        if (IsBlank(game.cover)) {
            game.cover = LocalCover(game.path);
        }
        if (IsBlank(game.cover)) {
            game.cover = {};
        }
        games.push_back(std::move(game));
    }
    const auto add = [&](const QStandardItem* item) {
        if (item->data(GameListItem::TypeRole).toInt() !=
            static_cast<int>(GameListItemType::Game)) {
            return;
        }
        const QString path = item->data(GameListItemPath::FullPathRole).toString();
        if (path.isEmpty() || IsStrayDump(path) || !seen.insert(path).second) {
            return;
        }
        Game game;
        game.path = path;
        game.program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
        game.name = item->data(GameListItemPath::TitleRole).toString();
        if (game.name.isEmpty()) {
            game.name = QFileInfo(path).completeBaseName();
        }
        game.cover = LoadCover(CoverFor(path));
        if (game.cover.isNull()) {
            const QByteArray icon = item->data(GameListItemPath::IconDataRole).toByteArray();
            if (!game.cover.loadFromData(icon)) {
                game.cover = item->data(Qt::DecorationRole).value<QPixmap>();
            }
        }
        if (IsBlank(game.cover)) {
            game.cover = LocalCover(path);
        }
        if (IsBlank(game.cover)) {
            game.cover = {};
        }
        games.push_back(std::move(game));
    };
    if (hooks.model) {
        for (int r = 0; r < hooks.model->rowCount(); ++r) {
            const QStandardItem* top = hooks.model->item(r);
            if (!top) {
                continue;
            }
            if (top->hasChildren()) {
                for (int child = 0; child < top->rowCount(); ++child) {
                    add(top->child(child));
                }
            } else {
                add(top);
            }
        }
    }

    // The most recently played first, on the left; those never played from here follow in their
    // order.
    const QJsonObject played = config.value(QStringLiteral("lastPlayed")).toObject();
    const auto last_played = [&played](const Game& game) {
        return static_cast<qint64>(
            played.value(game.native ? game.exe : game.path).toDouble());
    };
    std::stable_sort(games.begin(), games.end(), [&](const Game& a, const Game& b) {
        return last_played(a) > last_played(b);
    });

    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if ((games[i].native ? games[i].exe : games[i].path) == selected_key) {
            game_index = i;
        }
    }
    if (was_empty && !games.empty()) {
        row = 0; // the games just arrived: start on the covers, at the last one played
        game_index = 0;
    }
    Select();
}

// ---- screens

void Home::ShowHome() {
    list_mode = false;
    game_list->hide();
    placeholder->hide();
    window->menuBar()->setFixedHeight(0);
    home_action->setVisible(false);
    if (window->statusBar()->isVisible()) {
        status_was_visible = true;
        window->statusBar()->hide();
    }
    show();
    setFocus();
    Refresh();
}

void Home::ShowList() {
    list_mode = true;
    hide();
    window->menuBar()->setMinimumHeight(0);
    window->menuBar()->setMaximumHeight(QWIDGETSIZE_MAX);
    home_action->setVisible(true);
    window->statusBar()->setVisible(status_was_visible);
    if (hooks.model && hooks.model->rowCount() == 0) {
        placeholder->show();
    } else {
        game_list->show();
    }
}

void Home::Restore() {
    // A restart shows the list for a moment, and nothing may cover a running game.
    if (hooks.single_window() && hooks.emulation_running()) {
        return;
    }
    if (list_mode) {
        hide();
    } else {
        ShowHome();
    }
    // Back from a game, which left the emulator's fullscreen.
    if (Prefs().value(QStringLiteral("fullscreen")).toBool() && !window->isFullScreen() &&
        !window->isMinimized()) {
        ApplyFullscreen();
    }
    if (closing && closing_emulated) {
        EndClosing(); // the game is gone: the home screen is back under the closing screen
    } else if (reveal_after_game && !list_mode) {
        StartReveal();
    }
    reveal_after_game = false;
}

void Home::OnEmulationStarting() {
    intro_pending = false; // no opening after a game (Notos started straight into one)
    unblock_on_release = false;
    Core::HID::EmulatedController::SetGameInputBlocked(false);
    if (!hooks.single_window()) {
        loading_game = {};
        return;
    }
    CloseMenu();
    hide();
    window->statusBar()->setVisible(status_was_visible);
    if (!loading_game.name.isEmpty()) {
        // The loading screen covers the emulator's own until the game shows. The overlay appears
        // on the first tick, once the window took its game size.
        loading = true;
        loading_emulated = true;
        loading_phase = 0;
        loading_progress = -1;
        loading_status = QStringLiteral("Iniciando…");
        ResetLoadingScreen();
        loading_timer->start();
        qApp->installEventFilter(this);
    }
}

void Home::RestartGame() {
    if (!hooks.restart) {
        return;
    }
    LeaveGameMenu(false);
    // The loading screen shows again for the same game, in place of the closing screen.
    restarting = true;
    loading_game = running_game;
    hooks.restart();
    restarting = false;
}

void Home::MultiplayerMenu() {
    // The emulator's rooms: local wireless play with players over the internet.
    const auto room = [this](int action) {
        return [this, action] {
            if (hooks.multiplayer) {
                hooks.multiplayer(action);
            }
        };
    };
    std::vector<Entry> entries{
        Entry{[] { return QStringLiteral("Buscar salas públicas"); }, room(0), {}, true},
        Entry{[] { return QStringLiteral("Crear sala"); }, room(1), {}, true},
        Entry{[] { return QStringLiteral("Conectarse a una sala (IP)"); }, room(2), {}, true},
    };
    if (hooks.in_room && hooks.in_room()) {
        entries.push_back(
            Entry{[] { return QStringLiteral("Ver sala actual"); }, room(3), {}, true});
        entries.push_back(
            Entry{[] { return QStringLiteral("Salir de la sala"); }, room(4), {}, true});
    }
    OpenMenu(QStringLiteral("Multijugador"), std::move(entries));
}

void Home::OnEmulationStopping() {
    if (loading_emulated) {
        EndLoading();
    }
    home_down = false;
    if (restarting) {
        return;
    }
    reveal_after_game = true;
    if (hooks.single_window()) {
        StartClosing(true); // in place of the Home menu, if it was open
    } else if (overlay->isVisible()) {
        LeaveGameMenu(false);
    }
}

void Home::BlockGameInput(bool block) {
    if (block) {
        unblock_on_release = false;
        Core::HID::EmulatedController::SetGameInputBlocked(true);
    } else if (Core::HID::EmulatedController::GameInputBlocked()) {
        unblock_on_release = true; // PollPad, once nothing is held
    }
}

// ---- closing a game

void Home::StartClosing(bool emulated) {
    menu.clear();
    paused_by_menu = false;
    BlockGameInput(false);
    closing = true;
    closing_emulated = emulated;
    closing_start = anim_clock.elapsed();
    loading_game = running_game;
    if (loading_game.name.isEmpty()) {
        const QStringList parts = window->windowTitle().split(QStringLiteral(" | "));
        loading_game.name = parts.size() > 3 ? parts[3] : QStringLiteral("Notos");
    }
    loading_progress = -1;
    loading_status = QStringLiteral("Cerrando…");
    ResetLoadingScreen();
    if (emulated) {
        // Over the whole screen: the window leaves its game size underneath.
        PlaceOverlay();
        overlay->show();
        overlay->raise();
        qApp->installEventFilter(this);
    }
    update();
}

void Home::EndClosing() {
    if (!closing) {
        return;
    }
    LOG_INFO(Frontend, "Notos: closing screen ends");
    // Long enough to be read, even when the game is gone at once.
    const qint64 shown = anim_clock.elapsed() - closing_start;
    if (shown < 800) {
        QTimer::singleShot(800 - shown, this, &Home::EndClosing);
        return;
    }
    const bool emulated = closing_emulated;
    closing = false;
    closing_emulated = false;
    loading_game = {};
    running_game = {};
    loading_status.clear();
    if (emulated) {
        overlay_timer->stop();
        overlay->hide();
        qApp->removeEventFilter(this);
        setFocus();
    }
    StartReveal();
    update();
}

void Home::StartReveal() {
    // The covers and buttons come in as at the opening, without the emblem.
    if (intro_pending || (intro_start >= 0 && !intro_reveal_only)) {
        return;
    }
    intro_reveal_only = true;
    intro_start = IntroUi - 60;
    sounds.Play(QStringLiteral("home"));
    update();
}

void Home::OnLoadProgress(int stage, qulonglong value, qulonglong total) {
    if (!loading_emulated) {
        return;
    }
    if (stage == 1 && total > 0) {
        loading_progress = std::clamp(static_cast<double>(value) / total, 0.0, 1.0);
        loading_status = QStringLiteral("Cargando shaders… %1 de %2").arg(value).arg(total);
    } else if (stage == 0) {
        loading_progress = -1;
        loading_status = QStringLiteral("Preparando…");
    } else {
        loading_progress = -1;
        loading_status = QStringLiteral("Iniciando…");
    }
}

void Home::OnLoadFinished() {
    if (loading_emulated) {
        EndLoading();
    }
}

// ---- the loading screen

void Home::LoadingTick() {
    if (!loading) {
        loading_timer->stop();
        return;
    }
    ++loading_phase;
    if (loading_emulated) {
        PlaceOverlay();
        if (!overlay->isVisible()) {
            loading_start = anim_clock.elapsed(); // it comes in from now
            overlay->show();
        }
        overlay->raise();
        overlay->update();
        return;
    }
#ifdef _WIN32
    if (native_window) {
        // Handed over: the loading screen stays behind the game's window while the game draws its
        // first frames (an unpainted window shows what is behind it), then Notos steps aside.
        if (native_window_seen.elapsed() > 15000) {
            EndLoading();
            window->showMinimized();
            return;
        }
        update();
        return;
    }
    if (!native_process && !native_dir.isEmpty()) {
        loading_status = QStringLiteral("Abriendo en Steam…");
        update();
        return;
    }
    if (loading_phase % 10 == 1) {
        ReadNativeProgress();
    }
    HWND game_window = NativeWindow(native_process);
    if (game_window && !native_ready && !native_ping_out) {
        // Games show their window before they load and answer nothing until then: a message
        // answered quickly says the game is there. Asked without waiting (no Notos freeze).
        native_ping_out = true;
        native_ping.restart();
        const auto answered = [](HWND, UINT, ULONG_PTR data, LRESULT) {
            auto* self = reinterpret_cast<Home*>(data);
            self->native_ping_out = false;
            if (self->native_ping.elapsed() < 250) {
                self->native_ready = true;
            }
        };
        if (!SendMessageCallbackW(game_window, WM_NULL, 0, 0, answered,
                                  reinterpret_cast<ULONG_PTR>(this))) {
            native_ping_out = false;
        }
    }
    if (game_window && native_ready) {
        // The game shows: it takes the screen.
        native_window = game_window;
        native_window_seen.restart(); // native_timer keeps it borderless for a while
        if (GamesFullscreen()) {
            MakeBorderlessAsync(game_window);
        }
        loading_progress = 1;
        loading_status = QStringLiteral("Iniciando…");
        SetForegroundWindow(game_window);
        update();
        return;
    }
#endif
    update();
}

void Home::ReadNativeProgress() {
    // Recompiled ports log their shader preparation ("123 of 456 shaders"): the newest log next
    // to the game written since it started.
    const QDir dir = native_dir.isEmpty() ? QFileInfo(native_exe).absoluteDir() : QDir(native_dir);
    QFileInfo newest;
    for (const QString& folder : {dir.filePath(QStringLiteral("logs")), dir.absolutePath()}) {
        const QFileInfoList logs = QDir(folder).entryInfoList({QStringLiteral("*.log")},
                                                               QDir::Files, QDir::Time);
        if (!logs.isEmpty() && logs.first().lastModified() >= launch_time.addSecs(-2) &&
            (!newest.exists() || logs.first().lastModified() > newest.lastModified())) {
            newest = logs.first();
        }
    }
    if (!newest.exists()) {
        return;
    }
    QFile file(newest.absoluteFilePath());
    if (!file.open(QFile::ReadOnly)) {
        return;
    }
    file.seek(std::max<qint64>(0, file.size() - 16 * 1024));
    const QString tail = QString::fromUtf8(file.readAll());
    static const QRegularExpression shaders(QStringLiteral("(\\d+) of (\\d+) shaders"));
    QRegularExpressionMatch last;
    for (auto it = shaders.globalMatch(tail); it.hasNext();) {
        last = it.next();
    }
    if (last.hasMatch()) {
        const qulonglong done = last.captured(1).toULongLong();
        const qulonglong total = last.captured(2).toULongLong();
        if (total > 0) {
            loading_progress = std::clamp(static_cast<double>(done) / total, 0.0, 1.0);
            loading_status = QStringLiteral("Cargando shaders… %1 de %2").arg(done).arg(total);
        }
    }
}

void Home::EndLoading() {
    sounds.Stop(QStringLiteral("loading"));
    const bool emulated = loading_emulated;
    loading = false;
    loading_emulated = false;
    loading_timer->stop();
    loading_game = {};
    loading_progress = -1;
    loading_status.clear();
    if (emulated) {
        overlay->hide();
        qApp->removeEventFilter(this);
        if (QWidget* render = hooks.render ? hooks.render() : nullptr) {
            render->activateWindow();
            render->setFocus();
        }
    }
    update();
}

void Home::ResetLoadingScreen() {
    loading_start = anim_clock.elapsed();
    loading_shown = 0;
    status_key.clear();
    status_prev.clear();
    status_now.clear();
}

void Home::PaintLoading(QPainter& p, const QRect& area) {
    // Notos' own loading screen: the game's art far out of focus behind, wind crossing it, the
    // emblem breathing inside a ring that fills as the game loads, the name and what is happening.
    const bool dark = Dark();
    const QColor bg = dark ? QColor(0x2B, 0x2B, 0x2E) : QColor(0xEC, 0xEC, 0xEC);
    const QColor text = dark ? QColor(Qt::white) : QColor(Qt::black);
    const QColor sub = dark ? QColor(0xC4, 0xC4, 0xCC) : QColor(0x4A, 0x4A, 0x52);
    const QColor accent = Accent();
    const double w = area.width();
    const double h = area.height();
    const double s = std::max(0.45, std::min(w / 1920.0, h / 1080.0)) * 1.1;
    p.save();
    p.translate(area.topLeft());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);
    const double now = static_cast<double>(anim_clock.elapsed());
    // Milliseconds on screen. Closing a game, the same screen comes in over it.
    const double t = now - static_cast<double>(closing ? closing_start : loading_start);
    const double show = EaseOut(t / 450.0);
    QColor backdrop = bg;
    // Over a game (the Home menu's window) it fades in; in Notos' own window it is the ground.
    backdrop.setAlphaF(closing && closing_emulated ? show : 1.0);
    p.fillRect(QRectF(0, 0, w, h), backdrop);

    // The game's cover over the whole screen, blurred to colours and toned to the theme. Built
    // once (a little larger than the screen: it drifts).
    const QString key = QStringLiteral("%1|%2x%3|%4")
                            .arg(loading_game.native ? loading_game.exe : loading_game.path)
                            .arg(area.width())
                            .arg(area.height())
                            .arg(dark);
    if (key != loading_backdrop_key) {
        loading_backdrop_key = key;
        loading_backdrop = {};
        if (!loading_game.cover.isNull() && w > 0 && h > 0) {
            const QSize full(static_cast<int>(w * 1.12), static_cast<int>(h * 1.12));
            QImage art = loading_game.cover.toImage()
                             .scaled(QSize(64, 36), Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
            art = art.copy((art.width() - 64) / 2, (art.height() - 36) / 2, 64, 36)
                      .scaled(QSize(16, 9), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .scaled(QSize(160, 90), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter tone(&art);
            QColor veil = bg;
            veil.setAlphaF(dark ? 0.62 : 0.70);
            tone.fillRect(art.rect(), veil);
            tone.end();
            loading_backdrop = QPixmap::fromImage(art);
        }
    }
    if (!loading_backdrop.isNull()) {
        p.setOpacity(show);
        const double dx = std::sin(now / 7000.0) * 0.045 * w;
        const double dy = std::cos(now / 9000.0) * 0.040 * h;
        p.drawPixmap(QPointF((w - loading_backdrop.width()) / 2.0 + dx,
                             (h - loading_backdrop.height()) / 2.0 + dy),
                     loading_backdrop);
    }
    p.setOpacity(1);

    const QPointF centre(w / 2.0, h / 2.0 - 70 * s);
    const double ring_r = 122 * s;

    // Light behind the emblem, breathing.
    {
        const double breath = 0.82 + 0.18 * std::sin(now / 520.0);
        QRadialGradient glow(centre, 620 * s);
        QColor inner = accent;
        inner.setAlphaF(0.30 * show * breath);
        QColor mid = accent;
        mid.setAlphaF(0.09 * show);
        QColor outer = accent;
        outer.setAlphaF(0);
        glow.setColorAt(0, inner);
        glow.setColorAt(0.45, mid);
        glow.setColorAt(1, outer);
        p.fillRect(QRectF(0, 0, w, h), glow);
    }

    // The south wind: long gusts crossing from the left, each on its own wave, and the dust they
    // carry.
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 9; ++i) {
        const double a = std::fmod(i * 0.6180339887, 1.0);
        const double b = std::fmod(i * 0.7548776662, 1.0);
        const double c = std::fmod(i * 0.5698402910, 1.0);
        const double u = std::fmod(now / (5200.0 + 5200.0 * a) + b, 1.0);
        const double length = (0.22 + 0.25 * c) * w;
        const double x1 = u * (w + length);
        const double x0 = x1 - length;
        const double base = h * (0.10 + 0.80 * std::fmod(i * 0.3819660113 + 0.07, 1.0));
        const double swell = (14 + 26 * c) * s;
        const double wave = (260 + 240 * a) * s;
        QPainterPath gust;
        for (int step = 0; step <= 28; ++step) {
            const double x = x0 + length * step / 28.0;
            const double y = base + swell * std::sin(x / wave + i * 1.7 + now / 2600.0);
            if (step == 0) {
                gust.moveTo(x, y);
            } else {
                gust.lineTo(x, y);
            }
        }
        QColor colour = i % 3 == 0 ? accent : text;
        colour.setAlphaF((0.14 + 0.22 * c) * show);
        QColor clear = colour;
        clear.setAlphaF(0);
        QLinearGradient fade(QPointF(x0, 0), QPointF(x1, 0));
        fade.setColorAt(0, clear);
        fade.setColorAt(0.72, colour);
        fade.setColorAt(1, clear);
        p.setPen(QPen(QBrush(fade), (1.6 + 2.2 * c) * s, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(gust);
    }
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 26; ++i) {
        const double a = std::fmod(i * 0.6180339887 + 0.31, 1.0);
        const double b = std::fmod(i * 0.7548776662 + 0.11, 1.0);
        const double c = std::fmod(i * 0.5698402910 + 0.53, 1.0);
        const double x = std::fmod(a + now / (7000.0 + 9000.0 * c), 1.0) * w * 1.1 - w * 0.05;
        const double y = h * b + std::sin(x / (210 * s) + i) * 22 * s;
        QColor speck = i % 3 == 0 ? accent : text;
        speck.setAlphaF((0.10 + 0.32 * c) * show);
        p.setBrush(speck);
        const double r = (1.1 + 2.0 * c) * s;
        p.drawEllipse(QPointF(x, y), r, r);
    }

    // Rings leaving the emblem, one after the other.
    p.setBrush(Qt::NoBrush);
    for (int ring = 0; ring < 2; ++ring) {
        const double rt = std::fmod(now / 2800.0 + ring * 0.5, 1.0);
        QColor colour = accent;
        colour.setAlphaF(0.34 * (1 - rt) * (1 - rt) * show);
        p.setPen(QPen(colour, (2.6 - 1.8 * rt) * s));
        const double radius = ring_r + EaseOut(rt) * 250 * s;
        p.drawEllipse(centre, radius, radius);
    }

    // The ring: the share loaded, or an arc going round while nobody knows.
    const QRectF ring_rect(centre.x() - ring_r, centre.y() - ring_r, 2 * ring_r, 2 * ring_r);
    p.setOpacity(show);
    QColor track = text;
    track.setAlphaF(0.14);
    p.setPen(QPen(track, 6 * s));
    p.drawEllipse(ring_rect);
    QPen arc(AccentBrush(ring_rect), 6 * s, Qt::SolidLine, Qt::RoundCap);
    p.setPen(arc);
    if (loading_progress >= 0) {
        loading_shown += (loading_progress - loading_shown) * 0.14;
        const double span = std::max(1.5, 360.0 * std::clamp(loading_shown, 0.0, 1.0));
        p.drawArc(ring_rect, 90 * 16, -static_cast<int>(span * 16));
    } else {
        loading_shown = 0;
        const double turn = std::fmod(now * 0.21, 360.0);
        const double span = 80 + 45 * std::sin(now / 640.0);
        p.drawArc(ring_rect, static_cast<int>((90 - turn) * 16), -static_cast<int>(span * 16));
    }

    // The emblem, settling into place and breathing.
    {
        const double big = 150 * s * (1.14 - 0.14 * show) * (1 + 0.022 * std::sin(now / 520.0));
        const QPixmap icon = window->windowIcon().pixmap(256);
        p.drawPixmap(QRectF(centre.x() - big / 2, centre.y() - big / 2, big, big), icon,
                     QRectF(icon.rect()));
    }

    // The game's name, its letters settling together.
    double y = centre.y() + ring_r + 44 * s;
    {
        const double word = EaseOut((t - 120) / 650.0);
        QFont name_font = Font(font(), 30 * s, QFont::DemiBold);
        name_font.setLetterSpacing(QFont::AbsoluteSpacing, (1 + 16 * (1 - word)) * s);
        const QFontMetricsF metrics(name_font);
        QColor colour = text;
        colour.setAlphaF(word);
        p.setOpacity(show);
        p.setPen(colour);
        p.setFont(name_font);
        p.drawText(QRectF(32 * s, y, w - 64 * s, metrics.height()), Qt::AlignCenter,
                   metrics.elidedText(loading_game.name, Qt::ElideRight, w - 64 * s));
        y += metrics.height() + 12 * s;
    }

    // What is happening: a new step fades in over the last one (a count going up just changes).
    {
        QString step_key;
        for (const QChar ch : loading_status) {
            if (!ch.isDigit()) {
                step_key.append(ch);
            }
        }
        if (step_key != status_key) {
            status_key = step_key;
            status_prev = status_now;
            status_changed = anim_clock.elapsed();
        }
        status_now = loading_status;
        const double f = EaseOut((now - static_cast<double>(status_changed)) / 320.0);
        const QFont status_font = Font(font(), 21 * s);
        const double status_h = QFontMetricsF(status_font).height();
        p.setFont(status_font);
        p.setPen(sub);
        if (f < 1 && !status_prev.isEmpty()) {
            p.setOpacity(show * (1 - f));
            p.drawText(QRectF(32 * s, y - 8 * s * f, w - 64 * s, status_h), Qt::AlignCenter,
                       status_prev);
        }
        p.setOpacity(show * f);
        p.drawText(QRectF(32 * s, y + 8 * s * (1 - f), w - 64 * s, status_h), Qt::AlignCenter,
                   status_now);
    }
    p.setOpacity(show);

    if (closing) {
        p.restore();
        return;
    }
    // How to cancel.
    PaintHints(p, {{QStringLiteral("Esc"), QStringLiteral("○"), QStringLiteral("Cancelar")}},
               QRectF(0, h - BottomBar * s, w - SideMargin * s, BottomBar * s), Qt::AlignRight,
               HintSize * 0.85 * s, sub);
    p.restore();
}

// ---- the Home menu over a running game

namespace {
constexpr Qt::WindowFlags OverlayFlags =
    Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
}

Overlay::Overlay(Home* home_) : QWidget(home_->window, OverlayFlags), home{home_} {
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
}

void Overlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // A native game's loading and closing screens are Notos' own window, never this one.
    if ((home->loading && home->loading_emulated) || (home->closing && home->closing_emulated)) {
        home->PaintLoading(p, rect());
        return;
    }
    if (home->loading || home->closing) {
        return;
    } else {
        home->PaintMenu(p, rect());
    }
}

void Overlay::keyPressEvent(QKeyEvent* event) {
    home->pad_last = false;
    if (event->key() == Qt::Key_Home) {
        home->ToggleGameMenu();
        return;
    }
    const QString cmd = home->KeyCommand(event->key());
    if (cmd.isEmpty() || cmd == QStringLiteral("fullscreen")) {
        QWidget::keyPressEvent(event);
        return;
    }
    home->Nav(cmd);
    update();
}

void Overlay::mousePressEvent(QMouseEvent* event) {
    if (home->loading || home->closing) {
        return;
    }
    home->MenuMouse(event->position().toPoint(), event->button());
    update();
}

void Home::PlaceOverlay() {
#ifdef _WIN32
    if (native_menu) {
        // Over the native game's window, on top of everything: Notos itself is minimized.
        if (overlay->parentWidget() != nullptr) {
            const bool shown = overlay->isVisible();
            overlay->setParent(nullptr, OverlayFlags | Qt::WindowStaysOnTopHint);
            if (shown) {
                overlay->show();
            }
        }
        QRect area = QGuiApplication::primaryScreen()->geometry();
        if (HWND game_window = NativeWindow(native_process)) {
            RECT r{};
            GetWindowRect(game_window, &r);
            // Windows gives physical pixels; Qt places windows in logical ones.
            const qreal ratio = overlay->devicePixelRatioF();
            area = QRect(QPoint(qRound(r.left / ratio), qRound(r.top / ratio)),
                         QPoint(qRound(r.right / ratio) - 1, qRound(r.bottom / ratio) - 1));
        }
        if (overlay->geometry() != area) {
            overlay->setGeometry(area);
        }
        return;
    }
#endif
    QWidget* render = hooks.render ? hooks.render() : nullptr;
    // A separate game window (single window mode off) owns the menu, so it stays above it.
    QWidget* owner = render && render->isVisible() ? render->window() : window;
    if (overlay->parentWidget() != owner) {
        const bool shown = overlay->isVisible();
        overlay->setParent(owner, OverlayFlags);
        if (shown) {
            overlay->show();
        }
    }
    if (closing) {
        const QRect screen = window->screen()->geometry();
        if (overlay->geometry() != screen) {
            overlay->setGeometry(screen);
        }
        return;
    }
    const QRect area = render && render->isVisible()
                           ? QRect(render->mapToGlobal(QPoint(0, 0)), render->size())
                           : QRect(window->mapToGlobal(QPoint(0, 0)), window->size());
    if (overlay->geometry() != area) {
        overlay->setGeometry(area);
    }
}

void Home::LeaveGameMenu(bool resume) {
    menu.clear();
    overlay_timer->stop();
    overlay->hide();
    qApp->removeEventFilter(this);
    if (native_menu) {
        // A native game always runs on (closing it needs it awake to handle the request).
        native_menu = false;
        SuspendNative(false);
        overlay->setParent(window, OverlayFlags);
#ifdef _WIN32
        if (resume) {
            if (HWND game_window = NativeWindow(native_process)) {
                SetForegroundWindow(game_window);
            }
        }
#endif
        return;
    }
    BlockGameInput(false);
    if (resume && paused_by_menu && hooks.emulation_running()) {
        hooks.resume();
    }
    paused_by_menu = false;
    if (QWidget* render = hooks.render ? hooks.render() : nullptr) {
        render->activateWindow();
        render->setFocus();
    }
}

void Home::ToggleNativeMenu() {
    if (closing) {
        return;
    }
    if (loading && !native_window) {
        return;
    }
    if (loading) {
        // The game is up already: the loading screen behind it goes.
        EndLoading();
        window->showMinimized();
    }
    if (overlay->isVisible()) {
        sounds.Play(QStringLiteral("select"));
        LeaveGameMenu(true);
        return;
    }
    if (!native_process) {
        return;
    }
    native_menu = true;
    NativeMenuEntries();
    sounds.Play(QStringLiteral("select"));
    PlaceOverlay();
    overlay->show();
    overlay->raise();
#ifdef _WIN32
    // The keyboard too: Windows gives the foreground to a background program only when it joins
    // the input of the window that has it (the game's, still running at this point).
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = GetWindowThreadProcessId(foreground, nullptr);
    const DWORD own_thread = GetCurrentThreadId();
    const bool attached = foreground_thread != own_thread &&
                          AttachThreadInput(own_thread, foreground_thread, TRUE);
    SetForegroundWindow(reinterpret_cast<HWND>(overlay->winId()));
    if (attached) {
        AttachThreadInput(own_thread, foreground_thread, FALSE);
    }
#endif
    overlay->activateWindow();
    // The game goes on running under the menu.
    overlay_timer->start();
    qApp->installEventFilter(this);
}

void Home::NativeMenuEntries() {
    std::vector<Entry> entries{
        Entry{[] { return QStringLiteral("Volver al juego"); }, [this] { LeaveGameMenu(true); },
              {}, false},
        Entry{[] { return QStringLiteral("Cerrar juego"); },
              [this] {
                  OpenMenu(QStringLiteral("¿Cerrar el juego? Lo que no hayas guardado se pierde."),
                           {Entry{[] { return QStringLiteral("No"); },
                                  [this] { NativeMenuEntries(); }, {}, false},
                            Entry{[] { return QStringLiteral("Sí, cerrar"); },
                                  [this] {
                                      LeaveGameMenu(false);
                                      overlay->hide(); // also after being re-parented
                                      CloseNative();
                                      // Notos comes back over it with the closing screen.
                                      StartClosing(false);
                                      ShowNotos();
                                  },
                                  {}, false}});
              },
              {}, false},
    };
    menu_title = native_name;
    menu = std::move(entries);
    menu_index = 0;
    update();
}

void Home::SuspendNative(bool suspend) {
#ifdef _WIN32
    if (!native_process || suspend == native_suspended) {
        return;
    }
    // NtSuspendProcess / NtResumeProcess: the whole game stops while its menu shows, like an
    // emulated one (and the gamepad's presses on the menu don't reach it).
    using ProcessCall = LONG(NTAPI*)(HANDLE);
    static const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    static const auto suspend_call =
        reinterpret_cast<ProcessCall>(GetProcAddress(ntdll, "NtSuspendProcess"));
    static const auto resume_call =
        reinterpret_cast<ProcessCall>(GetProcAddress(ntdll, "NtResumeProcess"));
    const ProcessCall call = suspend ? suspend_call : resume_call;
    if (call && call(native_process) >= 0) {
        native_suspended = suspend;
    }
#endif
}

void Home::CloseNative() {
#ifdef _WIN32
    if (!native_process) {
        return;
    }
    SuspendNative(false);
    // Ask its windows to close, as the game's own close button would; a game that is still there
    // a few seconds later is ended.
    struct Close {
        DWORD pid;
    } close{GetProcessId(native_process)};
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == reinterpret_cast<Close*>(data)->pid) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&close));
    LOG_INFO(Frontend, "Notos: closing native game {} (pid {})", native_name.toStdString(),
             close.pid);
    // Out of the way while it closes: a fullscreen game keeps the screen until it lets it go,
    // and Notos' closing screen would show frozen over it.
    if (HWND game_window = NativeWindow(native_process)) {
        ShowWindowAsync(game_window, SW_MINIMIZE);
    }
    HANDLE process = native_process;
    QTimer::singleShot(5000, this, [this, process] {
        if (native_process == process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            LOG_WARNING(Frontend, "Notos: native game still running after 5 s, ending it");
            TerminateProcess(process, 0);
        }
    });
#endif
}

void Home::ToggleGameMenu() {
    if (closing) {
        return;
    }
    if (loading) {
        return;
    }
    if (overlay->isVisible()) {
        sounds.Play(QStringLiteral("select"));
        LeaveGameMenu(true);
        return;
    }
    if (!hooks.emulation_running() || QApplication::activeModalWidget()) {
        return;
    }
    // The menu shows over the game, which goes on running underneath.
    paused_by_menu = false;
    BlockGameInput(true);
    GameMenuEntries();
    sounds.Play(QStringLiteral("select"));
    PlaceOverlay();
    overlay->show();
    overlay->raise();
    overlay->activateWindow();
    overlay->setFocus();
    overlay_timer->start();
    // Windows may keep the keyboard on the game's window: every key comes to the menu while it
    // shows (and none reaches the paused game or the main window's shortcuts).
    qApp->installEventFilter(this);
}

void Home::GameMenuEntries() {
    // A settings dialog shows over the paused game; the game goes on once it closes.
    const auto dialog = [this](std::function<void()> open) {
        return [this, open] {
            const bool resume = paused_by_menu;
            menu.clear();
            overlay_timer->stop();
            overlay->hide();
            paused_by_menu = false;
            if (open) {
                open();
            }
            BlockGameInput(false);
            if (resume && hooks.emulation_running()) {
                hooks.resume();
            }
        };
    };
    std::vector<Entry> entries{
        Entry{[] { return QStringLiteral("Volver al juego"); }, [this] { LeaveGameMenu(true); },
              {}, false},
        Entry{[] { return QStringLiteral("Ajustes del emulador"); }, dialog(hooks.open_settings),
              {}, false},
        Entry{[] { return QStringLiteral("Ajustes de este juego"); }, dialog(hooks.game_config),
              {}, false},
        Entry{[this] {
                  return QStringLiteral("Pantalla completa: ") +
                         (hooks.fullscreen && hooks.fullscreen() ? QStringLiteral("Sí")
                                                                 : QStringLiteral("No"));
              },
              {},
              [this](int) {
                  if (hooks.toggle_fullscreen) {
                      hooks.toggle_fullscreen();
                      // The next games start the way the player left this one.
                      SetSetting(QStringLiteral("gamesFullscreen"), hooks.fullscreen());
                  }
              },
              false},
        Entry{[] { return QStringLiteral("Reiniciar juego"); },
              [this] {
                  OpenMenu(QStringLiteral("¿Reiniciar el juego? Lo que no hayas guardado se pierde."),
                           {Entry{[] { return QStringLiteral("No"); },
                                  [this] { GameMenuEntries(); }, {}, false},
                            Entry{[] { return QStringLiteral("Sí, reiniciar"); },
                                  [this] { RestartGame(); }, {}, false}});
              },
              {}, false},
        Entry{[] { return QStringLiteral("Cerrar juego"); },
              [this] {
                  // Asked here rather than in the emulator's own dialog, so the gamepad answers.
                  OpenMenu(QStringLiteral("¿Cerrar el juego? Lo que no hayas guardado se pierde."),
                           {Entry{[] { return QStringLiteral("No"); },
                                  [this] { GameMenuEntries(); }, {}, false},
                            Entry{[] { return QStringLiteral("Sí, cerrar"); },
                                  [this] {
                                      LeaveGameMenu(false);
                                      hooks.stop();
                                  },
                                  {}, false}});
              },
              {}, false},
    };
    // The game's name from the window title ("Notos | version | compiler | game | ...").
    const QStringList parts = window->windowTitle().split(QStringLiteral(" | "));
    menu_title = parts.size() > 3 ? parts[3] : QStringLiteral("Notos");
    menu = std::move(entries);
    menu_index = 0;
    update();
}

void Home::ApplyFullscreen() {
    if (Prefs().value(QStringLiteral("fullscreen")).toBool()) {
        window->showFullScreen();
    } else if (window->isFullScreen()) {
        window->showNormal();
    }
}

bool Home::eventFilter(QObject* watched, QEvent* event) {
    if (overlay && overlay->isVisible() && !QApplication::activeModalWidget() &&
        (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride ||
         event->type() == QEvent::KeyRelease)) {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept(); // the key goes on as a key press, to here
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            pad_last = false;
            if (key->key() == Qt::Key_Home) {
                if (!key->isAutoRepeat()) {
                    ToggleGameMenu();
                }
            } else if (const QString cmd = KeyCommand(key->key());
                       !cmd.isEmpty() && cmd != QStringLiteral("fullscreen")) {
                Nav(cmd);
                if (overlay->isVisible()) {
                    overlay->update();
                }
            }
        }
        return true;
    }
    if (event->type() == QEvent::Show && (watched == game_list || watched == placeholder)) {
        QTimer::singleShot(0, this, &Home::Restore);
    }
    if (event->type() == QEvent::KeyPress && list_mode &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        ShowHome();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// ---- drawing

void Home::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);

    const bool dark = Dark();
    const QColor bg = dark ? QColor(0x2B, 0x2B, 0x2E) : QColor(0xEC, 0xEC, 0xEC);
    const QColor panel = dark ? QColor(0x3A, 0x3A, 0x3F) : QColor(0xFF, 0xFF, 0xFF);
    const QColor text = dark ? QColor(0xF2, 0xF2, 0xF2) : QColor(0x2B, 0x2B, 0x2E);
    const QColor sub = dark ? QColor(0xA0, 0xA0, 0xA8) : QColor(0x6A, 0x6A, 0x72);
    const QColor accent = Accent();
    const double s = std::max(0.45, std::min(width() / 1920.0, height() / 1080.0)) * 1.1;
    const int w = width();
    const int h = height();
    if (loading || closing) {
        PaintLoading(p, rect());
        return;
    }
    p.fillRect(rect(), bg);

    // The opening sequence, once: the emblem appears alone in the middle of light, then flies to
    // its corner while the home screen comes in. `merge` is how far the emblem has flown, `ui`
    // how far the home screen has come in.
    if (intro_pending) {
        return; // the empty screen until Notos has started up (AnimTick)
    }
    const double it = intro_start;
    const bool intro = it >= 0;
    const bool emblem = intro && !intro_reveal_only;
    const double merge = emblem ? EaseInOut((it - IntroMerge) / IntroMergeTime) : 1.0;
    const double ui = intro ? EaseOut((it - IntroUi) / 650.0) : 1.0;

    // Top bar: the Notos badge, the clock.
    const double badge = BadgeSize * s;
    const QRectF badge_rect(48 * s, TopBar * s - badge, badge, badge);
    QRectF icon_rect = badge_rect;
    if (emblem) {
        const double appear = EaseOut(it / 800.0);
        const double alone = 1 - merge;
        const QPointF centre(w / 2.0, h / 2.0 - 36 * s);
        const double big = 200 * s * (0.72 + 0.28 * appear);

        // Light behind the emblem, breathing.
        const double breath = 0.85 + 0.15 * std::sin(it / 260.0);
        QRadialGradient glow(centre, 560 * s);
        QColor inner = accent;
        inner.setAlphaF(0.34 * appear * alone * breath);
        QColor mid = accent;
        mid.setAlphaF(0.10 * appear * alone);
        QColor outer_colour = accent;
        outer_colour.setAlphaF(0);
        glow.setColorAt(0, inner);
        glow.setColorAt(0.45, mid);
        glow.setColorAt(1, outer_colour);
        p.fillRect(rect(), glow);

        // Specks of light drifting up.
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 46; ++i) {
            const double a = std::fmod(i * 0.6180339887, 1.0);
            const double b = std::fmod(i * 0.7548776662, 1.0);
            const double c = std::fmod(i * 0.5698402910, 1.0);
            const double x = w * a + std::sin(it / 900.0 + i) * 14 * s;
            const double y = h * (1.08 - std::fmod(b + it / (9000.0 + 9000.0 * c), 1.0) * 1.16);
            const double closeness = 1 - Clamp01(std::hypot(x - centre.x(), y - centre.y()) / (w * 0.6));
            QColor speck = i % 3 == 0 ? accent : QColor(dark ? Qt::white : Qt::black);
            speck.setAlphaF(Clamp01((0.10 + 0.45 * c) * appear * alone * (0.35 + 0.65 * closeness)));
            p.setBrush(speck);
            const double r = (1.2 + 2.4 * c) * s;
            p.drawEllipse(QPointF(x, y), r, r);
        }

        // Rings leaving the emblem.
        p.setBrush(Qt::NoBrush);
        for (int ring = 0; ring < 3; ++ring) {
            const double rt = (it - 250 - ring * 300) / 1300.0;
            if (rt <= 0 || rt >= 1) {
                continue;
            }
            QColor colour = accent;
            colour.setAlphaF(0.55 * (1 - rt) * (1 - rt) * alone);
            p.setPen(QPen(colour, (3.0 - 2.0 * rt) * s));
            const double radius = big * 0.62 + EaseOut(rt) * 620 * s;
            p.drawEllipse(centre, radius, radius);
        }

        // The name under it, its letters settling together.
        const double word = EaseOut((it - 550) / 650.0) * Clamp01(1 - merge * 3);
        if (word > 0) {
            QFont name_font = Font(font(), 46 * s, QFont::Medium);
            name_font.setLetterSpacing(QFont::AbsoluteSpacing, (10 + 26 * (1 - word)) * s);
            QColor colour = text;
            colour.setAlphaF(word);
            p.setPen(colour);
            p.setFont(name_font);
            p.drawText(QRectF(0, centre.y() + 130 * s, w, 80 * s), Qt::AlignCenter,
                       QStringLiteral("Notos"));
        }

        const QRectF big_rect(centre.x() - big / 2, centre.y() - big / 2, big, big);
        icon_rect = QRectF(big_rect.left() + (badge_rect.left() - big_rect.left()) * merge,
                           big_rect.top() + (badge_rect.top() - big_rect.top()) * merge,
                           big_rect.width() + (badge_rect.width() - big_rect.width()) * merge,
                           big_rect.height() + (badge_rect.height() - big_rect.height()) * merge);
        p.setOpacity(appear);
    }
    const QPixmap icon = window->windowIcon().pixmap(256);
    p.drawPixmap(icon_rect, icon, QRectF(icon.rect()));
    p.setOpacity(merge);
    p.setPen(sub);
    p.setFont(Font(font(), 18 * s));
    p.drawText(QRectF(badge_rect.right() + 14 * s, badge_rect.top(), 400 * s, badge),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Notos"));
    // Next to the name: the Mii Editor and multiplayer, a row of their own above the covers.
    {
        const double name_w = QFontMetricsF(p.font()).horizontalAdvance(QStringLiteral("Notos"));
        const QFont pill_font = Font(font(), 15 * s, QFont::Medium);
        const int glyph = static_cast<int>(18 * s);
        const double pill_h = std::max(badge * 0.8, 30 * s);
        double x = badge_rect.right() + 14 * s + name_w + 26 * s;
        top_rects.clear();
        for (int i = 0; i < static_cast<int>(TopButtons.size()); ++i) {
            const QString label = QString::fromUtf8(TopButtons[i].label);
            const double pill_w =
                QFontMetricsF(pill_font).horizontalAdvance(label) + glyph + 40 * s;
            const QRectF pill(x, badge_rect.center().y() - pill_h / 2, pill_w, pill_h);
            x += pill_w + 10 * s;
            top_rects.push_back(pill.toAlignedRect());
            const bool on = row == -1 && top_index == i && menu.empty();
            p.setBrush(panel);
            p.setPen(on ? QPen(AccentBrush(pill), 3 * s) : QPen(Qt::NoPen));
            p.drawRoundedRect(pill.adjusted(1.5 * s, 1.5 * s, -1.5 * s, -1.5 * s), pill_h / 2,
                              pill_h / 2);
            const QPixmap icon_pixmap = FritchiiUi::Glyph(QString::fromLatin1(TopButtons[i].icon),
                                                          text.rgb() & 0xFFFFFF, glyph)
                                            .pixmap(glyph, glyph);
            p.drawPixmap(
                QRectF(pill.left() + 16 * s, pill.center().y() - glyph / 2.0, glyph, glyph),
                icon_pixmap, QRectF(icon_pixmap.rect()));
            p.setPen(text);
            p.setFont(pill_font);
            p.drawText(QRectF(pill.left() + 16 * s + glyph + 8 * s, pill.top(),
                              pill_w - glyph - 32 * s, pill_h),
                       Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }
    p.setOpacity(ui);
    const bool clock24 = Prefs().value(QStringLiteral("clock24")).toBool();
    p.setPen(text);
    const QFont clock_font = Font(font(), ClockSize * s);
    p.setFont(clock_font);
    const QString clock = QTime::currentTime().toString(clock24 ? QStringLiteral("HH:mm")
                                                                : QStringLiteral("h:mm AP"));
    const QRectF clock_rect(w / 2.0, badge_rect.top() - 20 * s, w / 2.0 - 48 * s, badge + 40 * s);
    p.drawText(clock_rect, Qt::AlignVCenter | Qt::AlignRight, clock);

    // Left of the clock: the controller in use, as the launcher showed it.
    {
        const double clock_w = QFontMetricsF(clock_font).horizontalAdvance(clock);
        const double right = clock_rect.right() - clock_w - 28 * s;
        const QFont pad_font = Font(font(), ClockSize * 0.75 * s);
        const QString pad_text = pad_name.isEmpty() ? QStringLiteral("Teclado y ratón") : pad_name;
        const double text_w = std::min(QFontMetricsF(pad_font).horizontalAdvance(pad_text),
                                       w * 0.35);
        p.setPen(sub);
        p.setFont(pad_font);
        p.drawText(QRectF(right - text_w, clock_rect.top(), text_w, clock_rect.height()),
                   Qt::AlignVCenter | Qt::AlignRight,
                   QFontMetricsF(pad_font).elidedText(pad_text, Qt::ElideRight, text_w));
        const int glyph = static_cast<int>(ClockSize * s);
        const QPixmap pad_icon =
            FritchiiUi::Glyph(QStringLiteral("gamepad"), sub.rgb() & 0xFFFFFF, glyph)
                .pixmap(glyph, glyph);
        p.drawPixmap(QRectF(right - text_w - 10 * s - glyph, clock_rect.center().y() - glyph / 2.0,
                            glyph, glyph),
                     pad_icon, QRectF(pad_icon.rect()));
    }

    // Centre: the selected game's title over the covers.
    const double tile = Tile * s;
    const double outer = tile + 2 * (TileBorder + TilePad) * s;
    const double cell = outer + 2 * TileGap * s;
    const double info_h = HintSize * 1.4 * s;
    const double title_h = TitleSize * 1.5 * s + info_h + 12 * s;
    const double label_h = HintSize * 1.5 * s;
    const double area_top = TopBar * s;
    const double area_bottom = h - (IconRow + BottomBar) * s;
    const double block = title_h + cell + label_h;
    const double top = area_top + std::max(0.0, (area_bottom - area_top - block) / 2);
    const double left = SideMargin * s;

    if (row == 0 && !games.empty()) {
        p.setPen(dark ? Qt::white : Qt::black);
        p.setFont(Font(font(), TitleSize * s, QFont::Medium));
        const QRectF title_rect(left + TileGap * s + 10 * s, top, w - 2 * left, TitleSize * 1.5 * s);
        p.drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(games[game_index].name, Qt::ElideRight,
                                              static_cast<int>(title_rect.width())));
        // Under it: "Switch · Jugado hace 2 días · 3 h 22 min".
        p.setPen(sub);
        p.setFont(Font(font(), HintSize * s));
        const QRectF info_rect(title_rect.left(), title_rect.bottom(), title_rect.width(), info_h);
        p.drawText(info_rect, Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(InfoLine(games[game_index]), Qt::ElideRight,
                                              static_cast<int>(info_rect.width())));
    }

    tile_rects.assign(games.size(), QRect());
    p.save();
    p.setClipRect(QRectF(0, area_top, w, area_bottom - area_top));
    if (focus.size() != games.size()) {
        focus.assign(games.size(), 0.0);
    }
    // Choosing a game: its cover is drawn again over the fading screen, answering.
    const double lt = launching ? static_cast<double>(anim_clock.elapsed() - launch_start) : -1;
    bool launch_pass = false;
    const auto draw_tile = [&](int i) {
        const double x = left + i * cell - scroll + TileGap * s;
        if (x + outer * 1.1 < 0 || x - outer * 0.1 > w) {
            return;
        }
        const QRectF rest(x, top + title_h + TileGap * s, outer, outer);
        tile_rects[i] = rest.toAlignedRect();
        // The selected cover grows a little, from its centre.
        const double lift = focus[i];
        const double grown = outer * 0.075 * lift / 2;
        QRectF outer_rect = rest.adjusted(-grown, -grown, grown, grown);
        double tile_opacity = ui;
        if (launch_pass) {
            // Pressed like a button, then it springs out a little larger and settles.
            double scale = 1;
            if (lt < 90) {
                scale = 1 - 0.06 * EaseOut(lt / 90.0);
            } else if (lt < 300) {
                scale = 0.94 + 0.13 * EaseOut((lt - 90) / 210.0);
            } else {
                scale = 1.07 - 0.03 * EaseInOut((lt - 300) / 180.0);
            }
            const double more = outer_rect.width() * (scale - 1) / 2;
            outer_rect.adjust(-more, -more, more, more);
            tile_opacity = 1 - Clamp01((lt - (LaunchTime - 130)) / 110.0); // into the loading screen
        }
        if (intro) {
            // The covers come in from the right, one after the other.
            const double order = std::max(0.0, (x - left) / cell);
            const double in = EaseOut((it - IntroUi - order * 85) / 600.0);
            outer_rect.translate((1 - in) * 180 * s, 0);
            const double shrink = outer_rect.width() * 0.12 * (1 - in) / 2; // and grow into place
            outer_rect.adjust(shrink, shrink, -shrink, -shrink);
            tile_opacity = in;
        }
        p.setOpacity(tile_opacity);
        const double k = outer_rect.width() / outer; // everything inside scales with it
        if (lift > 0.01) {
            // A soft shadow lifts it off the row.
            p.setPen(Qt::NoPen);
            for (int layer = 7; layer >= 1; --layer) {
                const double spread = layer * 3 * s * lift;
                p.setBrush(QColor(0, 0, 0, static_cast<int>((dark ? 13 : 8) * lift)));
                p.drawRoundedRect(outer_rect.adjusted(-spread, -spread + 4 * s * lift, spread,
                                                      spread + 6 * s * lift),
                                  8 * s, 8 * s);
            }
            p.setOpacity(tile_opacity * Clamp01(lift * 1.4));
            p.setPen(QPen(AccentBrush(outer_rect), TileBorder * s * k));
            p.setBrush(Qt::NoBrush);
            const double half = TileBorder * s * k / 2;
            p.drawRoundedRect(outer_rect.adjusted(half, half, -half, -half), 4 * s, 4 * s);
            p.setOpacity(tile_opacity);
        }
        const double inset = (TileBorder + TilePad) * s * k;
        const QRectF face = outer_rect.adjusted(inset, inset, -inset, -inset);
        const Game& game = games[i];
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(face, 2 * s, 2 * s);
        p.setClipPath(clip, Qt::IntersectClip);
        if (!game.cover.isNull()) {
            // Fill the square (crop the longer side), like the launcher's covers.
            const QSizeF size = game.cover.size();
            const double scale = std::max(face.width() / size.width(), face.height() / size.height());
            const QSizeF drawn = size * scale;
            p.drawPixmap(QRectF(face.center().x() - drawn.width() / 2,
                                face.center().y() - drawn.height() / 2, drawn.width(),
                                drawn.height()),
                         game.cover, QRectF(QPointF(0, 0), size));
        } else {
            int hash = 0;
            for (const QChar c : game.name) {
                hash = hash * 31 + c.unicode();
            }
            const double hue = std::abs(hash % 360);
            QLinearGradient gradient(face.topLeft(), face.bottomRight());
            gradient.setColorAt(0, Hsv(hue, 0.55, 0.75));
            gradient.setColorAt(1, Hsv(hue + 40, 0.65, 0.40));
            p.fillRect(face, gradient);
            p.setPen(Qt::white);
            p.setFont(Font(font(), tile / 11.0, QFont::DemiBold));
            // Whole words, unless one is wider than the cover (file names like "dk-040074...").
            const QRectF box = face.adjusted(tile / 16, tile / 16, -tile / 16, -tile / 16);
            bool long_word = false;
            for (const QString& word : game.name.split(QLatin1Char(' '))) {
                long_word = long_word || p.fontMetrics().horizontalAdvance(word) > box.width();
            }
            p.drawText(box,
                       Qt::AlignCenter | (long_word ? Qt::TextWrapAnywhere : Qt::TextWordWrap),
                       game.name);
        }
        if (launch_pass) {
            // A shine crosses the cover.
            const double sweep = (lt - 100) / 320.0;
            if (sweep > 0 && sweep < 1) {
                const double band = face.width() * 0.55;
                const double x0 = face.left() - band + (face.width() + band) * EaseInOut(sweep);
                QLinearGradient shine(QPointF(x0, face.top()),
                                      QPointF(x0 + band, face.top() + band * 0.45));
                shine.setColorAt(0, QColor(255, 255, 255, 0));
                shine.setColorAt(0.5, QColor(255, 255, 255,
                                             static_cast<int>(150 * std::sin(sweep * 3.14159))));
                shine.setColorAt(1, QColor(255, 255, 255, 0));
                p.fillRect(face, shine);
            }
        }
        // The platform, in a small badge in the cover's corner.
        {
            p.setFont(Font(font(), tile / 19.0 * k, QFont::Bold));
            const QString platform = PlatformOf(game);
            const QFontMetricsF metrics(p.font());
            const double pad_x = metrics.height() * 0.45;
            const QRectF badge(face.left() + tile / 28 * k, face.bottom() - tile / 28 * k -
                                   metrics.height() * 1.25,
                               metrics.horizontalAdvance(platform) + 2 * pad_x,
                               metrics.height() * 1.25);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 165));
            p.drawRoundedRect(badge, badge.height() / 2, badge.height() / 2);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, platform);
        }
        p.restore();
        if (launch_pass) {
            // Its border leaves it, as a ring of light.
            const double rt = (lt - 130) / 340.0;
            if (rt > 0 && rt < 1) {
                const double grow = EaseOut(rt) * 46 * s;
                p.setOpacity(tile_opacity * 0.75 * (1 - rt) * (1 - rt));
                p.setPen(QPen(AccentBrush(outer_rect), (4.5 - 3.0 * rt) * s));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(outer_rect.adjusted(-grow, -grow, grow, grow), 4 * s + grow,
                                  4 * s + grow);
                p.setOpacity(tile_opacity);
            }
            return;
        }
        // Under the cover: a clock and the time played (nothing for a game never played).
        if (const QString text = PlayTimeText(PlayTimeOf(game)); !text.isEmpty()) {
            p.save();
            p.setFont(Font(font(), HintSize * s, QFont::Medium));
            const QFontMetricsF metrics(p.font());
            const double icon = metrics.ascent() * 0.95;
            const double gap = icon * 0.45;
            const double width = icon + gap + metrics.horizontalAdvance(text);
            const double top = outer_rect.bottom() + TileGap * s;
            const double left = outer_rect.center().x() - width / 2;
            const QRectF dial(left, top + (metrics.height() - icon) / 2, icon, icon);
            p.setRenderHint(QPainter::Antialiasing);
            QPen pen(sub, std::max(1.0, icon * 0.11));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(dial.adjusted(pen.widthF() / 2, pen.widthF() / 2, -pen.widthF() / 2,
                                        -pen.widthF() / 2));
            const QPointF centre = dial.center();
            p.drawLine(centre, centre + QPointF(0, -icon * 0.30));
            p.drawLine(centre, centre + QPointF(icon * 0.22, 0));
            p.drawText(QRectF(left + icon + gap, top, width, label_h),
                       Qt::AlignLeft | Qt::AlignTop, text);
            p.restore();
        }
    };
    // The selected cover last: it grows over its neighbours.
    const int selected = row == 0 && !games.empty() ? game_index : -1;
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if (i != selected) {
            draw_tile(i);
        }
    }
    if (selected >= 0) {
        draw_tile(selected);
    }
    p.restore();
    p.setOpacity(ui);
    if (games.empty()) {
        p.setPen(sub);
        p.setFont(Font(font(), 22 * s));
        p.drawText(QRectF(left + 16 * s, top, w - 2 * left, block), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Agrega tus juegos con el botón +"));
    }

    // Round buttons.
    const double icon_d = IconSize * s;
    const double button_w = icon_d + 82 * s;
    const double buttons_x = (w - button_w * Buttons.size()) / 2;
    const double buttons_y = area_bottom;
    button_rects.assign(Buttons.size(), QRect());
    for (int i = 0; i < static_cast<int>(Buttons.size()); ++i) {
        const bool on = row == 1 && i == button_index;
        // At the opening they are revealed one after the other: each fades in, rises and pops to
        // its size.
        double reveal = 1, pop = 1;
        if (intro) {
            const double x = Clamp01((it - IntroUi - 220 - i * 130) / 520.0);
            reveal = EaseOut(x);
            pop = 1 + 2.70158 * std::pow(x - 1, 3) + 1.70158 * std::pow(x - 1, 2); // overshoots
        }
        p.setOpacity(reveal);
        const double d = icon_d * (0.55 + 0.45 * pop);
        const QRectF circle(buttons_x + i * button_w + (button_w - d) / 2,
                            buttons_y + (icon_d - d) / 2 + (1 - reveal) * 34 * s, d, d);
        button_rects[i] = QRectF(buttons_x + i * button_w, buttons_y, button_w, icon_d + 34 * s)
                              .toAlignedRect();
        p.setBrush(panel);
        p.setPen(on ? QPen(AccentBrush(circle), 4 * s) : QPen(Qt::NoPen));
        p.drawEllipse(circle.adjusted(2 * s, 2 * s, -2 * s, -2 * s));
        const int glyph = static_cast<int>(icon_d * 0.42);
        const QPixmap pixmap = FritchiiUi::Glyph(QString::fromLatin1(Buttons[i].icon),
                                                 text.rgb() & 0xFFFFFF, glyph)
                                   .pixmap(glyph, glyph);
        const double drawn = glyph * d / icon_d;
        p.drawPixmap(QRectF(circle.center().x() - drawn / 2.0, circle.center().y() - drawn / 2.0,
                            drawn, drawn),
                     pixmap, QRectF(pixmap.rect()));
        if (on) {
            p.setPen(accent);
            p.setFont(Font(font(), 16 * s));
            p.drawText(QRectF(circle.left() - 60 * s, circle.bottom() + 6 * s,
                              circle.width() + 120 * s, 28 * s),
                       Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(Buttons[i].label));
        }
    }

    p.setOpacity(ui);
    // Bottom bar: the keys and buttons that do something here.
    const double bar_y = h - BottomBar * s;
    p.setPen(QPen(sub, 1));
    p.drawLine(QPointF(36 * s, bar_y), QPointF(w - 36 * s, bar_y));
    std::vector<Hint> hints;
    const Hint accept{QStringLiteral("Enter"), QStringLiteral("✕"), QStringLiteral("Aceptar")};
    if (!menu.empty()) {
        hints = {accept, {QStringLiteral("Esc"), QStringLiteral("○"), QStringLiteral("Volver")}};
    } else if (row == 0) {
        hints = {{QStringLiteral("Enter"), QStringLiteral("✕"), QStringLiteral("Jugar")},
                 {QStringLiteral("Tab"), QStringLiteral("△"), QStringLiteral("Opciones")}};
    } else {
        hints = {accept};
    }
    if (!pad_last && menu.empty()) {
        hints.push_back({QStringLiteral("F11"), {}, QStringLiteral("Pantalla completa")});
    }
    PaintHints(p, hints, QRectF(36 * s, bar_y, w - 88 * s, BottomBar * s), Qt::AlignRight,
               HintSize * 0.8 * s, text);

    p.setOpacity(1);
    if (launching) {
        // Everything fades to the loading screen's ground; the chosen cover stays over it.
        QColor veil = bg;
        veil.setAlphaF(EaseOut(lt / 260.0));
        p.fillRect(rect(), veil);
        if (launch_index >= 0 && launch_index < static_cast<int>(games.size())) {
            launch_pass = true;
            draw_tile(launch_index);
            launch_pass = false;
        }
        p.setOpacity(1);
    }
    PaintMenu(p, rect());
}

void Home::PaintMenu(QPainter& p, const QRect& area) {
    // A menu over everything, in the centre: a card that comes in, a title with its mark, and a
    // highlight that glides between the entries.
    menu_rects.clear();
    menu_box = {};
    if (menu.empty()) {
        return;
    }
    const bool dark = Dark();
    const QColor panel = dark ? QColor(0x30, 0x30, 0x35) : QColor(0xFB, 0xFB, 0xFC);
    const QColor text = dark ? QColor(0xF2, 0xF2, 0xF2) : QColor(0x2B, 0x2B, 0x2E);
    const QColor sub = dark ? QColor(0xA0, 0xA0, 0xA8) : QColor(0x6A, 0x6A, 0x72);
    const QColor accent = Accent();
    const double s =
        std::max(0.45, std::min(area.width() / 1920.0, area.height() / 1080.0)) * 1.1;
    const int w = area.width();
    const int h = area.height();
    const double show = EaseOut(menu_show);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(area, QColor(0, 0, 0, static_cast<int>(165 * show)));

    const double menu_w = std::min(MenuWidth * s, w - 32.0);
    const double pad = 26 * s;
    const double row_gap = 6 * s;
    const QFont item_font = Font(font(), MenuFont * s);
    const QFont title_font = Font(font(), MenuFont * 1.25 * s, QFont::DemiBold);
    const double item_h = QFontMetricsF(item_font).height() + MenuFont * 1.25 * s;
    const double title_w = menu_w - 2 * pad - 22 * s;
    const double title_h = QFontMetricsF(title_font)
                               .boundingRect(QRectF(0, 0, title_w, 10000), Qt::TextWordWrap,
                                             menu_title)
                               .height();
    const double header_h = title_h + 34 * s; // the title, the line under it and air
    const int fit =
        std::max(1, static_cast<int>((h - 260 * s - header_h - 2 * pad) / (item_h + row_gap)));
    const int count = static_cast<int>(menu.size());
    const int visible = std::min(count, fit);
    const int first = std::clamp(menu_index - visible / 2, 0, count - visible);
    const double box_h = 2 * pad + header_h + visible * (item_h + row_gap) - row_gap;
    const QRectF box((w - menu_w) / 2, (h - box_h) / 2, menu_w, box_h);
    menu_box = box.toAlignedRect();

    // The card grows into place.
    p.setOpacity(show);
    p.translate(box.center().x(), box.center().y() + (1 - show) * 18 * s);
    p.scale(0.94 + 0.06 * show, 0.94 + 0.06 * show);
    p.translate(-box.center().x(), -box.center().y());

    p.setPen(Qt::NoPen);
    for (int layer = 8; layer >= 1; --layer) {
        const double spread = layer * 4 * s;
        p.setBrush(QColor(0, 0, 0, 9));
        p.drawRoundedRect(box.adjusted(-spread, -spread + 6 * s, spread, spread + 10 * s),
                          16 * s + spread, 16 * s + spread);
    }
    p.setBrush(panel);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 22), 1));
    p.drawRoundedRect(box, 16 * s, 16 * s);

    // Title: a mark in the accent colour, the text, a thin line.
    const QRectF title_rect(box.left() + pad + 22 * s, box.top() + pad, title_w, title_h);
    p.setPen(Qt::NoPen);
    const QRectF mark(box.left() + pad + 2 * s, title_rect.top() + 3 * s, 6 * s,
                      title_h - 6 * s);
    p.setBrush(AccentBrush(mark.adjusted(-40 * s, 0, 40 * s, 0)));
    p.drawRoundedRect(mark, 3 * s, 3 * s);
    p.setPen(text);
    p.setFont(title_font);
    p.drawText(title_rect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, menu_title);
    QColor line = sub;
    line.setAlpha(60);
    p.setPen(QPen(line, 1));
    const double line_y = title_rect.bottom() + 17 * s;
    p.drawLine(QPointF(box.left() + pad, line_y), QPointF(box.right() - pad, line_y));

    const double items_top = box.top() + pad + header_h;
    const auto item_rect = [&](double position) {
        return QRectF(box.left() + pad, items_top + position * (item_h + row_gap),
                      menu_w - 2 * pad, item_h);
    };

    // The highlight, where the selection is gliding to.
    p.save();
    p.setClipRect(QRectF(box.left(), items_top - 6 * s, menu_w,
                         visible * (item_h + row_gap) + 6 * s));
    const QRectF glow = item_rect(menu_cursor - first);
    QColor halo = accent;
    halo.setAlpha(55);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawRoundedRect(glow.adjusted(-4 * s, -4 * s, 4 * s, 4 * s), 14 * s, 14 * s);
    QLinearGradient fill(glow.topLeft(), glow.topRight());
    fill.setColorAt(0, accent);
    fill.setColorAt(1, Rainbow() ? Hsv(accent.hsvHueF() * 360 + 55, 0.62, 1.0)
                                 : accent.lighter(118));
    p.setBrush(fill);
    p.drawRoundedRect(glow, 10 * s, 10 * s);
    p.restore();

    menu_rects.assign(menu.size(), QRect());
    p.setFont(item_font);
    const QFontMetricsF metrics(item_font);
    // On a light highlight (yellow, the rainbow passing through green) the text goes dark.
    const double light =
        (0.299 * accent.redF() + 0.587 * accent.greenF() + 0.114 * accent.blueF());
    const QColor on_text = light > 0.62 ? QColor(0x1E, 0x1E, 0x22) : QColor(Qt::white);
    for (int i = first; i < first + visible; ++i) {
        const QRectF item = item_rect(i - first);
        menu_rects[i] = item.toAlignedRect();
        const bool on = i == menu_index;
        const QRectF inner = item.adjusted(18 * s, 0, -18 * s, 0);
        QString label = menu[i].label();
        QString value;
        // "Tema: Oscuro" is a setting: its value goes to the right, with arrows when selected.
        if (const int colon = label.indexOf(QStringLiteral(": ")); menu[i].adjust && colon > 0) {
            value = label.mid(colon + 2);
            label = label.left(colon);
        }
        double value_w = 0;
        if (!value.isEmpty()) {
            const QString shown =
                on ? QString(QStringLiteral("‹  ") + value + QStringLiteral("  ›")) : value;
            value_w = std::min(metrics.horizontalAdvance(shown), inner.width() * 0.6);
            p.setPen(on ? on_text : sub);
            p.drawText(QRectF(inner.right() - value_w, inner.top(), value_w, inner.height()),
                       Qt::AlignVCenter | Qt::AlignRight,
                       metrics.elidedText(shown, Qt::ElideRight, value_w));
            value_w += 16 * s;
        }
        p.setPen(on ? on_text : text);
        p.drawText(QRectF(inner.left(), inner.top(), inner.width() - value_w, inner.height()),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   metrics.elidedText(label, Qt::ElideRight, inner.width() - value_w));
    }

    // More entries than fit: a small bar says where we are.
    if (visible < count) {
        const double track_h = visible * (item_h + row_gap) - row_gap;
        const double thumb_h = track_h * visible / count;
        p.setPen(Qt::NoPen);
        p.setBrush(line);
        p.drawRoundedRect(QRectF(box.right() - pad / 2 - 2 * s,
                                 items_top + (track_h - thumb_h) * first / (count - visible),
                                 4 * s, thumb_h),
                          2 * s, 2 * s);
    }

    // Under the card: what the buttons do here.
    std::vector<Hint> hints;
    if (menu[menu_index].adjust) {
        hints.push_back({QStringLiteral("◀ ▶"), QStringLiteral("◀ ▶"), QStringLiteral("Cambiar")});
    }
    hints.push_back({QStringLiteral("Enter"), QStringLiteral("✕"), QStringLiteral("Aceptar")});
    hints.push_back({QStringLiteral("Esc"), QStringLiteral("○"), QStringLiteral("Volver")});
    PaintHints(p, hints, QRectF(box.left() - 200 * s, box.bottom() + 16 * s, menu_w + 400 * s, 34 * s),
               Qt::AlignHCenter, HintSize * 0.72 * s, QColor(255, 255, 255, 200));
    p.restore();
}

void Home::PaintHints(QPainter& p, const std::vector<Hint>& hints, const QRectF& area,
                      Qt::Alignment align, double px, const QColor& colour) const {
    const QFont label_font = Font(font(), px);
    const QFont key_font = Font(font(), px * 0.78, QFont::DemiBold);
    const QFont glyph_font = Font(font(), px * 0.86, QFont::DemiBold);
    const QFontMetricsF label_metrics(label_font);
    const double cap_h = px * 1.55;
    const double inner_gap = px * 0.45;
    const double outer_gap = px * 1.6;

    struct Laid {
        QString cap;
        bool round;
        double cap_w;
        QString label;
        double label_w;
    };
    std::vector<Laid> laid;
    double total = 0;
    for (const Hint& hint : hints) {
        const QString cap = pad_last ? hint.pad : hint.key;
        if (cap.isEmpty()) {
            continue;
        }
        // A gamepad button is a circle (a pill for the arrows); a key is a key cap.
        const bool round = pad_last;
        const QFontMetricsF cap_metrics(round ? glyph_font : key_font);
        const double cap_w = std::max(cap_h, cap_metrics.horizontalAdvance(cap) + px * (round ? 0.8 : 0.9));
        const double label_w = label_metrics.horizontalAdvance(hint.label);
        laid.push_back({cap, round, cap_w, hint.label, label_w});
        total += (laid.empty() ? 0 : 0) + cap_w + inner_gap + label_w;
    }
    if (laid.empty()) {
        return;
    }
    total += outer_gap * (laid.size() - 1);
    double x = area.left();
    if (align & Qt::AlignRight) {
        x = area.right() - total;
    } else if (align & Qt::AlignHCenter) {
        x = area.center().x() - total / 2;
    }
    const double mid = area.center().y();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const Laid& item : laid) {
        const QRectF cap(x, mid - cap_h / 2, item.cap_w, cap_h);
        QColor ring = colour;
        ring.setAlphaF(colour.alphaF() * 0.75);
        QColor fill = colour;
        fill.setAlphaF(colour.alphaF() * 0.10);
        p.setPen(QPen(ring, std::max(1.0, px * 0.08)));
        p.setBrush(fill);
        const double radius = item.round ? cap_h / 2 : px * 0.28;
        p.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        p.setPen(colour);
        p.setFont(item.round ? glyph_font : key_font);
        p.drawText(cap, Qt::AlignCenter, item.cap);
        x += item.cap_w + inner_gap;
        p.setFont(label_font);
        p.drawText(QRectF(x, area.top(), item.label_w + 2, area.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, item.label);
        x += item.label_w + outer_gap;
    }
    p.restore();
}

// ---- input

bool Home::event(QEvent* event) {
    // Arrows, Enter, Esc, Tab and F11 are the home screen's own while it shows: the main window's
    // shortcuts on them (Esc leaves fullscreen, F11 the game's fullscreen) don't take them.
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const int k = key->key();
        const bool ours = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier &&
                          (k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up ||
                           k == Qt::Key_Down || k == Qt::Key_Return || k == Qt::Key_Enter ||
                           k == Qt::Key_Space || k == Qt::Key_Escape || k == Qt::Key_Backspace ||
                           k == Qt::Key_Tab || k == Qt::Key_Menu || k == Qt::Key_F11);
        if (ours) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
            } else {
                keyPressEvent(key);
            }
            return true;
        }
    }
    return QWidget::event(event);
}

QString Home::KeyCommand(int key) const {
    switch (key) {
    case Qt::Key_Left:
        return QStringLiteral("left");
    case Qt::Key_Right:
        return QStringLiteral("right");
    case Qt::Key_Up:
        return QStringLiteral("up");
    case Qt::Key_Down:
        return QStringLiteral("down");
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        return QStringLiteral("accept");
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        return QStringLiteral("back");
    case Qt::Key_Tab:
    case Qt::Key_Menu:
        return QStringLiteral("options");
    case Qt::Key_F11:
        return QStringLiteral("fullscreen");
    default:
        return {};
    }
}

void Home::keyPressEvent(QKeyEvent* event) {
    pad_last = false;
    const QString cmd = KeyCommand(event->key());
    if (cmd.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
    Nav(cmd);
}

void Home::Nav(const QString& cmd) {
    if (closing || launching) {
        return;
    }
    if (intro_start >= 0 && !intro_reveal_only) {
        // Any press skips the opening.
        sounds.Stop(QStringLiteral("startup"));
        intro_start = -1;
        intro_reveal_only = false;
        update();
        return;
    }
    if (loading) {
        // Only cancelling while a game loads (not once it shows).
        if (cmd != QStringLiteral("back") || (!loading_emulated && native_window)) {
            return;
        }
        if (loading_emulated) {
            EndLoading();
            hooks.stop();
            return;
        }
        EndLoading();
#ifdef _WIN32
        if (native_process) {
            native_dir.clear(); // no other program of the game is waited for
            TerminateProcess(native_process, 1); // nothing to save yet
        } else if (!native_dir.isEmpty()) {
            NativeEnded(); // Steam hadn't started it yet
        }
#endif
        return;
    }
    if (cmd == QStringLiteral("fullscreen")) {
        SetSetting(QStringLiteral("fullscreen"),
                   !Prefs().value(QStringLiteral("fullscreen")).toBool());
        ApplyFullscreen();
        return;
    }
    if (!menu.empty()) {
        const int count = static_cast<int>(menu.size());
        Entry& at = menu[menu_index];
        if (cmd == QStringLiteral("up")) {
            menu_index = (menu_index + count - 1) % count;
            sounds.Play(QStringLiteral("move"));
        } else if (cmd == QStringLiteral("down")) {
            menu_index = (menu_index + 1) % count;
            sounds.Play(QStringLiteral("move"));
        } else if (cmd == QStringLiteral("back")) {
            CloseMenu();
            return;
        } else if ((cmd == QStringLiteral("left") || cmd == QStringLiteral("right")) && at.adjust) {
            at.adjust(cmd == QStringLiteral("left") ? -1 : 1);
            sounds.Play(QStringLiteral("move"));
        } else if (cmd == QStringLiteral("accept")) {
            if (at.adjust && !at.act) {
                at.adjust(1);
                sounds.Play(QStringLiteral("select"));
            } else {
                RunEntry(menu_index);
                return;
            }
        }
        update();
        return;
    }
    const int was_row = row, was_game = game_index, was_button = button_index,
              was_top = top_index;
    if (cmd == QStringLiteral("left") || cmd == QStringLiteral("right")) {
        const int d = cmd == QStringLiteral("left") ? -1 : 1;
        if (row == 0) {
            game_index += d;
        } else if (row == 1) {
            button_index += d;
        } else {
            top_index = std::clamp(top_index + d, 0, static_cast<int>(TopButtons.size()) - 1);
        }
    } else if (cmd == QStringLiteral("up")) {
        row = row == 1 && !games.empty() ? 0 : -1;
    } else if (cmd == QStringLiteral("down")) {
        row = row == -1 && !games.empty() ? 0 : 1;
    } else if (cmd == QStringLiteral("accept")) {
        if (row == -1) {
            if (top_index == 0) {
                sounds.Play(QStringLiteral("select"));
                if (hooks.mii_editor) {
                    hooks.mii_editor();
                }
            } else {
                MultiplayerMenu();
            }
        } else if (row == 0 && !games.empty()) {
            BeginLaunch(games[game_index]);
        } else if (row == 1) {
            switch (button_index) {
            case 0:
                AddMenu();
                break;
            case 1:
                SettingsMenu();
                break;
            default:
                window->close();
                break;
            }
        }
        return;
    } else if (cmd == QStringLiteral("options") && row == 0 && !games.empty()) {
        GameMenu();
        return;
    }
    Select();
    if (row != was_row || game_index != was_game || button_index != was_button ||
        top_index != was_top) {
        sounds.Play(QStringLiteral("move"));
    }
}

void Home::Select() {
    if (games.empty() && row == 0) {
        row = 1;
    }
    game_index = std::clamp(game_index, 0, std::max(0, static_cast<int>(games.size()) - 1));
    button_index = std::clamp(button_index, 0, static_cast<int>(Buttons.size()) - 1);
    // Keep the selected cover in view, with a little of its neighbours.
    const double s = std::max(0.45, std::min(width() / 1920.0, height() / 1080.0)) * 1.1;
    const double cell = (Tile + 2 * (TileBorder + TilePad) + 2 * TileGap) * s;
    const double view = width() - 2 * SideMargin * s;
    const double x = game_index * cell;
    const double margin = std::min(80 * s, std::max(0.0, (view - cell) / 2));
    if (x - scroll_target < margin) {
        scroll_target = x - margin;
    } else if (x + cell - scroll_target > view - margin) {
        scroll_target = x + cell - view + margin;
    }
    const double max_scroll = std::max(0.0, games.size() * cell - view);
    scroll_target = std::clamp(scroll_target, 0.0, max_scroll);
    update();
}

void Home::MenuMouse(const QPoint& pos, Qt::MouseButton button) {
    for (int i = 0; i < static_cast<int>(menu_rects.size()); ++i) {
        if (!menu_rects[i].contains(pos)) {
            continue;
        }
        menu_index = i;
        if (button == Qt::RightButton) {
            if (menu[i].adjust) {
                menu[i].adjust(-1);
                sounds.Play(QStringLiteral("move"));
            }
        } else if (menu[i].adjust && !menu[i].act) {
            menu[i].adjust(1);
            sounds.Play(QStringLiteral("select"));
        } else {
            RunEntry(i);
            return;
        }
        update();
        return;
    }
    if (!menu_box.contains(pos)) {
        CloseMenu();
    }
}

void Home::mousePressEvent(QMouseEvent* event) {
    pad_last = false;
    if (loading || launching) {
        return;
    }
    const QPoint pos = event->position().toPoint();
    if (!menu.empty()) {
        MenuMouse(pos, event->button());
        return;
    }
    for (int i = 0; i < static_cast<int>(top_rects.size()); ++i) {
        if (top_rects[i].contains(pos) && event->button() == Qt::LeftButton) {
            row = -1;
            top_index = i;
            Nav(QStringLiteral("accept"));
            return;
        }
    }
    for (int i = 0; i < static_cast<int>(tile_rects.size()); ++i) {
        if (!tile_rects[i].contains(pos)) {
            continue;
        }
        const bool again = row == 0 && game_index == i;
        row = 0;
        game_index = i;
        Select();
        if (!again) {
            sounds.Play(QStringLiteral("move"));
        }
        if (event->button() == Qt::RightButton) {
            GameMenu();
        } else if (again) {
            BeginLaunch(games[i]);
        }
        return;
    }
    for (int i = 0; i < static_cast<int>(button_rects.size()); ++i) {
        if (button_rects[i].contains(pos) && event->button() == Qt::LeftButton) {
            row = 1;
            button_index = i;
            Select();
            Nav(QStringLiteral("accept"));
            return;
        }
    }
}

void Home::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (!loading && !launching && menu.empty() && event->button() == Qt::LeftButton) {
        for (int i = 0; i < static_cast<int>(tile_rects.size()); ++i) {
            if (tile_rects[i].contains(pos)) {
                game_index = i;
                BeginLaunch(games[i]);
                return;
            }
        }
    }
    mousePressEvent(event);
}

void Home::wheelEvent(QWheelEvent* event) {
    if (loading || launching || !menu.empty() || games.empty()) {
        return;
    }
    const int steps = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    if (steps == 0) {
        return;
    }
    row = 0;
    const int was = game_index;
    game_index += steps > 0 ? -1 : 1;
    Select();
    if (game_index != was) {
        sounds.Play(QStringLiteral("move"));
    }
}

void Home::PollPad() {
    Core::HID::EmulatedController* controller = hooks.controller ? hooks.controller() : nullptr;
    if (!controller) {
        return;
    }
    const u64 buttons = static_cast<u64>(controller->GetNpadButtons().raw);
    const u64 pressed = buttons & ~pad_held;
    if (pressed != 0 && !pad_last) {
        pad_last = true; // the hints show the gamepad's buttons
        update();
        overlay->update();
    }
    pad_held = buttons;
    if (unblock_on_release && buttons == 0 && controller->GetHomeButtons().home.Value() == 0) {
        unblock_on_release = false;
        Core::HID::EmulatedController::SetGameInputBlocked(false);
    }

    // Home pressed and let go on its own opens or closes the Home menu during a game; Home with
    // another button is one of the emulator's hotkeys (Home+B fullscreen...) and does not.
    const bool home = controller->GetHomeButtons().home.Value() != 0;
    if (home && !home_down) {
        home_down = true;
        home_combo = false;
    } else if (home && pressed != 0) {
        home_combo = true;
    } else if (!home && home_down) {
        home_down = false;
        if (!home_combo && !QApplication::activeModalWidget()) {
            if (hooks.emulation_running()) {
                ToggleGameMenu();
                return;
            }
            if (native_process) {
                ToggleNativeMenu();
                return;
            }
        }
    }

    const bool listening = ((isVisible() && window->isActiveWindow()) || overlay->isVisible()) &&
                           !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    if (!listening) {
        pad_repeat_cmd.clear();
        return;
    }
    const auto direction = [](u64 bits) -> QString {
        if (bits & PadLeft) {
            return QStringLiteral("left");
        }
        if (bits & PadRight) {
            return QStringLiteral("right");
        }
        if (bits & PadUp) {
            return QStringLiteral("up");
        }
        if (bits & PadDown) {
            return QStringLiteral("down");
        }
        return {};
    };
    if (const QString cmd = direction(pressed); !cmd.isEmpty()) {
        pad_repeat_cmd = cmd;
        pad_repeat.restart();
        pad_repeat_delay = 350;
        Nav(cmd);
    } else if (!pad_repeat_cmd.isEmpty()) {
        if (direction(buttons) != pad_repeat_cmd) {
            pad_repeat_cmd.clear();
        } else if (pad_repeat.elapsed() >= pad_repeat_delay) {
            pad_repeat.restart();
            pad_repeat_delay = 120;
            Nav(pad_repeat_cmd);
        }
    }
    if (pressed & PadB) {
        Nav(QStringLiteral("accept"));
    } else if (pressed & PadA) {
        Nav(QStringLiteral("back"));
    } else if (pressed & PadX) {
        Nav(QStringLiteral("options"));
    }
}

// ---- launching

void Home::BeginLaunch(const Game& game) {
    if (loading || launching) {
        return;
    }
    launch_index = -1;
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if (games[i].native == game.native &&
            (game.native ? games[i].exe == game.exe : games[i].path == game.path)) {
            launch_index = i;
        }
    }
    if (launch_index < 0 || !isVisible()) {
        Launch(game); // not on screen: nothing to animate
        return;
    }
    launch_game = game;
    launching = true;
    launch_start = anim_clock.elapsed();
    sounds.Play(QStringLiteral("select"));
    update();
    QTimer::singleShot(LaunchTime, this, [this] {
        if (!launching) {
            return;
        }
        // Still "launching" while the game starts: the screen stays faded until the loading
        // screen takes over (or comes back, if the game could not start).
        const Game game = launch_game;
        Launch(game);
        launching = false;
        launch_index = -1;
        launch_game = {};
        update();
    });
}

void Home::Launch(const Game& game) {
    if (loading) {
        return;
    }
    sounds.Play(QStringLiteral("loading"));
    {
        QJsonObject played = config.value(QStringLiteral("lastPlayed")).toObject();
        played.insert(game.native ? game.exe : game.path, QDateTime::currentSecsSinceEpoch());
        config.insert(QStringLiteral("lastPlayed"), played);
        SaveConfig();
        // `game` is in the list being sorted: the covers move once this returns.
        QTimer::singleShot(0, this, &Home::Refresh);
    }
    if (game.native) {
        LaunchNative(game);
        return;
    }
    if (hooks.set_game_fullscreen) {
        hooks.set_game_fullscreen(GamesFullscreen());
    }
    // OnEmulationStarting shows the loading screen for it (the boot may also fail first).
    running_game = game;
    loading_game = game;
    hooks.play(game.path, game.program_id);
    if (!loading) {
        loading_game = {};
    }
}

void Home::LaunchNative(const Game& game) {
#ifdef _WIN32
    if (native_process || !native_dir.isEmpty()) {
        // The game already running: back to it. Another one: it has to close first.
        if (game.exe != native_exe) {
            Message(QStringLiteral("%1 sigue abierto. Ciérralo desde su menú Home antes de abrir "
                                   "otro juego.")
                        .arg(native_name));
            return;
        }
        if (!native_process) {
            return; // still starting
        }
        if (HWND game_window = NativeWindow(native_process)) {
            ShowWindowAsync(game_window, IsIconic(game_window) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(game_window);
        }
        window->showMinimized();
        return;
    }
    if (!game.dir.isEmpty()) {
        // A store's game: the store starts it (its overlay, cloud saves and DRM with it), then
        // Notos finds it running from its folder.
        if (!QFileInfo(game.dir).isDir()) {
            Message(QStringLiteral("No se encontró %1").arg(QDir::toNativeSeparators(game.dir)));
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                                                    game.exe.toStdWString().c_str(), nullptr,
                                                    nullptr, SW_SHOWNORMAL)) <= 32) {
            Message(QStringLiteral("No se pudo abrir %1.").arg(game.name));
            return;
        }
        native_process = nullptr;
        native_dir = QDir::cleanPath(QFileInfo(game.dir).absoluteFilePath());
        native_wait.start();
    } else {
        const QFileInfo info(game.exe);
        if (!info.isFile()) {
            Message(QStringLiteral("No se encontró %1").arg(QDir::toNativeSeparators(game.exe)));
            return;
        }
        std::wstring exe = QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
        std::wstring dir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
        std::wstring command = L"\"" + exe + L"\"";
        if (!game.args.isEmpty()) {
            command += L" " + game.args.toStdWString();
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        // A console program starts without its console window unless the player wants it.
        const DWORD flags = game.show_console ? 0 : CREATE_NO_WINDOW;
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                            dir.c_str(), &startup, &process)) {
            Message(QStringLiteral("No se pudo abrir %1 (error %2).")
                        .arg(game.name)
                        .arg(static_cast<qulonglong>(GetLastError())));
            return;
        }
        CloseHandle(process.hThread);
        native_process = process.hProcess;
        native_played.start();
    }
    native_exe = game.exe;
    native_name = game.name;
    native_window = nullptr;
    native_ping_out = false;
    native_ready = false;
    native_stepped_aside = false;
    native_handover.invalidate();
    native_timer->start();
    // The loading screen until the game's window shows (LoadingTick).
    CloseMenu();
    loading = true;
    loading_emulated = false;
    running_game = game;
    loading_game = game;
    loading_phase = 0;
    loading_progress = -1;
    loading_status = QStringLiteral("Iniciando…");
    ResetLoadingScreen();
    launch_time = QDateTime::currentDateTime();
    loading_timer->start();
    update();
#else
    Message(QStringLiteral("Los juegos nativos solo se abren en Windows."));
#endif
}

void Home::NativeEnded() {
    const bool was_closing = closing;
    LOG_INFO(Frontend, "Notos: native game ended (closing screen: {}, Notos minimized: {})",
             was_closing, window->isMinimized());
    if (loading && !loading_emulated) {
        EndLoading();
    }
    native_window = nullptr;
    if (native_menu && overlay->isVisible()) {
        LeaveGameMenu(false);
    }
#ifdef _WIN32
    native_timer->stop();
    if (native_process) {
        CloseHandle(native_process);
        native_process = nullptr;
    }
#endif
    native_suspended = false;
    if (native_played.isValid() && !native_exe.isEmpty()) {
        // Notos keeps a native game's time played itself (the emulator only knows its own).
        QJsonObject times = config.value(QStringLiteral("playTime")).toObject();
        times.insert(native_exe, static_cast<qint64>(times.value(native_exe).toInteger() +
                                                      native_played.elapsed() / 1000));
        config.insert(QStringLiteral("playTime"), times);
        SaveConfig();
        native_played.invalidate();
    }
    native_exe.clear();
    native_name.clear();
    native_dir.clear();
    native_handover.invalidate();
    ShowNotos();
    if (was_closing) {
        EndClosing();
    } else {
        running_game = {};
        StartReveal();
    }
}

bool Home::FindNativeProcess() {
#ifdef _WIN32
    // The newest program running from the game's folder (a launcher may come first).
    const QString folder = QDir::toNativeSeparators(native_dir) + QLatin1Char('\\');
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE found = nullptr;
    FILETIME newest{};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        const HANDLE process =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE |
                            PROCESS_SUSPEND_RESUME,
                        FALSE, entry.th32ProcessID);
        if (!process) {
            continue;
        }
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        FILETIME created{}, exited{}, kernel{}, user{};
        if (QueryFullProcessImageNameW(process, 0, path, &size) &&
            QString::fromWCharArray(path, static_cast<int>(size))
                .startsWith(folder, Qt::CaseInsensitive) &&
            GetProcessTimes(process, &created, &exited, &kernel, &user) &&
            (!found || CompareFileTime(&created, &newest) > 0)) {
            if (found) {
                CloseHandle(found);
            }
            found = process;
            newest = created;
            continue;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    if (!found) {
        return false;
    }
    native_process = found;
    if (!native_played.isValid()) {
        native_played.start(); // the time played counts from the game, not from Steam's start
    }
    native_window = nullptr;
    native_ping_out = false;
    native_ready = false;
    return true;
#else
    return false;
#endif
}

void Home::AddSteam() {
    // Steam's installed games: its library folders, each game's manifest, and the art Steam
    // keeps for its library (no download).
    const QString steam = QDir::cleanPath(
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                  QSettings::NativeFormat)
            .value(QStringLiteral("SteamPath"))
            .toString());
    if (steam.isEmpty() || !QDir(steam).exists()) {
        Message(QStringLiteral("No se encontró Steam en este PC."));
        return;
    }
    QStringList libraries{steam};
    if (QFile vdf(steam + QStringLiteral("/steamapps/libraryfolders.vdf"));
        vdf.open(QFile::ReadOnly)) {
        static const QRegularExpression path(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
        for (auto it = path.globalMatch(QString::fromUtf8(vdf.readAll())); it.hasNext();) {
            QString library = it.next().captured(1);
            library.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            library = QDir::cleanPath(library);
            if (!libraries.contains(library, Qt::CaseInsensitive)) {
                libraries.push_back(library);
            }
        }
    }
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    std::set<QString> known;
    for (const QJsonValue& value : natives) {
        known.insert(value.toObject().value(QStringLiteral("exe")).toString());
    }
    const QString art_dir = QFileInfo(ConfigPath()).absolutePath() + QStringLiteral("/notos_icons");
    QDir().mkpath(art_dir);
    const auto field = [](const QString& text, const char* key) {
        const QRegularExpression re(QStringLiteral("\"%1\"\\s+\"([^\"]*)\"").arg(QLatin1String(key)),
                                    QRegularExpression::CaseInsensitiveOption);
        return re.match(text).captured(1);
    };
    int added = 0;
    for (const QString& library : libraries) {
        const QDir apps(library + QStringLiteral("/steamapps"));
        for (const QFileInfo& manifest :
             apps.entryInfoList({QStringLiteral("appmanifest_*.acf")}, QDir::Files)) {
            QFile file(manifest.absoluteFilePath());
            if (!file.open(QFile::ReadOnly)) {
                continue;
            }
            const QString text = QString::fromUtf8(file.readAll());
            const QString appid = field(text, "appid");
            const QString name = field(text, "name");
            const QString folder = apps.filePath(QStringLiteral("common/") + field(text, "installdir"));
            // Tools Steam installs alongside games (redistributables) are no games.
            if (appid.isEmpty() || appid == QStringLiteral("228980") || !QDir(folder).exists()) {
                continue;
            }
            const QString link = QStringLiteral("steam://rungameid/") + appid;
            if (!known.insert(link).second) {
                continue;
            }
            natives.append(QJsonObject{{QStringLiteral("name"), name},
                                       {QStringLiteral("exe"), link},
                                       {QStringLiteral("dir"), QDir::toNativeSeparators(folder)},
                                       {QStringLiteral("args"), QString()},
                                       {QStringLiteral("showConsole"), false},
                                       {QStringLiteral("cover"), SteamCover(steam, appid, art_dir)}});
            ++added;
        }
    }
    config.insert(QStringLiteral("native"), natives);
    SaveConfig();
    Refresh();
    Message(added == 0   ? QStringLiteral("No hay juegos de Steam nuevos para agregar.")
            : added == 1 ? QStringLiteral("Se agregó 1 juego de Steam.")
                         : QStringLiteral("Se agregaron %1 juegos de Steam.").arg(added));
}

QString Home::SteamCover(const QString& steam, const QString& appid, const QString& art_dir) {
    // Steam keeps each game's library art under appcache/librarycache/<appid>, in subfolders
    // named by hash in newer versions.
    QString hero, logo, capsule, header;
    QDirIterator it(steam + QStringLiteral("/appcache/librarycache/") + appid, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString name = QFileInfo(file).fileName().toLower();
        if (name == QStringLiteral("library_hero.jpg")) {
            hero = file;
        } else if (name == QStringLiteral("logo.png")) {
            logo = file;
        } else if (name == QStringLiteral("library_600x900.jpg") ||
                   name == QStringLiteral("library_capsule.jpg")) {
            capsule = file;
        } else if (name == QStringLiteral("library_header.jpg") ||
                   name == QStringLiteral("header.jpg")) {
            header = file;
        }
    }
    // A square cover like the others: the game's wide art with its logo over it.
    if (QImage background(hero.isEmpty() ? header : hero); !background.isNull()) {
        constexpr int Size = 600;
        QImage cover(Size, Size, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&cover);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        const double scale = std::max(static_cast<double>(Size) / background.width(),
                                      static_cast<double>(Size) / background.height());
        const QSizeF drawn = QSizeF(background.size()) * scale;
        p.drawImage(QRectF((Size - drawn.width()) / 2, (Size - drawn.height()) / 2, drawn.width(),
                           drawn.height()),
                    background);
        if (QImage mark(logo); !mark.isNull()) {
            QLinearGradient shade(0, Size * 0.45, 0, Size);
            shade.setColorAt(0, QColor(0, 0, 0, 0));
            shade.setColorAt(1, QColor(0, 0, 0, 150));
            p.fillRect(QRectF(0, 0, Size, Size), shade);
            const QSizeF box(Size * 0.8, Size * 0.4);
            const double fit = std::min(box.width() / mark.width(), box.height() / mark.height());
            const QSizeF size = QSizeF(mark.size()) * fit;
            p.drawImage(QRectF((Size - size.width()) / 2, Size * 0.92 - size.height(),
                               size.width(), size.height()),
                        mark);
        }
        p.end();
        const QString path = QStringLiteral("%1/steam_%2.png").arg(art_dir, appid);
        if (cover.save(path, "PNG")) {
            return QDir::toNativeSeparators(path);
        }
    }
    return QDir::toNativeSeparators(capsule.isEmpty() ? header : capsule);
}

u64 Home::PlayTimeOf(const Game& game) const {
    if (game.native) {
        u64 seconds = static_cast<u64>(
            config.value(QStringLiteral("playTime")).toObject().value(game.exe).toInteger());
        if (native_played.isValid() && game.exe == native_exe) {
            seconds += static_cast<u64>(native_played.elapsed() / 1000); // running now
        }
        return seconds;
    }
    return hooks.play_time && game.program_id != 0 ? hooks.play_time(game.program_id) : 0;
}

QString Home::PlatformOf(const Game& game) {
    if (game.native) {
        return game.exe.startsWith(QStringLiteral("steam://")) ? QStringLiteral("Steam")
                                                               : QStringLiteral("PC");
    }
    // Switch 2 titles have 04... program ids (Switch ones 01...); a .nup is always one.
    const bool switch2 = (game.program_id >> 56) == 0x04 ||
                         game.path.endsWith(QStringLiteral(".nup"), Qt::CaseInsensitive);
    return switch2 ? QStringLiteral("Switch 2") : QStringLiteral("Switch");
}

QString Home::InfoLine(const Game& game) const {
    QStringList parts{PlatformOf(game)};
    const qint64 when = static_cast<qint64>(config.value(QStringLiteral("lastPlayed"))
                                                .toObject()
                                                .value(game.native ? game.exe : game.path)
                                                .toDouble());
    if (when > 0) {
        const qint64 days =
            QDateTime::fromSecsSinceEpoch(when).date().daysTo(QDate::currentDate());
        if (days <= 0) {
            parts << QStringLiteral("Jugado hoy");
        } else if (days == 1) {
            parts << QStringLiteral("Jugado ayer");
        } else if (days < 30) {
            parts << QStringLiteral("Jugado hace %1 días").arg(days);
        } else if (days < 365) {
            const qint64 months = days / 30;
            parts << (months == 1 ? QStringLiteral("Jugado hace 1 mes")
                                  : QStringLiteral("Jugado hace %1 meses").arg(months));
        } else {
            const qint64 years = days / 365;
            parts << (years == 1 ? QStringLiteral("Jugado hace 1 año")
                                 : QStringLiteral("Jugado hace %1 años").arg(years));
        }
    }
    if (const QString time = PlayTimeText(PlayTimeOf(game)); !time.isEmpty()) {
        parts << time;
    }
    return parts.join(QStringLiteral(" · "));
}

QString Home::PlayTimeText(u64 seconds) {
    if (seconds < 60) {
        return {};
    }
    const u64 hours = seconds / 3600;
    const u64 minutes = seconds % 3600 / 60;
    if (hours == 0) {
        return QStringLiteral("%1 min").arg(minutes);
    }
    return minutes == 0 ? QStringLiteral("%1 h").arg(hours)
                        : QStringLiteral("%1 h %2 min").arg(hours).arg(minutes);
}

void Home::ShowNotos() {
    if (Prefs().value(QStringLiteral("fullscreen")).toBool()) {
        window->showFullScreen();
    } else {
        window->showNormal();
    }
    window->raise();
    window->activateWindow();
    setFocus();
}

// ---- menus

void Home::OpenMenu(const QString& title, std::vector<Entry> entries) {
    menu_title = title;
    menu = std::move(entries);
    menu_index = 0;
    sounds.Play(title == QStringLiteral("Ajustes") ? QStringLiteral("settings")
                                                   : QStringLiteral("select"));
    update();
}

void Home::CloseMenu() {
    if (!menu.empty()) {
        sounds.Play(QStringLiteral("select"));
    }
    if (overlay && overlay->isVisible()) {
        LeaveGameMenu(true);
        return;
    }
    menu.clear();
    update();
}

void Home::RunEntry(int index) {
    sounds.Play(QStringLiteral("select"));
    // The entry may replace the menu: run a copy.
    const Entry entry = menu[index];
    if (entry.close) {
        CloseMenu();
    }
    if (entry.act) {
        entry.act();
    }
    update();
}

void Home::Message(const QString& text) {
    OpenMenu(text, {Entry{[] { return QStringLiteral("Aceptar"); }, [] {}, {}, true}});
}

bool Home::Idle() const {
    const bool game = hooks.emulation_running && hooks.emulation_running();
    return isVisible() && !intro_pending && intro_start < 0 && menu.empty() && !loading &&
           !launching && !closing && !game;
}

void Home::OfferUpdate(const QString& version, std::function<void()> install,
                       std::function<void()> later, std::function<void()> skip) {
    OpenMenu(QStringLiteral("Notos v%1 está disponible").arg(version),
             {
                 Entry{[] { return QStringLiteral("Actualizar ahora"); }, std::move(install), {},
                       true},
                 Entry{[] { return QStringLiteral("Más tarde"); }, std::move(later), {}, true},
                 Entry{[] { return QStringLiteral("Omitir esta versión"); }, std::move(skip), {},
                       true},
             });
}

void Home::Notice(const QString& text) {
    Message(text);
}

void Home::AddMenu() {
    OpenMenu(QStringLiteral("Agregar"),
             {
                 Entry{[] { return QStringLiteral("Carpeta de juegos de Switch"); },
                       [this] { hooks.add_folder(); }, {}, true},
                 Entry{[] { return QStringLiteral("Juego de Switch (.nup, .nsp, .xci…)"); },
                       [this] { AddSwitchFile(); }, {}, true},
                 Entry{[] { return QStringLiteral("Actualización o DLC de un juego de Switch"); },
                       [this] {
                           if (hooks.install_content) {
                               hooks.install_content();
                           }
                       },
                       {}, true},
                 Entry{[] { return QStringLiteral("Juego nativo (.exe)"); },
                       [this] { AddNative(); }, {}, true},
                 Entry{[] { return QStringLiteral("Juegos de Steam"); },
                       [this] { AddSteam(); }, {}, true},
                 Entry{[] { return QStringLiteral("Importar juegos del Recomp Launcher"); },
                       [this] { ImportLauncher(); }, {}, true},
             });
}

void Home::SettingsMenu() {
    const auto yes_no = [](bool value) {
        return value ? QStringLiteral("Sí") : QStringLiteral("No");
    };
    const auto accent_step = [this](int d) {
        const QString current = Prefs().value(QStringLiteral("accent")).toString().toUpper();
        int at = 0;
        for (int i = 0; i < static_cast<int>(Accents.size()); ++i) {
            if (current == QString::fromLatin1(Accents[i].second).toUpper()) {
                at = i;
            }
        }
        const int n = static_cast<int>(Accents.size());
        SetSetting(QStringLiteral("accent"),
                   QString::fromLatin1(Accents[((at + d) % n + n) % n].second));
    };
    const auto toggle = [this](const char* key) {
        return [this, key](int) {
            SetSetting(QLatin1String(key), !Prefs().value(QLatin1String(key)).toBool());
            if (QLatin1String(key) == QLatin1String("fullscreen")) {
                ApplyFullscreen();
            }
        };
    };
    OpenMenu(
        QStringLiteral("Ajustes"),
        {
            Entry{[this] {
                      return QStringLiteral("Tema: ") +
                             (Dark() ? QStringLiteral("Oscuro") : QStringLiteral("Claro"));
                  },
                  {},
                  [this](int) {
                      SetSetting(QStringLiteral("theme"),
                                 Dark() ? QStringLiteral("light") : QStringLiteral("dark"));
                  },
                  false},
            Entry{[this]() -> QString {
                      const QString current =
                          Prefs().value(QStringLiteral("accent")).toString().toUpper();
                      for (const auto& [name, hex] : Accents) {
                          if (current == QString::fromLatin1(hex).toUpper()) {
                              return QStringLiteral("Color: ") + QString::fromUtf8(name);
                          }
                      }
                      return QStringLiteral("Color: ") + current;
                  },
                  {},
                  accent_step,
                  false},
            Entry{[this, yes_no] {
                      return QStringLiteral("Pantalla completa: ") +
                             yes_no(Prefs().value(QStringLiteral("fullscreen")).toBool());
                  },
                  {},
                  toggle("fullscreen"),
                  false},
            Entry{[this, yes_no] {
                      return QStringLiteral("Juegos en pantalla completa: ") +
                             yes_no(GamesFullscreen());
                  },
                  {},
                  toggle("gamesFullscreen"),
                  false},
            Entry{[this] {
                      return QStringLiteral("Reloj: ") +
                             (Prefs().value(QStringLiteral("clock24")).toBool()
                                  ? QStringLiteral("24 horas")
                                  : QStringLiteral("12 horas"));
                  },
                  {},
                  toggle("clock24"),
                  false},
            Entry{[this, yes_no] {
                      return QStringLiteral("Música y sonidos: ") +
                             yes_no(Prefs().value(QStringLiteral("sound")).toBool());
                  },
                  {},
                  [this](int) {
                      SetSetting(QStringLiteral("sound"),
                                 !Prefs().value(QStringLiteral("sound")).toBool());
                      ApplySound();
                  },
                  false},
            Entry{[this] {
                      return QStringLiteral("Volumen: %1%")
                          .arg(Prefs().value(QStringLiteral("volume")).toInt());
                  },
                  {},
                  [this](int d) {
                      SetSetting(QStringLiteral("volume"),
                                 std::clamp(Prefs().value(QStringLiteral("volume")).toInt() +
                                                d * 10,
                                            0, 100));
                      ApplySound();
                  },
                  false},
            Entry{[] { return QStringLiteral("Ajustes del emulador"); },
                  [this] { hooks.open_settings(); }, {}, true},
            Entry{[] { return QStringLiteral("Partidas guardadas"); },
                  [this] { hooks.open_saves(); }, {}, true},
            Entry{[this] {
                      return QStringLiteral("Buscar actualizaciones (Notos v%1)")
                          .arg(hooks.version ? hooks.version() : QString{});
                  },
                  [this] {
                      if (hooks.check_updates) {
                          hooks.check_updates();
                      }
                  },
                  {}, true},
        });
}

void Home::GameMenu() {
    if (games.empty()) {
        return;
    }
    const Game game = games[game_index];
    std::vector<Entry> entries;
    entries.push_back(
        Entry{[] { return QStringLiteral("Jugar"); }, [this, game] { BeginLaunch(game); }, {}, true});
    if (game.native) {
        const int index = NativeIndex(game);
        entries.push_back(Entry{[] { return QStringLiteral("Cambiar nombre"); },
                                [this, index] { RenameNative(index); }, {}, true});
        entries.push_back(Entry{[] { return QStringLiteral("Cambiar portada"); },
                                [this, game] { PickCover(game); }, {}, true});
        entries.push_back(Entry{[] { return QStringLiteral("Mover a la izquierda"); },
                                [this, index] { MoveNative(index, -1); }, {}, true});
        entries.push_back(Entry{[] { return QStringLiteral("Mover a la derecha"); },
                                [this, index] { MoveNative(index, 1); }, {}, true});
        entries.push_back(Entry{[] { return QStringLiteral("Quitar de la lista"); },
                                [this, index] { RemoveNative(index); }, {}, true});
    } else {
        entries.push_back(Entry{[] { return QStringLiteral("Ajustes del juego"); },
                                [this, game] {
                                    hooks.game_settings(game.program_id, game.path.toStdString());
                                },
                                {},
                                true});
        entries.push_back(Entry{[] { return QStringLiteral("Cambiar portada"); },
                                [this, game] { PickCover(game); }, {}, true});
        if (!CoverFor(game.path).isEmpty()) {
            entries.push_back(Entry{[] { return QStringLiteral("Quitar portada propia"); },
                                    [this, game] {
                                        QJsonObject covers =
                                            config.value(QStringLiteral("covers")).toObject();
                                        covers.remove(game.path);
                                        config.insert(QStringLiteral("covers"), covers);
                                        SaveConfig();
                                        Refresh();
                                    },
                                    {},
                                    true});
        }
        entries.push_back(Entry{[] { return QStringLiteral("Más opciones…"); },
                                [this, game] {
                                    QMenu more(this);
                                    if (hooks.game_menu) {
                                        hooks.game_menu(more, game.program_id,
                                                        game.path.toStdString());
                                    }
                                    more.exec(mapToGlobal(rect().center()));
                                },
                                {},
                                true});
        entries.push_back(Entry{[] { return QStringLiteral("Borrar caché de shaders"); },
                                [this, game] { hooks.delete_shader_cache(game.program_id); }, {},
                                true});
        const QString folder = !game.picked && hooks.game_dir_of ? hooks.game_dir_of(game.path)
                                                                 : QString{};
        if (game.picked) {
            entries.push_back(Entry{
                [] { return QStringLiteral("Borrar juego"); },
                [this, game] {
                    OpenMenu(QStringLiteral("¿Borrar %1 de la lista? El archivo queda en tu disco.")
                                 .arg(game.name),
                             {Entry{[] { return QStringLiteral("No"); }, [] {}, {}, true},
                              Entry{[] { return QStringLiteral("Sí, borrar"); },
                                    [this, game] { RemoveSwitchFile(game.path); }, {}, true}});
                },
                {},
                false});
        } else if (!folder.isEmpty()) {
            entries.push_back(Entry{
                [] { return QStringLiteral("Borrar juego"); },
                [this, game, folder] {
                    // Its folder leaves the emulator's list (the file stays on disk); other games
                    // listed from the same folder go with it, so the question names them.
                    int others = 0;
                    for (const Game& other : games) {
                        if (!other.native && !other.picked && other.path != game.path &&
                            QDir::cleanPath(other.path).startsWith(folder + QLatin1Char('/'), Qt::CaseInsensitive)) {
                            ++others;
                        }
                    }
                    QString question = QStringLiteral("¿Borrar %1 de la lista? El archivo queda "
                                                      "en tu disco.")
                                           .arg(game.name);
                    if (others > 0) {
                        question += QStringLiteral(" Se quita la carpeta %1 y con ella %2 juego%3 "
                                                   "más.")
                                        .arg(QDir::toNativeSeparators(folder))
                                        .arg(others)
                                        .arg(others == 1 ? QString() : QStringLiteral("s"));
                    }
                    OpenMenu(question,
                             {Entry{[] { return QStringLiteral("No"); }, [] {}, {}, true},
                              Entry{[] { return QStringLiteral("Sí, borrar"); },
                                    [this, game, folder] {
                                        QJsonObject covers =
                                            config.value(QStringLiteral("covers")).toObject();
                                        covers.remove(game.path);
                                        config.insert(QStringLiteral("covers"), covers);
                                        SaveConfig();
                                        hooks.remove_game_dir(folder);
                                    },
                                    {},
                                    true}});
                },
                {},
                false});
        }
    }
    OpenMenu(game.name, std::move(entries));
}

// ---- actions

int Home::NativeIndex(const Game& game) const {
    const QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    for (int i = 0; i < natives.size(); ++i) {
        if (natives[i].toObject().value(QStringLiteral("exe")).toString() == game.exe) {
            return i;
        }
    }
    return -1;
}

void Home::AddNative() {
    const QString exe = QFileDialog::getOpenFileName(window, QStringLiteral("Juego nativo"), {},
                                                     QStringLiteral("Programas (*.exe)"));
    if (exe.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(window, QStringLiteral("Juego nativo"), QStringLiteral("Nombre:"),
                              QLineEdit::Normal, QFileInfo(exe).completeBaseName(), &ok)
            .trimmed();
    if (!ok) {
        return;
    }
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    natives.append(QJsonObject{{QStringLiteral("name"), name.isEmpty()
                                                            ? QFileInfo(exe).completeBaseName()
                                                            : name},
                               {QStringLiteral("exe"), QDir::toNativeSeparators(exe)},
                               {QStringLiteral("args"), QString()},
                               {QStringLiteral("showConsole"), false},
                               {QStringLiteral("cover"), QString()}});
    config.insert(QStringLiteral("native"), natives);
    SaveConfig();
    game_index = natives.size() - 1;
    row = 0;
    Refresh();
}

void Home::AddSwitchFile() {
    const QString path = QFileDialog::getOpenFileName(
        window, QStringLiteral("Juego de Switch"), {},
        QStringLiteral("Juegos de Switch y Switch 2 (*.nup *.nsp *.xci *.nsz *.xcz *.nx);;"
                       "Switch 2 (*.nup);;Switch (*.nsp *.xci *.nsz *.xcz *.nx);;"
                       "Todos los archivos (*)"));
    if (path.isEmpty()) {
        return;
    }
    const QString file = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    QString name;
    QByteArray icon;
    u64 program_id = 0;
    if (!hooks.read_game || !hooks.read_game(file, name, icon, program_id)) {
        Message(QStringLiteral("%1 no es un juego que Notos pueda abrir.")
                    .arg(QFileInfo(path).fileName()));
        return;
    }
    if (name.trimmed().isEmpty()) {
        name = QFileInfo(path).completeBaseName();
    }
    // Its icon is kept next to notos.json: the file isn't read again at every start.
    QString icon_path;
    if (QPixmap pixmap; pixmap.loadFromData(icon)) {
        const QString dir = QFileInfo(ConfigPath()).absolutePath() + QStringLiteral("/notos_icons");
        QDir().mkpath(dir);
        icon_path = QStringLiteral("%1/%2.png")
                        .arg(dir)
                        .arg(program_id != 0 ? QString::number(program_id, 16).toUpper()
                                             : QString::number(qHash(file), 16));
        if (!pixmap.save(icon_path, "PNG")) {
            icon_path.clear();
        }
    }
    QJsonArray files = config.value(QStringLiteral("files")).toArray();
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].toObject().value(QStringLiteral("path")).toString().compare(
                file, Qt::CaseInsensitive) == 0) {
            files.removeAt(i);
            break;
        }
    }
    files.append(QJsonObject{
        {QStringLiteral("path"), file},
        {QStringLiteral("name"), name.trimmed()},
        {QStringLiteral("programId"), QString::number(program_id, 16).toUpper()},
        {QStringLiteral("icon"), QDir::toNativeSeparators(icon_path)}});
    config.insert(QStringLiteral("files"), files);
    SaveConfig();
    Refresh();
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if (!games[i].native && games[i].path == file) {
            game_index = i;
        }
    }
    row = 0;
    Select();
}

void Home::RemoveSwitchFile(const QString& path) {
    QJsonArray files = config.value(QStringLiteral("files")).toArray();
    for (int i = 0; i < files.size(); ++i) {
        const QJsonObject object = files[i].toObject();
        if (object.value(QStringLiteral("path")).toString() == path) {
            QFile::remove(object.value(QStringLiteral("icon")).toString());
            files.removeAt(i);
            break;
        }
    }
    config.insert(QStringLiteral("files"), files);
    QJsonObject covers = config.value(QStringLiteral("covers")).toObject();
    covers.remove(path);
    config.insert(QStringLiteral("covers"), covers);
    SaveConfig();
    Refresh();
}

void Home::ImportLauncher() {
    const QString path = QFileDialog::getOpenFileName(
        window, QStringLiteral("launcher.json del Recomp Launcher"), {},
        QStringLiteral("Recomp Launcher (launcher.json);;JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) {
        Message(QStringLiteral("No se pudo leer %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    const QJsonObject launcher = QJsonDocument::fromJson(file.readAll()).object();
    const QDir base = QFileInfo(path).absoluteDir();
    const auto resolve = [&base](const QString& value) {
        return value.isEmpty() || QFileInfo(value).isAbsolute() ? value
                                                                : base.absoluteFilePath(value);
    };
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    std::set<QString> known;
    for (const QJsonValue& value : natives) {
        known.insert(value.toObject().value(QStringLiteral("exe")).toString().toLower());
    }
    int added = 0;
    for (const QJsonValue& value : launcher.value(QStringLiteral("games")).toArray()) {
        const QJsonObject game = value.toObject();
        // Native games only: Notos opens the emulated ones itself, from its game list.
        const QString exe = game.value(QStringLiteral("exe")).toString();
        if (!game.value(QStringLiteral("emulator")).toString().isEmpty() || exe.isEmpty()) {
            continue;
        }
        const QString full = QDir::toNativeSeparators(resolve(exe));
        if (!known.insert(full.toLower()).second) {
            continue;
        }
        natives.append(QJsonObject{
            {QStringLiteral("name"), game.value(QStringLiteral("name")).toString()},
            {QStringLiteral("exe"), full},
            {QStringLiteral("args"), game.value(QStringLiteral("args")).toString()},
            {QStringLiteral("showConsole"), game.value(QStringLiteral("showConsole")).toBool()},
            {QStringLiteral("cover"),
             QDir::toNativeSeparators(resolve(game.value(QStringLiteral("cover")).toString()))}});
        ++added;
    }
    config.insert(QStringLiteral("native"), natives);
    // The launcher's look comes along: theme, colour and clock.
    const QJsonObject settings = launcher.value(QStringLiteral("settings")).toObject();
    QJsonObject own = config.value(QStringLiteral("settings")).toObject();
    for (const char* key : {"theme", "accent", "clock24"}) {
        if (settings.contains(QLatin1String(key))) {
            own.insert(QLatin1String(key), settings.value(QLatin1String(key)));
        }
    }
    config.insert(QStringLiteral("settings"), own);
    SaveConfig();
    Refresh();
    Message(added == 1 ? QStringLiteral("Se agregó 1 juego nativo.")
                       : QStringLiteral("Se agregaron %1 juegos nativos.").arg(added));
}

void Home::PickCover(const Game& game) {
    const QString image = QFileDialog::getOpenFileName(
        window, QStringLiteral("Portada de %1").arg(game.name), {},
        QStringLiteral("Imágenes (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (image.isEmpty()) {
        return;
    }
    const QString native_path = QDir::toNativeSeparators(image);
    if (game.native) {
        QJsonArray natives = config.value(QStringLiteral("native")).toArray();
        const int index = NativeIndex(game);
        if (index < 0) {
            return;
        }
        QJsonObject entry = natives[index].toObject();
        entry.insert(QStringLiteral("cover"), native_path);
        natives[index] = entry;
        config.insert(QStringLiteral("native"), natives);
    } else {
        QJsonObject covers = config.value(QStringLiteral("covers")).toObject();
        covers.insert(game.path, native_path);
        config.insert(QStringLiteral("covers"), covers);
    }
    SaveConfig();
    Refresh();
}

void Home::RenameNative(int index) {
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    if (index < 0 || index >= natives.size()) {
        return;
    }
    QJsonObject entry = natives[index].toObject();
    bool ok = false;
    const QString name =
        QInputDialog::getText(window, QStringLiteral("Cambiar nombre"), QStringLiteral("Nombre:"),
                              QLineEdit::Normal, entry.value(QStringLiteral("name")).toString(),
                              &ok)
            .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    entry.insert(QStringLiteral("name"), name);
    natives[index] = entry;
    config.insert(QStringLiteral("native"), natives);
    SaveConfig();
    Refresh();
}

void Home::MoveNative(int index, int delta) {
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    const int to = index + delta;
    if (index < 0 || index >= natives.size() || to < 0 || to >= natives.size()) {
        return;
    }
    const QJsonValue moved = natives[index];
    natives[index] = natives[to];
    natives[to] = moved;
    config.insert(QStringLiteral("native"), natives);
    SaveConfig();
    game_index = to;
    Refresh();
    game_index = to;
    Select();
}

void Home::RemoveNative(int index) {
    QJsonArray natives = config.value(QStringLiteral("native")).toArray();
    if (index < 0 || index >= natives.size()) {
        return;
    }
    natives.removeAt(index);
    config.insert(QStringLiteral("native"), natives);
    SaveConfig();
    Refresh();
}

} // namespace Notos
