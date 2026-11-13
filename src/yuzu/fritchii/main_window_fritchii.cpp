// SPDX-License-Identifier: GPL-3.0-or-later

// MainWindow's Fritchii setup, kept out of main_window.cpp so Lichii merges stay clean. It runs
// once at the end of the constructor and does nothing in a Lichii build.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>

#include "core/core.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "ui_main.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/bootmanager.h"
#include "yuzu/fritchii/game_controls.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/render/emu_thread.h"
#include "video_core/rasterizer_interface.h"
#include "yuzu/loading_screen.h"
#include "yuzu/fritchii/saves_window.h"
#include "yuzu/notos/notos_home.h"
#include "yuzu/fritchii/precompile_gate.h"
#include "yuzu/fritchii/whats_new.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"
#include "yuzu/multiplayer/state.h"
#include "yuzu/util/clickable_label.h"

void MainWindow::SetUpFritchiiUi() {
    if (!FritchiiUi::Enabled()) {
        return;
    }

    // For players whose cache went bad: the game's whole shader folder (pipeline caches,
    // precompiled pipelines and what their preparation kept), after a confirmation. Not while a
    // game runs, which has those files open.
    constexpr u64 OcarinaOfTime = 0x0400486024DEC000ULL;
    const auto delete_shader_cache = [this](u64 program_id) {
        const QString title = tr("Delete Shader Cache");
        if (emulation_running || boot_in_progress) {
            QMessageBox::information(this, title, tr("Stop the game first."));
            return;
        }
        QMessageBox box(QMessageBox::Warning, title,
                        program_id == OcarinaOfTime ? tr("Delete OoT's shader cache?")
                                                    : tr("Delete this game's shader cache?"),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Only if the game crashes or looks wrong because of it. The "
                                  "shaders are prepared again on the next start."));
        QPushButton* remove = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
        box.setDefaultButton(box.addButton(QMessageBox::Cancel));
        box.exec();
        if (box.clickedButton() == remove) {
            QtCommon::Game::RemoveAllTransferableShaderCaches(program_id);
        }
    };

    FritchiiUi::RestructureMenus(
        *ui, *this,
        {.export_log = [this] { OnExportLog(); },
         .whats_new = [this] { FritchiiUi::ShowWhatsNew(this); },
         .delete_shader_cache = [delete_shader_cache] { delete_shader_cache(OcarinaOfTime); }});

    // "What's new" once per version after an update, when nothing else is on screen: no other
    // dialog (first-launch question, update offer, restart note) and no game running.
    const bool used_before = Settings::values.oot_fps_asked.GetValue();
    if (FritchiiUi::ShouldShowWhatsNew(used_before)) {
        auto* timer = new QTimer(this);
        timer->setInterval(1500);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            const bool first_question_pending =
                !launched_with_game && !Settings::values.oot_fps_asked.GetValue();
            if (QApplication::activeModalWidget() != nullptr || first_question_pending ||
                emulation_running || boot_in_progress) {
                return;
            }
            timer->stop();
            timer->deleteLater();
            FritchiiUi::MarkWhatsNewSeen();
            OnSaveConfig();
            FritchiiUi::ShowWhatsNew(this);
        });
        timer->start();
    } else if (!used_before) {
        OnSaveConfig(); // a first install recorded its version
    }

    // Status bar: the everyday controls stay (60/30 FPS, docked, volume); renderer, accuracy,
    // filter, anti-aliasing and effects move into one Graphics button. The old buttons stay
    // hidden: they still hold the state the rest of the window updates.
    for (QWidget* expert : {static_cast<QWidget*>(renderer_status_button),
                            static_cast<QWidget*>(gpu_accuracy_button),
                            static_cast<QWidget*>(filter_status_button),
                            static_cast<QWidget*>(aa_status_button),
                            static_cast<QWidget*>(post_shader_status_button),
                            static_cast<QWidget*>(refresh_button)}) {
        if (expert) {
            expert->hide();
        }
    }
    multiplayer_state->GetStatusText()->hide();
    FritchiiUi::GraphicsHooks graphics{
        .refresh =
            [this] {
                UpdateStatusButtons();
#ifdef HAS_RESHADE
                UpdatePostShaderText();
#endif
            },
        .renderer_locked = [this] { return !renderer_status_button->isEnabled(); },
        .open_settings = [this] { OnConfigure(); },
#ifdef HAS_RESHADE
        .open_effects = [this] { OnPostProcessingShaders(); },
#endif
    };
    statusBar()->insertPermanentWidget(0, FritchiiUi::MakeGraphicsButton(this, graphics));

    // Home screen.
    Notos::HomeHooks home{
        .play = [this](const QString& path, u64 program_id) { OnGameListLoadFile(path, program_id); },
        .add_folder = [this] { OnGameListAddDirectory(); },
        .open_settings = [this] { OnConfigure(); },
        .game_settings =
            [this](u64 program_id, const std::string& path) {
                OpenPerGameConfiguration(program_id, path);
            },
        .delete_shader_cache = delete_shader_cache,
        .game_menu =
            [this](QMenu& menu, u64 program_id, const std::string& path) {
                game_list->AddGameMenu(menu, program_id, path);
            },
        .open_saves =
            [this] {
                FritchiiUi::ShowSavesWindow(this, [this] { return emulation_running; });
            },
        .single_window = [this] { return ui->action_Single_Window_Mode->isChecked(); },
        .emulation_running = [this] { return emulation_running; },
        .controller =
            [] {
                return QtCommon::system ? QtCommon::system->HIDCore().GetEmulatedController(
                                              Core::HID::NpadIdType::Player1)
                                        : nullptr;
            },
        .model = game_list->GetModel(),
        .render = [this] { return static_cast<QWidget*>(render_window); },
        .game_running = [] { return QtCommon::emu_thread && QtCommon::emu_thread->IsRunning(); },
        .pause = [this] { OnPauseGame(); },
        .resume = [this] { OnStartGame(); },
        .stop =
            [this] {
                // OnStopGame without its question: the Home menu asked already.
                play_time_manager->Stop();
                if (OnShutdownBegin()) {
                    OnShutdownBeginDialog();
                } else {
                    OnEmulationStopped();
                }
            },
        .game_config = [this] { ui->action_Configure_Current_Game->trigger(); },
        .toggle_fullscreen = [this] { ui->action_Fullscreen->trigger(); },
        .fullscreen = [this] { return ui->action_Fullscreen->isChecked(); },
        .game_dir_of =
            [](const QString& path) {
                // The emulator's game folder holding the file: the deepest one that contains it.
                const QString file = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
                QString best;
                for (const UISettings::GameDir& dir : UISettings::values.game_dirs) {
                    const QString folder = QDir::cleanPath(QString::fromStdString(dir.path));
                    if (folder == QStringLiteral("SDMC") || folder == QStringLiteral("UserNAND") ||
                        folder == QStringLiteral("SysNAND")) {
                        continue;
                    }
                    if (file.startsWith(folder + QLatin1Char('/'), Qt::CaseInsensitive) &&
                        folder.size() > best.size()) {
                        best = folder;
                    }
                }
                return best;
            },
        .remove_game_dir =
            [this](const QString& folder) {
                // As the game list's "Remove Game Directory".
                auto& dirs = UISettings::values.game_dirs;
                for (int i = 0; i < dirs.size(); ++i) {
                    if (QDir::cleanPath(QString::fromStdString(dirs[i].path))
                            .compare(folder, Qt::CaseInsensitive) == 0) {
                        dirs.removeAt(i);
                        break;
                    }
                }
                game_list->PopulateAsync(dirs);
                OnSaveConfig();
            },
        .set_game_fullscreen =
            [this](bool on) {
                if (!emulation_running) {
                    ui->action_Fullscreen->setChecked(on);
                }
            },
        .read_game =
            [](const QString& path, QString& name, QByteArray& icon, u64& program_id) {
                // As the game list reads the files of its folders.
                if (!QtCommon::system || !QtCommon::vfs) {
                    return false;
                }
                const auto file =
                    QtCommon::vfs->OpenFile(path.toStdString(), FileSys::OpenMode::Read);
                if (!file) {
                    return false;
                }
                const auto loader = Loader::GetLoader(*QtCommon::system, file);
                if (!loader) {
                    return false;
                }
                const auto type = loader->GetFileType();
                if (type == Loader::FileType::Unknown || type == Loader::FileType::Error) {
                    return false;
                }
                program_id = 0;
                void(loader->ReadProgramId(program_id));
                std::vector<u8> data;
                if (loader->ReadIcon(data) == Loader::ResultStatus::Success) {
                    icon = QByteArray(reinterpret_cast<const char*>(data.data()),
                                      static_cast<qsizetype>(data.size()));
                }
                std::string title;
                if (loader->ReadTitle(title) == Loader::ResultStatus::Success &&
                    title != " ") {
                    name = QString::fromStdString(title).trimmed();
                }
                return true;
            },
        .play_time = [this](u64 program_id) { return play_time_manager->GetPlayTime(program_id); },
        .mii_editor = [this] { ui->action_Launch_MiiEdit->trigger(); },
    };
    // Notos: the launcher-style home screen in place of Fritchii's (FritchiiUi::Home).
    auto* notos_home = new Notos::Home(this, ui->horizontalLayout, game_list,
                                       game_list_placeholder, std::move(home));
    connect(this, &MainWindow::EmulationStarting, notos_home, &Notos::Home::OnEmulationStarting);
    // Its loading screen follows the emulator's: shader progress, then gone with it.
    connect(this, &MainWindow::EmulationStarting, notos_home, [notos_home] {
        if (QtCommon::emu_thread) {
            connect(
                QtCommon::emu_thread.get(), &EmuThread::LoadProgress, notos_home,
                [notos_home](VideoCore::LoadCallbackStage stage, std::size_t value,
                             std::size_t total) {
                    notos_home->OnLoadProgress(static_cast<int>(stage), value, total);
                },
                Qt::QueuedConnection);
        }
    });
    connect(loading_screen, &LoadingScreen::Hidden, notos_home, &Notos::Home::OnLoadFinished);
    connect(this, &MainWindow::EmulationStopping, notos_home, &Notos::Home::OnEmulationStopping);

    // In-game controls: the menu bar is gone and the rail steps aside while a game runs.
    auto* controls = new FritchiiUi::GameControls(
        this, {.render = [this] { return static_cast<QWidget*>(render_window); },
               .running = [] { return QtCommon::emu_thread && QtCommon::emu_thread->IsRunning(); },
               .pause = ui->action_Pause,
               .restart = ui->action_Restart,
               .stop = ui->action_Stop,
               .fullscreen = ui->action_Fullscreen,
               .menus = {ui->menu_File, ui->menu_Emulation, ui->menu_View, ui->menu_Tools,
                         ui->menu_Help}});
    connect(this, &MainWindow::EmulationStarting, controls, &FritchiiUi::GameControls::Start);
    connect(this, &MainWindow::EmulationStopping, controls, &FritchiiUi::GameControls::Stop);

    // "Preparing shaders" before the game starts when its precompiled pipelines need preparing.
    auto* gate = new FritchiiUi::PrecompileGate(
        this, statusBar(),
        {.pause = [this] { OnPauseGame(); },
         .resume = [this] { OnStartGame(); },
         .game_name = [] {
             return QtCommon::system &&
                            QtCommon::system->GetApplicationProcessProgramID() == OcarinaOfTime
                        ? QStringLiteral("OoT")
                        : tr("the game");
         }});
    connect(this, &MainWindow::EmulationStarting, gate,
            &FritchiiUi::PrecompileGate::OnEmulationStarting);
    connect(this, &MainWindow::EmulationStopping, gate,
            &FritchiiUi::PrecompileGate::OnEmulationStopping);
}
