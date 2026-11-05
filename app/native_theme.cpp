#include "native_theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QStyleOptionComplex>
#include <QStyleOptionGroupBox>
#include <QStyleOptionProgressBar>
#include <QStyleOptionSlider>
#include <QStyleOptionTab>
#include <QStyleOptionToolBar>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace edgeecho::ui {
namespace {

QColor lerpColor(const QColor &a, const QColor &b, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(
        static_cast<float>(a.redF() + (b.redF() - a.redF()) * clamped),
        static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * clamped),
        static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * clamped),
        static_cast<float>(a.alphaF() + (b.alphaF() - a.alphaF()) * clamped));
}

void ensureLinuxIconThemeConfigured() {
#ifdef __linux__
    static bool configured = false;
    if (configured)
        return;
    configured = true;
    QStringList paths = QIcon::themeSearchPaths();
    for (const QString &sysPath : {QStringLiteral("/usr/share/icons"),
                                   QStringLiteral("/usr/local/share/icons")}) {
        if (QDir(sysPath).exists() && !paths.contains(sysPath))
            paths.prepend(sysPath);
    }
    QIcon::setThemeSearchPaths(paths);
    if (QDir(QStringLiteral("/usr/share/icons/Yaru")).exists()) {
        QIcon::setThemeName(QStringLiteral("Yaru"));
        QIcon::setFallbackThemeName(QStringLiteral("Adwaita"));
    } else if (QDir(QStringLiteral("/usr/share/icons/Adwaita")).exists()) {
        QIcon::setThemeName(QStringLiteral("Adwaita"));
    }
#endif
}

QIcon tryLoadLinuxSystemPngIcon(const QStringList &relativeCandidates) {
    for (const QString &rel : relativeCandidates) {
        const QString full = QStringLiteral("/usr/share/icons/") + rel;
        if (QFile::exists(full)) {
            QPixmap px(full);
            if (!px.isNull())
                return QIcon(px);
        }
    }
    return {};
}

} // namespace

PlatformFlavor defaultPlatformFlavor() {
    if (const char *env = std::getenv("EDGEECHO_OS_STYLE")) {
        const QString val = QString::fromUtf8(env).trimmed().toLower();
        if (val == QStringLiteral("win11") || val == QStringLiteral("windows") ||
            val == QStringLiteral("fluent"))
            return PlatformFlavor::Windows11Fluent;
        if (val == QStringLiteral("linux") || val == QStringLiteral("ubuntu") ||
            val == QStringLiteral("yaru") || val == QStringLiteral("gtk"))
            return PlatformFlavor::LinuxUbuntuYaru;
    }
#ifdef _WIN32
    return PlatformFlavor::Windows11Fluent;
#else
    return PlatformFlavor::LinuxUbuntuYaru;
#endif
}

PlatformFlavor parsePlatformFlavor(const QString &name) {
    const QString lower = name.trimmed().toLower();
    if (lower == QStringLiteral("win11") || lower == QStringLiteral("windows") ||
        lower == QStringLiteral("fluent"))
        return PlatformFlavor::Windows11Fluent;
    if (lower == QStringLiteral("linux") || lower == QStringLiteral("ubuntu") ||
        lower == QStringLiteral("yaru") || lower == QStringLiteral("gtk"))
        return PlatformFlavor::LinuxUbuntuYaru;
    return defaultPlatformFlavor();
}

QString platformFlavorName(PlatformFlavor flavor) {
    return flavor == PlatformFlavor::Windows11Fluent
        ? QStringLiteral("Windows 11 Fluent (WinUI 3)")
        : QStringLiteral("Linux Ubuntu GNOME (GTK4 / Yaru)");
}

bool detectSystemDarkMode() {
    if (const char *env = std::getenv("EDGEECHO_SYSTEM_THEME")) {
        const QString val = QString::fromUtf8(env).trimmed().toLower();
        if (val == QStringLiteral("dark") || val == QStringLiteral("1"))
            return true;
        if (val == QStringLiteral("light") || val == QStringLiteral("0"))
            return false;
    }
#ifdef _WIN32
    DWORD useLight = 1;
    DWORD dataSize = sizeof(useLight);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &useLight,
                     &dataSize) == ERROR_SUCCESS) {
        return useLight == 0;
    }
#elif defined(__linux__)
    if (const char *gtkTheme = std::getenv("GTK_THEME")) {
        if (QString::fromUtf8(gtkTheme).toLower().contains(QStringLiteral("dark")))
            return true;
    }
    const QString home = QDir::homePath();
    for (const QString &iniPath : {home + QStringLiteral("/.config/gtk-4.0/settings.ini"),
                                   home + QStringLiteral("/.config/gtk-3.0/settings.ini")}) {
        QFile f(iniPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll()).toLower();
            if (content.contains(QStringLiteral("gtk-application-prefer-dark-theme=1")) ||
                content.contains(QStringLiteral("gtk-application-prefer-dark-theme=true")) ||
                content.contains(QStringLiteral("-dark")))
                return true;
        }
    }
    if (const char *platform = std::getenv("QT_QPA_PLATFORM")) {
        if (QString::fromUtf8(platform) == QStringLiteral("offscreen"))
            return false;
    }
    if (QFile::exists(QStringLiteral("/usr/bin/gsettings"))) {
        QProcess proc;
        proc.start(QStringLiteral("/usr/bin/gsettings"),
                   {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                    QStringLiteral("color-scheme")});
        if (proc.waitForFinished(180) && proc.exitCode() == 0) {
            const QString out = QString::fromUtf8(proc.readAllStandardOutput()).toLower();
            if (out.contains(QStringLiteral("prefer-dark")))
                return true;
            if (out.contains(QStringLiteral("default")) || out.contains(QStringLiteral("prefer-light")))
                return false;
        }
    }
#endif
    return false;
}

ThemeSpec makeThemeSpec(PlatformFlavor flavor, bool dark) {
    ThemeSpec s;
    s.flavor = flavor;
    s.dark = dark;
    s.transitionProgress = 1.0;

    if (flavor == PlatformFlavor::Windows11Fluent) {
        if (!dark) {
            // Windows 11 Light Mode (Mica + WinUI 3 Layer Fill)
            s.windowBg = QColor("#F3F3F3");
            s.headerBg = QColor("#EFEFEF");
            s.cardBg = QColor("#FFFFFF");
            s.cardAltBg = QColor("#F9F9F9");
            s.controlBg = QColor("#FBFBFB");
            s.controlHoverBg = QColor("#F5F5F5");
            s.controlPressBg = QColor("#EAEAEA");
            s.inputBg = QColor("#FFFFFF");
            s.textPrimary = QColor("#1A1A1A");
            s.textSecondary = QColor("#5C5C5C");
            s.textDisabled = QColor("#9E9E9E");
            s.borderSubtle = QColor("#E5E5E5");
            s.borderStrong = QColor("#CCCCCC");
            s.accent = QColor("#0067C0");
            s.accentHover = QColor("#1975C5");
            s.accentText = QColor("#FFFFFF");
            s.plotCanvasBg = QColor("#FFFFFF");
            s.plotFrameBg = QColor("#F8F9FA");
            s.plotGrid = QColor("#E6E8EB");
            s.badgeBg = QColor(249, 249, 249, 238);
            s.tracePrimary = QColor("#0067C0");
            s.traceSecondary = QColor("#0F7B0F");
            s.traceTertiary = QColor("#C42B1C");
        } else {
            // Windows 11 Dark Mode (Dark Mica + WinUI 3 Dark Layer)
            s.windowBg = QColor("#202020");
            s.headerBg = QColor("#1C1C1C");
            s.cardBg = QColor("#2B2B2B");
            s.cardAltBg = QColor("#252525");
            s.controlBg = QColor("#323232");
            s.controlHoverBg = QColor("#3B3B3B");
            s.controlPressBg = QColor("#282828");
            s.inputBg = QColor("#272727");
            s.textPrimary = QColor("#F5F5F5");
            s.textSecondary = QColor("#ABABAB");
            s.textDisabled = QColor("#6E6E6E");
            s.borderSubtle = QColor("#383838");
            s.borderStrong = QColor("#4A4A4A");
            s.accent = QColor("#60CDFF");
            s.accentHover = QColor("#78D5FF");
            s.accentText = QColor("#001A29");
            s.plotCanvasBg = QColor("#1A1A1A");
            s.plotFrameBg = QColor("#242424");
            s.plotGrid = QColor("#333333");
            s.badgeBg = QColor(43, 43, 43, 235);
            s.tracePrimary = QColor("#60CDFF");
            s.traceSecondary = QColor("#6CCB5F");
            s.traceTertiary = QColor("#FF99A4");
        }
    } else {
        // Linux Ubuntu GNOME (GTK4 Libadwaita + Yaru Palette)
        if (!dark) {
            s.windowBg = QColor("#FAFAFA");
            s.headerBg = QColor("#EBE9E7");
            s.cardBg = QColor("#FFFFFF");
            s.cardAltBg = QColor("#F6F5F4");
            s.controlBg = QColor("#F6F5F4");
            s.controlHoverBg = QColor("#ECEAE8");
            s.controlPressBg = QColor("#DFDCD8");
            s.inputBg = QColor("#FFFFFF");
            s.textPrimary = QColor("#2E2E2E");
            s.textSecondary = QColor("#676767");
            s.textDisabled = QColor("#A5A5A5");
            s.borderSubtle = QColor("#DEDDDA");
            s.borderStrong = QColor("#C6C4C0");
            s.accent = QColor("#E95420");
            s.accentHover = QColor("#F06535");
            s.accentText = QColor("#FFFFFF");
            s.plotCanvasBg = QColor("#FFFFFF");
            s.plotFrameBg = QColor("#F6F5F4");
            s.plotGrid = QColor("#E8E6E3");
            s.badgeBg = QColor(255, 255, 255, 236);
            s.tracePrimary = QColor("#E95420");
            s.traceSecondary = QColor("#26A269");
            s.traceTertiary = QColor("#1C71D8");
        } else {
            s.windowBg = QColor("#1E1E1E");
            s.headerBg = QColor("#282828");
            s.cardBg = QColor("#2A2A2A");
            s.cardAltBg = QColor("#242424");
            s.controlBg = QColor("#343434");
            s.controlHoverBg = QColor("#3E3E3E");
            s.controlPressBg = QColor("#2B2B2B");
            s.inputBg = QColor("#242424");
            s.textPrimary = QColor("#F2F2F2");
            s.textSecondary = QColor("#A8A8A8");
            s.textDisabled = QColor("#6E6E6E");
            s.borderSubtle = QColor("#3B3B3B");
            s.borderStrong = QColor("#4D4D4D");
            s.accent = QColor("#E95420");
            s.accentHover = QColor("#F26C3D");
            s.accentText = QColor("#FFFFFF");
            s.plotCanvasBg = QColor("#171717");
            s.plotFrameBg = QColor("#232323");
            s.plotGrid = QColor("#323232");
            s.badgeBg = QColor(42, 42, 42, 236);
            s.tracePrimary = QColor("#FF7849");
            s.traceSecondary = QColor("#57E389");
            s.traceTertiary = QColor("#62A0EA");
        }
    }
    return s;
}

ThemeSpec interpolateThemeSpec(const ThemeSpec &from, const ThemeSpec &to, double t) {
    ThemeSpec out = to;
    out.transitionProgress = std::clamp(t, 0.0, 1.0);
    out.windowBg = lerpColor(from.windowBg, to.windowBg, t);
    out.headerBg = lerpColor(from.headerBg, to.headerBg, t);
    out.cardBg = lerpColor(from.cardBg, to.cardBg, t);
    out.cardAltBg = lerpColor(from.cardAltBg, to.cardAltBg, t);
    out.controlBg = lerpColor(from.controlBg, to.controlBg, t);
    out.controlHoverBg = lerpColor(from.controlHoverBg, to.controlHoverBg, t);
    out.controlPressBg = lerpColor(from.controlPressBg, to.controlPressBg, t);
    out.inputBg = lerpColor(from.inputBg, to.inputBg, t);
    out.textPrimary = lerpColor(from.textPrimary, to.textPrimary, t);
    out.textSecondary = lerpColor(from.textSecondary, to.textSecondary, t);
    out.textDisabled = lerpColor(from.textDisabled, to.textDisabled, t);
    out.borderSubtle = lerpColor(from.borderSubtle, to.borderSubtle, t);
    out.borderStrong = lerpColor(from.borderStrong, to.borderStrong, t);
    out.accent = lerpColor(from.accent, to.accent, t);
    out.accentHover = lerpColor(from.accentHover, to.accentHover, t);
    out.accentText = lerpColor(from.accentText, to.accentText, t);
    out.plotCanvasBg = lerpColor(from.plotCanvasBg, to.plotCanvasBg, t);
    out.plotFrameBg = lerpColor(from.plotFrameBg, to.plotFrameBg, t);
    out.plotGrid = lerpColor(from.plotGrid, to.plotGrid, t);
    out.badgeBg = lerpColor(from.badgeBg, to.badgeBg, t);
    out.tracePrimary = lerpColor(from.tracePrimary, to.tracePrimary, t);
    out.traceSecondary = lerpColor(from.traceSecondary, to.traceSecondary, t);
    out.traceTertiary = lerpColor(from.traceTertiary, to.traceTertiary, t);
    return out;
}

QPalette buildPalette(const ThemeSpec &spec) {
    QPalette pal;
    pal.setColor(QPalette::Window, spec.windowBg);
    pal.setColor(QPalette::WindowText, spec.textPrimary);
    pal.setColor(QPalette::Base, spec.inputBg);
    pal.setColor(QPalette::AlternateBase, spec.cardAltBg);
    pal.setColor(QPalette::ToolTipBase, spec.cardBg);
    pal.setColor(QPalette::ToolTipText, spec.textPrimary);
    pal.setColor(QPalette::PlaceholderText, spec.textSecondary);
    pal.setColor(QPalette::Text, spec.textPrimary);
    pal.setColor(QPalette::Button, spec.controlBg);
    pal.setColor(QPalette::ButtonText, spec.textPrimary);
    pal.setColor(QPalette::BrightText, spec.accentText);
    pal.setColor(QPalette::Light, spec.cardBg);
    pal.setColor(QPalette::Midlight, spec.controlHoverBg);
    pal.setColor(QPalette::Mid, spec.borderSubtle);
    pal.setColor(QPalette::Dark, spec.borderStrong);
    pal.setColor(QPalette::Shadow, QColor(0, 0, 0, spec.dark ? 130 : 40));
    pal.setColor(QPalette::Highlight, spec.accent);
    pal.setColor(QPalette::HighlightedText, spec.accentText);
    pal.setColor(QPalette::Link, spec.accent);

    pal.setColor(QPalette::Disabled, QPalette::WindowText, spec.textDisabled);
    pal.setColor(QPalette::Disabled, QPalette::Text, spec.textDisabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, spec.textDisabled);
    pal.setColor(QPalette::Disabled, QPalette::Base, spec.cardAltBg);
    pal.setColor(QPalette::Disabled, QPalette::Button, spec.cardAltBg);
    return pal;
}

QIcon createNativeIcon(NativeIconKind kind, const ThemeSpec &spec) {
    ensureLinuxIconThemeConfigured();

    if (spec.flavor == PlatformFlavor::LinuxUbuntuYaru) {
        switch (kind) {
        case NativeIconKind::OpenDatabase: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/actions/document-open.png"),
                QStringLiteral("Yaru/22x22/actions/document-open.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        case NativeIconKind::OpenFolder: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/status/folder-open.png"),
                QStringLiteral("Yaru/24x24@2x/places/folder.png"),
                QStringLiteral("Adwaita/16x16/places/folder.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        case NativeIconKind::NewDemo: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/actions/folder-new.png"),
                QStringLiteral("Yaru/22x22/actions/folder-new.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        case NativeIconKind::ExportCsv: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/actions/document-save.png"),
                QStringLiteral("Yaru/24x24@2x/actions/document-save-as.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        case NativeIconKind::ExportWav:
        case NativeIconKind::AudioWave: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/devices/audio-speakers.png"),
                QStringLiteral("Yaru/24x24@2x/mimetypes/audio-x-wav.png"),
                QStringLiteral("Adwaita/16x16/mimetypes/audio-x-generic.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        case NativeIconKind::HelpInfo: {
            auto ic = tryLoadLinuxSystemPngIcon({
                QStringLiteral("Yaru/24x24@2x/status/dialog-information.png"),
                QStringLiteral("Yaru/24x24@2x/apps/help-app.png"),
            });
            if (!ic.isNull())
                return ic;
            break;
        }
        default:
            break;
        }
    }

    // Crisp hi-DPI OS-native vector icon rendering (Windows 11 Segoe Fluent & Linux GNOME Symbolic)
    QPixmap px(36, 36);
    px.setDevicePixelRatio(2.0);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor fg = spec.textPrimary;
    const QColor accent = spec.accent;
    const bool win11 = (spec.flavor == PlatformFlavor::Windows11Fluent);

    switch (kind) {
    case NativeIconKind::OpenDatabase: {
        // Win11 Fluent Cylinder Database / GNOME Database
        p.setPen(QPen(accent, 1.5));
        p.setBrush(win11 ? QColor(accent.red(), accent.green(), accent.blue(), 45) : Qt::NoBrush);
        p.drawEllipse(QRectF(3.5, 2.5, 11.0, 3.8));
        QPainterPath body;
        body.moveTo(3.5, 4.4);
        body.lineTo(3.5, 13.2);
        body.arcTo(QRectF(3.5, 11.3, 11.0, 3.8), 180.0, 180.0);
        body.lineTo(14.5, 4.4);
        p.drawPath(body);
        p.drawArc(QRectF(3.5, 7.0, 11.0, 3.4), 180 * 16, 180 * 16);
        break;
    }
    case NativeIconKind::OpenFolder: {
        const QColor folderFill = win11 ? QColor("#F5B82E") : accent;
        const QColor folderStroke = win11 ? QColor("#D49613") : accent.darker(115);
        p.setPen(QPen(folderStroke, 1.2));
        p.setBrush(folderFill);
        QPainterPath path;
        path.moveTo(2.0, 4.5);
        path.lineTo(6.8, 4.5);
        path.lineTo(8.3, 6.2);
        path.lineTo(15.8, 6.2);
        path.lineTo(15.8, 14.2);
        path.lineTo(2.0, 14.2);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case NativeIconKind::NewDemo: {
        p.setPen(QPen(accent, 1.5));
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 38));
        p.drawRoundedRect(QRectF(2.5, 3.5, 13.0, 11.0), 2.5, 2.5);
        p.setPen(QPen(fg, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9.0, 6.2), QPointF(9.0, 11.8));
        p.drawLine(QPointF(6.2, 9.0), QPointF(11.8, 9.0));
        break;
    }
    case NativeIconKind::ExportCsv: {
        p.setPen(QPen(fg, 1.4));
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 28));
        p.drawRoundedRect(QRectF(3.0, 2.5, 12.0, 13.0), 2.0, 2.0);
        p.setPen(QPen(accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(9.0, 5.2), QPointF(9.0, 11.2));
        p.drawLine(QPointF(6.5, 8.8), QPointF(9.0, 11.4));
        p.drawLine(QPointF(11.5, 8.8), QPointF(9.0, 11.4));
        break;
    }
    case NativeIconKind::ExportWav:
    case NativeIconKind::AudioWave: {
        p.setPen(QPen(accent, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath spk;
        spk.moveTo(2.8, 7.2);
        spk.lineTo(5.4, 7.2);
        spk.lineTo(8.6, 4.2);
        spk.lineTo(8.6, 13.8);
        spk.lineTo(5.4, 10.8);
        spk.lineTo(2.8, 10.8);
        spk.closeSubpath();
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 60));
        p.drawPath(spk);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(8.2, 6.0, 4.2, 6.0), -55 * 16, 110 * 16);
        p.drawArc(QRectF(9.2, 4.0, 6.2, 10.0), -55 * 16, 110 * 16);
        break;
    }
    case NativeIconKind::ToggleDrawer: {
        p.setPen(QPen(fg, 1.4));
        p.drawRoundedRect(QRectF(2.5, 3.0, 13.0, 12.0), 2.2, 2.2);
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 95));
        p.drawRoundedRect(QRectF(2.5, 10.2, 13.0, 4.8), 1.5, 1.5);
        break;
    }
    case NativeIconKind::ThemeSwitch: {
        if (spec.dark) {
            // Crescent Moon icon for Dark mode
            p.setPen(QPen(QColor("#F7C948"), 1.5));
            p.setBrush(QColor(247, 201, 72, 75));
            QPainterPath moon;
            moon.addEllipse(QRectF(3.5, 3.5, 11.0, 11.0));
            QPainterPath cut;
            cut.addEllipse(QRectF(6.8, 2.2, 10.0, 10.0));
            p.drawPath(moon.subtracted(cut));
        } else {
            // Crisp Sun icon for Light mode
            p.setPen(QPen(accent, 1.5, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 55));
            p.drawEllipse(QPointF(9.0, 9.0), 3.4, 3.4);
            for (int i = 0; i < 8; ++i) {
                const double a = i * 3.14159265 / 4.0;
                p.drawLine(QPointF(9.0 + std::cos(a) * 5.0, 9.0 + std::sin(a) * 5.0),
                           QPointF(9.0 + std::cos(a) * 6.8, 9.0 + std::sin(a) * 6.8));
            }
        }
        break;
    }
    case NativeIconKind::HelpInfo: {
        p.setPen(QPen(accent, 1.5));
        p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 35));
        p.drawEllipse(QRectF(2.5, 2.5, 13.0, 13.0));
        p.setPen(QPen(fg, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawPoint(QPointF(9.0, 6.0));
        p.drawLine(QPointF(9.0, 8.5), QPointF(9.0, 12.5));
        break;
    }
    case NativeIconKind::Reload: {
        p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawArc(QRectF(3.5, 3.5, 11.0, 11.0), 35 * 16, 285 * 16);
        QPainterPath arrow;
        arrow.moveTo(14.2, 3.2);
        arrow.lineTo(14.8, 7.2);
        arrow.lineTo(10.8, 6.6);
        p.drawPath(arrow);
        break;
    }
    case NativeIconKind::Play: {
        p.setPen(QPen(accent, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(accent);
        QPainterPath tri;
        tri.moveTo(5.0, 3.6);
        tri.lineTo(14.2, 9.0);
        tri.lineTo(5.0, 14.4);
        tri.closeSubpath();
        p.drawPath(tri);
        break;
    }
    case NativeIconKind::Pause: {
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(4.5, 3.8, 3.2, 10.4), 1.0, 1.0);
        p.drawRoundedRect(QRectF(10.3, 3.8, 3.2, 10.4), 1.0, 1.0);
        break;
    }
    case NativeIconKind::Stop: {
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(4.2, 4.2, 9.6, 9.6), 1.6, 1.6);
        break;
    }
    case NativeIconKind::SensorScalar: {
        p.setPen(QPen(accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath wave;
        wave.moveTo(2.0, 10.5);
        wave.lineTo(5.2, 10.5);
        wave.lineTo(7.2, 4.2);
        wave.lineTo(10.4, 13.8);
        wave.lineTo(12.6, 8.2);
        wave.lineTo(16.0, 8.2);
        p.drawPath(wave);
        break;
    }
    }
    p.end();
    return QIcon(px);
}

void applyNativeWindowFrame(QWidget *window, const ThemeSpec &spec) {
    if (!window)
        return;
#ifdef _WIN32
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        if (auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            HWND hwnd = reinterpret_cast<HWND>(window->winId());
            const BOOL darkVal = spec.dark ? TRUE : FALSE;
            fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkVal, sizeof(darkVal));
            fn(hwnd, 19 /* Pre-20H1 dark mode attribute */, &darkVal, sizeof(darkVal));
            const DWORD cornerRound = 2 /* DWMWCP_ROUND */;
            fn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerRound, sizeof(cornerRound));
            const DWORD micaBackdrop = 2 /* DWMSBT_MAINWINDOW */;
            fn(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &micaBackdrop, sizeof(micaBackdrop));
        }
        FreeLibrary(dwm);
    }
#else
    (void)spec;
#endif
}

NativeOsProxyStyle::NativeOsProxyStyle(const ThemeSpec &spec)
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))), spec_(spec) {}

void NativeOsProxyStyle::setThemeSpec(const ThemeSpec &spec) {
    spec_ = spec;
}

int NativeOsProxyStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                                    const QWidget *widget) const {
    const bool win11 = (spec_.flavor == PlatformFlavor::Windows11Fluent);
    switch (metric) {
    case PM_ButtonMargin:
        return win11 ? 10 : 12;
    case PM_ToolBarIconSize:
        return 18;
    case PM_SmallIconSize:
        return 16;
    case PM_SliderThickness:
        return 22;
    case PM_SliderControlThickness:
        return 18;
    case PM_SliderLength:
        return 18;
    case PM_IndicatorWidth:
        return win11 ? 38 : 34;
    case PM_IndicatorHeight:
        return 18;
    case PM_TabBarTabHSpace:
        return 22;
    case PM_TabBarTabVSpace:
        return 10;
    case PM_SplitterWidth:
        return 6;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QRect NativeOsProxyStyle::subElementRect(SubElement element, const QStyleOption *option,
                                         const QWidget *widget) const {
    if (element == SE_CheckBoxIndicator && option) {
        const int w = pixelMetric(PM_IndicatorWidth, option, widget);
        const int h = pixelMetric(PM_IndicatorHeight, option, widget);
        const int y = option->rect.y() + (option->rect.height() - h) / 2;
        return QRect(option->rect.x(), y, w, h);
    }
    if (element == SE_CheckBoxContents && option) {
        const int w = pixelMetric(PM_IndicatorWidth, option, widget) + 7;
        return QRect(option->rect.x() + w, option->rect.y(),
                     std::max(0, option->rect.width() - w), option->rect.height());
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

void NativeOsProxyStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                                       QPainter *painter, const QWidget *widget) const {
    if (!option || !painter)
        return;
    const bool win11 = (spec_.flavor == PlatformFlavor::Windows11Fluent);
    const bool enabled = (option->state & State_Enabled);
    const bool hover = enabled && (option->state & State_MouseOver);
    const bool sunken = enabled && (option->state & (State_Sunken | State_On));
    const bool hasFocus = enabled && (option->state & State_HasFocus);

    if (element == PE_PanelButtonCommand || element == PE_PanelButtonTool) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
        const double radius = win11 ? 4.5 : 6.5;

        QColor fill = spec_.controlBg;
        if (!enabled)
            fill = spec_.cardAltBg;
        else if (sunken)
            fill = spec_.controlPressBg;
        else if (hover)
            fill = spec_.controlHoverBg;

        if (element == PE_PanelButtonTool && !hover && !sunken)
            fill = Qt::transparent;

        if (fill.alpha() > 0) {
            painter->setPen(QPen(spec_.borderSubtle, 1.0));
            painter->setBrush(fill);
            painter->drawRoundedRect(r, radius, radius);

            if (win11 && enabled && !sunken) {
                // Windows 11 Fluent control bottom/top elevation stroke
                painter->setPen(QPen(spec_.borderStrong, 1.2));
                if (!spec_.dark)
                    painter->drawLine(QPointF(r.left() + radius, r.bottom()),
                                      QPointF(r.right() - radius, r.bottom()));
                else
                    painter->drawLine(QPointF(r.left() + radius, r.top()),
                                      QPointF(r.right() - radius, r.top()));
            }
        }
        painter->restore();
        return;
    }

    if (element == PE_PanelLineEdit) {
        if (widget && qobject_cast<const QAbstractSpinBox *>(widget->parentWidget())) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(option->rect).adjusted(0.8, 0.8, -0.8, -0.8);
        const double radius = win11 ? 4.0 : 6.0;
        painter->setBrush(enabled ? spec_.inputBg : spec_.cardAltBg);
        if (!win11 && hasFocus) {
            painter->setPen(QPen(spec_.accent, 1.8));
        } else {
            painter->setPen(QPen(spec_.borderSubtle, 1.0));
        }
        painter->drawRoundedRect(r, radius, radius);

        if (win11) {
            // Win11 Fluent bottom accent line on TextBox
            const QColor bottomColor = hasFocus ? spec_.accent : spec_.borderStrong;
            const double thickness = hasFocus ? 2.0 : 1.1;
            painter->setPen(QPen(bottomColor, thickness));
            painter->drawLine(QPointF(r.left() + 3.0, r.bottom()),
                              QPointF(r.right() - 3.0, r.bottom()));
        }
        painter->restore();
        return;
    }

    if (element == PE_IndicatorCheckBox) {
        // Native OS Toggle Switch (Win11 ToggleSwitch / Linux GTK4 Switch)
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
        const bool checked = (option->state & State_On);
        const double radius = r.height() / 2.0;

        QColor trackFill = checked ? spec_.accent : (win11 ? Qt::transparent : spec_.borderStrong);
        if (hover && checked)
            trackFill = spec_.accentHover;
        QColor trackBorder = checked ? spec_.accent : spec_.borderStrong;

        painter->setPen(QPen(trackBorder, 1.3));
        painter->setBrush(trackFill);
        painter->drawRoundedRect(r, radius, radius);

        const double knobSize = r.height() - (win11 && !checked ? 6.0 : 4.4);
        const double knobX = checked ? (r.right() - knobSize - 2.4) : (r.left() + 2.4);
        const double knobY = r.center().y() - knobSize / 2.0;
        QColor knobColor = checked
            ? (win11 && spec_.dark ? QColor("#001A29") : QColor("#FFFFFF"))
            : (win11 ? spec_.textSecondary : QColor("#FFFFFF"));
        painter->setPen(Qt::NoPen);
        painter->setBrush(knobColor);
        painter->drawEllipse(QRectF(knobX, knobY, knobSize, knobSize));
        painter->restore();
        return;
    }

    if (element == PE_FrameFocusRect) {
        return;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void NativeOsProxyStyle::drawControl(ControlElement element, const QStyleOption *option,
                                     QPainter *painter, const QWidget *widget) const {
    if (!option || !painter)
        return;
    const bool win11 = (spec_.flavor == PlatformFlavor::Windows11Fluent);

    if (element == CE_ToolBar) {
        painter->save();
        painter->fillRect(option->rect, spec_.headerBg);
        painter->setPen(QPen(spec_.borderSubtle, 1.0));
        painter->drawLine(option->rect.bottomLeft(), option->rect.bottomRight());
        painter->restore();
        return;
    }

    if (element == CE_MenuBarEmptyArea) {
        painter->fillRect(option->rect, spec_.headerBg);
        return;
    }

    if (element == CE_MenuBarItem) {
        if (const auto *mbi = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->fillRect(mbi->rect, spec_.headerBg);
            const bool active = (mbi->state & (State_Selected | State_Sunken));
            if (active) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(spec_.controlHoverBg);
                painter->drawRoundedRect(QRectF(mbi->rect).adjusted(2, 2, -2, -2),
                                         win11 ? 4.0 : 6.0, win11 ? 4.0 : 6.0);
            }
            painter->setPen(spec_.textPrimary);
            painter->drawText(mbi->rect, Qt::AlignCenter | Qt::TextShowMnemonic, mbi->text);
            painter->restore();
            return;
        }
    }

    if (element == CE_TabBarTab) {
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const bool selected = (tab->state & State_Selected);
            const bool hover = (tab->state & State_MouseOver);
            const QRectF r = QRectF(tab->rect).adjusted(2.0, 2.0, -2.0, -2.0);

            if (win11) {
                // Windows 11 Pivot / SelectorBar Tab with rounded bottom accent pill
                if (selected || hover) {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(selected ? spec_.cardBg : spec_.controlHoverBg);
                    painter->drawRoundedRect(r, 5.0, 5.0);
                }
                if (selected) {
                    const double pillW = std::min(36.0, r.width() * 0.45);
                    const QRectF pill(r.center().x() - pillW / 2.0, r.bottom() - 3.0, pillW, 3.0);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(spec_.accent);
                    painter->drawRoundedRect(pill, 1.5, 1.5);
                }
            } else {
                // Linux GTK4 / Libadwaita Segmented ViewSwitcher Pill Tab
                if (selected) {
                    painter->setPen(QPen(spec_.borderSubtle, 1.0));
                    painter->setBrush(spec_.cardBg);
                    painter->drawRoundedRect(r, 6.5, 6.5);
                    painter->setPen(QPen(spec_.accent, 2.0));
                    painter->drawLine(QPointF(r.left() + 8.0, r.bottom() - 0.5),
                                      QPointF(r.right() - 8.0, r.bottom() - 0.5));
                } else if (hover) {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(spec_.controlHoverBg);
                    painter->drawRoundedRect(r, 6.5, 6.5);
                }
            }

            QFont f = painter->font();
            f.setBold(selected);
            painter->setFont(f);
            painter->setPen(selected ? spec_.textPrimary : spec_.textSecondary);
            painter->drawText(tab->rect, Qt::AlignCenter | Qt::TextShowMnemonic, tab->text);
            painter->restore();
            return;
        }
    }

    if (element == CE_ProgressBarGroove || element == CE_ProgressBarContents ||
        element == CE_ProgressBar) {
        if (const auto *pb = qstyleoption_cast<const QStyleOptionProgressBar *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const QRectF r = QRectF(pb->rect).adjusted(1.0, 1.0, -1.0, -1.0);
            const double radius = r.height() / 2.0;
            painter->setPen(QPen(spec_.borderSubtle, 1.0));
            painter->setBrush(spec_.cardAltBg);
            painter->drawRoundedRect(r, radius, radius);

            if (pb->maximum > pb->minimum && pb->progress > pb->minimum) {
                const double frac = std::clamp(
                    static_cast<double>(pb->progress - pb->minimum) /
                        static_cast<double>(pb->maximum - pb->minimum),
                    0.0, 1.0);
                const QRectF fillRect(r.left(), r.top(), std::max(radius * 2.0, r.width() * frac),
                                      r.height());
                QLinearGradient grad(fillRect.topLeft(), fillRect.topRight());
                grad.setColorAt(0.0, spec_.accent);
                grad.setColorAt(1.0, spec_.traceSecondary);
                painter->setPen(Qt::NoPen);
                painter->setBrush(grad);
                painter->drawRoundedRect(fillRect, radius, radius);
            }
            painter->restore();
            return;
        }
    }

    QProxyStyle::drawControl(element, option, painter, widget);
}

void NativeOsProxyStyle::drawComplexControl(ComplexControl control,
                                            const QStyleOptionComplex *option,
                                            QPainter *painter, const QWidget *widget) const {
    if (!option || !painter)
        return;
    const bool win11 = (spec_.flavor == PlatformFlavor::Windows11Fluent);

    if (control == CC_GroupBox) {
        if (const auto *gb = qstyleoption_cast<const QStyleOptionGroupBox *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const bool hasTitle = !gb->text.isEmpty();
            const int topOffset = hasTitle ? 22 : 2;
            const QRectF cardRect = QRectF(gb->rect).adjusted(1.0, topOffset, -1.0, -1.0);
            const double radius = win11 ? 7.0 : 9.0;

            painter->setPen(QPen(spec_.borderSubtle, 1.0));
            painter->setBrush(spec_.cardBg);
            painter->drawRoundedRect(cardRect, radius, radius);

            if (hasTitle) {
                QFont f = painter->font();
                f.setBold(true);
                painter->setFont(f);
                painter->setPen(spec_.textPrimary);
                const QRect titleRect(gb->rect.left() + 6, gb->rect.top(),
                                      gb->rect.width() - 12, 20);
                painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, gb->text);
            }
            painter->restore();
            return;
        }
    }

    if (control == CC_Slider) {
        if (const auto *sl = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const QRect groove = subControlRect(CC_Slider, sl, SC_SliderGroove, widget);
            const QRect handle = subControlRect(CC_Slider, sl, SC_SliderHandle, widget);

            const double trackH = win11 ? 4.0 : 6.0;
            const QRectF trackRect(groove.left() + 4.0,
                                   groove.center().y() - trackH / 2.0,
                                   std::max(8.0, groove.width() - 8.0), trackH);
            painter->setPen(Qt::NoPen);
            painter->setBrush(spec_.borderStrong);
            painter->drawRoundedRect(trackRect, trackH / 2.0, trackH / 2.0);

            const double fillW = std::clamp(
                handle.center().x() - trackRect.left(), 0.0, trackRect.width());
            if (fillW > 1.0) {
                painter->setBrush(spec_.accent);
                painter->drawRoundedRect(
                    QRectF(trackRect.left(), trackRect.top(), fillW, trackH),
                    trackH / 2.0, trackH / 2.0);
            }

            const QPointF center = QRectF(handle).center();
            const bool hover = (sl->state & State_MouseOver);
            const bool pressed = (sl->state & State_Sunken);

            if (win11) {
                // Windows 11 Fluent Slider Thumb: outer ring + inner accent circle
                const double outerR = 8.5;
                const double innerR = pressed ? 3.8 : (hover ? 5.5 : 4.5);
                painter->setPen(QPen(spec_.borderStrong, 1.0));
                painter->setBrush(spec_.dark ? QColor("#454545") : QColor("#FFFFFF"));
                painter->drawEllipse(center, outerR, outerR);
                painter->setPen(Qt::NoPen);
                painter->setBrush(spec_.accent);
                painter->drawEllipse(center, innerR, innerR);
            } else {
                // Linux GTK4 / Yaru Slider Thumb: elevated white circular knob
                const double knobR = hover ? 8.2 : 7.5;
                painter->setPen(QPen(spec_.borderStrong, 1.2));
                painter->setBrush(spec_.dark ? QColor("#F2F2F2") : QColor("#FFFFFF"));
                painter->drawEllipse(center, knobR, knobR);
            }
            painter->restore();
            return;
        }
    }

    if (control == CC_ComboBox) {
        if (const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(option)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const QRectF r = QRectF(cb->rect).adjusted(1.0, 1.0, -1.0, -1.0);
            const double radius = win11 ? 4.5 : 6.5;
            const bool hover = (cb->state & State_MouseOver);

            painter->setPen(QPen(spec_.borderSubtle, 1.0));
            painter->setBrush(hover ? spec_.controlHoverBg : spec_.controlBg);
            painter->drawRoundedRect(r, radius, radius);

            if (win11) {
                painter->setPen(QPen(
                    (cb->state & State_HasFocus) ? spec_.accent : spec_.borderStrong,
                    (cb->state & State_HasFocus) ? 2.0 : 1.1));
                painter->drawLine(QPointF(r.left() + 4.0, r.bottom()),
                                  QPointF(r.right() - 4.0, r.bottom()));
            }

            // Crisp chevron arrow on the right
            const double cx = r.right() - 13.0;
            const double cy = r.center().y();
            painter->setPen(QPen(spec_.textSecondary, 1.5, Qt::SolidLine,
                                 Qt::RoundCap, Qt::RoundJoin));
            painter->drawLine(QPointF(cx - 3.5, cy - 1.5), QPointF(cx, cy + 2.0));
            painter->drawLine(QPointF(cx, cy + 2.0), QPointF(cx + 3.5, cy - 1.5));
            painter->restore();
            return;
        }
    }

    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

} // namespace edgeecho::ui

