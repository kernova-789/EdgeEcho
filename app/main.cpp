#include "edgeecho/core.hpp"
#include "native_theme.hpp"
#include "plot_widget.hpp"
#include "time_range_editor.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPropertyAnimation>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QVariantAnimation>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <memory>
#include <deque>

namespace {
using namespace edgeecho;
QString qs(const std::string &value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString &value) { const auto bytes = value.toUtf8(); return {bytes.constData(), static_cast<std::size_t>(bytes.size())}; }
std::filesystem::path pathFromQt(const QString &value) { return std::filesystem::u8path(utf8(value)); }
QString pathToQt(const std::filesystem::path &value) { return qs(value.u8string()); }
QString rawTime(Time value) { return QString::number(static_cast<qlonglong>(value)); }
QString utcTime(Time value) {
    return QDateTime::fromMSecsSinceEpoch(value / 1000, Qt::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) +
           QStringLiteral("%1 UTC").arg(value % 1000, 3, 10, QLatin1Char('0'));
}
QString number(double value) { return QString::number(value, 'g', 12); }
QString optionalNumber(const std::optional<double> &value) { return value ? number(*value) : QStringLiteral("未知"); }
QString stateText(PlaybackState state) {
    switch (state) {
    case PlaybackState::stopped: return QStringLiteral("已停止");
    case PlaybackState::playing: return QStringLiteral("播放中");
    case PlaybackState::paused: return QStringLiteral("已暂停");
    case PlaybackState::finished: return QStringLiteral("已到末尾");
    }
    return {};
}

using edgeecho::ui::NativeIconKind;
using edgeecho::ui::NativeOsProxyStyle;
using edgeecho::ui::PlatformFlavor;
using edgeecho::ui::PlotTrace;
using edgeecho::ui::PlotWidget;
using edgeecho::ui::ThemePreference;
using edgeecho::ui::ThemeSpec;
using edgeecho::ui::TimeRangeEditor;
using edgeecho::ui::applyNativeWindowFrame;
using edgeecho::ui::buildPalette;
using edgeecho::ui::createNativeIcon;
using edgeecho::ui::defaultPlatformFlavor;
using edgeecho::ui::detectSystemDarkMode;
using edgeecho::ui::interpolateThemeSpec;
using edgeecho::ui::makeThemeSpec;
using edgeecho::ui::parsePlatformFlavor;
using edgeecho::ui::platformFlavorName;

class ThemeTransitionOverlay final : public QWidget {
  public:
    explicit ThemeTransitionOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        hide();
    }
    void begin(QPixmap beforeSnapshot, QPointF origin, QColor waveAccent) {
        snapshot_ = std::move(beforeSnapshot);
        origin_ = origin;
        accent_ = waveAccent;
        progress_ = 0.0;
        if (parentWidget())
            setGeometry(parentWidget()->rect());
        raise();
        show();
        update();
    }
    void setProgress(double p) {
        progress_ = std::clamp(p, 0.0, 1.0);
        if (parentWidget() && geometry() != parentWidget()->rect())
            setGeometry(parentWidget()->rect());
        update();
    }
    void finish() {
        snapshot_ = {};
        hide();
    }
  protected:
    void paintEvent(QPaintEvent *) override {
        if (snapshot_.isNull() || progress_ >= 1.0)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const double maxRadius = std::hypot(static_cast<double>(width()), static_cast<double>(height())) * 1.05;
        const double radius = maxRadius * progress_;

        // Draw old theme snapshot outside the expanding circular wave, fading smoothly
        QPainterPath fullPath;
        fullPath.addRect(rect());
        QPainterPath waveCircle;
        waveCircle.addEllipse(origin_, radius, radius);
        painter.save();
        painter.setClipPath(fullPath.subtracted(waveCircle));
        painter.setOpacity(std::clamp(1.0 - progress_ * 0.65, 0.0, 1.0));
        painter.drawPixmap(0, 0, snapshot_);
        painter.restore();

        // Draw luminous theme-transition wave ring
        const int ringAlpha = static_cast<int>(std::lround((1.0 - progress_) * 155.0));
        if (ringAlpha > 4 && radius > 2.0) {
            painter.setPen(QPen(QColor(accent_.red(), accent_.green(), accent_.blue(), ringAlpha), 2.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(origin_, radius, radius);
        }
    }
  private:
    QPixmap snapshot_;
    QPointF origin_{640.0, 28.0};
    QColor accent_{"#0067C0"};
    double progress_ = 1.0;
};

struct Result {
    QString error, warning;
    std::filesystem::path path;
    Overview overview;
    Plot plot;
    std::vector<Plot> plots;
    std::optional<Spectrum> spectrum;
    std::optional<AudioClip> audioClip;
    QString readout;
    Page page;
    std::vector<Event> events;
    std::uint64_t exported = 0;
    std::optional<SourceIdentity> source;
    bool sourceChanged = false;
};
struct Job {
    std::uint64_t generation = 0;
    QString title;
    std::function<Result(Cancel)> work;
    std::function<void(Result)> apply;
    bool background = false;
};

class MainWindow final : public QMainWindow {
  public:
    MainWindow(QString smokePath, QString screenshot, QString interactionPath = {}, QString testOutput = {},
               PlatformFlavor initialFlavor = defaultPlatformFlavor(),
               ThemePreference initialTheme = ThemePreference::FollowSystem)
        : smoke_(!smokePath.isEmpty()), interaction_(!interactionPath.isEmpty()),
          screenshot_(std::move(screenshot)), testOutput_(pathFromQt(testOutput)),
          platformFlavor_(initialFlavor), themePref_(initialTheme) {
        lastSystemDark_ = detectSystemDarkMode();
        const bool startDark = (themePref_ == ThemePreference::Dark) ||
                               (themePref_ == ThemePreference::FollowSystem && lastSystemDark_);
        currentThemeSpec_ = makeThemeSpec(platformFlavor_, startDark);
        proxyStyle_ = new NativeOsProxyStyle(currentThemeSpec_);
        QApplication::setStyle(proxyStyle_);
        QApplication::setPalette(buildPalette(currentThemeSpec_));

        setWindowTitle(QStringLiteral("EdgeEcho %1 · 数据回放").arg(qs(version())));
        resize(1280, 860);
        buildUi();
        applyNativeWindowFrame(this, currentThemeSpec_);
        refreshThemeIcons();

        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] { completed(); });
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
        lastTick_ = std::chrono::steady_clock::now();
        if (smoke_) {
            QTimer::singleShot(0, this, [this, smokePath] { openPath(pathFromQt(smokePath)); });
            QTimer::singleShot(45000, this, [this] { if (!smokeDone_) fail(QStringLiteral("GUI smoke timeout")); });
        }
        if (interaction_) {
            if (testOutput.isEmpty()) throw Error("--interaction-test requires --test-output DIR");
            auto input = pathFromQt(interactionPath);
            if (std::filesystem::is_directory(input)) input /= "replay.db";
            std::filesystem::create_directories(testOutput_ / "interaction-source");
            interactionSource_ = testOutput_ / "interaction-source" / "replay.db";
            if (std::filesystem::weakly_canonical(input) == std::filesystem::weakly_canonical(interactionSource_))
                throw Error("Interaction test input must differ from its private output copy");
            std::filesystem::copy_file(input, interactionSource_, std::filesystem::copy_options::overwrite_existing);
            connect(&interactionTimer_, &QTimer::timeout, this, [this] { interactionStep(); });
            interactionTimer_.setInterval(50); interactionTimer_.start();
            QTimer::singleShot(0, this, [this] { openPath(interactionSource_); });
            QTimer::singleShot(150000, this, [this] { if (!interactionDone_) fail(QStringLiteral("GUI interaction timeout")); });
        }
    }
    ~MainWindow() override {
        audioPlayer_.stop();
        if (cancel_) cancel_->store(true);
        watcher_.waitForFinished();
    }
    void openArgument(const QString &path) { if (!path.isEmpty()) openPath(pathFromQt(path)); }
  protected:
    void closeEvent(QCloseEvent *event) override {
        closing_ = true;
        audioPlayer_.stop();
        pending_.reset();
        if (cancel_) cancel_->store(true);
        watcher_.waitForFinished();
        event->accept();
    }
  private:
    void buildUi() {
        auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
        auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
        auto *themeMenu = viewMenu->addMenu(QStringLiteral("外观主题(&T)"));
        auto *osStyleMenu = viewMenu->addMenu(QStringLiteral("系统组件风格(&S)"));
        auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));

        auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
        toolbar->setMovable(false);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

        openAction_ = toolbar->addAction(QStringLiteral("打开数据库"));
        dirAction_ = toolbar->addAction(QStringLiteral("打开目录"));
        demoAction_ = toolbar->addAction(QStringLiteral("新建演示库"));
        toolbar->addSeparator();
        exportAction_ = toolbar->addAction(QStringLiteral("导出区间 CSV"));
        exportAction_->setEnabled(false);
        exportWavAction_ = toolbar->addAction(QStringLiteral("导出音频 WAV"));
        exportWavAction_->setEnabled(false);
        toolbar->addSeparator();
        toggleBottomAction_ = toolbar->addAction(QStringLiteral("隐藏/显示详情面板"));

        toolbar->addSeparator();
        auto *themeLabel = new QLabel(QStringLiteral(" 主题: "));
        toolbar->addWidget(themeLabel);
        themeBox_ = new QComboBox;
        themeBox_->addItem(QStringLiteral("跟随系统 (自动)"), QStringLiteral("system"));
        themeBox_->addItem(QStringLiteral("浅色模式 (Light)"), QStringLiteral("light"));
        themeBox_->addItem(QStringLiteral("深色模式 (Dark)"), QStringLiteral("dark"));
        themeBox_->setCurrentIndex(themePref_ == ThemePreference::FollowSystem ? 0
                                   : themePref_ == ThemePreference::Light      ? 1
                                                                               : 2);
        themeBox_->setToolTip(QStringLiteral("切换浅色 / 深色模式或跟随操作系统设置（带平滑色彩过渡动画）"));
        toolbar->addWidget(themeBox_);

        helpAction_ = toolbar->addAction(QStringLiteral("使用说明"));

        auto *themeGroup = new QActionGroup(this);
        themeFollowAction_ = themeMenu->addAction(QStringLiteral("跟随系统 (Follow System)"));
        themeLightAction_ = themeMenu->addAction(QStringLiteral("固定浅色模式 (Light)"));
        themeDarkAction_ = themeMenu->addAction(QStringLiteral("固定深色模式 (Dark)"));
        for (auto *act : {themeFollowAction_, themeLightAction_, themeDarkAction_}) {
            act->setCheckable(true);
            themeGroup->addAction(act);
        }
        themeFollowAction_->setChecked(themePref_ == ThemePreference::FollowSystem);
        themeLightAction_->setChecked(themePref_ == ThemePreference::Light);
        themeDarkAction_->setChecked(themePref_ == ThemePreference::Dark);

        auto *osGroup = new QActionGroup(this);
        osLinuxAction_ = osStyleMenu->addAction(QStringLiteral("Linux Ubuntu GNOME (GTK4 / Yaru 官方组件风格)"));
        osWin11Action_ = osStyleMenu->addAction(QStringLiteral("Windows 11 Fluent (WinUI 3 / Mica 官方组件风格)"));
        for (auto *act : {osLinuxAction_, osWin11Action_}) {
            act->setCheckable(true);
            osGroup->addAction(act);
        }
        osLinuxAction_->setChecked(platformFlavor_ == PlatformFlavor::LinuxUbuntuYaru);
        osWin11Action_->setChecked(platformFlavor_ == PlatformFlavor::Windows11Fluent);

        fileMenu->addAction(openAction_);
        fileMenu->addAction(dirAction_);
        fileMenu->addAction(demoAction_);
        fileMenu->addSeparator();
        fileMenu->addAction(exportAction_);
        fileMenu->addAction(exportWavAction_);
        viewMenu->addAction(toggleBottomAction_);
        helpMenu->addAction(helpAction_);

        connect(openAction_, &QAction::triggered, this, [this] {
            const QString selected = QFileDialog::getOpenFileName(this, QStringLiteral("选择离线 Replay 数据库"), {}, QStringLiteral("SQLite 数据库 (*.db *.sqlite *.sqlite3);;所有文件 (*)"));
            if (!selected.isEmpty()) openPath(pathFromQt(selected));
        });
        connect(dirAction_, &QAction::triggered, this, [this] {
            const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择包含 replay.db 的目录"));
            if (!selected.isEmpty()) openPath(pathFromQt(selected));
        });
        connect(demoAction_, &QAction::triggered, this, [this] { createDemo(); });
        connect(exportAction_, &QAction::triggered, this, [this] { exportCsv(); });
        connect(exportWavAction_, &QAction::triggered, this, [this] { exportWav(); });
        connect(helpAction_, &QAction::triggered, this, [this] {
            QMessageBox::information(this, QStringLiteral("EdgeEcho 使用说明"), QStringLiteral(
                "1. 关闭数据写入程序后，打开已 checkpoint 的 replay.db 或存储目录。非空 WAL 会被拒绝，避免遗漏尚在 WAL 中的数据。\n"
                "2. 左侧选择传感器 / 指标 / 单位，以及显示通道与视图模式（时域波形 / FFT 频谱分析）。\n"
                "3. 播放、暂停、倍速、时间轴和曲线单击均可控制位置。查看声音（PCM）或波形数据时，支持实时同步发声播放、音量调节与导出标准 WAV 音频。\n"
                "4. 支持浅色模式、深色模式及跟随系统自动切换，切换过程具备平滑色彩与波纹动画；Linux 使用 Ubuntu GNOME (GTK4/Yaru) 原生风格，Windows 使用 Windows 11 Fluent 风格。\n"
                "5. CSV 导出整个所选区间中 timestamp 属于 [start,end) 的原始记录，BLOB 使用十六进制保留；WAV 导出所选窗口/区间的 16-bit PCM 音频。"));
        });

        connect(themeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
            const QString mode = themeBox_->itemData(idx).toString();
            if (mode == QStringLiteral("light")) setThemePreference(ThemePreference::Light, true);
            else if (mode == QStringLiteral("dark")) setThemePreference(ThemePreference::Dark, true);
            else setThemePreference(ThemePreference::FollowSystem, true);
        });
        connect(themeFollowAction_, &QAction::triggered, this, [this] { setThemePreference(ThemePreference::FollowSystem, true); });
        connect(themeLightAction_, &QAction::triggered, this, [this] { setThemePreference(ThemePreference::Light, true); });
        connect(themeDarkAction_, &QAction::triggered, this, [this] { setThemePreference(ThemePreference::Dark, true); });
        connect(osLinuxAction_, &QAction::triggered, this, [this] { setPlatformFlavor(PlatformFlavor::LinuxUbuntuYaru, true); });
        connect(osWin11Action_, &QAction::triggered, this, [this] { setPlatformFlavor(PlatformFlavor::Windows11Fluent, true); });

        auto *root = new QWidget;
        auto *outer = new QVBoxLayout(root);
        outer->setContentsMargins(8, 6, 8, 6);
        outer->setSpacing(6);

        // Top concise status & quick window bar
        auto *topBar = new QHBoxLayout;
        sourceLabel_ = new QLabel(QStringLiteral("未打开数据库 · 点击工具栏「打开数据库」或「新建演示库」开始"));
        sourceLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sourceLabel_->setWordWrap(true);
        topBar->addWidget(sourceLabel_, 1);

        topBar->addWidget(new QLabel(QStringLiteral("窗口时长:")));
        windowSeconds_ = new QDoubleSpinBox;
        windowSeconds_->setDecimals(3); windowSeconds_->setRange(.001, 3600); windowSeconds_->setValue(10);
        windowSeconds_->setSuffix(QStringLiteral(" s"));
        topBar->addWidget(windowSeconds_);

        for (const int seconds : {1, 10, 60}) {
            auto *button = new QPushButton(QStringLiteral("%1s").arg(seconds));
            button->setToolTip(QStringLiteral("从当前位置起查看 %1 秒区间").arg(seconds));
            topBar->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, seconds] {
                if (!validSelection()) return;
                const auto sourceRange = overview_.series[static_cast<std::size_t>(selected_)].range;
                const Time begin = std::clamp(clock_.position(), sourceRange.start, sourceRange.end - 1);
                setRangeFields({begin, begin + std::min<Time>(static_cast<Time>(seconds) * 1000000, sourceRange.end - begin)});
                applyRange();
            });
        }
        auto *full = new QPushButton(QStringLiteral("全区间"));
        full->setToolTip(QStringLiteral("恢复当前系列的完整时间区间"));
        topBar->addWidget(full);
        connect(full, &QPushButton::clicked, this, [this] {
            if (validSelection()) { setRangeFields(overview_.series[static_cast<std::size_t>(selected_)].range); applyRange(); }
        });

        reloadButton_ = new QPushButton(QStringLiteral("刷新"));
        topBar->addWidget(reloadButton_);
        outer->addLayout(topBar);

        connect(windowSeconds_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { customView_.reset(); loadWindow(true); });
        connect(reloadButton_, &QPushButton::clicked, this, [this] { loadWindow(true); });

        // Main horizontal splitter: Left Navigation Sidebar | Right Visualization & Inspector
        auto *split = new QSplitter(Qt::Horizontal);

        auto *side = new QWidget;
        auto *sideLayout = new QVBoxLayout(side);
        sideLayout->setContentsMargins(0, 0, 4, 0);
        sideLayout->setSpacing(6);

        auto *seriesGroup = new QGroupBox(QStringLiteral("传感器数据系列"));
        auto *seriesGroupLayout = new QVBoxLayout(seriesGroup);
        seriesList_ = new QListWidget;
        seriesList_->setMinimumWidth(220);
        seriesList_->setWordWrap(true);
        seriesList_->setAlternatingRowColors(true);
        seriesList_->setAccessibleName(QStringLiteral("数据系列"));
        seriesGroupLayout->addWidget(seriesList_, 1);
        seriesInfo_ = new QLabel(QStringLiteral("未选择系列"));
        seriesInfo_->setWordWrap(true);
        seriesGroupLayout->addWidget(seriesInfo_);
        sideLayout->addWidget(seriesGroup, 1);

        auto *channelGroup = new QGroupBox(QStringLiteral("通道与视图模式"));
        auto *channelForm = new QFormLayout(channelGroup);
        channelBox_ = new QComboBox;
        channelForm->addRow(QStringLiteral("数据通道:"), channelBox_);
        viewModeBox_ = new QComboBox;
        viewModeBox_->addItem(QStringLiteral("时域波形 (Waveform)"), QStringLiteral("time"));
        viewModeBox_->addItem(QStringLiteral("FFT 频谱图 (Spectrum)"), QStringLiteral("spectrum"));
        channelForm->addRow(QStringLiteral("图像视图:"), viewModeBox_);
        sideLayout->addWidget(channelGroup);

        split->addWidget(side);

        // Right Workspace: Vertical Splitter (Plot + Transport on top, Inspector Tabs on bottom)
        auto *body = new QWidget;
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(4, 0, 0, 0);
        bodyLayout->setSpacing(6);

        auto *vertical = new QSplitter(Qt::Vertical);

        // Top pane of vertical splitter: PlotWidget + Transport & Audio Controls
        auto * upperPane = new QWidget;
        auto *upperLayout = new QVBoxLayout(upperPane);
        upperLayout->setContentsMargins(0, 0, 0, 0);
        upperLayout->setSpacing(4);

        plot_ = new PlotWidget;
        plot_->seek = [this](Time position) { seekTo(position); };
        plot_->viewChanged = [this](Range range) {
            if (!validSelection()) return;
            clock_.pause(); audioPlayer_.pause(); customView_ = range; refreshPosition(); loadWindow(true);
        };
        plot_->inspectRequested = [this](Time target) {
            if (!validSelection() || !loaded_) return;
            pendingProbe_ = target; ++probeSerial_;
        };
        upperLayout->addWidget(plot_, 1);

        // Transport & Audio Control Group
        auto *transportGroup = new QGroupBox;
        auto *transportLayout = new QVBoxLayout(transportGroup);
        transportLayout->setContentsMargins(8, 6, 8, 6);
        transportLayout->setSpacing(4);

        auto *playRow = new QHBoxLayout;
        play_ = new QPushButton(QStringLiteral("播放"));
        pause_ = new QPushButton(QStringLiteral("暂停"));
        stop_ = new QPushButton(QStringLiteral("停止"));
        playRow->addWidget(play_);
        playRow->addWidget(pause_);
        playRow->addWidget(stop_);

        timeline_ = new QSlider(Qt::Horizontal);
        timeline_->setRange(0, 1000000);
        timeline_->setSingleStep(1000);
        timeline_->setPageStep(100000);
        timeline_->setAccessibleName(QStringLiteral("回放时间轴"));
        playRow->addWidget(timeline_, 1);

        playRow->addWidget(new QLabel(QStringLiteral("倍速:")));
        speed_ = new QDoubleSpinBox;
        speed_->setRange(.01, 1000); speed_->setDecimals(2); speed_->setValue(1); speed_->setSuffix(QStringLiteral(" ×"));
        playRow->addWidget(speed_);

        stateLabel_ = new QLabel(QStringLiteral("已停止"));
        playRow->addWidget(stateLabel_);
        transportLayout->addLayout(playRow);

        auto *audioRow = new QHBoxLayout;
        positionLabel_ = new QLabel(QStringLiteral("—"));
        positionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        audioRow->addWidget(positionLabel_, 1);

        windowInfo_ = new QLabel;
        audioRow->addWidget(windowInfo_);

        audioEnable_ = new QCheckBox(QStringLiteral("声音输出"));
        audioEnable_->setChecked(true);
        audioEnable_->setToolTip(QStringLiteral("播放声音/波形系列时同步通过系统声卡输出音频"));
        audioRow->addWidget(audioEnable_);

        audioRow->addWidget(new QLabel(QStringLiteral("音量:")));
        volumeSlider_ = new QSlider(Qt::Horizontal);
        volumeSlider_->setRange(0, 100);
        volumeSlider_->setValue(85);
        volumeSlider_->setFixedWidth(90);
        volumeSlider_->setToolTip(QStringLiteral("调节音频回放音量"));
        audioRow->addWidget(volumeSlider_);

        playAudioNow_ = new QPushButton(QStringLiteral("试听当前窗口"));
        playAudioNow_->setEnabled(false);
        audioRow->addWidget(playAudioNow_);

        audioLevelMeter_ = new QProgressBar;
        audioLevelMeter_->setRange(0, 100);
        audioLevelMeter_->setValue(0);
        audioLevelMeter_->setTextVisible(false);
        audioLevelMeter_->setFixedWidth(68);
        audioLevelMeter_->setFixedHeight(12);
        audioLevelMeter_->setToolTip(QStringLiteral("实时音频动态电平指示"));
        audioRow->addWidget(audioLevelMeter_);

        audioStatusLabel_ = new QLabel(QStringLiteral("音频待命"));
        audioRow->addWidget(audioStatusLabel_);
        transportLayout->addLayout(audioRow);

        upperLayout->addWidget(transportGroup);
        vertical->addWidget(upperPane);

        // Bottom pane of vertical splitter: Tabbed Inspector
        auto *tabs = new QTabWidget;
        inspectorTabs_ = tabs;

        // Tab 1: Raw Records
        auto *recordPane = new QWidget;
        auto *recordLayout = new QVBoxLayout(recordPane);
        recordLayout->setContentsMargins(4, 4, 4, 4);
        records_ = new QTableWidget(0, 7);
        records_->setHorizontalHeaderLabels({QStringLiteral("记录 ID"), QStringLiteral("UTC 微秒"), QStringLiteral("载荷"),
            QStringLiteral("值 / 字节数"), QStringLiteral("质量"), QStringLiteral("时长 μs"), QStringLiteral("采样率 Hz")});
        configureTable(records_);
        recordLayout->addWidget(records_);
        auto *pageControls = new QHBoxLayout;
        pageLabel_ = new QLabel(QStringLiteral("未加载记录"));
        firstPage_ = new QPushButton(QStringLiteral("第一页"));
        nextPage_ = new QPushButton(QStringLiteral("下一页"));
        nextPage_->setEnabled(false);
        pageControls->addWidget(pageLabel_, 1);
        pageControls->addWidget(firstPage_);
        pageControls->addWidget(nextPage_);
        recordLayout->addLayout(pageControls);
        tabs->addTab(recordPane, QStringLiteral("原始记录表"));

        // Tab 2: Diagnostic Events
        events_ = new QTableWidget(0, 7);
        events_->setHorizontalHeaderLabels({QStringLiteral("开始 UTC μs"), QStringLiteral("结束 UTC μs"), QStringLiteral("评估"),
            QStringLiteral("严重度"), QStringLiteral("类型"), QStringLiteral("版本 / 确认"), QStringLiteral("说明")});
        configureTable(events_);
        events_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
        tabs->addTab(events_, QStringLiteral("诊断事件"));

        // Tab 3: Record Details & Hex Preview
        auto *detailsSplit = new QSplitter(Qt::Horizontal);
        details_ = new QTextBrowser;
        detailPreview_ = new QTextBrowser;
        detailsSplit->addWidget(details_);
        detailsSplit->addWidget(detailPreview_);
        detailsSplit->setStretchFactor(0, 1);
        detailsSplit->setStretchFactor(1, 1);
        tabs->addTab(detailsSplit, QStringLiteral("选中记录详情"));

        // Tab 4: Precise Range & Time Editor (keeps main view clean!)
        auto *rangePane = new QWidget;
        auto *rangePaneLayout = new QVBoxLayout(rangePane);
        timeEditor_ = new TimeRangeEditor;
        rangePaneLayout->addWidget(timeEditor_);

        auto *rangeGroup = new QGroupBox(QStringLiteral("精确 Unix UTC 微秒区间 [开始, 结束) 与定位"));
        auto *rangeLayout = new QHBoxLayout(rangeGroup);
        fromEdit_ = new QLineEdit; toEdit_ = new QLineEdit;
        const QRegularExpression digits(QStringLiteral("[0-9]{1,18}"));
        fromEdit_->setValidator(new QRegularExpressionValidator(digits, fromEdit_));
        toEdit_->setValidator(new QRegularExpressionValidator(digits, toEdit_));
        fromEdit_->setAccessibleName(QStringLiteral("UTC区间开始微秒"));
        toEdit_->setAccessibleName(QStringLiteral("UTC区间结束微秒"));
        rangeLayout->addWidget(new QLabel(QStringLiteral("开始:"))); rangeLayout->addWidget(fromEdit_, 1);
        rangeLayout->addWidget(new QLabel(QStringLiteral("结束:"))); rangeLayout->addWidget(toEdit_, 1);
        auto *apply = new QPushButton(QStringLiteral("应用区间"));
        rangeLayout->addWidget(apply);
        rangeLayout->addSpacing(12);
        seekEdit_ = new QLineEdit; seekEdit_->setMaximumWidth(200); seekEdit_->setPlaceholderText(QStringLiteral("跳转到 UTC 微秒"));
        seekEdit_->setValidator(new QRegularExpressionValidator(digits, seekEdit_));
        auto *go = new QPushButton(QStringLiteral("跳转"));
        rangeLayout->addWidget(seekEdit_); rangeLayout->addWidget(go);
        rangePaneLayout->addWidget(rangeGroup);
        rangePaneLayout->addStretch();
        tabs->addTab(rangePane, QStringLiteral("时间与区间设置"));

        vertical->addWidget(tabs);
        vertical->setStretchFactor(0, 4);
        vertical->setStretchFactor(1, 2);
        bodyLayout->addWidget(vertical, 1);

        split->addWidget(body);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        outer->addWidget(split, 1);
        setCentralWidget(root);

        themeOverlay_ = new ThemeTransitionOverlay(this);
        themeAnim_ = new QVariantAnimation(this);
        themeAnim_->setDuration(380);
        themeAnim_->setEasingCurve(QEasingCurve::InOutCubic);
        connect(themeAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &val) {
            const double t = val.toDouble();
            currentThemeSpec_ = interpolateThemeSpec(themeFromSpec_, themeToSpec_, t);
            if (proxyStyle_) proxyStyle_->setThemeSpec(currentThemeSpec_);
            const QPalette pal = buildPalette(currentThemeSpec_);
            QApplication::setPalette(pal);
            setPalette(pal);
            plot_->setPalette(pal);
            if (themeOverlay_) themeOverlay_->setProgress(t);
            update();
            plot_->update();
        });
        connect(themeAnim_, &QVariantAnimation::finished, this, [this] {
            currentThemeSpec_ = themeToSpec_;
            if (proxyStyle_) proxyStyle_->setThemeSpec(currentThemeSpec_);
            const QPalette pal = buildPalette(currentThemeSpec_);
            QApplication::setPalette(pal);
            setPalette(pal);
            plot_->setPalette(pal);
            applyNativeWindowFrame(this, currentThemeSpec_);
            refreshThemeIcons();
            updateTraceThemeColors();
            if (themeOverlay_) themeOverlay_->finish();
            update();
            plot_->update();
        });

        // Connections
        drawerAnim_ = new QVariantAnimation(this);
        drawerAnim_->setDuration(260);
        drawerAnim_->setEasingCurve(QEasingCurve::InOutCubic);
        connect(drawerAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &val) {
            inspectorTabs_->setMaximumHeight(val.toInt());
        });
        connect(drawerAnim_, &QVariantAnimation::finished, this, [this] {
            if (drawerExpanding_) {
                inspectorTabs_->setMaximumHeight(QWIDGETSIZE_MAX);
            } else {
                inspectorTabs_->setVisible(false);
                inspectorTabs_->setMaximumHeight(QWIDGETSIZE_MAX);
            }
        });
        connect(toggleBottomAction_, &QAction::triggered, this, [this] {
            drawerAnim_->stop();
            if (inspectorTabs_->isVisible()) {
                drawerExpanding_ = false;
                drawerLastHeight_ = std::max(180, inspectorTabs_->height());
                drawerAnim_->setStartValue(inspectorTabs_->height());
                drawerAnim_->setEndValue(0);
                drawerAnim_->start();
            } else {
                drawerExpanding_ = true;
                inspectorTabs_->setMaximumHeight(0);
                inspectorTabs_->setVisible(true);
                drawerAnim_->setStartValue(0);
                drawerAnim_->setEndValue(std::max(180, drawerLastHeight_));
                drawerAnim_->start();
            }
        });
        connect(inspectorTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (smoke_ || interaction_) return;
            if (auto *page = inspectorTabs_->widget(index)) {
                auto *effect = qobject_cast<QGraphicsOpacityEffect *>(page->graphicsEffect());
                if (!effect) {
                    effect = new QGraphicsOpacityEffect(page);
                    page->setGraphicsEffect(effect);
                }
                auto *fade = new QPropertyAnimation(effect, "opacity", page);
                fade->setDuration(200);
                fade->setStartValue(0.25);
                fade->setEndValue(1.0);
                fade->setEasingCurve(QEasingCurve::OutCubic);
                fade->start(QAbstractAnimation::DeleteWhenStopped);
            }
        });
        timeEditor_->edited = [this](Range range) {
            const QSignalBlocker a(fromEdit_), b(toEdit_);
            fromEdit_->setText(rawTime(range.start)); toEdit_->setText(rawTime(range.end));
        };
        const auto syncCalendar = [this] {
            bool a = false, b = false;
            const Time start = fromEdit_->text().toLongLong(&a), end = toEdit_->text().toLongLong(&b);
            if (a && b && start >= 0 && end > start && end <= max_time) timeEditor_->setRange({start, end});
        };
        connect(fromEdit_, &QLineEdit::textEdited, this, syncCalendar);
        connect(toEdit_, &QLineEdit::textEdited, this, syncCalendar);
        connect(apply, &QPushButton::clicked, this, [this] { applyRange(); });
        connect(fromEdit_, &QLineEdit::returnPressed, this, [this] { applyRange(); });
        connect(toEdit_, &QLineEdit::returnPressed, this, [this] { applyRange(); });
        const auto goTo = [this] {
            bool ok = false; const Time value = seekEdit_->text().toLongLong(&ok);
            if (ok) seekTo(value); else fail(QStringLiteral("请输入有效 UTC 微秒整数"));
        };
        connect(go, &QPushButton::clicked, this, goTo);
        connect(seekEdit_, &QLineEdit::returnPressed, this, goTo);

        connect(firstPage_, &QPushButton::clicked, this, [this] { loadPage({}); });
        connect(nextPage_, &QPushButton::clicked, this, [this] { loadPage(page_.next); });
        connect(records_, &QTableWidget::itemSelectionChanged, this, [this] { showDetails(); });
        connect(events_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row >= 0 && static_cast<std::size_t>(row) < eventRows_.size())
                seekTo(std::clamp(eventRows_[static_cast<std::size_t>(row)].start, clock_.range().start, clock_.range().end));
        });

        connect(play_, &QPushButton::clicked, this, [this] {
            if (!validSelection()) return;
            customView_.reset();
            clock_.play();
            lastTick_ = std::chrono::steady_clock::now();
            loadWindow();
            if (audioEnable_->isChecked() && audioPlayer_.has_clip()) {
                audioPlayer_.seek(clock_.position());
                audioPlayer_.play();
            }
            refreshPosition();
        });
        connect(pause_, &QPushButton::clicked, this, [this] {
            clock_.pause();
            audioPlayer_.pause();
            refreshPosition();
        });
        connect(stop_, &QPushButton::clicked, this, [this] {
            customView_.reset();
            clock_.stop();
            audioPlayer_.stop();
            loadWindow();
            refreshPosition();
        });
        connect(speed_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            clock_.set_speed(value);
        });
        connect(audioEnable_, &QCheckBox::toggled, this, [this](bool checked) {
            audioPlayer_.set_muted(!checked);
            if (!checked) audioPlayer_.pause();
            else if (clock_.state() == PlaybackState::playing && audioPlayer_.has_clip()) {
                audioPlayer_.seek(clock_.position());
                audioPlayer_.play();
            }
        });
        connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
            audioPlayer_.set_volume(static_cast<double>(std::clamp(value, 0, 100)) / 100.0);
        });
        connect(playAudioNow_, &QPushButton::clicked, this, [this] {
            if (!validSelection() || !audioPlayer_.has_clip()) return;
            audioEnable_->setChecked(true);
            if (loaded_) clock_.seek(loaded_->start);
            clock_.play();
            lastTick_ = std::chrono::steady_clock::now();
            audioPlayer_.seek(clock_.position());
            audioPlayer_.play();
            refreshPosition();
        });

        connect(timeline_, &QSlider::valueChanged, this, [this](int value) {
            if (!timeline_->isSliderDown()) seekFromTimeline(value);
        });
        connect(timeline_, &QSlider::sliderReleased, this, [this] {
            seekFromTimeline(timeline_->sliderPosition());
        });

        busy_ = new QProgressBar; busy_->setRange(0, 0); busy_->setMaximumWidth(120); busy_->hide();
        auto *cancelButton = new QPushButton(QStringLiteral("取消查询")); cancelButton_ = cancelButton;
        connect(cancelButton, &QPushButton::clicked, this, [this] {
            ++generation_; pending_.reset(); requested_.reset(); pendingProbe_.reset(); ++probeSerial_;
            if (cancel_) cancel_->store(true);
            clock_.pause(); audioPlayer_.pause(); refreshPosition();
            statusBar()->showMessage(QStringLiteral("正在取消查询…"));
        });
        statusBar()->addPermanentWidget(busy_); statusBar()->addPermanentWidget(cancelButton);
        statusBar()->showMessage(QStringLiteral("就绪 · 数据库仅离线只读"));

        connect(seriesList_, &QListWidget::currentRowChanged, this, [this](int row) { selectSeries(row); });
        connect(channelBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            if (!updatingChannels_) loadWindow(true);
        });
        connect(viewModeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            refreshPlotView();
        });
    }
    static void configureTable(QTableWidget *table) {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true); table->setSortingEnabled(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setResizeContentsPrecision(32);
        table->verticalHeader()->setVisible(false);
        table->setWordWrap(false);
    }
    void setThemePreference(ThemePreference pref, bool animated = true) {
        themePref_ = pref;
        if (themeBox_) {
            const QSignalBlocker block(themeBox_);
            themeBox_->setCurrentIndex(pref == ThemePreference::FollowSystem ? 0
                                       : pref == ThemePreference::Light      ? 1
                                                                             : 2);
        }
        if (themeFollowAction_) themeFollowAction_->setChecked(pref == ThemePreference::FollowSystem);
        if (themeLightAction_) themeLightAction_->setChecked(pref == ThemePreference::Light);
        if (themeDarkAction_) themeDarkAction_->setChecked(pref == ThemePreference::Dark);

        lastSystemDark_ = detectSystemDarkMode();
        const bool wantDark = (pref == ThemePreference::Dark) ||
                              (pref == ThemePreference::FollowSystem && lastSystemDark_);
        transitionToTheme(wantDark, platformFlavor_, animated);
    }
    void setPlatformFlavor(PlatformFlavor flavor, bool animated = true) {
        platformFlavor_ = flavor;
        if (osLinuxAction_) osLinuxAction_->setChecked(flavor == PlatformFlavor::LinuxUbuntuYaru);
        if (osWin11Action_) osWin11Action_->setChecked(flavor == PlatformFlavor::Windows11Fluent);
        const bool wantDark = (themePref_ == ThemePreference::Dark) ||
                              (themePref_ == ThemePreference::FollowSystem && detectSystemDarkMode());
        transitionToTheme(wantDark, flavor, animated);
    }
    void transitionToTheme(bool dark, PlatformFlavor flavor, bool animated) {
        const ThemeSpec target = makeThemeSpec(flavor, dark);
        if (themeAnim_) themeAnim_->stop();
        if (!animated || !isVisible()) {
            currentThemeSpec_ = target;
            if (proxyStyle_) proxyStyle_->setThemeSpec(currentThemeSpec_);
            const QPalette pal = buildPalette(currentThemeSpec_);
            QApplication::setPalette(pal);
            setPalette(pal);
            if (plot_) plot_->setPalette(pal);
            applyNativeWindowFrame(this, currentThemeSpec_);
            refreshThemeIcons();
            updateTraceThemeColors();
            if (themeOverlay_) themeOverlay_->finish();
            update();
            if (plot_) plot_->update();
            return;
        }
        QPointF waveOrigin(width() * 0.72, 24.0);
        if (themeBox_ && themeBox_->isVisible()) {
            waveOrigin = themeBox_->mapTo(this, themeBox_->rect().center());
        }
        const QPixmap beforeSnapshot = grab();
        themeFromSpec_ = currentThemeSpec_;
        themeToSpec_ = target;
        if (themeOverlay_)
            themeOverlay_->begin(beforeSnapshot, waveOrigin, target.accent);
        themeAnim_->setStartValue(0.0);
        themeAnim_->setEndValue(1.0);
        themeAnim_->start();
    }
    void refreshThemeIcons() {
        if (openAction_) openAction_->setIcon(createNativeIcon(NativeIconKind::OpenDatabase, currentThemeSpec_));
        if (dirAction_) dirAction_->setIcon(createNativeIcon(NativeIconKind::OpenFolder, currentThemeSpec_));
        if (demoAction_) demoAction_->setIcon(createNativeIcon(NativeIconKind::NewDemo, currentThemeSpec_));
        if (exportAction_) exportAction_->setIcon(createNativeIcon(NativeIconKind::ExportCsv, currentThemeSpec_));
        if (exportWavAction_) exportWavAction_->setIcon(createNativeIcon(NativeIconKind::ExportWav, currentThemeSpec_));
        if (toggleBottomAction_) toggleBottomAction_->setIcon(createNativeIcon(NativeIconKind::ToggleDrawer, currentThemeSpec_));
        if (helpAction_) helpAction_->setIcon(createNativeIcon(NativeIconKind::HelpInfo, currentThemeSpec_));
        if (reloadButton_) reloadButton_->setIcon(createNativeIcon(NativeIconKind::Reload, currentThemeSpec_));
        if (play_) play_->setIcon(createNativeIcon(NativeIconKind::Play, currentThemeSpec_));
        if (pause_) pause_->setIcon(createNativeIcon(NativeIconKind::Pause, currentThemeSpec_));
        if (stop_) stop_->setIcon(createNativeIcon(NativeIconKind::Stop, currentThemeSpec_));
        if (playAudioNow_) playAudioNow_->setIcon(createNativeIcon(NativeIconKind::AudioWave, currentThemeSpec_));
        if (seriesList_) {
            for (int i = 0; i < seriesList_->count() && static_cast<std::size_t>(i) < overview_.series.size(); ++i) {
                const auto &s = overview_.series[static_cast<std::size_t>(i)];
                const bool isSound = s.encoding == "iio-le-16-v1" || s.key.type.find("sound") != std::string::npos;
                if (auto *item = seriesList_->item(i)) {
                    item->setIcon(createNativeIcon(isSound ? NativeIconKind::AudioWave : NativeIconKind::SensorScalar,
                                                   currentThemeSpec_));
                }
            }
        }
    }
    void updateTraceThemeColors() {
        if (currentTraces_.empty())
            return;
        const QColor colors[] = {
            currentThemeSpec_.tracePrimary,
            currentThemeSpec_.traceSecondary,
            currentThemeSpec_.traceTertiary
        };
        for (std::size_t i = 0; i < currentTraces_.size(); ++i)
            currentTraces_[i].color = colors[i % 3];
        if (validSelection() && loaded_)
            refreshPlotView();
    }
    bool validSelection() const {
        return source_.has_value() && !opening_ && selected_ >= 0 &&
            static_cast<std::size_t>(selected_) < overview_.series.size() && !path_.empty();
    }
    Filter filter() const { return overview_.series[static_cast<std::size_t>(selected_)].key; }
    void fail(const QString &error) {
        statusBar()->showMessage(error);
        if (interaction_) {
            if (expectSourceRejection_ && lastFailureSourceChanged_) {
                expectedSourceErrorSeen_ = true; expectSourceRejection_ = false;
                interactionChecks_.append(QStringLiteral("source replacement rejected"));
                return;
            }
            finishInteraction(false, error); return;
        }
        if (smoke_) {
            std::cerr << "GUI smoke failed: " << utf8(error) << '\n'; smokeDone_ = true; QApplication::exit(2);
        } else QMessageBox::warning(this, QStringLiteral("EdgeEcho"), error);
    }
    void submit(QString title, std::function<Result(Cancel)> work, std::function<void(Result)> apply) {
        Job job{++generation_, std::move(title), std::move(work), std::move(apply)};
        if (active_) { if (cancel_) cancel_->store(true); pending_ = std::move(job); }
        else start(std::move(job));
    }
    void start(Job job) {
        active_ = true; cancel_ = std::make_shared<std::atomic_bool>(false); current_ = std::move(job);
        if (!current_->background) { statusBar()->showMessage(current_->title); busy_->show(); }
        auto work = current_->work; auto cancel = cancel_;
        watcher_.setFuture(QtConcurrent::run([work = std::move(work), cancel]() {
            try { return work(cancel); }
            catch (const SourceChanged &error) { Result result; result.sourceChanged = true; result.error = qs(error.what()); return result; }
            catch (const std::exception &error) { Result result; result.error = qs(error.what()); return result; }
            catch (...) { Result result; result.error = QStringLiteral("未知后台错误"); return result; }
        }));
    }
    void completed() {
        if (closing_ || !current_) return;
        Result result;
        try { result = watcher_.result(); }
        catch (...) { result.error = QStringLiteral("后台任务未能返回结果"); }
        auto job = std::move(*current_); current_.reset(); active_ = false;
        if (job.generation == generation_) {
            if (!job.background) requested_.reset();
            if (!result.error.isEmpty()) {
                if (job.background && !result.sourceChanged) {
                    if (job.title == QStringLiteral("精确原始读数")) plot_->setReadout(QStringLiteral("精确读数不可用：") + result.error);
                } else {
                lastFailureSourceChanged_ = result.sourceChanged;
                if (result.sourceChanged) invalidateSession(QStringLiteral("源数据库已变化，请重新打开。"));
                opening_ = false; clock_.pause(); audioPlayer_.pause(); refreshPosition();
                fail(result.sourceChanged ? QStringLiteral("源数据库已变化，请重新打开。\n") + result.error : result.error);
                }
            }
            else {
                try { job.apply(std::move(result)); }
                catch (const std::exception &error) { fail(qs(error.what())); }
            }
        }
        if (!active_ && pending_) { auto next = std::move(*pending_); pending_.reset(); start(std::move(next)); }
        if (!active_) busy_->hide();
    }
    void setSessionControls(bool enabled) {
        seriesList_->setEnabled(enabled); channelBox_->setEnabled(enabled); viewModeBox_->setEnabled(enabled);
        timeline_->setEnabled(enabled);
        play_->setEnabled(enabled); pause_->setEnabled(enabled); stop_->setEnabled(enabled);
        fromEdit_->setEnabled(enabled); toEdit_->setEnabled(enabled); seekEdit_->setEnabled(enabled); timeEditor_->setEnabled(enabled);
        exportAction_->setEnabled(enabled);
        if (!enabled) {
            exportWavAction_->setEnabled(false);
            playAudioNow_->setEnabled(false);
        }
    }
    void invalidateSession(const QString &message) {
        clock_.stop(); audioPlayer_.clear(); source_.reset(); exportProgress_.reset(); selected_ = -1; overview_ = {}; path_.clear();
        loaded_.reset(); requested_.reset(); customView_.reset(); page_ = {}; eventRows_.clear();
        currentTraces_.clear(); currentSpectrum_.reset(); clearBrowsingCache();
        const QSignalBlocker seriesBlock(seriesList_), channelBlock(channelBox_);
        seriesList_->clear(); channelBox_->clear(); records_->setRowCount(0); events_->setRowCount(0);
        details_->clear(); detailPreview_->clear(); seriesInfo_->clear(); plot_->clear(message);
        plot_->setPlaying(false, 0.0);
        audioLevelMeter_->setValue(0);
        audioStatusLabel_->setText(QStringLiteral("音频待命"));
        sourceLabel_->setText(message); setSessionControls(false);
    }
    void openPath(std::filesystem::path path) {
        invalidateSession(QStringLiteral("正在打开离线数据库…")); opening_ = true;
        submit(QStringLiteral("读取数据库目录…"), [path = std::move(path)](Cancel cancel) {
            Database db(path, {}, cancel); Result result; result.path = db.path(); result.source = db.identity(); result.overview = db.overview(); return result;
        }, [this](Result result) {
            opening_ = false; source_ = std::move(result.source); path_ = std::move(result.path); overview_ = std::move(result.overview); selected_ = -1;
            sourceLabel_->setText(pathToQt(path_) + QStringLiteral("   ·   %1 条记录 / %2 个系列 · 离线只读").arg(overview_.records).arg(overview_.series.size()));
            const QSignalBlocker block(seriesList_); seriesList_->clear();
            for (const auto &series : overview_.series) {
                const bool isSound = series.encoding == "iio-le-16-v1" || series.key.type.find("sound") != std::string::npos;
                const QString text = qs(series.key.sensor) + QLatin1Char('\n') + qs(series.key.type) + QStringLiteral("  [%1]").arg(qs(series.key.unit));
                auto *item = new QListWidgetItem(createNativeIcon(isSound ? NativeIconKind::AudioWave : NativeIconKind::SensorScalar, currentThemeSpec_), text, seriesList_);
                item->setToolTip(text + QStringLiteral("\n%1 条记录\n%2 — %3").arg(series.records).arg(utcTime(series.range.start), utcTime(series.range.end)));
            }
            setSessionControls(!overview_.series.empty());
            if (!overview_.series.empty()) { seriesList_->setCurrentRow(0); selectSeries(0); }
            else { statusBar()->showMessage(QStringLiteral("数据库中没有记录")); if (smoke_) finishSmoke(); }
        });
    }
    void selectSeries(int row) {
        if (!source_ || opening_) return;
        if (row < 0 || static_cast<std::size_t>(row) >= overview_.series.size()) return;
        selected_ = row;
        audioPlayer_.stop();
        const auto &series = overview_.series[static_cast<std::size_t>(row)];
        clock_.set_range(series.range); clock_.set_speed(speed_->value());
        setRangeFields(series.range);
        customView_.reset(); clearBrowsingCache();
        plot_->setOverallRange(series.range);
        seriesInfo_->setText(QStringLiteral("%1 条记录\n%2\n至 %3\n编码：%4")
            .arg(series.records).arg(utcTime(series.range.start), utcTime(series.range.end), series.encoding.empty() ? QStringLiteral("标量 / 空编码") : qs(series.encoding)));
        updatingChannels_ = true; channelBox_->clear();
        auto available = channels(series.encoding, series.kind);
        if (series.encoding == "iio-le-24-v1" && series.kind == 2)
            channelBox_->addItem(QStringLiteral("XYZ 同屏"), QStringLiteral("xyz"));
        if (available.empty() && series.kind == -1) {
            channelBox_->addItem(QStringLiteral("混合载荷：value（不兼容记录将报错）"), QStringLiteral("value"));
            for (const auto &encoding : {"iio-le-24-v1", "iio-le-16-v1", "iio-le-72-v1"})
                for (const auto &channel : channels(encoding)) channelBox_->addItem(qs(channel.label), qs(channel.id));
        } else for (const auto &channel : available) channelBox_->addItem(qs(channel.label), qs(channel.id));
        if (channelBox_->count() == 0) channelBox_->addItem(QStringLiteral("未知编码 · 查看原始记录"), QString());
        updatingChannels_ = false;
        loaded_.reset(); requested_.reset(); page_ = {};
        exportAction_->setEnabled(true); refreshPosition(); loadWindow(true);
    }
    void applyRange() {
        if (!validSelection()) return;
        bool startOk = false, endOk = false;
        const Time start = fromEdit_->text().toLongLong(&startOk), end = toEdit_->text().toLongLong(&endOk);
        if (!startOk || !endOk || start < 0 || end <= start || end > max_time) { fail(QStringLiteral("区间应满足 0 ≤ 开始 < 结束 ≤ 253402300800000000，单位为 UTC 微秒")); return; }
        if (!timeEditor_->valid()) { fail(timeEditor_->error()); return; }
        clock_.set_range({start, end}); clock_.set_speed(speed_->value()); customView_.reset();
        timeEditor_->setRange({start, end}); plot_->setOverallRange({start, end}); clearBrowsingCache();
        loaded_.reset(); requested_.reset(); refreshPosition(); loadWindow(true);
    }
    Range currentWindow() const {
        if (customView_) return *customView_;
        return windowAt(clock_.position());
    }
    Range windowAt(Time position) const {
        const auto total = clock_.range();
        const Time width = static_cast<Time>(std::llround(windowSeconds_->value() * 1000000.0));
        Time start = std::clamp(position, total.start, total.end - 1);
        if (total.end - start < width) start = std::max(total.start, total.end - width);
        const Time end = start + std::min(width, total.end - start);
        return {start, end};
    }
    void setRangeFields(Range range) {
        fromEdit_->setText(rawTime(range.start)); toEdit_->setText(rawTime(range.end)); timeEditor_->setRange(range);
    }
    std::vector<std::string> selectedChannels() const {
        const auto channel = utf8(channelBox_->currentData().toString());
        return channel == "xyz" ? std::vector<std::string>{"x", "y", "z"} : channel.empty() ? std::vector<std::string>{} : std::vector<std::string>{channel};
    }
    struct CacheEntry {
        SourceIdentity source;
        Filter filter;
        Range range;
        std::vector<std::string> channels;
        std::size_t buckets = 0, bytes = 0;
        std::shared_ptr<Result> value;
    };
    void clearBrowsingCache() {
        cache_.clear(); cacheBytes_ = 0; prefetchAttempt_.clear(); pendingProbe_.reset(); lastExactReadout_.clear(); ++probeSerial_;
    }
    std::string cacheKey(Range range, const std::vector<std::string> &ids, std::size_t buckets) const {
        std::string key = std::to_string(selected_) + ":" + std::to_string(range.start) + ":" + std::to_string(range.end) + ":" + std::to_string(buckets);
        for (const auto &id : ids) key += ":" + id;
        return key;
    }
    std::shared_ptr<Result> cached(Range range, const std::vector<std::string> &ids, std::size_t buckets) {
        if (!source_) return {};
        const auto f = filter();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->source == *source_ && it->filter.sensor == f.sensor && it->filter.type == f.type && it->filter.unit == f.unit &&
                it->range.start == range.start && it->range.end == range.end && it->channels == ids && it->buckets == buckets) {
                auto entry = std::move(*it); cache_.erase(it); cache_.push_front(std::move(entry)); return cache_.front().value;
            }
        }
        return {};
    }
    static std::size_t resultBytes(const Result &r) {
        std::size_t bytes = sizeof(Result);
        for (const auto &p : r.plots) bytes += p.points.capacity() * sizeof(Point);
        bytes += r.plot.points.capacity() * sizeof(Point);
        if (r.spectrum) bytes += r.spectrum->bins.capacity() * sizeof(SpectrumBin);
        if (r.audioClip) bytes += r.audioClip->pcm16.capacity() * sizeof(std::int16_t);
        for (const auto &row : r.page.records) bytes += sizeof(Record) + row.block.capacity() + row.id.size() + row.sensor.size() + row.type.size() + row.unit.size() + row.metadata.size() + row.source.size() + row.batch.size() + row.encoding.size();
        for (const auto &e : r.events) bytes += sizeof(Event) + e.id.size() + e.sensor.size() + e.type.size() + e.event_type.size() + e.description.size();
        return bytes;
    }
    void remember(Range range, const std::vector<std::string> &ids, std::size_t buckets, const Result &r) {
        if (!source_ || !r.error.isEmpty() || !r.warning.isEmpty()) return;
        const auto bytes = resultBytes(r); constexpr std::size_t maxBytes = 32 * 1024 * 1024;
        if (bytes > maxBytes) return;
        const auto f = filter();
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->range.start == range.start && it->range.end == range.end && it->channels == ids && it->buckets == buckets &&
                it->filter.sensor == f.sensor && it->filter.type == f.type && it->filter.unit == f.unit) { cacheBytes_ -= it->bytes; it = cache_.erase(it); } else ++it;
        }
        cache_.push_front({*source_, f, range, ids, buckets, bytes, std::make_shared<Result>(r)}); cacheBytes_ += bytes;
        while (cache_.size() > 3 || cacheBytes_ > maxBytes) { cacheBytes_ -= cache_.back().bytes; cache_.pop_back(); }
    }
    static Result readWindow(const std::filesystem::path &input, const std::optional<SourceIdentity> &expected,
                             const Filter &f, Range range, const std::vector<std::string> &ids, std::size_t buckets, Cancel cancel) {
        Database db(input, {}, cancel, expected); Result result;
        result.page = db.page(f, range); result.events = db.events(f, range);
        if (ids.empty()) result.warning = QStringLiteral("未知编码：仅显示记录和原始字节，未猜测解码");
        else {
            try {
                result.plots = db.plots(f, range, ids, buckets);
                if (!result.plots.empty()) result.plot = result.plots.front();
                try { result.spectrum = db.spectrum(f, range, ids.front(), 1024); }
                catch (const SourceChanged &) { throw; }
                catch (...) {}
                const bool soundOrWave = ids.front() == "sound" || f.type.find("sound") != std::string::npos;
                if (soundOrWave && range.end - range.start <= 60000000LL) {
                    try { result.audioClip = db.audio(f, range, ids.front()); }
                    catch (const SourceChanged &) { throw; }
                    catch (...) {}
                }
            }
            catch (const SourceChanged &) { throw; }
            catch (const Error &error) { if (cancel->load()) throw; result.warning = qs(error.what()); }
        }
        return result;
    }
    void refreshPlotView() {
        if (!validSelection() || !loaded_) return;
        const bool wantSpectrum = viewModeBox_->currentData().toString() == QStringLiteral("spectrum");
        if (wantSpectrum && currentSpectrum_) {
            plot_->setSpectrum(*currentSpectrum_, currentUnit_, currentCaption_,
                               QStringLiteral("FFT 频谱视图 · 悬停查看各频点幅值"));
        } else {
            plot_->setTraces(currentTraces_, *loaded_, currentUnit_, currentCaption_, eventRows_, currentNote_);
            plot_->setPosition(clock_.position());
            plot_->setReadout(lastExactReadout_.isEmpty()
                ? QStringLiteral("悬停后读取原始样本；预览包络不是完整样本序列")
                : lastExactReadout_);
        }
        plot_->animateTransition();
    }
    void presentWindow(Result result, Range range, const std::vector<std::string> &ids, const QString &caption, const Filter &f, bool hit) {
        loaded_ = range; page_ = std::move(result.page); eventRows_ = std::move(result.events); populateRecords(); populateEvents();
        QString note = result.warning;
        if (note.isEmpty()) {
            const bool estimated = std::any_of(result.plots.begin(), result.plots.end(), [](const Plot &p) { return p.estimated_time; });
            note = estimated ? QStringLiteral("块内时间 = UTC 锚点 + index / rate（估计）") : QStringLiteral("原始记录 UTC 时间");
            note += QStringLiteral(" · 非好质量断线 · ") + (hit ? QStringLiteral("缓存命中") : QStringLiteral("原始读取"));
        }
        std::size_t decoded = 0; for (const auto &p : result.plots) decoded += p.samples_decoded;
        std::vector<PlotTrace> traces;
        const QColor colors[] = {
            currentThemeSpec_.tracePrimary,
            currentThemeSpec_.traceSecondary,
            currentThemeSpec_.traceTertiary
        };
        for (std::size_t n = 0; n < result.plots.size(); ++n)
            traces.push_back({qs(ids[n]), colors[n % 3], std::move(result.plots[n])});
        const auto scanned = result.plot.records_scanned, skipped = result.plot.quality_skipped;
        windowInfo_->setText(QStringLiteral("窗口 %1 s · 扫描 %2 条 / 解码 %3 点 / 跳过 %4")
            .arg(static_cast<double>(range.end - range.start) / 1e6, 0, 'g', 7).arg(scanned).arg(decoded).arg(skipped));

        currentTraces_ = std::move(traces);
        currentSpectrum_ = std::move(result.spectrum);
        currentUnit_ = qs(f.unit);
        currentCaption_ = caption;
        currentNote_ = note;

        if (result.audioClip && !result.audioClip->pcm16.empty()) {
            audioPlayer_.load(std::move(*result.audioClip));
            audioPlayer_.set_volume(static_cast<double>(volumeSlider_->value()) / 100.0);
            audioPlayer_.set_muted(!audioEnable_->isChecked());
            playAudioNow_->setEnabled(true);
            exportWavAction_->setEnabled(true);
            audioStatusLabel_->setText(QStringLiteral("声音就绪 (%1 Hz)").arg(audioPlayer_.sample_rate_hz()));
            if (clock_.state() == PlaybackState::playing && audioEnable_->isChecked()) {
                audioPlayer_.seek(clock_.position());
                audioPlayer_.play();
            }
        } else {
            audioPlayer_.clear();
            const bool canExportWave = !ids.empty() && overview_.series[static_cast<std::size_t>(selected_)].kind == 2 &&
                                       overview_.series[static_cast<std::size_t>(selected_)].encoding != "iio-le-72-v1";
            playAudioNow_->setEnabled(false);
            exportWavAction_->setEnabled(canExportWave);
            audioStatusLabel_->setText(QStringLiteral("非声音通道"));
        }

        refreshPlotView();
        statusBar()->showMessage(result.warning.isEmpty() ? QStringLiteral("当前窗口已加载 · 导出使用完整所选区间") : result.warning);
        if (smoke_) smokeWindowLoaded(result.warning);
    }
    void loadWindow(bool force = false) {
        if (!validSelection()) return;
        const auto position = clock_.position();
        if (!force && loaded_ && position >= loaded_->start && (position < loaded_->end || position == clock_.range().end)) return;
        if (!force && requested_) return;
        const auto range = currentWindow(); const auto input = path_; const auto expected = source_; const auto f = filter();
        const auto ids = selectedChannels(); const auto buckets = static_cast<std::size_t>(std::clamp(plot_->width() * 2, 300, 3000));
        const QString caption = qs(f.sensor) + QStringLiteral(" / ") + qs(f.type) + QStringLiteral(" / ") + channelBox_->currentText();
        auto entry = cached(range, ids, buckets); requested_ = range; pendingProbe_.reset(); ++probeSerial_;
        submit(QStringLiteral("加载当前窗口…"), [input, expected, f, range, ids, buckets, entry](Cancel cancel) {
            if (entry) { Database verify(input, {}, cancel, expected); Result result = *entry; (void)verify.identity(); return result; }
            return readWindow(input, expected, f, range, ids, buckets, cancel);
        }, [this, range, ids, buckets, caption, f, hit = static_cast<bool>(entry)](Result result) {
            if (hit) ++cacheHits_; else ++cacheMisses_;
            remember(range, ids, buckets, result); presentWindow(std::move(result), range, ids, caption, f, hit);
        });
    }
    void startBackground(QString title, std::function<Result(Cancel)> work, std::function<void(Result)> apply) {
        if (active_ || pending_ || closing_) return;
        start(Job{generation_, std::move(title), std::move(work), std::move(apply), true});
    }
    void scheduleBackground() {
        if (active_ || pending_ || requested_ || !validSelection() || !loaded_) return;
        const auto input = path_; const auto expected = source_; const auto f = filter(); const auto range = *loaded_; const auto ids = selectedChannels();
        if (pendingProbe_ && !ids.empty()) {
            const Time target = *pendingProbe_; pendingProbe_.reset(); const auto serial = probeSerial_;
            const bool estimated = overview_.series[static_cast<std::size_t>(selected_)].kind == 2 && overview_.series[static_cast<std::size_t>(selected_)].encoding != "iio-le-72-v1";
            startBackground(QStringLiteral("精确原始读数"), [input, expected, f, range, ids, target, estimated](Cancel cancel) {
                Database db(input, {}, cancel, expected); Result result; QStringList values;
                for (const auto &id : ids) {
                    const auto point = db.probe(f, range, id, target);
                    if (point) values << QStringLiteral("%1=%2 @ %3（Δ=%4 μs）").arg(qs(id), number(point->value), utcTime(point->timestamp)).arg(point->timestamp - target);
                    else values << qs(id) + QStringLiteral("：窗口内无有效原始样本");
                }
                result.readout = QStringLiteral("原始样本，不插值；最近有效样本%1：").arg(estimated ? QStringLiteral("；时间为采样率重建估计") : QString()) + values.join(QStringLiteral(" | ")); return result;
            }, [this, serial](Result result) { if (serial == probeSerial_) { lastExactReadout_ = result.readout; plot_->setReadout(result.readout); } }); return;
        }
        if (customView_) return;
        const auto total = clock_.range(); if (range.end >= total.end) return;
        const Range next = windowAt(range.end);
        if (next.start == range.start && next.end == range.end) return;
        const auto buckets = static_cast<std::size_t>(std::clamp(plot_->width() * 2, 300, 3000));
        const auto key = cacheKey(next, ids, buckets);
        if (key == prefetchAttempt_ || cached(next, ids, buckets)) return;
        prefetchAttempt_ = key;
        startBackground(QStringLiteral("预取下一窗口"), [input, expected, f, next, ids, buckets](Cancel cancel) { return readWindow(input, expected, f, next, ids, buckets, cancel); },
            [this, next, ids, buckets](Result result) { remember(next, ids, buckets, result); });
    }
    void loadPage(Cursor cursor) {
        if (!validSelection() || !loaded_) return;
        const auto input = path_; const auto expected = source_; const auto selectedFilter = filter(); const Range range = *loaded_;
        submit(QStringLiteral("读取记录页…"), [input, expected, selectedFilter, range, cursor](Cancel cancel) {
            Database db(input, {}, cancel, expected); Result result; result.page = db.page(selectedFilter, range, cursor); return result;
        }, [this](Result result) { page_ = std::move(result.page); populateRecords(); statusBar()->showMessage(QStringLiteral("记录页已加载")); });
    }
    void populateRecords() {
        const QSignalBlocker block(records_);
        records_->setUpdatesEnabled(false);
        records_->clearContents(); records_->setRowCount(static_cast<int>(page_.records.size()));
        for (std::size_t i = 0; i < page_.records.size(); ++i) {
            const auto &r = page_.records[i];
            const QString kind = r.kind == 1 ? QStringLiteral("标量") : r.kind == 2 ? qs(r.encoding) : QStringLiteral("缺失");
            const QString value = r.kind == 1 ? optionalNumber(r.scalar) : r.kind == 2 ? QStringLiteral("%1 B").arg(r.block.size()) : QStringLiteral("—");
            const QStringList values{qs(r.id), rawTime(r.timestamp), kind, value, qs(quality_text(r.quality)), rawTime(r.duration), optionalNumber(r.rate)};
            for (int col = 0; col < values.size(); ++col) records_->setItem(static_cast<int>(i), col, new QTableWidgetItem(values[col]));
        }
        records_->resizeColumnsToContents();
        for (int column = 0; column < records_->columnCount(); ++column)
            records_->setColumnWidth(column, std::clamp(records_->columnWidth(column), 64, 360));
        records_->setUpdatesEnabled(true);
        nextPage_->setEnabled(page_.more);
        pageLabel_->setText(QStringLiteral("当前页 %1 条%2 · 仅含 timestamp 位于窗口内的记录")
            .arg(page_.records.size()).arg(page_.more ? QStringLiteral("，还有后续页") : QStringLiteral("，已到末页")));
        if (!page_.records.empty()) { records_->selectRow(0); showDetails(); } else { details_->clear(); detailPreview_->clear(); }
    }
    void populateEvents() {
        events_->setUpdatesEnabled(false);
        events_->clearContents(); events_->setRowCount(static_cast<int>(eventRows_.size()));
        static const QStringList assessment{QStringLiteral("未评估"), QStringLiteral("正常"), QStringLiteral("警告"), QStringLiteral("异常"), QStringLiteral("证据不足")};
        for (std::size_t i = 0; i < eventRows_.size(); ++i) {
            const auto &event = eventRows_[i];
            const QString assessmentText = event.assessment >= 0 && event.assessment < assessment.size() ? assessment[event.assessment] : QString::number(event.assessment);
            const QStringList values{rawTime(event.start), rawTime(event.end), assessmentText, QString::number(event.severity), qs(event.event_type),
                QStringLiteral("r%1 / %2").arg(event.revision).arg(event.confirmed ? QStringLiteral("已确认") : QStringLiteral("未确认")), qs(event.description)};
            for (int col = 0; col < values.size(); ++col) events_->setItem(static_cast<int>(i), col, new QTableWidgetItem(values[col]));
        }
        for (int column = 0; column < 6; ++column) events_->resizeColumnToContents(column);
        events_->setUpdatesEnabled(true);
    }
    void showDetails() {
        const int row = records_->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= page_.records.size()) return;
        const auto &r = page_.records[static_cast<std::size_t>(row)];
        QString text = QStringLiteral("ID: %1\n传感器: %2\n指标: %3\n单位: %4\nUTC: %5\n时间 μs: %6\n序列: %7\nkind: %8\n质量: %9\n时长 μs: %10\n采样率 Hz: %11\n编码: %12\n来源: %13\n批次: %14\n")
            .arg(qs(r.id), qs(r.sensor), qs(r.type), qs(r.unit), utcTime(r.timestamp), rawTime(r.timestamp))
            .arg(r.sequence).arg(r.kind).arg(qs(quality_text(r.quality)), rawTime(r.duration), optionalNumber(r.rate), qs(r.encoding), qs(r.source), qs(r.batch));
        if (r.scalar) text += QStringLiteral("值: %1\n").arg(number(*r.scalar));
        text += QStringLiteral("\n元数据（长度前缀解码）:\n");
        try {
            for (const auto &entry : unpack_metadata(r.metadata)) text += qs(entry.first) + QStringLiteral(" = ") + qs(entry.second) + QLatin1Char('\n');
        } catch (const std::exception &error) { text += QStringLiteral("元数据错误: ") + qs(error.what()) + QLatin1Char('\n'); }
        if (!r.block.empty()) {
            text += QStringLiteral("\nBLOB: %1 字节\n十六进制前 256 字节:\n").arg(r.block.size());
            const auto count = std::min<std::size_t>(256, r.block.size());
            for (std::size_t i = 0; i < count; ++i) {
                text += QStringLiteral("%1 ").arg(r.block[i], 2, 16, QLatin1Char('0'));
                if ((i + 1) % 16 == 0) text += QLatin1Char('\n');
            }
            if (r.block.size() > count) text += QStringLiteral("\n…完整载荷可通过 CSV 的 block_hex 导出。");
        }
        details_->setPlainText(text); detailPreview_->setPlainText(text);
    }
    void refreshPosition() {
        if (!validSelection()) return;
        const auto range = clock_.range(); const auto position = clock_.position();
        stateLabel_->setText(stateText(clock_.state()));
        positionLabel_->setText(utcTime(position) + QStringLiteral("   (%1 μs)").arg(rawTime(position)));
        if (!timeline_->isSliderDown()) {
            const auto value = static_cast<int>(static_cast<long double>(position - range.start) * 1000000.0L / static_cast<long double>(range.end - range.start));
            const QSignalBlocker block(timeline_);
            timeline_->setValue(std::clamp(value, 0, 1000000));
        }
        plot_->setPosition(position);
        const bool isPlaying = clock_.state() == PlaybackState::playing;
        double level = 0.0;
        if (isPlaying) {
            const double phase = static_cast<double>(position % 1000000LL) * 1e-6;
            const double wave = 0.42 + 0.35 * std::abs(std::sin(phase * 18.84955)) + 0.23 * std::abs(std::cos(phase * 31.4159));
            const double vol = audioEnable_->isChecked() ? static_cast<double>(volumeSlider_->value()) / 100.0 : 0.35;
            level = std::clamp(wave * vol, 0.12, 1.0);
        }
        plot_->setPlaying(isPlaying, level);
        audioLevelMeter_->setValue(isPlaying && audioPlayer_.has_clip() && audioEnable_->isChecked()
                                       ? static_cast<int>(std::lround(level * 100.0))
                                       : 0);
    }
    void seekFromTimeline(int value) {
        if (!validSelection()) return;
        const auto range = clock_.range();
        const auto offset = static_cast<Time>(static_cast<long double>(range.end - range.start) * value / 1000000.0L);
        seekTo(std::min(range.end, range.start + offset));
    }
    void seekTo(Time time) {
        if (!validSelection()) return;
        const auto range = clock_.range();
        if (time < range.start || time > range.end) { fail(QStringLiteral("跳转时间超出当前回放区间")); return; }
        if (customView_ && (time < customView_->start || time >= customView_->end)) customView_.reset();
        clock_.seek(time);
        if (audioPlayer_.has_clip()) audioPlayer_.seek(time);
        lastTick_ = std::chrono::steady_clock::now();
        refreshPosition(); loadWindow();
    }
    void tick() {
        if (themePref_ == ThemePreference::FollowSystem) {
            if (++sysThemeCheckTicks_ >= 20) {
                sysThemeCheckTicks_ = 0;
                const bool sysDark = detectSystemDarkMode();
                if (sysDark != lastSystemDark_) {
                    lastSystemDark_ = sysDark;
                    transitionToTheme(sysDark, platformFlavor_, true);
                }
            }
        }
        if (active_ && current_ && current_->title == QStringLiteral("导出原始记录…") && exportProgress_) {
            statusBar()->showMessage(QStringLiteral("正在导出：%1 条记录 / %2 字节 · 可取消")
                .arg(exportProgress_->records.load()).arg(exportProgress_->bytes.load()));
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastTick_); lastTick_ = now;
        if (!validSelection() || clock_.state() != PlaybackState::playing) { scheduleBackground(); return; }
        clock_.advance(elapsed);
        if (clock_.state() == PlaybackState::finished) audioPlayer_.stop();
        if (customView_ && (clock_.position() < customView_->start || clock_.position() >= customView_->end)) customView_.reset();
        refreshPosition(); loadWindow(); scheduleBackground();
    }
    void exportCsv() {
        if (!validSelection()) return;
        const QString output = QFileDialog::getSaveFileName(this, QStringLiteral("导出选中区间的原始记录"), QStringLiteral("edgeecho-export.csv"), QStringLiteral("CSV (*.csv)"));
        if (output.isEmpty()) return;
        const auto input = path_; const auto expected = source_; const auto destination = pathFromQt(output); const auto selectedFilter = filter(); const auto range = clock_.range();
        clock_.pause(); audioPlayer_.pause(); pendingProbe_.reset(); ++probeSerial_; refreshPosition();
        const auto progress = std::make_shared<ProgressCounters>(); exportProgress_ = progress;
        submit(QStringLiteral("导出原始记录…"), [input, expected, destination, selectedFilter, range, progress](Cancel cancel) {
            Database db(input, {}, cancel, expected); Result result;
            ExportOptions options;
            options.progress = [progress](const ExportProgress &value) {
                progress->records.store(value.records_written); progress->bytes.store(value.bytes_written);
            };
            result.exported = db.export_csv(destination, selectedFilter, range, std::move(options)); result.path = destination; return result;
        }, [this](Result result) { exportProgress_.reset(); statusBar()->showMessage(QStringLiteral("已导出 %1 条记录：%2").arg(result.exported).arg(pathToQt(result.path))); });
    }
    void exportWav() {
        if (!validSelection()) return;
        const auto ids = selectedChannels();
        if (ids.empty()) { fail(QStringLiteral("当前通道不支持导出 WAV 音频")); return; }
        const QString output = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前窗口/区间 WAV 音频（不覆盖已有文件）"), QStringLiteral("edgeecho-audio.wav"), QStringLiteral("WAV 音频 (*.wav)"));
        if (output.isEmpty()) return;
        const auto input = path_; const auto expected = source_; const auto destination = pathFromQt(output);
        const auto selectedFilter = filter(); const auto range = loaded_ ? *loaded_ : clock_.range();
        const auto channel = ids.front();
        clock_.pause(); audioPlayer_.pause(); refreshPosition();
        submit(QStringLiteral("导出 WAV 音频…"), [input, expected, destination, selectedFilter, range, channel](Cancel cancel) {
            Database db(input, {}, cancel, expected); Result result;
            result.exported = db.export_wav(destination, selectedFilter, range, channel);
            result.path = destination;
            return result;
        }, [this](Result result) {
            statusBar()->showMessage(QStringLiteral("已导出 %1 个音频样本：%2").arg(result.exported).arg(pathToQt(result.path)));
        });
    }
    void createDemo() {
        const QString output = QFileDialog::getSaveFileName(this, QStringLiteral("新建演示数据库（不覆盖已有文件）"), QStringLiteral("edgeecho-demo.db"), QStringLiteral("SQLite 数据库 (*.db)"));
        if (output.isEmpty()) return;
        const auto destination = pathFromQt(output);
        clock_.pause(); audioPlayer_.pause(); refreshPosition();
        submit(QStringLiteral("创建演示数据…"), [destination](Cancel cancel) {
            if (cancel->load()) throw Error("cancelled");
            create_demo(destination); Result result; result.path = destination; return result;
        }, [this](Result result) { openPath(std::move(result.path)); });
    }
    void smokeWindowLoaded(const QString &warning) {
        if (smokeDone_) return;
        if (!warning.isEmpty() && !channelBox_->currentData().toString().isEmpty()) {
            fail(QStringLiteral("Known-channel plot failed: ") + warning); return;
        }
        ++smokeWindows_;
        if (smokePhase_ == 0) {
            if (smokeChannelChecks_ == 0 && channelBox_->count() > 1) {
                smokeChannelChecks_ = 1;
                channelBox_->setCurrentIndex(channelBox_->count() - 1);
                return;
            }
            const int seriesLimit = static_cast<int>(std::min<std::size_t>(4, overview_.series.size()));
            if (selected_ + 1 < seriesLimit) {
                smokeChannelChecks_ = 0;
                seriesList_->setCurrentRow(selected_ + 1);
                return;
            }
            smokePhase_ = 1;
            loadWindow(true);
            return;
        }
        if (smokePhase_ == 1) {
            const auto range = clock_.range();
            clock_.stop(); clock_.play(); clock_.advance(std::chrono::microseconds(1500)); clock_.pause();
            const Time paused = clock_.position(); clock_.advance(std::chrono::seconds(1));
            if (clock_.position() != paused) { fail(QStringLiteral("Paused clock advanced")); return; }
            speed_->setValue(8);
            clock_.stop(); clock_.play(); clock_.advance(std::chrono::microseconds(1000)); clock_.pause();
            if (clock_.position() != range.start + std::min<Time>(8000, range.end - range.start)) {
                fail(QStringLiteral("GUI speed control failed")); return;
            }
            speed_->setValue(1);
            smokePhase_ = 2;
            timeline_->setValue(750000);
            QMetaObject::invokeMethod(timeline_, "sliderReleased", Qt::DirectConnection);
            smokeExpected_ = clock_.position();
            loadWindow(true);
            return;
        }
        if (smokePhase_ == 2) {
            if (clock_.position() != smokeExpected_ || !loaded_ ||
                clock_.position() < loaded_->start || clock_.position() > loaded_->end) {
                fail(QStringLiteral("GUI timeline/window seek failed")); return;
            }
            smokePhase_ = 3;
            clock_.stop(); refreshPosition(); loadWindow(true);
            return;
        }
        finishSmoke();
    }
    void finishSmoke() {
        if (smokeDone_) return;
        smokeDone_ = true;
        QTimer::singleShot(150, this, [this] {
            if (validSelection()) {
                clock_.play(); clock_.advance(std::chrono::microseconds(1500)); clock_.pause();
                clock_.seek(clock_.range().start); refreshPosition();
            }
            if (!screenshot_.isEmpty() && !grab().save(screenshot_)) {
                std::cerr << "GUI smoke screenshot failed\n"; QApplication::exit(3); return;
            }
            std::cout << "GUI smoke passed: series=" << overview_.series.size()
                      << " records_on_page=" << page_.records.size()
                      << " plot_points=" << plot_->plot().points.size()
                      << " windows_checked=" << smokeWindows_ << '\n';
            QApplication::exit(0);
        });
    }

#include "interaction_tests.hpp"

    struct ProgressCounters { std::atomic<std::uint64_t> records{0}, bytes{0}; };
    std::shared_ptr<ProgressCounters> exportProgress_;
    std::filesystem::path path_;
    std::optional<SourceIdentity> source_;
    bool opening_ = false, lastFailureSourceChanged_ = false;
    Overview overview_;
    int selected_ = -1;
    Page page_;
    std::vector<Event> eventRows_;
    std::vector<PlotTrace> currentTraces_;
    std::optional<Spectrum> currentSpectrum_;
    QString currentUnit_, currentCaption_, currentNote_;
    PlaybackClock clock_;
    AudioPlayer audioPlayer_;
    std::optional<Range> loaded_, requested_, customView_;
    std::deque<CacheEntry> cache_;
    std::size_t cacheBytes_ = 0, cacheHits_ = 0, cacheMisses_ = 0;
    std::string prefetchAttempt_;
    std::optional<Time> pendingProbe_;
    std::uint64_t probeSerial_ = 0;
    QString lastExactReadout_;
    std::size_t interactionExpectedCacheHits_ = 0;
    TimeRangeEditor *timeEditor_ = nullptr;
    std::chrono::steady_clock::time_point lastTick_;
    QFutureWatcher<Result> watcher_;
    Cancel cancel_;
    std::optional<Job> current_, pending_;
    std::uint64_t generation_ = 0;
    bool active_ = false, closing_ = false, updatingChannels_ = false, smoke_ = false, smokeDone_ = false;
    bool interaction_ = false, interactionDone_ = false, expectSourceRejection_ = false, expectedSourceErrorSeen_ = false;
    QString screenshot_;
    std::filesystem::path testOutput_, interactionSource_;
    QTimer interactionTimer_;
    int interactionPhase_ = 0, interactionScalar_ = -1, interactionLoggedPhase_ = -1;
    bool interactionWasSourceValid_ = false;
    Time interactionPosition_ = 0;
    std::string interactionLastId_;
    QJsonArray interactionChecks_;
    std::chrono::steady_clock::time_point interactionWait_;
    int smokePhase_ = 0, smokeChannelChecks_ = 0;
    std::size_t smokeWindows_ = 0;
    Time smokeExpected_ = 0;
    QTimer timer_;
    PlatformFlavor platformFlavor_ = defaultPlatformFlavor();
    ThemePreference themePref_ = ThemePreference::FollowSystem;
    bool lastSystemDark_ = false;
    int sysThemeCheckTicks_ = 0;
    ThemeSpec currentThemeSpec_{}, themeFromSpec_{}, themeToSpec_{};
    NativeOsProxyStyle *proxyStyle_ = nullptr;
    ThemeTransitionOverlay *themeOverlay_ = nullptr;
    QVariantAnimation *themeAnim_ = nullptr;
    QLabel *sourceLabel_ = nullptr, *seriesInfo_ = nullptr, *windowInfo_ = nullptr, *stateLabel_ = nullptr, *positionLabel_ = nullptr, *pageLabel_ = nullptr, *audioStatusLabel_ = nullptr;
    QListWidget *seriesList_ = nullptr;
    QComboBox *channelBox_ = nullptr, *viewModeBox_ = nullptr, *themeBox_ = nullptr;
    QLineEdit *fromEdit_ = nullptr, *toEdit_ = nullptr, *seekEdit_ = nullptr;
    QDoubleSpinBox *windowSeconds_ = nullptr, *speed_ = nullptr;
    QSlider *timeline_ = nullptr, *volumeSlider_ = nullptr;
    QCheckBox *audioEnable_ = nullptr;
    QProgressBar *busy_ = nullptr, *audioLevelMeter_ = nullptr;
    QVariantAnimation *drawerAnim_ = nullptr;
    bool drawerExpanding_ = true;
    int drawerLastHeight_ = 240;
    QTabWidget *inspectorTabs_ = nullptr;
    QTableWidget *records_ = nullptr, *events_ = nullptr;
    QTextBrowser *details_ = nullptr, *detailPreview_ = nullptr;
    QPushButton *nextPage_ = nullptr, *firstPage_ = nullptr, *play_ = nullptr, *pause_ = nullptr, *stop_ = nullptr, *cancelButton_ = nullptr, *playAudioNow_ = nullptr, *reloadButton_ = nullptr;
    QAction *openAction_ = nullptr, *dirAction_ = nullptr, *demoAction_ = nullptr, *exportAction_ = nullptr, *exportWavAction_ = nullptr, *toggleBottomAction_ = nullptr, *helpAction_ = nullptr;
    QAction *themeFollowAction_ = nullptr, *themeLightAction_ = nullptr, *themeDarkAction_ = nullptr;
    QAction *osLinuxAction_ = nullptr, *osWin11Action_ = nullptr;
    PlotWidget *plot_ = nullptr;
};
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "EdgeEcho " << edgeecho::version() << " " << edgeecho::build_id() << '\n'; return 0;
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("EdgeEcho"));
    QApplication::setOrganizationName(QStringLiteral("EdgeEcho"));
    QString input, smoke, screenshot, interaction, testOutput, osStyle, themeArg;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--smoke") && i + 1 < args.size()) smoke = args[++i];
        else if (args[i] == QStringLiteral("--interaction-test") && i + 1 < args.size()) interaction = args[++i];
        else if (args[i] == QStringLiteral("--test-output") && i + 1 < args.size()) testOutput = args[++i];
        else if (args[i] == QStringLiteral("--screenshot") && i + 1 < args.size()) screenshot = args[++i];
        else if (args[i] == QStringLiteral("--os-style") && i + 1 < args.size()) osStyle = args[++i];
        else if (args[i] == QStringLiteral("--theme") && i + 1 < args.size()) themeArg = args[++i];
        else if (args[i] == QStringLiteral("--help") || args[i] == QStringLiteral("-h")) {
            std::cout << "Usage: edgeecho [database_or_directory] [--os-style linux|win11] [--theme system|light|dark]\n"
                         "       edgeecho --smoke DATABASE [--screenshot PNG] [--os-style linux|win11] [--theme system|light|dark]\n"
                         "       edgeecho --interaction-test DATABASE --test-output DIR [--screenshot PNG]\n";
            return 0;
        } else if (input.isEmpty() && !args[i].startsWith(QLatin1Char('-'))) input = args[i];
        else { std::cerr << "Unknown or incomplete argument\n"; return 1; }
    }
    try {
        if (!smoke.isEmpty() && !interaction.isEmpty()) throw Error("Choose either --smoke or --interaction-test");
        if (!interaction.isEmpty() && testOutput.isEmpty()) throw Error("--interaction-test requires --test-output DIR");
        const auto flavor = osStyle.isEmpty() ? defaultPlatformFlavor() : parsePlatformFlavor(osStyle);
        const auto themeLower = themeArg.trimmed().toLower();
        const auto themePref = themeLower == QStringLiteral("dark")  ? ThemePreference::Dark
                             : themeLower == QStringLiteral("light") ? ThemePreference::Light
                                                                     : ThemePreference::FollowSystem;
        MainWindow window(smoke, screenshot, interaction, testOutput, flavor, themePref);
        window.show();
        if (smoke.isEmpty() && interaction.isEmpty() && !input.isEmpty()) QTimer::singleShot(0, &window, [&window, input] { window.openArgument(input); });
        return app.exec();
    } catch (const std::exception &error) {
        std::cerr << "EdgeEcho: " << error.what() << '\n'; return 1;
    }
}
