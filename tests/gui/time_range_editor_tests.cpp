#include "time_range_editor.hpp"
#include <QApplication>
#include <QDateTimeEdit>
#include <QMetaObject>
#include <QSpinBox>
#include <iostream>

using namespace edgeecho;
using namespace edgeecho::ui;
namespace {
void expect(bool condition,const char *message){if(!condition)throw Error(message);}
template<class F>void reject(F action,const char*message){bool threw=false;try{action();}catch(const Error&){threw=true;}expect(threw,message);}
}
int main(int argc,char**argv){
    QApplication application(argc,argv);
    try{
        const QTimeZone ny("America/New_York");expect(ny.isValid(),"New York time-zone database unavailable");
        expect(resolveLocalWallTime(QDate(1969,12,31),QTime(19,0),123,ny)==123,"1969 local date should map to valid UTC epoch");
        reject([&]{resolveLocalWallTime(QDate(2026,3,8),QTime(2,30),0,ny);},"DST missing hour must reject");
        reject([&]{resolveLocalWallTime(QDate(2026,11,1),QTime(1,30),0,ny);},"DST repeated hour must reject");
        const auto after=resolveLocalWallTime(QDate(2026,11,1),QTime(3,30),987,ny);
        const auto roundtrip=QDateTime::fromMSecsSinceEpoch(after/1000,ny);
        expect(roundtrip.date()==QDate(2026,11,1)&&roundtrip.time()==QTime(3,30)&&after%1000==987,"nonambiguous local time roundtrip");
        reject([&]{resolveLocalWallTime(QDate(1969,12,31),QTime(18,59),0,ny);},"negative UTC input must reject");
        reject([&]{resolveLocalWallTime(QDate(2026,1,1),QTime(0,0),1000,ny);},"invalid microsecond remainder");
        TimeRangeEditor editor;
        const Range precise{1793145600000123LL,1793145605000987LL};
        unsigned changes=0;editor.edited=[&](Range){++changes;};
        editor.setRange(precise);editor.setLocalDisplay(true);editor.setLocalDisplay(false);
        expect(editor.range().start==precise.start&&editor.range().end==precise.end&&changes==0,"zone switch changes UTC or emits editing callback");
        auto remainders=editor.findChildren<QSpinBox*>();expect(remainders.size()==2,"microsecond controls missing");
        remainders.front()->setValue(456);QMetaObject::invokeMethod(remainders.front(),"editingFinished",Qt::DirectConnection);
        expect(editor.valid()&&editor.range().start==1793145600000456LL&&changes==1,"calendar microsecond edit lost precision");
        editor.setRange({0,1});editor.setLocalDisplay(true);editor.setLocalDisplay(false);expect(editor.range().start==0&&editor.range().end==1,"epoch boundary roundtrip");
        editor.setRange({max_time-1001,max_time});editor.setLocalDisplay(true);editor.setLocalDisplay(false);
        expect(editor.range().end==max_time&&editor.range().start==max_time-1001,"exclusive maximum boundary lost");
        expect(remainders.back()->value()==999 && !remainders.back()->isEnabled(),"maximum-boundary preview must show the preceding microsecond exactly");
        editor.setRange(precise);const auto dates=editor.findChildren<QDateTimeEdit*>();expect(dates.size()==2,"calendar controls missing");
        dates.back()->setDateTime(dates.front()->dateTime().addSecs(-1));QMetaObject::invokeMethod(dates.back(),"editingFinished",Qt::DirectConnection);
        expect(!editor.valid()&&editor.range().start==precise.start&&editor.range().end==precise.end,"invalid range changed canonical UTC");
        std::cout<<"Calendar precision, time-zone display, DST ambiguity/gap, epoch/max and invalid-range checks passed\n";return 0;
    }catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}
}
