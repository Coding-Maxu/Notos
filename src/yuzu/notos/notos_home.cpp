// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "yuzu/notos/notos_home.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTime>
#include <QTimer>

#include "common/fs/path_util.h"
#include "hid_core/frontend/emulated_controller.h"
#include "qt_common/game_list/game_list_p.h"
#include "yuzu/fritchii/fritchii_ui.h"

namespace Notos {
namespace {

// The launcher's colour choices: Spanish name, colour.
constexpr std::array<std::pair<const char*, const char*>, 6> Accents{{
    {"Azul", "#2F9BFF"},
    {"Rojo", "#FF4D4D"},
    {"Verde", "#3DDC84"},
    {"Amarillo", "#FFC83D"},
    {"Morado", "#A66BFF"},
    {"Naranja", "#FF8A3D"},
}};

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
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    central->addWidget(this, 1);

    LoadConfig();

    // The classic menus stay in a zero-height bar (their shortcuts keep working); the classic game
    // list shows them, with "Inicio" to come back.
    home_action = window->menuBar()->addAction(QStringLiteral("◀ Inicio"));
    connect(home_action, &QAction::triggered, this, &Home::ShowHome);

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
        if (isVisible()) {
            update();
        }
    });
    clock_timer->start();

    pad_timer = new QTimer(this);
    pad_timer->setInterval(16);
    connect(pad_timer, &QTimer::timeout, this, &Home::PollPad);
    pad_timer->start();

    native_timer = new QTimer(this);
    native_timer->setInterval(500);
    connect(native_timer, &QTimer::timeout, this, [this] {
#ifdef _WIN32
        if (native_process && WaitForSingleObject(native_process, 0) == WAIT_OBJECT_0) {
            NativeEnded();
        }
#endif
    });

    QTimer::singleShot(0, this, [this] {
        Restore();
        if (Prefs().value(QStringLiteral("fullscreen")).toBool()) {
            ApplyFullscreen();
        }
    });
}

// ---- data

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
    fallback("accent", QString::fromLatin1(Accents[0].second));
    fallback("fullscreen", false);
    fallback("clock24", true);
    return settings;
}

void Home::SetSetting(const QString& key, const QJsonValue& value) {
    QJsonObject settings = config.value(QStringLiteral("settings")).toObject();
    settings.insert(key, value);
    config.insert(QStringLiteral("settings"), settings);
    SaveConfig();
    update();
}

QColor Home::Accent() const {
    const QColor color(Prefs().value(QStringLiteral("accent")).toString());
    return color.isValid() ? color : QColor(Accents[0].second);
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
        game.cover = LoadCover(object.value(QStringLiteral("cover")).toString());
        if (game.name.isEmpty()) {
            game.name = QFileInfo(game.exe).completeBaseName();
        }
        games.push_back(std::move(game));
    }

    // Then the Switch games of the game list.
    std::set<QString> seen;
    const auto add = [&](const QStandardItem* item) {
        if (item->data(GameListItem::TypeRole).toInt() !=
            static_cast<int>(GameListItemType::Game)) {
            return;
        }
        const QString path = item->data(GameListItemPath::FullPathRole).toString();
        if (path.isEmpty() || !seen.insert(path).second) {
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

    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if ((games[i].native ? games[i].exe : games[i].path) == selected_key) {
            game_index = i;
        }
    }
    if (was_empty && !games.empty()) {
        row = 0; // the games just arrived: start on the covers
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
}

void Home::OnEmulationStarting() {
    if (!hooks.single_window()) {
        return;
    }
    CloseMenu();
    hide();
    window->statusBar()->setVisible(status_was_visible);
}

void Home::ApplyFullscreen() {
    if (Prefs().value(QStringLiteral("fullscreen")).toBool()) {
        window->showFullScreen();
    } else if (window->isFullScreen()) {
        window->showNormal();
    }
}

bool Home::eventFilter(QObject* watched, QEvent* event) {
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
    p.fillRect(rect(), bg);

    // Top bar: the Notos badge, the clock.
    const double badge = BadgeSize * s;
    const QRectF badge_rect(48 * s, TopBar * s - badge, badge, badge);
    const QPixmap icon = window->windowIcon().pixmap(static_cast<int>(badge * 2));
    p.drawPixmap(badge_rect, icon, QRectF(icon.rect()));
    p.setPen(sub);
    p.setFont(Font(font(), 18 * s));
    p.drawText(QRectF(badge_rect.right() + 14 * s, badge_rect.top(), 400 * s, badge),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Notos"));
    const bool clock24 = Prefs().value(QStringLiteral("clock24")).toBool();
    p.setPen(text);
    p.setFont(Font(font(), ClockSize * s));
    p.drawText(QRectF(w / 2.0, badge_rect.top() - 20 * s, w / 2.0 - 48 * s, badge + 40 * s),
               Qt::AlignVCenter | Qt::AlignRight,
               QTime::currentTime().toString(clock24 ? QStringLiteral("HH:mm")
                                                     : QStringLiteral("h:mm AP")));

    // Centre: the selected game's title over the covers.
    const double tile = Tile * s;
    const double outer = tile + 2 * (TileBorder + TilePad) * s;
    const double cell = outer + 2 * TileGap * s;
    const double title_h = TitleSize * 1.5 * s + 12 * s;
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
    }

    tile_rects.assign(games.size(), QRect());
    p.save();
    p.setClipRect(QRectF(0, area_top, w, area_bottom - area_top));
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        const double x = left + i * cell - scroll + TileGap * s;
        if (x + outer < 0 || x > w) {
            continue;
        }
        const QRectF outer_rect(x, top + title_h + TileGap * s, outer, outer);
        tile_rects[i] = outer_rect.toAlignedRect();
        const bool on = row == 0 && i == game_index;
        if (on) {
            p.setPen(QPen(accent, TileBorder * s));
            p.setBrush(Qt::NoBrush);
            const double half = TileBorder * s / 2;
            p.drawRoundedRect(outer_rect.adjusted(half, half, -half, -half), 4 * s, 4 * s);
        }
        const double inset = (TileBorder + TilePad) * s;
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
        p.restore();
        // Under the cover: "Emulado" for a Switch game; nothing for a native one.
        if (!game.native) {
            p.setPen(sub);
            p.setFont(Font(font(), HintSize * s, QFont::Medium));
            p.drawText(QRectF(outer_rect.left(), outer_rect.bottom() + TileGap * s, outer, label_h),
                       Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Emulado"));
        }
    }
    p.restore();
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
        const QRectF circle(buttons_x + i * button_w + (button_w - icon_d) / 2, buttons_y, icon_d,
                            icon_d);
        button_rects[i] = QRectF(buttons_x + i * button_w, buttons_y, button_w, icon_d + 34 * s)
                              .toAlignedRect();
        p.setBrush(panel);
        p.setPen(on ? QPen(accent, 4 * s) : QPen(Qt::NoPen));
        p.drawEllipse(circle.adjusted(2 * s, 2 * s, -2 * s, -2 * s));
        const int glyph = static_cast<int>(icon_d * 0.42);
        const QPixmap pixmap = FritchiiUi::Glyph(QString::fromLatin1(Buttons[i].icon),
                                                 text.rgb() & 0xFFFFFF, glyph)
                                   .pixmap(glyph, glyph);
        p.drawPixmap(QRectF(circle.center().x() - glyph / 2.0, circle.center().y() - glyph / 2.0,
                            glyph, glyph),
                     pixmap, QRectF(pixmap.rect()));
        if (on) {
            p.setPen(accent);
            p.setFont(Font(font(), 16 * s));
            p.drawText(QRectF(circle.left() - 60 * s, circle.bottom() + 6 * s,
                              circle.width() + 120 * s, 28 * s),
                       Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(Buttons[i].label));
        }
    }

    // Bottom bar: the keys and buttons that do something here.
    const double bar_y = h - BottomBar * s;
    p.setPen(QPen(sub, 1));
    p.drawLine(QPointF(36 * s, bar_y), QPointF(w - 36 * s, bar_y));
    const QString gap = QStringLiteral("        ");
    QString hint;
    if (!menu.empty()) {
        hint = QStringLiteral("Enter / ✕  Aceptar") + gap + QStringLiteral("Esc / ○  Volver");
    } else if (row == 0) {
        hint = QStringLiteral("Enter / ✕  Jugar") + gap + QStringLiteral("Tab / △  Opciones") + gap +
               QStringLiteral("F11  Pantalla completa");
    } else {
        hint = QStringLiteral("Enter / ✕  Aceptar") + gap + QStringLiteral("F11  Pantalla completa");
    }
    p.setPen(text);
    p.setFont(Font(font(), HintSize * 0.8 * s));
    p.drawText(QRectF(36 * s, bar_y, w - 88 * s, BottomBar * s), Qt::AlignVCenter | Qt::AlignRight,
               hint);

    // A menu over everything, in the centre.
    menu_rects.clear();
    menu_box = {};
    if (menu.empty()) {
        return;
    }
    p.fillRect(rect(), QColor(0, 0, 0, 170));
    const double menu_w = std::min(MenuWidth * s, w - 32.0);
    const double pad = 28 * s;
    const QFont item_font = Font(font(), MenuFont * s);
    const QFont title_font = Font(font(), MenuFont * 1.3 * s, QFont::Medium);
    const double item_h = QFontMetricsF(item_font).height() + MenuFont * s;
    const double title_box_h =
        QFontMetricsF(title_font)
            .boundingRect(QRectF(0, 0, menu_w - 2 * pad - 16 * s, 10000), Qt::TextWordWrap,
                          menu_title)
            .height() +
        16 * s;
    const int fit = std::max(
        1, static_cast<int>((h - 220 * s - title_box_h - 2 * pad) / (item_h + 4 * s)));
    const int count = static_cast<int>(menu.size());
    const int visible = std::min(count, fit);
    const int first = std::clamp(menu_index - visible / 2, 0, count - visible);
    const double box_h = 2 * pad + title_box_h + visible * (item_h + 4 * s);
    menu_box = QRectF((w - menu_w) / 2, (h - box_h) / 2, menu_w, box_h).toAlignedRect();
    p.setPen(Qt::NoPen);
    p.setBrush(panel);
    p.drawRoundedRect(menu_box, 8 * s, 8 * s);
    p.setPen(text);
    p.setFont(title_font);
    p.drawText(QRectF(menu_box.left() + pad + 8 * s, menu_box.top() + pad,
                      menu_w - 2 * pad - 16 * s, title_box_h),
               Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, menu_title);
    menu_rects.assign(menu.size(), QRect());
    p.setFont(item_font);
    for (int i = first; i < first + visible; ++i) {
        const QRectF item(menu_box.left() + pad,
                          menu_box.top() + pad + title_box_h + (i - first) * (item_h + 4 * s) +
                              2 * s,
                          menu_w - 2 * pad, item_h);
        menu_rects[i] = item.toAlignedRect();
        const bool on = i == menu_index;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            p.drawRoundedRect(item, 4 * s, 4 * s);
        }
        p.setPen(on ? QColor(Qt::white) : text);
        p.drawText(item.adjusted(14 * s, 0, -14 * s, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(menu[i].label(), Qt::ElideRight,
                                              static_cast<int>(item.width() - 28 * s)));
    }
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

void Home::keyPressEvent(QKeyEvent* event) {
    QString cmd;
    switch (event->key()) {
    case Qt::Key_Left:
        cmd = QStringLiteral("left");
        break;
    case Qt::Key_Right:
        cmd = QStringLiteral("right");
        break;
    case Qt::Key_Up:
        cmd = QStringLiteral("up");
        break;
    case Qt::Key_Down:
        cmd = QStringLiteral("down");
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        cmd = QStringLiteral("accept");
        break;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        cmd = QStringLiteral("back");
        break;
    case Qt::Key_Tab:
    case Qt::Key_Menu:
        cmd = QStringLiteral("options");
        break;
    case Qt::Key_F11:
        cmd = QStringLiteral("fullscreen");
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
    Nav(cmd);
}

void Home::Nav(const QString& cmd) {
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
        } else if (cmd == QStringLiteral("down")) {
            menu_index = (menu_index + 1) % count;
        } else if (cmd == QStringLiteral("back")) {
            CloseMenu();
            return;
        } else if ((cmd == QStringLiteral("left") || cmd == QStringLiteral("right")) && at.adjust) {
            at.adjust(cmd == QStringLiteral("left") ? -1 : 1);
        } else if (cmd == QStringLiteral("accept")) {
            if (at.adjust && !at.act) {
                at.adjust(1);
            } else {
                RunEntry(menu_index);
                return;
            }
        }
        update();
        return;
    }
    if (cmd == QStringLiteral("left") || cmd == QStringLiteral("right")) {
        const int d = cmd == QStringLiteral("left") ? -1 : 1;
        if (row == 0) {
            game_index += d;
        } else {
            button_index += d;
        }
    } else if (cmd == QStringLiteral("up")) {
        row = 0;
    } else if (cmd == QStringLiteral("down")) {
        row = 1;
    } else if (cmd == QStringLiteral("accept")) {
        if (row == 0 && !games.empty()) {
            Launch(games[game_index]);
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
}

void Home::Select() {
    if (games.empty()) {
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
    if (x - scroll < margin) {
        scroll = x - margin;
    } else if (x + cell - scroll > view - margin) {
        scroll = x + cell - view + margin;
    }
    const double max_scroll = std::max(0.0, games.size() * cell - view);
    scroll = std::clamp(scroll, 0.0, max_scroll);
    update();
}

void Home::mousePressEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (!menu.empty()) {
        for (int i = 0; i < static_cast<int>(menu_rects.size()); ++i) {
            if (!menu_rects[i].contains(pos)) {
                continue;
            }
            menu_index = i;
            if (event->button() == Qt::RightButton) {
                if (menu[i].adjust) {
                    menu[i].adjust(-1);
                }
            } else if (menu[i].adjust && !menu[i].act) {
                menu[i].adjust(1);
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
        return;
    }
    for (int i = 0; i < static_cast<int>(tile_rects.size()); ++i) {
        if (!tile_rects[i].contains(pos)) {
            continue;
        }
        const bool again = row == 0 && game_index == i;
        row = 0;
        game_index = i;
        Select();
        if (event->button() == Qt::RightButton) {
            GameMenu();
        } else if (again) {
            Launch(games[i]);
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
    if (menu.empty() && event->button() == Qt::LeftButton) {
        for (int i = 0; i < static_cast<int>(tile_rects.size()); ++i) {
            if (tile_rects[i].contains(pos)) {
                game_index = i;
                Launch(games[i]);
                return;
            }
        }
    }
    mousePressEvent(event);
}

void Home::wheelEvent(QWheelEvent* event) {
    if (!menu.empty() || games.empty()) {
        return;
    }
    const int steps = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    if (steps == 0) {
        return;
    }
    row = 0;
    game_index += steps > 0 ? -1 : 1;
    Select();
}

void Home::PollPad() {
    Core::HID::EmulatedController* controller = hooks.controller ? hooks.controller() : nullptr;
    if (!controller) {
        return;
    }
    const u64 buttons = static_cast<u64>(controller->GetNpadButtons().raw);
    const bool listening = isVisible() && window->isActiveWindow() &&
                           !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    const u64 pressed = buttons & ~pad_held;
    pad_held = buttons;
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

void Home::Launch(const Game& game) {
    if (game.native) {
        LaunchNative(game);
    } else {
        hooks.play(game.path, game.program_id);
    }
}

void Home::LaunchNative(const Game& game) {
#ifdef _WIN32
    if (native_process) {
        Message(QStringLiteral("Ya hay un juego abierto."));
        return;
    }
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
    native_timer->start();
    window->showMinimized();
#else
    Message(QStringLiteral("Los juegos nativos solo se abren en Windows."));
#endif
}

void Home::NativeEnded() {
#ifdef _WIN32
    native_timer->stop();
    if (native_process) {
        CloseHandle(native_process);
        native_process = nullptr;
    }
#endif
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
    update();
}

void Home::CloseMenu() {
    menu.clear();
    update();
}

void Home::RunEntry(int index) {
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

void Home::AddMenu() {
    OpenMenu(QStringLiteral("Agregar"),
             {
                 Entry{[] { return QStringLiteral("Carpeta de juegos de Switch"); },
                       [this] { hooks.add_folder(); }, {}, true},
                 Entry{[] { return QStringLiteral("Juego nativo (.exe)"); },
                       [this] { AddNative(); }, {}, true},
                 Entry{[] { return QStringLiteral("Importar juegos del Recomp Launcher"); },
                       [this] { ImportLauncher(); }, {}, true},
             });
}

void Home::SettingsMenu() {
    const auto yes_no = [](bool value) {
        return value ? QStringLiteral("Sí") : QStringLiteral("No");
    };
    const auto accent_step = [this](int d) {
        const QString current = Accent().name(QColor::HexRgb).toUpper();
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
                      const QString current = Accent().name(QColor::HexRgb).toUpper();
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
            Entry{[this] {
                      return QStringLiteral("Reloj: ") +
                             (Prefs().value(QStringLiteral("clock24")).toBool()
                                  ? QStringLiteral("24 horas")
                                  : QStringLiteral("12 horas"));
                  },
                  {},
                  toggle("clock24"),
                  false},
            Entry{[] { return QStringLiteral("Ajustes del emulador"); },
                  [this] { hooks.open_settings(); }, {}, true},
            Entry{[] { return QStringLiteral("Partidas guardadas"); },
                  [this] { hooks.open_saves(); }, {}, true},
            Entry{[] { return QStringLiteral("Lista clásica de juegos"); },
                  [this] { ShowList(); }, {}, true},
        });
}

void Home::GameMenu() {
    if (games.empty()) {
        return;
    }
    const Game game = games[game_index];
    std::vector<Entry> entries;
    entries.push_back(
        Entry{[] { return QStringLiteral("Jugar"); }, [this, game] { Launch(game); }, {}, true});
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
