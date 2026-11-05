#include "plot_widget.hpp"

#include <QApplication>
#include <QDateTime>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QStyle>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace edgeecho::ui {
namespace {
bool valid(Range range) {
    return range.start >= 0 && range.end > range.start && range.end <= max_time;
}
QString utc(Time time) {
    return QDateTime::fromMSecsSinceEpoch(time / 1000, Qt::UTC)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) +
        QStringLiteral("%1 UTC").arg(time % 1000, 3, 10, QLatin1Char('0'));
}
bool same(Range a, Range b) { return a.start == b.start && a.end == b.end; }
const QColor fallbackColor("#0066cc");
QString navigationHelp() {
    return QStringLiteral("滚轮缩放；左键框选；右键或中键平移；单击跳转；双击或 Esc 复位。悬停包络值仅供预览，精确值单独查询。");
}
}

PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("数据回放曲线"));
    setToolTip(navigationHelp());
    inspectTimer_.setSingleShot(true);
    inspectTimer_.setInterval(120);
    connect(&inspectTimer_, &QTimer::timeout, this, [this] {
        const auto requested = pendingInspect_; pendingInspect_.reset();
        if (requested && inspectRequested) inspectRequested(*requested);
    });

    transitionAnim_.setDuration(360);
    transitionAnim_.setStartValue(0.0);
    transitionAnim_.setEndValue(1.0);
    transitionAnim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&transitionAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        transitionProgress_ = v.toDouble();
        update();
    });

    hoverAnim_.setDuration(160);
    hoverAnim_.setEasingCurve(QEasingCurve::OutQuad);
    connect(&hoverAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        hoverAlpha_ = v.toDouble();
        update();
    });

    pulseTimer_.setInterval(30);
    connect(&pulseTimer_, &QTimer::timeout, this, [this] {
        pulsePhase_ += 0.18;
        if (pulsePhase_ > 6.283185307179586)
            pulsePhase_ -= 6.283185307179586;
        update();
    });
}
void PlotWidget::animateTransition() {
    transitionAnim_.stop();
    transitionProgress_ = 0.0;
    transitionAnim_.start();
}
void PlotWidget::setPlaying(bool playing, double audioLevel) {
    playing_ = playing;
    audioLevel_ = std::clamp(audioLevel, 0.0, 1.0);
    if (playing_ && !pulseTimer_.isActive())
        pulseTimer_.start();
    else if (!playing_ && !spectrumMode_ && !spectrogramMode_ && !dualMode_ && pulseTimer_.isActive())
        pulseTimer_.stop();
    update();
}
void PlotWidget::setData(Plot plotData, Range range, QString unit, QString caption,
                         std::vector<Event> events, QString note) {
    std::vector<PlotTrace> traces;
    traces.push_back({QStringLiteral("Value"), fallbackColor, std::move(plotData)});
    setTraces(std::move(traces), range, std::move(unit), std::move(caption), std::move(events), std::move(note));
}
void PlotWidget::setTraces(std::vector<PlotTrace> traces, Range range, QString unit,
                           QString caption, std::vector<Event> events, QString note) {
    if (!valid(range)) throw Error("Plot requires a nonempty UTC range");
    if (traces.size() > 16) throw Error("Too many simultaneous plot traces");
    if (!overallSet_) overall_ = range;
    if (range.start < overall_.start || range.end > overall_.end)
        throw Error("Plot range exceeds selected source range");
    spectrumMode_ = false;
    spectrogramMode_ = false;
    dualMode_ = false;
    hoverFrequency_.reset();
    traces_ = std::move(traces); range_ = range; unit_ = std::move(unit);
    caption_ = std::move(caption); events_ = std::move(events); note_ = std::move(note);
    for (auto &trace : traces_) if (!trace.color.isValid()) trace.color = fallbackColor;
    hoverTime_.reset(); pendingInspect_.reset(); inspectTimer_.stop(); setReadout({});
    update();
}
void PlotWidget::setSpectrum(Spectrum spec, QString unit, QString caption, QString note) {
    spectrumMode_ = true;
    spectrogramMode_ = false;
    dualMode_ = false;
    spectrum_ = std::move(spec);
    unit_ = std::move(unit);
    caption_ = std::move(caption);
    note_ = std::move(note);
    hoverTime_.reset();
    hoverFrequency_.reset();
    pendingInspect_.reset();
    inspectTimer_.stop();
    setReadout({});
    if (!pulseTimer_.isActive())
        pulseTimer_.start();
    animateTransition();
    update();
}
void PlotWidget::setSpectrogram(Spectrogram sg, Range range, QString unit, QString caption, QString note) {
    if (!valid(range)) throw Error("Spectrogram requires a nonempty UTC range");
    if (!overallSet_) overall_ = range;
    spectrumMode_ = false;
    spectrogramMode_ = true;
    dualMode_ = false;
    spectrogram_ = std::move(sg);
    range_ = range;
    unit_ = std::move(unit);
    caption_ = std::move(caption);
    note_ = std::move(note);
    hoverTime_.reset();
    hoverFrequency_.reset();
    pendingInspect_.reset();
    inspectTimer_.stop();
    setReadout({});
    if (!pulseTimer_.isActive())
        pulseTimer_.start();
    animateTransition();
    update();
}
void PlotWidget::setDualView(std::vector<PlotTrace> traces, Spectrum spec, Range range, QString unit,
                             QString caption, std::vector<Event> events, QString note) {
    if (!valid(range)) throw Error("Dual view requires a nonempty UTC range");
    if (traces.size() > 16) throw Error("Too many simultaneous plot traces");
    if (!overallSet_) overall_ = range;
    spectrumMode_ = false;
    spectrogramMode_ = false;
    dualMode_ = true;
    traces_ = std::move(traces);
    for (auto &trace : traces_) if (!trace.color.isValid()) trace.color = fallbackColor;
    spectrum_ = std::move(spec);
    range_ = range;
    unit_ = std::move(unit);
    caption_ = std::move(caption);
    events_ = std::move(events);
    note_ = std::move(note);
    hoverTime_.reset();
    hoverFrequency_.reset();
    pendingInspect_.reset();
    inspectTimer_.stop();
    setReadout({});
    if (!pulseTimer_.isActive())
        pulseTimer_.start();
    animateTransition();
    update();
}
void PlotWidget::setOverallRange(Range range) {
    if (!valid(range)) throw Error("Plot navigation requires a nonempty source range");
    overall_ = range; overallSet_ = true;
    if (range_.start < range.start || range_.end > range.end) range_ = range;
}
const Plot &PlotWidget::plot() const noexcept { return traces_.empty() ? empty_ : traces_.front().plot; }
void PlotWidget::setPosition(Time position) { position_ = position; update(); }
void PlotWidget::clear(QString note) {
    traces_.clear(); events_.clear(); spectrum_ = {}; spectrogram_ = {};
    spectrumMode_ = false; spectrogramMode_ = false; dualMode_ = false;
    playing_ = false; audioLevel_ = 0.0; pulseTimer_.stop();
    setReadout({}); note_ = std::move(note);
    hoverTime_.reset(); hoverFrequency_.reset(); pendingInspect_.reset(); inspectTimer_.stop(); dragging_ = false;
    overallSet_ = false; range_ = overall_ = {0, 1}; update();
}
void PlotWidget::setReadout(QString text) {
    readout_ = std::move(text);
    setToolTip(readout_.isEmpty() ? navigationHelp() : navigationHelp() + QStringLiteral("\n\n") + readout_);
    update();
}
QRectF PlotWidget::plotArea() const {
    return QRectF(83, 66, std::max(1, width() - 103), std::max(1, height() - 144));
}
Time PlotWidget::timeAt(double x, bool upperBoundary) const {
    const auto box = plotArea();
    const auto fraction = std::clamp(static_cast<long double>((x - box.left()) / box.width()), 0.0L, 1.0L);
    const auto delta = fraction * static_cast<long double>(range_.end - range_.start);
    const auto offset = static_cast<Time>(upperBoundary ? std::ceil(delta) : std::floor(delta));
    return std::clamp(range_.start + offset, range_.start, upperBoundary ? range_.end : range_.end - 1);
}
Range PlotWidget::boundedRange(Time start, Time span) const {
    span = std::clamp(span, Time{1}, overall_.end - overall_.start);
    const auto bounded = std::clamp(start, overall_.start, overall_.end - span);
    return {bounded, bounded + span};
}
void PlotWidget::requestRange(Range requested) {
    if (!valid(requested)) return;
    requested = boundedRange(requested.start, requested.end - requested.start);
    if (same(requested, range_)) return;
    range_ = requested; hoverTime_.reset(); pendingInspect_.reset(); inspectTimer_.stop(); setReadout({});
    update();
    if (viewChanged) viewChanged(range_);
}
void PlotWidget::resetView() { requestRange(overall_); }
void PlotWidget::mousePressEvent(QMouseEvent *event) {
    if (spectrumMode_ || !plotArea().contains(event->position())) { QWidget::mousePressEvent(event); return; }
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton && event->button() != Qt::MiddleButton) return;
    setFocus(Qt::MouseFocusReason);
    dragging_ = true; moved_ = false; dragButton_ = event->button();
    dragStart_ = dragCurrent_ = event->position(); dragRange_ = range_;
    inspectTimer_.stop(); pendingInspect_.reset();
    if (dragButton_ != Qt::LeftButton) setCursor(Qt::ClosedHandCursor);
    event->accept();
}
void PlotWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!spectrumMode_ && dragging_) {
        dragCurrent_ = event->position();
        moved_ = moved_ || std::hypot(dragCurrent_.x() - dragStart_.x(), dragCurrent_.y() - dragStart_.y()) >= QApplication::startDragDistance();
        update(); event->accept(); return;
    }
    updateHover(event->position());
}
void PlotWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (spectrumMode_ || !dragging_ || event->button() != dragButton_) { QWidget::mouseReleaseEvent(event); return; }
    dragCurrent_ = event->position();
    moved_ = moved_ || std::hypot(dragCurrent_.x() - dragStart_.x(), dragCurrent_.y() - dragStart_.y()) >= QApplication::startDragDistance();
    const auto button = dragButton_; dragging_ = false; dragButton_ = Qt::NoButton; unsetCursor();
    if (button == Qt::LeftButton) {
        if (moved_ && std::abs(dragCurrent_.x() - dragStart_.x()) >= QApplication::startDragDistance()) {
            const auto begin = timeAt(std::min(dragStart_.x(), dragCurrent_.x()));
            const auto end = timeAt(std::max(dragStart_.x(), dragCurrent_.x()), true);
            if (end > begin) requestRange({begin, end});
        } else if (!moved_ && plotArea().contains(dragCurrent_) && seek) seek(timeAt(dragCurrent_.x()));
    } else if (moved_) {
        const auto shift = -static_cast<long double>(dragCurrent_.x() - dragStart_.x()) /
            static_cast<long double>(plotArea().width()) * static_cast<long double>(dragRange_.end - dragRange_.start);
        const auto maxShift = static_cast<long double>(overall_.end - overall_.start);
        const auto offset = static_cast<Time>(std::clamp(shift, -maxShift, maxShift));
        requestRange(boundedRange(dragRange_.start + offset, dragRange_.end - dragRange_.start));
    }
    update(); event->accept();
}
void PlotWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!spectrumMode_ && event->button() == Qt::LeftButton && plotArea().contains(event->position())) {
        dragging_ = false; dragButton_ = Qt::NoButton; resetView(); event->accept(); return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
void PlotWidget::wheelEvent(QWheelEvent *event) {
    if (spectrumMode_ || !plotArea().contains(event->position())) { QWidget::wheelEvent(event); return; }
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta == 0) { event->ignore(); return; }
    const auto box = plotArea();
    const auto fraction = std::clamp(static_cast<long double>((event->position().x() - box.left()) / box.width()), 0.0L, 1.0L);
    const auto steps = std::clamp(static_cast<long double>(delta) / 120.0L, -8.0L, 8.0L);
    const auto rawSpan = static_cast<long double>(range_.end - range_.start) / std::pow(1.5L, steps);
    const Time span = static_cast<Time>(std::clamp(std::round(rawSpan), 1.0L, static_cast<long double>(overall_.end - overall_.start)));
    const Time anchor = range_.start + static_cast<Time>(fraction * static_cast<long double>(range_.end - range_.start));
    const Time newOffset = static_cast<Time>(fraction * static_cast<long double>(span));
    requestRange(boundedRange(anchor - newOffset, span));
    event->accept();
}
void PlotWidget::keyPressEvent(QKeyEvent *event) {
    if (!spectrumMode_ && event->key() == Qt::Key_Escape) {
        dragging_ = false; dragButton_ = Qt::NoButton; unsetCursor(); resetView(); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}
void PlotWidget::leaveEvent(QEvent *event) {
    if (!dragging_) {
        hoverTime_.reset(); hoverFrequency_.reset(); pendingInspect_.reset(); inspectTimer_.stop(); setReadout({});
        hoverAnim_.stop(); hoverAnim_.setStartValue(hoverAlpha_); hoverAnim_.setEndValue(0.0); hoverAnim_.start();
        update();
    }
    QWidget::leaveEvent(event);
}
void PlotWidget::updateHover(QPointF position) {
    const auto box = plotArea();
    if (!box.contains(position)) {
        if (hoverTime_ || hoverFrequency_) {
            hoverTime_.reset(); hoverFrequency_.reset(); pendingInspect_.reset(); inspectTimer_.stop(); readout_.clear();
            hoverAnim_.stop(); hoverAnim_.setStartValue(hoverAlpha_); hoverAnim_.setEndValue(0.0); hoverAnim_.start();
            update();
        }
        return;
    }
    const bool entering = !hoverTime_.has_value() && !hoverFrequency_.has_value();
    animatedHoverX_ = animatedHoverX_ < 0.0 ? position.x() : animatedHoverX_ * 0.35 + position.x() * 0.65;
    if (entering) {
        hoverAnim_.stop(); hoverAnim_.setStartValue(hoverAlpha_); hoverAnim_.setEndValue(1.0); hoverAnim_.start();
    } else {
        hoverAlpha_ = 1.0;
    }
    if (spectrumMode_) {
        const double nyquist = spectrum_.sample_rate_hz > 0.0 ? spectrum_.sample_rate_hz * 0.5 : 1.0;
        const double frac = std::clamp((position.x() - box.left()) / box.width(), 0.0, 1.0);
        hoverFrequency_ = frac * nyquist;
        update();
        return;
    }
    if (spectrogramMode_) {
        const double nyquist = spectrogram_.sample_rate_hz > 0.0 ? spectrogram_.sample_rate_hz * 0.5 : 1.0;
        const double fy = std::clamp((box.bottom() - position.y()) / box.height(), 0.0, 1.0);
        hoverFrequency_ = fy * nyquist;
        hoverTime_ = timeAt(position.x());
        update();
        return;
    }
    const auto target = timeAt(position.x());
    if (!hoverTime_ || *hoverTime_ != target) readout_.clear();
    hoverTime_ = pendingInspect_ = target;
    if (!inspectTimer_.isActive()) inspectTimer_.start();
    update();
}
QString PlotWidget::hoverPreview() const {
    if (spectrumMode_) {
        if (!hoverFrequency_ || spectrum_.bins.empty()) return {};
        const SpectrumBin *nearest = &spectrum_.bins.front();
        double minDiff = std::abs(nearest->frequency_hz - *hoverFrequency_);
        for (const auto &bin : spectrum_.bins) {
            const double diff = std::abs(bin.frequency_hz - *hoverFrequency_);
            if (diff < minDiff) { nearest = &bin; minDiff = diff; }
        }
        return QStringLiteral("游标频率 %1 Hz  ·  幅值 %2 %3  ·  主频 %4 Hz (幅值 %5)")
            .arg(nearest->frequency_hz, 0, 'f', 2)
            .arg(nearest->magnitude, 0, 'g', 6)
            .arg(unit_)
            .arg(spectrum_.dominant_frequency_hz, 0, 'f', 2)
            .arg(spectrum_.dominant_magnitude, 0, 'g', 6);
    }
    if (spectrogramMode_) {
        if (!hoverTime_ || !hoverFrequency_ || spectrogram_.magnitudes_db.empty()) return {};
        const double nyquist = spectrogram_.sample_rate_hz > 0.0 ? spectrogram_.sample_rate_hz * 0.5 : 1.0;
        const double tx = std::clamp(static_cast<double>(*hoverTime_ - range_.start) /
                                     std::max<double>(1.0, static_cast<double>(range_.end - range_.start)), 0.0, 0.999999);
        const double fy = std::clamp(*hoverFrequency_ / nyquist, 0.0, 0.999999);
        const std::size_t col = std::min(spectrogram_.time_bins - 1, static_cast<std::size_t>(tx * static_cast<double>(spectrogram_.time_bins)));
        const std::size_t row = std::min(spectrogram_.freq_bins - 1, static_cast<std::size_t>(fy * static_cast<double>(spectrogram_.freq_bins)));
        const double dbVal = spectrogram_.magnitudes_db[col * spectrogram_.freq_bins + row];
        return QStringLiteral("游标 %1  ·  频率 %2 Hz  ·  能量 %3 dB  ·  主能峰 %4 Hz (%5 dB)")
            .arg(utc(*hoverTime_))
            .arg(*hoverFrequency_, 0, 'f', 1)
            .arg(dbVal, 0, 'f', 1)
            .arg(spectrogram_.dominant_frequency_hz, 0, 'f', 1)
            .arg(spectrogram_.max_db, 0, 'f', 1);
    }
    if (!hoverTime_) return {};
    QStringList pieces{QStringLiteral("游标 %1").arg(utc(*hoverTime_))};
    for (const auto &trace : traces_) {
        const Point *nearest = nullptr;
        Time distance = max_time;
        for (const auto &point : trace.plot.points) {
            const auto difference = point.timestamp > *hoverTime_ ? point.timestamp - *hoverTime_ : *hoverTime_ - point.timestamp;
            if (difference < distance) { nearest = &point; distance = difference; }
        }
        if (!nearest) continue;
        pieces << (nearest->gap ? trace.label + QStringLiteral("：质量断点")
            : QStringLiteral("%1≈%2").arg(trace.label, QString::number(nearest->value, 'g', 8)));
    }
    return pieces.join(QStringLiteral("  ·  ")) + QStringLiteral("（显示包络预览，非精确样本）");
}

void PlotWidget::paintSpectrum(QPainter &painter) {
    const auto pal = palette();
    const QColor textCol = pal.color(QPalette::WindowText);
    const QColor mutedText = pal.color(QPalette::PlaceholderText).isValid()
        ? pal.color(QPalette::PlaceholderText) : textCol.lighter(130);
    const QColor canvasBg = pal.color(QPalette::Base);
    const QColor borderCol = pal.color(QPalette::Mid);
    const bool darkCanvas = canvasBg.lightness() < 128;
    const QColor gridCol = darkCanvas ? canvasBg.lighter(135) : canvasBg.darker(110);
    const QColor curveCol = darkCanvas ? QColor("#4da3ff") : QColor("#005fb8");

    const auto box = plotArea();
    painter.fillRect(box, canvasBg);
    painter.setPen(QPen(gridCol, 1));
    for (int index = 0; index <= 4; ++index) {
        const auto y = box.top() + box.height() * index / 4.0;
        painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }
    for (int index = 0; index <= 5; ++index) {
        const auto x = box.left() + box.width() * index / 5.0;
        painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
    }
    painter.setPen(QPen(borderCol, 1));
    painter.drawRect(box);

    double maxMag = 0.0;
    for (const auto &bin : spectrum_.bins) {
        if (std::isfinite(bin.magnitude))
            maxMag = std::max(maxMag, bin.magnitude);
    }
    const double nyquist = spectrum_.sample_rate_hz > 0.0 ? spectrum_.sample_rate_hz * 0.5 : 1.0;
    const double yTop = maxMag > 0.0 ? maxMag * 1.08 : 1.0;
    const double reveal = std::clamp(transitionProgress_, 0.15, 1.0);

    if (!spectrum_.bins.empty() && maxMag > 0.0) {
        painter.save();
        painter.setClipRect(box);
        QPainterPath linePath;
        QPainterPath fillPath;
        bool first = true;
        QPointF peakPoint(box.left(), box.bottom());
        for (const auto &bin : spectrum_.bins) {
            const double fx = std::clamp(bin.frequency_hz / nyquist, 0.0, 1.0);
            const double fy = std::clamp((bin.magnitude / yTop) * reveal, 0.0, 1.0);
            const QPointF pt(box.left() + fx * box.width(), box.bottom() - fy * box.height());
            if (std::abs(bin.frequency_hz - spectrum_.dominant_frequency_hz) < 1e-6)
                peakPoint = pt;
            if (first) {
                linePath.moveTo(pt);
                fillPath.moveTo(box.left(), box.bottom());
                fillPath.lineTo(pt);
                first = false;
            } else {
                linePath.lineTo(pt);
                fillPath.lineTo(pt);
            }
        }
        fillPath.lineTo(box.right(), box.bottom());
        fillPath.closeSubpath();
        QLinearGradient grad(0, box.top(), 0, box.bottom());
        QColor topFill = curveCol; topFill.setAlpha(darkCanvas ? 90 : 65);
        QColor botFill = curveCol; botFill.setAlpha(darkCanvas ? 10 : 6);
        grad.setColorAt(0.0, topFill);
        grad.setColorAt(1.0, botFill);
        painter.fillPath(fillPath, grad);
        painter.setPen(QPen(curveCol, 2.0));
        painter.drawPath(linePath);

        if (spectrum_.dominant_frequency_hz > 0.0) {
            const double domX = box.left() + std::clamp(spectrum_.dominant_frequency_hz / nyquist, 0.0, 1.0) * box.width();
            painter.setPen(QPen(QColor("#d9534f"), 1.5, Qt::DashLine));
            painter.drawLine(QPointF(domX, box.top()), QPointF(domX, box.bottom()));

            const double pulseRadius = 4.0 + 3.0 * (0.5 + 0.5 * std::sin(pulsePhase_));
            QColor ringCol("#d9534f");
            ringCol.setAlpha(110);
            painter.setBrush(ringCol);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(peakPoint, pulseRadius, pulseRadius);
            painter.setBrush(QColor("#d9534f"));
            painter.drawEllipse(peakPoint, 3.2, 3.2);
        }
        if (hoverFrequency_) {
            const double hx = box.left() + std::clamp(*hoverFrequency_ / nyquist, 0.0, 1.0) * box.width();
            QColor hc = textCol; hc.setAlphaF(static_cast<float>(std::clamp(hoverAlpha_, 0.0, 1.0) * 0.65));
            painter.setPen(QPen(hc, 1.2, Qt::DotLine));
            painter.drawLine(QPointF(hx, box.top()), QPointF(hx, box.bottom()));
        }
        painter.restore();
    } else {
        painter.setPen(mutedText);
        painter.drawText(box.adjusted(15, 10, -15, -10), Qt::AlignCenter | Qt::TextWordWrap,
                         note_.isEmpty() ? QStringLiteral("当前区间没有足够样本计算频谱") : note_);
    }

    painter.setPen(textCol);
    for (int index = 0; index <= 4; ++index) {
        const double value = yTop * (4 - index) / 4.0;
        const double y = box.top() + box.height() * index / 4.0;
        painter.drawText(QRectF(2, y - 9, box.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(value, 'g', 4));
    }
    for (int index = 0; index <= 5; ++index) {
        const double freq = nyquist * index / 5.0;
        const double x = box.left() + box.width() * index / 5.0;
        painter.drawText(QRectF(x - 40, box.bottom() + 4, 80, 18), Qt::AlignCenter,
                         QStringLiteral("%1 Hz").arg(freq, 0, 'f', freq >= 100.0 ? 0 : 1));
    }
    painter.setPen(mutedText);
    painter.drawText(QRectF(box.left(), box.bottom() + 22, box.width(), 18), Qt::AlignLeft,
        QStringLiteral("FFT 单边幅值谱（Hann 窗） · 采样率 %1 Hz · 样本 %2 点 · 主频 %3 Hz")
            .arg(spectrum_.sample_rate_hz, 0, 'g', 6)
            .arg(spectrum_.samples_used)
            .arg(spectrum_.dominant_frequency_hz, 0, 'f', 2));
    painter.setPen(textCol);
    painter.drawText(QRectF(15, box.bottom() + 41, width() - 30, 18), Qt::AlignLeft, hoverPreview());
}

void PlotWidget::paintSpectrogram(QPainter &painter) {
    const auto pal = palette();
    const QColor textCol = pal.color(QPalette::WindowText);
    const QColor mutedText = pal.color(QPalette::PlaceholderText).isValid()
        ? pal.color(QPalette::PlaceholderText) : textCol.lighter(130);
    const QColor canvasBg = pal.color(QPalette::Base);
    const QColor borderCol = pal.color(QPalette::Mid);

    const auto fullBox = plotArea();
    const QRectF box(fullBox.left(), fullBox.top(), std::max(40.0, fullBox.width() - 36.0), fullBox.height());
    const QRectF barBox(box.right() + 8.0, box.top(), 14.0, box.height());

    const auto thermalColor = [](double t) -> QColor {
        t = std::clamp(t, 0.0, 1.0);
        struct Stop { double pos; int r, g, b; };
        static constexpr Stop stops[] = {
            {0.00,  14,  18,  46},
            {0.25,  24,  88, 168},
            {0.50,  18, 168, 142},
            {0.75, 240, 186,  42},
            {1.00, 232,  56,  52}
        };
        for (int i = 0; i < 4; ++i) {
            if (t <= stops[i + 1].pos) {
                const double u = (t - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
                const int r = static_cast<int>(std::lround(stops[i].r + (stops[i + 1].r - stops[i].r) * u));
                const int g = static_cast<int>(std::lround(stops[i].g + (stops[i + 1].g - stops[i].g) * u));
                const int b = static_cast<int>(std::lround(stops[i].b + (stops[i + 1].b - stops[i].b) * u));
                return QColor(r, g, b);
            }
        }
        return QColor(232, 56, 52);
    };

    painter.fillRect(box, canvasBg);
    const double nyquist = spectrogram_.sample_rate_hz > 0.0 ? spectrogram_.sample_rate_hz * 0.5 : 1.0;
    const double dbSpan = std::max(1.0, spectrogram_.max_db - spectrogram_.min_db);
    const double reveal = std::clamp(transitionProgress_, 0.15, 1.0);

    if (spectrogram_.time_bins > 0 && spectrogram_.freq_bins > 0 &&
        spectrogram_.magnitudes_db.size() == spectrogram_.time_bins * spectrogram_.freq_bins) {
        painter.save();
        painter.setClipRect(box);
        const double cellW = box.width() / static_cast<double>(spectrogram_.time_bins);
        const double cellH = box.height() / static_cast<double>(spectrogram_.freq_bins);
        const std::size_t visibleCols = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::ceil(static_cast<double>(spectrogram_.time_bins) * reveal)));

        for (std::size_t c = 0; c < visibleCols && c < spectrogram_.time_bins; ++c) {
            const double x = box.left() + static_cast<double>(c) * cellW;
            for (std::size_t r = 0; r < spectrogram_.freq_bins; ++r) {
                const double db = spectrogram_.magnitudes_db[c * spectrogram_.freq_bins + r];
                const double norm = std::clamp((db - spectrogram_.min_db) / dbSpan, 0.0, 1.0);
                const double y = box.bottom() - static_cast<double>(r + 1) * cellH;
                painter.fillRect(QRectF(x, y, cellW + 0.65, cellH + 0.65), thermalColor(norm));
            }
        }

        // Dominant time-frequency peak ring
        if (spectrogram_.dominant_frequency_hz > 0.0 && range_.end > range_.start) {
            const double px = box.left() + std::clamp(spectrogram_.dominant_time_ratio, 0.0, 1.0) * box.width();
            const double py = box.bottom() - std::clamp(spectrogram_.dominant_frequency_hz / nyquist, 0.0, 1.0) * box.height();
            const double pulseRadius = 5.0 + 3.0 * (0.5 + 0.5 * std::sin(pulsePhase_));
            painter.setPen(QPen(QColor(255, 255, 255, 220), 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(px, py), pulseRadius, pulseRadius);
        }

        // Playhead vertical line
        if (position_ >= range_.start && position_ < range_.end) {
            const double px = box.left() + static_cast<double>(position_ - range_.start) /
                static_cast<double>(range_.end - range_.start) * box.width();
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.8, Qt::DashLine));
            painter.drawLine(QPointF(px, box.top()), QPointF(px, box.bottom()));
        }

        // Hover crosshair
        if (hoverTime_ && hoverFrequency_) {
            const double hx = box.left() + std::clamp(
                static_cast<double>(*hoverTime_ - range_.start) /
                static_cast<double>(range_.end - range_.start), 0.0, 1.0) * box.width();
            const double hy = box.bottom() - std::clamp(*hoverFrequency_ / nyquist, 0.0, 1.0) * box.height();
            painter.setPen(QPen(QColor(255, 255, 255, 165), 1.0, Qt::DotLine));
            painter.drawLine(QPointF(hx, box.top()), QPointF(hx, box.bottom()));
            painter.drawLine(QPointF(box.left(), hy), QPointF(box.right(), hy));
        }
        painter.restore();
    } else {
        painter.setPen(mutedText);
        painter.drawText(box.adjusted(15, 10, -15, -10), Qt::AlignCenter | Qt::TextWordWrap,
                         note_.isEmpty() ? QStringLiteral("当前区间没有足够样本计算 STFT 时频瀑布图") : note_);
    }

    painter.setPen(QPen(borderCol, 1));
    painter.drawRect(box);

    // Thermal colorbar
    QLinearGradient barGrad(0, barBox.bottom(), 0, barBox.top());
    barGrad.setColorAt(0.00, thermalColor(0.00));
    barGrad.setColorAt(0.25, thermalColor(0.25));
    barGrad.setColorAt(0.50, thermalColor(0.50));
    barGrad.setColorAt(0.75, thermalColor(0.75));
    barGrad.setColorAt(1.00, thermalColor(1.00));
    painter.fillRect(barBox, barGrad);
    painter.drawRect(barBox);

    painter.setPen(textCol);
    for (int index = 0; index <= 4; ++index) {
        const double freq = nyquist * (4 - index) / 4.0;
        const double y = box.top() + box.height() * index / 4.0;
        painter.drawText(QRectF(2, y - 9, box.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 Hz").arg(freq, 0, 'f', freq >= 100.0 ? 0 : 1));
    }
    painter.drawText(QRectF(box.left(), box.bottom() + 4, box.width(), 18), Qt::AlignLeft, utc(range_.start));
    painter.setPen(mutedText);
    painter.drawText(QRectF(box.left(), box.bottom() + 22, fullBox.width(), 18), Qt::AlignLeft,
        QStringLiteral("STFT 时频瀑布图（50% 重叠 Hann 窗） · %1×%2 网格 · 采样率 %3 Hz · 主能峰 %4 Hz (%5 dB)")
            .arg(spectrogram_.time_bins)
            .arg(spectrogram_.freq_bins)
            .arg(spectrogram_.sample_rate_hz, 0, 'g', 6)
            .arg(spectrogram_.dominant_frequency_hz, 0, 'f', 1)
            .arg(spectrogram_.max_db, 0, 'f', 1));
    painter.setPen(textCol);
    painter.drawText(QRectF(12, box.bottom() + 40, width() - 24, 18), Qt::AlignLeft, hoverPreview());
}

void PlotWidget::paintDualView(QPainter &painter) {
    const auto pal = palette();
    const QColor textCol = pal.color(QPalette::WindowText);
    const QColor borderCol = pal.color(QPalette::Mid);
    const QColor canvasBg = pal.color(QPalette::Base);
    const bool darkCanvas = canvasBg.lightness() < 128;
    const QColor gridCol = darkCanvas ? canvasBg.lighter(135) : canvasBg.darker(110);
    const QColor mutedText = darkCanvas ? textCol.darker(125) : textCol.lighter(135);
    const QColor specCol = darkCanvas ? QColor("#4da3ff") : QColor("#005fb8");

    const auto fullBox = plotArea();
    const double gap = 22.0;
    const double topH = std::max(60.0, (fullBox.height() - gap) * 0.54);
    const double botH = std::max(60.0, fullBox.height() - gap - topH);
    const QRectF topBox(fullBox.left(), fullBox.top(), fullBox.width(), topH);
    const QRectF botBox(fullBox.left(), topBox.bottom() + gap, fullBox.width(), botH);

    // 1. Top Subplot: Time-Domain Waveform
    painter.fillRect(topBox, canvasBg);
    painter.setPen(QPen(gridCol, 1));
    for (int i = 0; i <= 2; ++i) {
        const double y = topBox.top() + topBox.height() * i / 2.0;
        painter.drawLine(QPointF(topBox.left(), y), QPointF(topBox.right(), y));
    }
    for (int i = 0; i <= 5; ++i) {
        const double x = topBox.left() + topBox.width() * i / 5.0;
        painter.drawLine(QPointF(x, topBox.top()), QPointF(x, topBox.bottom()));
    }
    painter.setPen(QPen(borderCol, 1));
    painter.drawRect(topBox);

    double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
    for (const auto &trace : traces_) {
        for (const auto &point : trace.plot.points)
            if (!point.gap && std::isfinite(point.value) && point.timestamp >= range_.start && point.timestamp < range_.end) {
                minimum = std::min(minimum, point.value);
                maximum = std::max(maximum, point.value);
            }
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) { minimum = -1.0; maximum = 1.0; }
    const double spanVal = (maximum > minimum) ? (maximum - minimum) * 1.12 : 2.0;
    const double midVal = 0.5 * (maximum + minimum);
    const double lowVal = midVal - 0.5 * spanVal;
    const double highVal = midVal + 0.5 * spanVal;

    const auto xFor = [&](Time time) {
        const double frac = static_cast<double>(time - range_.start) /
                            std::max<double>(1.0, static_cast<double>(range_.end - range_.start));
        return topBox.left() + frac * topBox.width();
    };
    const auto yFor = [&](double val) {
        const double frac = std::clamp((val - lowVal) / (highVal - lowVal), 0.0, 1.0);
        return topBox.bottom() - frac * topBox.height();
    };

    painter.save();
    painter.setClipRect(topBox);
    for (const auto &trace : traces_) {
        painter.setPen(QPen(trace.color, 2.0));
        QPainterPath curve;
        bool connected = false;
        for (const auto &pt : trace.plot.points) {
            if (pt.gap || !std::isfinite(pt.value)) { connected = false; continue; }
            const QPointF p(xFor(pt.timestamp), yFor(pt.value));
            if (!connected) { curve.moveTo(p); connected = true; } else curve.lineTo(p);
        }
        painter.drawPath(curve);
    }
    if (position_ >= range_.start && position_ < range_.end) {
        const double px = xFor(position_);
        painter.setPen(QPen(QColor("#d9534f"), 1.6, Qt::DashLine));
        painter.drawLine(QPointF(px, topBox.top()), QPointF(px, topBox.bottom()));
    }
    if (hoverTime_) {
        const double hx = xFor(*hoverTime_);
        painter.setPen(QPen(mutedText, 1.0, Qt::DotLine));
        painter.drawLine(QPointF(hx, topBox.top()), QPointF(hx, topBox.bottom()));
    }
    painter.restore();

    painter.setPen(textCol);
    painter.drawText(QRectF(2, topBox.top() - 4, topBox.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(highVal, 'g', 4));
    painter.drawText(QRectF(2, topBox.bottom() - 12, topBox.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(lowVal, 'g', 4));
    painter.setPen(mutedText);
    painter.drawText(QRectF(topBox.left() + 6, topBox.top() + 3, 240, 16), Qt::AlignLeft,
                     QStringLiteral("上视图：时域包络 (%1)").arg(unit_));

    // 2. Bottom Subplot: FFT Spectrum
    painter.fillRect(botBox, canvasBg);
    painter.setPen(QPen(gridCol, 1));
    for (int i = 0; i <= 2; ++i) {
        const double y = botBox.top() + botBox.height() * i / 2.0;
        painter.drawLine(QPointF(botBox.left(), y), QPointF(botBox.right(), y));
    }
    for (int i = 0; i <= 5; ++i) {
        const double x = botBox.left() + botBox.width() * i / 5.0;
        painter.drawLine(QPointF(x, botBox.top()), QPointF(x, botBox.bottom()));
    }
    painter.setPen(QPen(borderCol, 1));
    painter.drawRect(botBox);

    double maxMag = 0.0;
    for (const auto &bin : spectrum_.bins)
        if (std::isfinite(bin.magnitude)) maxMag = std::max(maxMag, bin.magnitude);
    const double nyquist = spectrum_.sample_rate_hz > 0.0 ? spectrum_.sample_rate_hz * 0.5 : 1.0;
    const double yTop = maxMag > 0.0 ? maxMag * 1.08 : 1.0;
    const double reveal = std::clamp(transitionProgress_, 0.15, 1.0);

    if (!spectrum_.bins.empty() && maxMag > 0.0) {
        painter.save();
        painter.setClipRect(botBox);
        QPainterPath linePath, fillPath;
        bool first = true;
        for (const auto &bin : spectrum_.bins) {
            const double fx = std::clamp(bin.frequency_hz / nyquist, 0.0, 1.0);
            const double fy = std::clamp((bin.magnitude / yTop) * reveal, 0.0, 1.0);
            const QPointF pt(botBox.left() + fx * botBox.width(), botBox.bottom() - fy * botBox.height());
            if (first) {
                linePath.moveTo(pt);
                fillPath.moveTo(botBox.left(), botBox.bottom());
                fillPath.lineTo(pt);
                first = false;
            } else {
                linePath.lineTo(pt);
                fillPath.lineTo(pt);
            }
        }
        fillPath.lineTo(botBox.right(), botBox.bottom());
        fillPath.closeSubpath();
        QLinearGradient grad(0, botBox.top(), 0, botBox.bottom());
        QColor topFill = specCol; topFill.setAlpha(darkCanvas ? 85 : 60);
        QColor botFill = specCol; botFill.setAlpha(darkCanvas ? 10 : 6);
        grad.setColorAt(0.0, topFill);
        grad.setColorAt(1.0, botFill);
        painter.fillPath(fillPath, grad);
        painter.setPen(QPen(specCol, 1.8));
        painter.drawPath(linePath);
        if (spectrum_.dominant_frequency_hz > 0.0) {
            const double domX = botBox.left() + std::clamp(spectrum_.dominant_frequency_hz / nyquist, 0.0, 1.0) * botBox.width();
            painter.setPen(QPen(QColor("#d9534f"), 1.4, Qt::DashLine));
            painter.drawLine(QPointF(domX, botBox.top()), QPointF(domX, botBox.bottom()));
        }
        painter.restore();
    }
    painter.setPen(mutedText);
    painter.drawText(QRectF(botBox.left() + 6, botBox.top() + 3, 320, 16), Qt::AlignLeft,
                     QStringLiteral("下视图：FFT 频谱 (主频 %1 Hz)").arg(spectrum_.dominant_frequency_hz, 0, 'f', 2));
    painter.setPen(textCol);
    for (int index = 0; index <= 5; ++index) {
        const double freq = nyquist * index / 5.0;
        const double x = botBox.left() + botBox.width() * index / 5.0;
        painter.drawText(QRectF(x - 40, botBox.bottom() + 4, 80, 18), Qt::AlignCenter,
                         QStringLiteral("%1 Hz").arg(freq, 0, 'f', freq >= 100.0 ? 0 : 1));
    }
    painter.setPen(mutedText);
    painter.drawText(QRectF(botBox.left(), botBox.bottom() + 21, botBox.width(), 18), Qt::AlignLeft,
                     QStringLiteral("联动双视图 · 窗口 +%1 s · 单位 %2 · 主频 %3 Hz")
                         .arg(static_cast<double>(range_.end - range_.start) / 1e6, 0, 'g', 7)
                         .arg(unit_)
                         .arg(spectrum_.dominant_frequency_hz, 0, 'f', 2));
    painter.setPen(textCol);
    painter.drawText(QRectF(12, botBox.bottom() + 39, width() - 24, 18), Qt::AlignLeft, hoverPreview());
    painter.drawText(QRectF(12, botBox.bottom() + 57, width() - 24, 18), Qt::AlignLeft, readout_);
}

void PlotWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto pal = palette();
    const QColor winBg = pal.color(QPalette::Window);
    const QColor canvasBg = pal.color(QPalette::Base);
    const QColor textCol = pal.color(QPalette::WindowText);
    const QColor borderCol = pal.color(QPalette::Mid);
    const bool darkCanvas = canvasBg.lightness() < 128;
    const QColor gridCol = darkCanvas ? canvasBg.lighter(135) : canvasBg.darker(110);
    const QColor mutedText = darkCanvas ? textCol.darker(125) : textCol.lighter(135);

    painter.fillRect(rect(), winBg);
    const auto box = plotArea();
    QFont titleFont = painter.font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(textCol);
    painter.drawText(QRectF(12, 4, width() - 24, 22), Qt::AlignLeft | Qt::AlignVCenter, caption_);
    titleFont.setBold(false);
    painter.setFont(titleFont);
    painter.setPen(mutedText);
    painter.drawText(QRectF(12, 24, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter, note_);

    // Animated Equalizer / Playback Activity Indicator in top-right corner
    if (playing_) {
        const double rightEdge = box.right() - 4.0;
        painter.save();
        painter.setPen(Qt::NoPen);
        const QColor barCol = pal.color(QPalette::Highlight);
        for (int b = 0; b < 5; ++b) {
            const double wave = 0.35 + 0.65 * std::abs(std::sin(pulsePhase_ * 1.3 + b * 0.85));
            const double h = std::clamp(4.0 + 14.0 * wave * (0.4 + 0.6 * audioLevel_), 3.0, 18.0);
            const QRectF barRect(rightEdge - (5 - b) * 6.0, 22.0 - h, 4.0, h);
            painter.setBrush(barCol);
            painter.drawRoundedRect(barRect, 1.5, 1.5);
        }
        painter.restore();
    }

    if (spectrumMode_) {
        paintSpectrum(painter);
        return;
    }
    if (spectrogramMode_) {
        paintSpectrogram(painter);
        return;
    }
    if (dualMode_) {
        paintDualView(painter);
        return;
    }

    int legendX = static_cast<int>(box.left());
    for (const auto &trace : traces_) {
        painter.setPen(QPen(trace.color, 2.2));
        painter.drawLine(legendX, 53, legendX + 18, 53);
        painter.setPen(textCol);
        painter.drawText(legendX + 24, 58, trace.label);
        legendX += 42 + painter.fontMetrics().horizontalAdvance(trace.label);
    }

    painter.fillRect(box, canvasBg);
    painter.setPen(QPen(gridCol, 1));
    for (int index = 0; index <= 4; ++index) {
        const auto y = box.top() + box.height() * index / 4.0;
        painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }
    for (int index = 0; index <= 5; ++index) {
        const auto x = box.left() + box.width() * index / 5.0;
        painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
    }
    painter.setPen(QPen(borderCol, 1));
    painter.drawRect(box);

    double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
    std::size_t pointCount = 0;
    for (const auto &trace : traces_) {
        pointCount += trace.plot.points.size();
        for (const auto &point : trace.plot.points) if (!point.gap && std::isfinite(point.value) && point.timestamp >= range_.start && point.timestamp < range_.end) {
            minimum = std::min(minimum, point.value); maximum = std::max(maximum, point.value);
        }
    }
    const bool hasValues = std::isfinite(minimum) && std::isfinite(maximum);
    if (!hasValues) { minimum = -1; maximum = 1; }
    const double scale = std::max({1.0, std::abs(minimum), std::abs(maximum)});
    const long double normalizedLow = static_cast<long double>(minimum / scale);
    const long double normalizedHigh = static_cast<long double>(maximum / scale);
    const long double margin = normalizedHigh == normalizedLow
        ? std::max(1.0L / static_cast<long double>(scale), std::abs(normalizedLow) * .05L)
        : (normalizedHigh - normalizedLow) * .06L;
    const auto normalizedLimit = static_cast<long double>(std::numeric_limits<double>::max()) / static_cast<long double>(scale);
    const long double axisLow = std::max(normalizedLow - margin, -normalizedLimit);
    const long double axisHigh = std::min(normalizedHigh + margin, normalizedLimit);
    const auto xFor = [&](Time time) {
        const auto fraction = static_cast<long double>(time - range_.start) / static_cast<long double>(range_.end - range_.start);
        return box.left() + static_cast<double>(fraction) * box.width();
    };
    const auto yFor = [&](double value) {
        const auto fraction = (static_cast<long double>(value / scale) - axisLow) / (axisHigh - axisLow);
        return box.bottom() - static_cast<double>(fraction) * box.height();
    };
    painter.save(); painter.setClipRect(box);
    for (const auto &event : events_) {
        if (event.start >= range_.end || event.end <= range_.start) continue;
        const auto left = xFor(std::max(event.start, range_.start));
        const auto right = xFor(std::min(event.end, range_.end));
        painter.fillRect(QRectF(left, box.top(), std::max(1.0, right - left), box.height()),
                         event.severity >= 2 ? QColor(220, 53, 69, 40) : QColor(255, 193, 7, 40));
    }

    // Subtle animated transition sweep wave when switching windows/channels
    if (transitionProgress_ < 0.999) {
        const double sweepX = box.left() + transitionProgress_ * box.width();
        QLinearGradient sweepGrad(sweepX - 70.0, 0, sweepX + 10.0, 0);
        QColor hi = pal.color(QPalette::Highlight);
        QColor c0 = hi; c0.setAlpha(0);
        QColor c1 = hi; c1.setAlpha(static_cast<int>((1.0 - transitionProgress_) * 48.0));
        sweepGrad.setColorAt(0.0, c0);
        sweepGrad.setColorAt(0.85, c1);
        sweepGrad.setColorAt(1.0, c0);
        painter.fillRect(QRectF(sweepX - 70.0, box.top(), 80.0, box.height()), sweepGrad);
    }

    for (const auto &trace : traces_) {
        painter.setPen(QPen(trace.color, 2.2)); QPainterPath curve; bool connected = false;
        for (const auto &point : trace.plot.points) {
            if (point.gap || !std::isfinite(point.value)) { connected = false; continue; }
            const QPointF pixel(xFor(point.timestamp), yFor(point.value));
            if (!connected) { curve.moveTo(pixel); connected = true; } else curve.lineTo(pixel);
        }
        painter.drawPath(curve);
        if (trace.plot.points.size() < 150) {
            painter.setBrush(trace.color);
            for (const auto &point : trace.plot.points) if (!point.gap && std::isfinite(point.value))
                painter.drawEllipse(QPointF(xFor(point.timestamp), yFor(point.value)), 2.2, 2.2);
        }
    }
    if (hoverTime_) {
        QColor hc = mutedText;
        hc.setAlphaF(static_cast<float>(std::clamp(hoverAlpha_, 0.25, 1.0)));
        painter.setPen(QPen(hc, 1, Qt::DotLine));
        painter.drawLine(QPointF(xFor(*hoverTime_), box.top()), QPointF(xFor(*hoverTime_), box.bottom()));
    }
    if (position_ >= range_.start && position_ < range_.end) {
        const double px = xFor(position_);
        if (playing_) {
            QColor glowCol("#d9534f");
            glowCol.setAlpha(static_cast<int>(28.0 + 22.0 * std::sin(pulsePhase_)));
            painter.fillRect(QRectF(px - 4.0, box.top(), 8.0, box.height()), glowCol);
        }
        painter.setPen(QPen(QColor("#d9534f"), 1.8, Qt::DashLine));
        painter.drawLine(QPointF(px, box.top()), QPointF(px, box.bottom()));
    }
    if (dragging_ && moved_ && dragButton_ == Qt::LeftButton) {
        const auto left = std::clamp(std::min(dragStart_.x(), dragCurrent_.x()), box.left(), box.right());
        const auto right = std::clamp(std::max(dragStart_.x(), dragCurrent_.x()), box.left(), box.right());
        QColor sel = pal.color(QPalette::Highlight);
        sel.setAlpha(45);
        painter.fillRect(QRectF(left, box.top(), right - left, box.height()), sel);
        painter.setPen(QPen(pal.color(QPalette::Highlight), 1.2, Qt::DashLine));
        painter.drawRect(QRectF(left, box.top(), right - left, box.height()));
    }
    painter.restore(); painter.setPen(textCol);
    for (int index = 0; index <= 4; ++index) {
        const auto normalized = axisHigh - (axisHigh - axisLow) * index / 4.0L;
        const auto largest = static_cast<long double>(std::numeric_limits<double>::max());
        const auto value = std::clamp(normalized, -normalizedLimit, normalizedLimit) * static_cast<long double>(scale);
        const auto y = box.top() + box.height() * index / 4.0;
        painter.drawText(QRectF(2, y - 9, box.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(static_cast<double>(std::clamp(value, -largest, largest)), 'g', 5));
    }
    painter.drawText(QRectF(box.left(), box.bottom() + 4, box.width(), 18), Qt::AlignLeft, utc(range_.start));
    painter.setPen(mutedText);
    painter.drawText(QRectF(box.left(), box.bottom() + 21, box.width(), 18), Qt::AlignLeft,
        QStringLiteral("+%1 s  ·  单位: %2  ·  %3 个显示点  ·  滚轮缩放 / 左键框选 / 右键平移 / 双击复位")
            .arg(static_cast<double>(range_.end - range_.start) / 1e6, 0, 'g', 8).arg(unit_).arg(pointCount));
    painter.setPen(textCol);
    painter.drawText(QRectF(12, box.bottom() + 39, width() - 24, 18), Qt::AlignLeft, hoverPreview());
    painter.drawText(QRectF(12, box.bottom() + 57, width() - 24, 18), Qt::AlignLeft, readout_);
    if (!hasValues) {
        painter.setPen(mutedText);
        painter.drawText(box.adjusted(15, 10, -15, -10), Qt::AlignCenter | Qt::TextWordWrap,
            note_.isEmpty() ? QStringLiteral("此窗口没有可绘制的有效值") : note_);
    }
}
} // namespace edgeecho::ui
