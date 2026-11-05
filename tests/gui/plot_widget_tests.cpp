#include "plot_widget.hpp"

#include <QApplication>
#include <QColor>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace edgeecho;
using namespace edgeecho::ui;
void expect(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void mouse(QWidget &widget, QEvent::Type type, QPointF at, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, at, widget.mapToGlobal(at.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void wheel(QWidget &widget, QPointF at, int delta) {
    QWheelEvent event(at, widget.mapToGlobal(at.toPoint()), QPoint{}, QPoint{0,delta},
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &event);
}
void key(QWidget &widget, int value) {
    QKeyEvent event(QEvent::KeyPress,value,Qt::NoModifier); QApplication::sendEvent(&widget,&event);
}
void settle(int milliseconds) {
    QEventLoop loop; QTimer::singleShot(milliseconds,&loop,&QEventLoop::quit); loop.exec();
}
QPointF location(const PlotWidget &widget,double fraction) {
    const auto area=widget.plotArea(); return {area.left()+area.width()*fraction,area.center().y()};
}
bool nearColor(QColor actual,QColor expected) {
    return std::abs(actual.red()-expected.red())<55 && std::abs(actual.green()-expected.green())<55 && std::abs(actual.blue()-expected.blue())<55;
}
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    try {
        PlotWidget widget; widget.resize(1100,600); widget.show(); QApplication::processEvents();
        // At a far-future epoch, even a 64-bit floating-point timestamp loses
        // microseconds. Navigation must perform absolute additions as integers.
        const Time anchor=max_time-20001;
        const Range full{anchor,anchor+10000};
        widget.setOverallRange(full);
        Plot first; first.points={{anchor+1000,0,false},{anchor+2000,0,false},
                                  {anchor+3000,0,true},{anchor+4000,0,false},{anchor+5000,0,false}};
        Plot second; second.points={{anchor+1000,1,false},{anchor+5000,1,false}};
        Plot third; third.points={{anchor+1000,2,false},{anchor+5000,2,false}};
        widget.setTraces({{QStringLiteral("X"),QColor("#ff3b30"),first},
                          {QStringLiteral("Y"),QColor("#35dc65"),second},
                          {QStringLiteral("Z"),QColor("#429bff"),third}},
                         full,QStringLiteral("count"),QStringLiteral("XYZ test"),{},QStringLiteral("time estimated"));
        expect(widget.traceCount()==3,"XYZ trace count");
        expect(widget.traces()[0].plot.points[2].gap,"quality gap lost");
        expect(widget.plot().points.size()==5,"legacy plot accessor changed");
        unsigned requests=0;
        widget.viewChanged=[&](Range r) {
            ++requests;
            expect(r.start>=full.start && r.end<=full.end && r.end>r.start,"navigation escaped source bounds");
        };
        wheel(widget,location(widget,.5),120);
        auto zoomed=widget.plotRange();
        expect(zoomed.start==anchor+1667 && zoomed.end==anchor+8334,"wheel lost absolute UTC microsecond precision");
        expect(requests==1,"wheel did not request a database refresh");
        const auto pointsBefore=widget.traces()[0].plot.points;
        auto a=location(widget,.25), b=location(widget,.75);
        mouse(widget,QEvent::MouseButtonPress,a,Qt::LeftButton,Qt::LeftButton);
        mouse(widget,QEvent::MouseMove,b,Qt::NoButton,Qt::LeftButton);
        mouse(widget,QEvent::MouseButtonRelease,b,Qt::LeftButton,Qt::NoButton);
        const auto selected=widget.plotRange();
        expect(selected.end-selected.start<zoomed.end-zoomed.start,"rectangle did not zoom");
        expect(selected.start>zoomed.start && selected.end<zoomed.end,"rectangle chose incorrect time interval");
        expect(widget.traces()[0].plot.points.size()==pointsBefore.size() && widget.traces()[0].plot.points[2].gap,
               "navigation mutated the source envelope or removed gaps");
        const auto span=selected.end-selected.start;
        a=location(widget,.7); b=location(widget,.9);
        mouse(widget,QEvent::MouseButtonPress,a,Qt::RightButton,Qt::RightButton);
        mouse(widget,QEvent::MouseMove,b,Qt::NoButton,Qt::RightButton);
        mouse(widget,QEvent::MouseButtonRelease,b,Qt::RightButton,Qt::NoButton);
        const auto panned=widget.plotRange();
        expect(panned.start<selected.start && panned.end-panned.start==span,"right drag did not preserve pan span");
        unsigned seeks=0; Time clicked=0;
        widget.seek=[&](Time time){++seeks;clicked=time;};
        const auto center=location(widget,.5);
        mouse(widget,QEvent::MouseButtonPress,center,Qt::LeftButton,Qt::LeftButton);
        expect(seeks==0,"left press should wait for click versus drag decision");
        mouse(widget,QEvent::MouseButtonRelease,center,Qt::LeftButton,Qt::NoButton);
        expect(seeks==1 && clicked>=panned.start && clicked<panned.end,"click did not seek within the half-open range");
        unsigned inspected=0;Time inspectedTime=0;
        widget.inspectRequested=[&](Time time){++inspected;inspectedTime=time;};
        mouse(widget,QEvent::MouseMove,center,Qt::NoButton,Qt::NoButton);
        settle(170);
        expect(inspected==1 && inspectedTime==clicked,"hover did not issue a throttled exact lookup request");
        widget.setReadout(QStringLiteral("Exact sample: X=12"));
        expect(widget.toolTip().contains(QStringLiteral("X=12")),"full exact readout unavailable in tooltip");
        mouse(widget,QEvent::MouseMove,location(widget,.6),Qt::NoButton,Qt::NoButton);
        mouse(widget,QEvent::MouseMove,location(widget,.7),Qt::NoButton,Qt::NoButton);
        settle(170);
        expect(inspected==2,"hover timer should coalesce rapid movement");
        key(widget,Qt::Key_Escape);
        expect(widget.plotRange().start==full.start && widget.plotRange().end==full.end,"Escape did not reset view");
        wheel(widget,location(widget,.5),120);
        mouse(widget,QEvent::MouseButtonDblClick,location(widget,.5),Qt::LeftButton,Qt::LeftButton);
        expect(widget.plotRange().start==full.start && widget.plotRange().end==full.end,"double-click did not reset view");
        for(int n=0;n<16;++n)wheel(widget,location(widget,.1),960);
        expect(widget.plotRange().end-widget.plotRange().start==1,"zoom should stop at one microsecond");
        for(int n=0;n<16;++n)wheel(widget,location(widget,.1),-960);
        expect(widget.plotRange().start==full.start && widget.plotRange().end==full.end,"zoom-out should clamp to source");
        // Rendering assertions use actual curve pixels, rather than only checking
        // the trace data: all three colors appear, and X does not bridge its gap.
        widget.setTraces({{QStringLiteral("X"),QColor("#ff3b30"),first},
                          {QStringLiteral("Y"),QColor("#35dc65"),second},
                          {QStringLiteral("Z"),QColor("#429bff"),third}},
                          full,QStringLiteral("count"),QStringLiteral("XYZ test"),{},QStringLiteral("time estimated"));
        widget.setPosition(anchor+9000);
        QImage rendered(widget.size(),QImage::Format_ARGB32);rendered.fill(Qt::transparent);widget.render(&rendered);
        const auto area=widget.plotArea();
        const int xConnected=static_cast<int>(std::lround(area.left()+area.width()*.15));
        const int xGap=static_cast<int>(std::lround(area.left()+area.width()*.30));
        auto hasColor=[&](int x,int y,QColor color) {
            for(int dx=-2;dx<=2;++dx)for(int dy=-2;dy<=2;++dy)
                if(rendered.rect().contains(x+dx,y+dy)&&nearColor(rendered.pixelColor(x+dx,y+dy),color))return true;
            return false;
        };
        const auto rowWithColor=[&](int x,QColor color) {
            for(int y=static_cast<int>(area.top())+3;y<static_cast<int>(area.bottom())-3;++y)
                if(hasColor(x,y,color))return y;
            return -1;
        };
        const int redY=rowWithColor(xConnected,QColor("#ff3b30"));
        expect(redY>=0,"X curve was not rendered");
        expect(!hasColor(xGap,redY,QColor("#ff3b30")),"X curve bridged a quality gap");
        expect(rowWithColor(xGap,QColor("#35dc65"))>=0,"Y curve was not rendered");
        expect(rowWithColor(xGap,QColor("#429bff"))>=0,"Z curve was not rendered");
        widget.clear();expect(widget.traceCount()==0 && widget.plot().points.empty(),"clear retained traces");
        expect(!widget.toolTip().contains(QStringLiteral("X=12")),"clear retained stale sample tooltip");
        std::cout<<"Plot wheel, rectangle, pan, exact-hover request, reset, XYZ and gap rendering checks passed\n";
        return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
