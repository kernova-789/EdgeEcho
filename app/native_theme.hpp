#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QProxyStyle>
#include <QString>
#include <QWidget>

namespace edgeecho::ui {

enum class PlatformFlavor {
    LinuxUbuntuYaru,
    Windows11Fluent
};

enum class ThemePreference {
    FollowSystem,
    Light,
    Dark
};

enum class NativeIconKind {
    OpenDatabase,
    OpenFolder,
    NewDemo,
    ExportCsv,
    ExportWav,
    ToggleDrawer,
    ThemeSwitch,
    HelpInfo,
    Reload,
    Play,
    Pause,
    Stop,
    AudioWave,
    SensorScalar
};

struct ThemeSpec {
    PlatformFlavor flavor = PlatformFlavor::LinuxUbuntuYaru;
    bool dark = false;
    double transitionProgress = 1.0;

    QColor windowBg;
    QColor headerBg;
    QColor cardBg;
    QColor cardAltBg;
    QColor controlBg;
    QColor controlHoverBg;
    QColor controlPressBg;
    QColor inputBg;
    QColor textPrimary;
    QColor textSecondary;
    QColor textDisabled;
    QColor borderSubtle;
    QColor borderStrong;
    QColor accent;
    QColor accentHover;
    QColor accentText;
    QColor plotCanvasBg;
    QColor plotFrameBg;
    QColor plotGrid;
    QColor badgeBg;
    QColor tracePrimary;
    QColor traceSecondary;
    QColor traceTertiary;
};

PlatformFlavor defaultPlatformFlavor();
PlatformFlavor parsePlatformFlavor(const QString &name);
QString platformFlavorName(PlatformFlavor flavor);

bool detectSystemDarkMode();
ThemeSpec makeThemeSpec(PlatformFlavor flavor, bool dark);
ThemeSpec interpolateThemeSpec(const ThemeSpec &from, const ThemeSpec &to, double t);
QPalette buildPalette(const ThemeSpec &spec);

QPixmap createAppLogoPixmap(int size = 256);
QIcon createAppLogoIcon();
QIcon createNativeIcon(NativeIconKind kind, const ThemeSpec &spec);
void applyNativeWindowFrame(QWidget *window, const ThemeSpec &spec);

class NativeOsProxyStyle final : public QProxyStyle {
  public:
    explicit NativeOsProxyStyle(const ThemeSpec &spec);
    void setThemeSpec(const ThemeSpec &spec);
    const ThemeSpec &themeSpec() const noexcept { return spec_; }

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption *option,
                         const QWidget *widget = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget = nullptr) const override;

  private:
    ThemeSpec spec_;
};

} // namespace edgeecho::ui

