// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPixmap>
#include <QRect>
#include <QWidget>

#include "common/common_types.h"
#include "yuzu/notos/notos_sound.h"

class QBoxLayout;
class QMainWindow;
class QMenu;
class QStandardItemModel;
class QTimer;

namespace Core::HID {
class EmulatedController;
}

namespace Notos {

/// What the Notos home screen asks the main window to do.
struct HomeHooks {
    std::function<void(const QString& path, u64 program_id)> play;
    std::function<void()> add_folder;
    std::function<void()> open_settings; ///< the emulator's settings
    std::function<void(u64 program_id, const std::string& path)> game_settings;
    std::function<void(u64 program_id)> delete_shader_cache; ///< after a confirmation
    /// The game list's own right-click entries for a Switch game.
    std::function<void(QMenu& menu, u64 program_id, const std::string& path)> game_menu;
    std::function<void()> open_saves;
    std::function<bool()> single_window;
    std::function<bool()> emulation_running;
    std::function<Core::HID::EmulatedController*()> controller; ///< player 1, for gamepad navigation
    QStandardItemModel* model = nullptr;

    // The Home menu over a running game.
    std::function<QWidget*()> render;     ///< the game's render widget
    std::function<bool()> game_running;   ///< running, not paused
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> stop;           ///< close the game
    std::function<void()> game_config;    ///< the running game's own settings
    std::function<void()> toggle_fullscreen;
    std::function<bool()> fullscreen;
    /// The emulator's game folder a game file is listed from (empty: none), and removing it.
    std::function<QString(const QString& path)> game_dir_of;
    std::function<void(const QString& folder)> remove_game_dir;
    /// Whether the next game starts in fullscreen (the emulator's Fullscreen action, unchecked
    /// without a game).
    std::function<void(bool)> set_game_fullscreen;
    /// A game file's title, icon and program id, read from the file itself (false: not a game).
    std::function<bool(const QString& path, QString& name, QByteArray& icon, u64& program_id)>
        read_game;
    /// Seconds a Switch game has been played (the emulator's own count).
    std::function<u64(u64 program_id)> play_time;
    /// The system's Mii Editor (from the firmware).
    std::function<void()> mii_editor;
    std::function<void()> install_content; ///< an update or DLC into the NAND
    std::function<void()> restart;         ///< the running game, without asking
    /// Multiplayer rooms: 0 public lobby, 1 create, 2 direct connect, 3 current room, 4 leave.
    std::function<void(int action)> multiplayer;
    std::function<bool()> in_room;
    std::function<void()> check_updates; ///< asks GitHub now; the answer comes back as a notice
    std::function<QString()> version;     ///< "1", "1.2"...
};

class Home;

/// The Home menu's window: a borderless window over the game that dims it and shows the home
/// screen's centred menu. The game's own render surface can't have widgets drawn on it.
class Overlay : public QWidget {
    Q_OBJECT
public:
    explicit Overlay(Home* home);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    Home* home;
};

/// The Notos main screen, drawn like the Recomp Launcher: the title of the selected game over a
/// row of large covers (Switch games from the game list, and native games such as recompiled
/// ports), round buttons (Add, Settings, Exit) and centred menus. Keyboard, mouse and gamepad.
class Home : public QWidget {
    Q_OBJECT
public:
    Home(QMainWindow* window, QBoxLayout* central, QWidget* game_list, QWidget* placeholder,
         HomeHooks hooks);

    /// A game is starting in the main window: the render view takes the space.
    void OnEmulationStarting();
    /// The game stopped: the Home menu goes with it.
    void OnEmulationStopping();

    /// The Home menu over the running game: opens it (pausing the game) or closes it (resuming).
    void ToggleGameMenu();

    /// Nothing else is on screen: the opening is over, no menu, game or loading screen.
    bool Idle() const;
    /// "Notos vN está disponible": update now, later, or skip this version.
    void OfferUpdate(const QString& version, std::function<void()> install,
                     std::function<void()> later, std::function<void()> skip);
    /// A message with an Aceptar button.
    void Notice(const QString& text);

    /// The emulator's shader loading at boot (stage 0 prepare, 1 build, 2 complete).
    void OnLoadProgress(int stage, qulonglong value, qulonglong total);
    /// The emulator's own loading screen went away: the game shows.
    void OnLoadFinished();

private:
    friend class Overlay;
    void PaintMenu(QPainter& p, const QRect& area);
    /// The loading screen: the game's cover, its name and a bar.
    void PaintLoading(QPainter& p, const QRect& area);
    void LoadingTick();
    void ReadNativeProgress();
    void EndLoading();
    void StartClosing(bool emulated);
    void EndClosing();
    void StartReveal();
    bool GamesFullscreen() const;
    /// A click on the open menu: an entry, or outside the menu (closes it).
    void MenuMouse(const QPoint& pos, Qt::MouseButton button);
    QString KeyCommand(int key) const;
    void PlaceOverlay();
    void LeaveGameMenu(bool resume);
    void GameMenuEntries();

    // The Home menu over a native game (an exe Notos started).
    void ToggleNativeMenu();
    void NativeMenuEntries();
    void SuspendNative(bool suspend);
    void CloseNative();
    void ShowNotos();

public:

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Game {
        QString name;
        bool native = false;
        QString exe;  ///< native: the program
        QString args; ///< native: its arguments
        /// native: the game's folder when a store starts it (exe is then the store's link, such as
        /// steam://rungameid/...): its process is found there.
        QString dir;
        bool show_console = false;
        QString path; ///< Switch: the game file
        u64 program_id = 0;
        bool picked = false; ///< Switch: a file the player added on its own, not from a folder
        QPixmap cover;
    };
    struct Entry {
        std::function<QString()> label;
        std::function<void()> act;
        std::function<void(int)> adjust; ///< left / right: -1 or +1
        bool close = true;               ///< close the menu before act
    };

    // Data
    void LoadConfig();
    /// At start: back on the monitor Notos was last on, which is remembered from then on.
    void MoveToLastScreen();
    void SaveConfig();
    void Refresh();
    QString CoverFor(const QString& key) const;
    QJsonObject Prefs() const;
    void SetSetting(const QString& key, const QJsonValue& value);
    QColor Accent() const;
    bool Rainbow() const; ///< the accent is the slowly moving rainbow
    QString PadName() const; ///< the connected gamepad, or empty for keyboard and mouse
    /// What marks the selection: the accent, or the rainbow turning around `rect`.
    QBrush AccentBrush(const QRectF& rect) const;
    bool Dark() const;
    void AnimTick();

    // Screens
    void ShowHome();
    void ShowList();
    void Restore();
    void ApplyFullscreen();

    // Navigation
    void Nav(const QString& cmd);
    void Select();
    void PollPad();
    void Launch(const Game& game);
    void LaunchNative(const Game& game);
    void NativeEnded();

    // Menus
    void OpenMenu(const QString& title, std::vector<Entry> entries);
    void CloseMenu();
    void RunEntry(int index);
    void AddMenu();
    void SettingsMenu();
    void GameMenu();
    void Message(const QString& text);

    // Actions
    void AddNative();
    void AddSwitchFile();
    void AddSteam();
    /// A square cover from Steam's library art for the game (its path), or Steam's own picture.
    static QString SteamCover(const QString& steam, const QString& appid, const QString& art_dir);
    /// A store-started game's process, once it runs from its folder: takes it as the native game.
    bool FindNativeProcess();
    void RemoveSwitchFile(const QString& path);
    void ImportLauncher();
    void PickCover(const Game& game);
    void RenameNative(int index);
    void MoveNative(int index, int delta);
    void RemoveNative(int index);
    int NativeIndex(const Game& game) const;
    u64 PlayTimeOf(const Game& game) const;
    static QString PlayTimeText(u64 seconds);

    QMainWindow* window;
    QWidget* game_list;
    QWidget* placeholder;
    HomeHooks hooks;
    Sounds sounds;
    void ApplySound(); ///< the settings' sound and volume
    QTimer* refresh_timer = nullptr;
    QTimer* clock_timer = nullptr;
    QTimer* pad_timer = nullptr;
    QTimer* native_timer = nullptr;
    QAction* home_action = nullptr;

    QJsonObject config;
    std::vector<Game> games;
    int row = 0; ///< -1: the Mii button at the top, 0: covers, 1: round buttons
    int game_index = 0;
    int button_index = 0;
    double scroll = 0;
    bool list_mode = false;
    QString pad_name; ///< shown next to the clock
    bool pad_last = false; ///< the last input came from the gamepad: hints show its buttons
    struct Hint {
        QString key;   ///< keyboard
        QString pad;   ///< gamepad
        QString label;
    };
    /// Hints as buttons (a circle for the gamepad's, a key cap for the keyboard's) then what
    /// they do, aligned in `area`.
    void PaintHints(QPainter& p, const std::vector<Hint>& hints, const QRectF& area,
                    Qt::Alignment align, double px, const QColor& colour) const;
    // Closing a game: the closing screen shows until the game is gone.
    bool closing = false;
    bool closing_emulated = false;
    qint64 closing_start = 0;
    Game running_game;            ///< the game started from here, for that screen
    bool reveal_after_game = false;
    bool intro_reveal_only = false; ///< the opening without its emblem: back from a game
    double menu_show = 0;   ///< 0..1: the open menu coming in
    double menu_cursor = 0; ///< where the highlight is, gliding to menu_index

    // Animation: the covers glide to where the selection wants them, the selected one grows, and
    // the opening sequence plays once.
    QTimer* anim_timer = nullptr;
    QElapsedTimer anim_clock;
    qint64 anim_last = 0;
    int anim_ticks = 0;
    double scroll_target = 0;
    std::vector<double> focus; ///< per cover: 0 resting .. 1 selected
    bool intro_pending = true;
    double intro_start = -1; ///< milliseconds played (frames shown, not the clock), or -1
    int intro_calm = 0;      ///< frames in a row on time: the start-up work is over
    bool status_was_visible = true;

    QString menu_title;
    std::vector<Entry> menu;
    int menu_index = 0;

    // Hit areas from the last paint.
    std::vector<QRect> tile_rects;
    std::vector<QRect> button_rects;
    std::vector<QRect> menu_rects;
    QRect menu_box;
    std::vector<QRect> top_rects; ///< the buttons next to the Notos name
    int top_index = 0;
    bool restarting = false; ///< the game stops only to start again: no closing screen
    void RestartGame();
    void MultiplayerMenu();

    // Gamepad: the buttons held at the last poll and when a direction repeats next.
    u64 pad_held = 0;
    QString pad_repeat_cmd;
    QElapsedTimer pad_repeat;
    int pad_repeat_delay = 0;

    // The native game running (Windows process handle), or none.
    void* native_process = nullptr;
    QString native_exe;
    QString native_name;
    QString native_dir;        ///< a store-started game's folder, while Notos looks for it
    QElapsedTimer native_wait; ///< since Notos asked the store to start it
    bool native_stepped_aside = false; ///< Notos minimized for the store's own windows
    QElapsedTimer native_handover; ///< its program closed: whether another one follows
    bool native_menu = false;      ///< the Home menu shows over the native game
    bool native_suspended = false; ///< its process is paused for the menu
    void* native_window = nullptr; ///< the game's window once it shows (HWND)
    QElapsedTimer native_window_seen;
    QElapsedTimer native_played; ///< since the native game started, for its time played
    // Whether the game's window answers messages yet (it doesn't while the game loads).
    QElapsedTimer native_ping;
    bool native_ping_out = false;
    bool native_ready = false;

    // The loading screen, from choosing a game until the game shows.
    bool loading = false;
    bool loading_emulated = false; ///< drawn in the overlay over the emulator's own loading screen
    Game loading_game;
    int loading_phase = 0;
    double loading_progress = -1; ///< 0..1, or -1 while unknown (a block runs along the bar)
    QString loading_status;
    QDateTime launch_time;
    QTimer* loading_timer = nullptr;

    // The Home menu over a running game.
    Overlay* overlay = nullptr;
    QTimer* overlay_timer = nullptr;
    bool paused_by_menu = false;
    /// The menu closed: the game gets the controllers back once every button is let go.
    bool unblock_on_release = false;
    void BlockGameInput(bool block);
    bool home_down = false;  ///< the gamepad's Home button is held
    bool home_combo = false; ///< another button was pressed with it (a hotkey like Home+B)
};

} // namespace Notos
