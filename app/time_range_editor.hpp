#pragma once
#include "edgeecho/core.hpp"
#include <QWidget>
#include <QString>
#include <QDate>
#include <QTime>
#include <QTimeZone>
#include <functional>

class QDateTimeEdit;
class QSpinBox;
class QComboBox;
class QLabel;
namespace edgeecho::ui {
// Reject nonexistent/ambiguous local wall time rather than choosing a DST fold.
Time resolveLocalWallTime(QDate, QTime, int microsecondRemainder, const QTimeZone &);
// Calendar editing never changes UTC storage when the display zone changes.
// A separate microsecond remainder preserves precision beyond Qt milliseconds.
class TimeRangeEditor final : public QWidget {
  public:
    explicit TimeRangeEditor(QWidget *parent = nullptr);
    void setRange(Range);
    Range range() const { return range_; }
    void setLocalDisplay(bool);
    bool localDisplay() const;
    bool valid() const { return valid_; }
    QString error() const { return error_; }
    std::function<void(Range)> edited;
  private:
    void refresh();
    void commit();
    Range range_{0,1};
    QDateTimeEdit *start_, *end_;
    QSpinBox *startMicro_, *endMicro_;
    QComboBox *zone_;
    QLabel *message_;
    bool updating_ = false, valid_ = true, endBoundary_ = false;
    QString error_;
};
}
