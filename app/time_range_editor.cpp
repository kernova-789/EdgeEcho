#include "time_range_editor.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimeZone>
#include <QVBoxLayout>
#include <set>

namespace edgeecho::ui {
namespace {
Time selected(QDateTimeEdit *edit, int remainder, bool local) {
    const auto date=edit->date(); const auto time=edit->time();
    if (!date.isValid() || !time.isValid()) throw Error("日期或时间无效");
    const QDateTime wall(date,time,Qt::UTC);
    qint64 milliseconds=wall.toMSecsSinceEpoch();
    if (local) return resolveLocalWallTime(date,time,remainder,QTimeZone::systemTimeZone());
    if (milliseconds<0 || milliseconds>max_time/1000)
        throw Error("日期须在1970..9999年UTC范围内");
    const auto value=static_cast<Time>(milliseconds)*1000+remainder;
    if (value<0 || value>max_time) throw Error("时间超出UTC微秒范围");
    return value;
}
}
Time resolveLocalWallTime(QDate date,QTime time,int remainder,const QTimeZone &zone) {
    if(!zone.isValid() || !date.isValid() || !time.isValid() || remainder<0 || remainder>999)
        throw Error("本地日期、时区或微秒余数无效");
    const QDateTime wall(date,time,Qt::UTC);
    const auto wallMilliseconds=wall.toMSecsSinceEpoch();
    std::set<int> offsets;
    for(int hour=-48;hour<=48;hour+=6)offsets.insert(zone.offsetFromUtc(wall.addSecs(hour*3600)));
    std::set<qint64> candidates;
    for(const int offset:offsets) {
        const auto candidate=wallMilliseconds-static_cast<qint64>(offset)*1000;
        const auto roundtrip=QDateTime::fromMSecsSinceEpoch(candidate,zone);
        if(roundtrip.date()==date && roundtrip.time()==time)candidates.insert(candidate);
    }
    if(candidates.size()!=1)throw Error(candidates.empty()?"本地时间处于夏令时跳变空档，请改用UTC输入":"本地时间存在夏令时歧义，请改用UTC或精确微秒输入");
    const auto milliseconds=*candidates.begin();
    if(milliseconds<0 || milliseconds>max_time/1000)throw Error("本地时间超出有效UTC范围");
    const auto value=static_cast<Time>(milliseconds)*1000+remainder;
    if(value<0 || value>max_time)throw Error("本地时间超出有效UTC范围");
    return value;
}
TimeRangeEditor::TimeRangeEditor(QWidget *parent):QWidget(parent) {
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    auto *row=new QHBoxLayout;
    zone_=new QComboBox;zone_->addItems({QStringLiteral("UTC"),QStringLiteral("本地时区")});
    row->addWidget(new QLabel(QStringLiteral("日期时间")));row->addWidget(zone_);
    start_=new QDateTimeEdit;end_=new QDateTimeEdit;
    startMicro_=new QSpinBox;endMicro_=new QSpinBox;
    for (auto *edit:{start_,end_}) {
        edit->setTimeSpec(Qt::UTC);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        edit->setCalendarPopup(true);
        edit->setDateTimeRange(QDateTime(QDate(1969,12,30),QTime(0,0),Qt::UTC),QDateTime(QDate(9999,12,31),QTime(23,59,59,999),Qt::UTC));
        edit->setMinimumWidth(205);
        connect(edit,&QDateTimeEdit::editingFinished,this,[this]{commit();});
    }
    for (auto *spin:{startMicro_,endMicro_}) {
        spin->setRange(0,999);spin->setSuffix(QStringLiteral(" μs"));
        spin->setToolTip(QStringLiteral("毫秒之后的微秒余数，保留六位小数精度"));
        connect(spin,&QSpinBox::editingFinished,this,[this]{commit();});
    }
    row->addWidget(new QLabel(QStringLiteral("开始")));row->addWidget(start_,1);row->addWidget(startMicro_);
    row->addWidget(new QLabel(QStringLiteral("结束")));row->addWidget(end_,1);row->addWidget(endMicro_);
    layout->addLayout(row);message_=new QLabel;message_->setWordWrap(true);layout->addWidget(message_);
    connect(zone_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{refresh();});
    refresh();
}
void TimeRangeEditor::setRange(Range range) {
    if (range.start<0 || range.end<=range.start || range.end>max_time) return;
    range_=range;valid_=true;error_.clear();refresh();
}
void TimeRangeEditor::setLocalDisplay(bool local) { zone_->setCurrentIndex(local?1:0); }
bool TimeRangeEditor::localDisplay() const { return zone_->currentIndex()==1; }
void TimeRangeEditor::refresh() {
    updating_=true;
    const QSignalBlocker a(start_),b(end_),c(startMicro_),d(endMicro_);
    const auto display=[this](Time value) {
        const auto utc=QDateTime::fromMSecsSinceEpoch(value/1000,Qt::UTC);
        return localDisplay()?utc.toLocalTime():utc;
    };
    // The edit stores wall-clock components in UTC deliberately. UTC/local
    // interpretation happens only in selected(), with explicit DST checks.
    endBoundary_=range_.end==max_time;
    end_->setEnabled(!endBoundary_);endMicro_->setEnabled(!endBoundary_);
    auto aTime=display(range_.start),bTime=display(endBoundary_?max_time-1:range_.end);
    const bool localOutOfRange=localDisplay() && (aTime.date().year()>9999 || bTime.date().year()>9999);
    if(localOutOfRange) {
        const QSignalBlocker zoneBlock(zone_);zone_->setCurrentIndex(0);
        aTime=display(range_.start);bTime=display(endBoundary_?max_time-1:range_.end);
    }
    start_->setDateTime(QDateTime(aTime.date(),aTime.time(),Qt::UTC));
    end_->setDateTime(QDateTime(bTime.date(),bTime.time(),Qt::UTC));
    startMicro_->setValue(static_cast<int>(range_.start%1000));
    endMicro_->setValue(static_cast<int>((endBoundary_ ? max_time-1 : range_.end)%1000));
    if (valid_) message_->setText(localDisplay()?QStringLiteral("仅转换显示时区；查询和导出始终使用UTC微秒。夏令时歧义时间须改用UTC输入。"):
        QStringLiteral("UTC日期时间 + 微秒余数；结束点不包含在区间内。"));
    if(localOutOfRange)message_->setText(QStringLiteral("本地显示超出日历年份范围，已使用UTC显示；原始微秒保持不变。"));
    if(endBoundary_)message_->setText(QStringLiteral("结束为10000年UTC边界，日历显示其前1微秒；请用精确微秒字段修改该边界。"));
    updating_=false;
}
void TimeRangeEditor::commit() {
    if (updating_) return;
    try {
        const Range candidate{selected(start_,startMicro_->value(),localDisplay()),endBoundary_?max_time:selected(end_,endMicro_->value(),localDisplay())};
        if (candidate.end<=candidate.start) throw Error("结束时间必须大于开始时间");
        range_=candidate;valid_=true;error_.clear();refresh();if(edited) edited(range_);
    }catch(const std::exception &error) {
        valid_=false;error_=QString::fromUtf8(error.what());message_->setText(error_);
    }
}
}
