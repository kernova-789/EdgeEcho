#pragma once

#include "edgeecho/core.hpp"
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

namespace edgeecho::ui {
struct PlotTrace {
    QString label;
    QColor color;
    edgeecho::Plot plot;
};

// Displays bounded envelopes and FFT spectra with smooth native-styled transitions.
// Navigation requests a fresh database window; exact sample values arrive via setReadout().
class PlotWidget final : public QWidget {
  public:
    explicit PlotWidget(QWidget *parent = nullptr);
    void setData(Plot, Range, QString unit, QString caption,
                 std::vector<Event>, QString note);
    void setTraces(std::vector<PlotTrace>, Range, QString unit, QString caption,
                   std::vector<Event>, QString note);
    void setSpectrum(Spectrum, QString unit, QString caption, QString note);
    void setSpectrogram(Spectrogram, Range, QString unit, QString caption, QString note);
    void setDualView(std::vector<PlotTrace>, Spectrum, Range, QString unit, QString caption,
                     std::vector<Event>, QString note);
    bool isSpectrumMode() const noexcept { return spectrumMode_; }
    bool isSpectrogramMode() const noexcept { return spectrogramMode_; }
    bool isDualMode() const noexcept { return dualMode_; }
    const Spectrum &spectrum() const noexcept { return spectrum_; }
    const Spectrogram &spectrogram() const noexcept { return spectrogram_; }
    void setOverallRange(Range);
    Range plotRange() const noexcept { return range_; }
    std::size_t traceCount() const noexcept { return traces_.size(); }
    const std::vector<PlotTrace> &traces() const noexcept { return traces_; }
    const Plot &plot() const noexcept;
    QRectF plotArea() const;
    void setPosition(Time);
    void setPlaying(bool playing, double audioLevel = 0.0);
    void animateTransition();
    void clear(QString note = {});
    void setReadout(QString);
    std::function<void(Time)> seek;
    std::function<void(Range)> viewChanged;
    std::function<void(Time)> inspectRequested;
  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
  private:
    Time timeAt(double x, bool upperBoundary = false) const;
    Range boundedRange(Time start, Time span) const;
    void requestRange(Range);
    void resetView();
    void updateHover(QPointF);
    QString hoverPreview() const;
    void paintSpectrum(QPainter &);
    void paintSpectrogram(QPainter &);
    void paintDualView(QPainter &);
    std::vector<PlotTrace> traces_;
    Plot empty_;
    Spectrum spectrum_;
    Spectrogram spectrogram_;
    bool spectrumMode_ = false;
    bool spectrogramMode_ = false;
    bool dualMode_ = false;
    std::optional<double> hoverFrequency_;
    Range range_{0, 1}, overall_{0, 1}, dragRange_{0, 1};
    bool overallSet_ = false, dragging_ = false, moved_ = false, playing_ = false;
    Qt::MouseButton dragButton_ = Qt::NoButton;
    QPointF dragStart_, dragCurrent_;
    std::optional<Time> hoverTime_, pendingInspect_;
    Time position_ = 0;
    double transitionProgress_ = 1.0;
    double hoverAlpha_ = 0.0;
    double animatedHoverX_ = -1.0;
    double pulsePhase_ = 0.0;
    double audioLevel_ = 0.0;
    QString unit_, caption_ = QStringLiteral("选择数据库开始回放"), note_, readout_;
    std::vector<Event> events_;
    QTimer inspectTimer_;
    QVariantAnimation transitionAnim_;
    QVariantAnimation hoverAnim_;
    QTimer pulseTimer_;
};
} // namespace edgeecho::ui
