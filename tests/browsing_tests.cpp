#include "edgeecho/core.hpp"
#include "sqlite3.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace edgeecho;
namespace {
void expect(bool condition, const std::string &message) { if (!condition) throw Error(message); }
template<class F> void fails(F action, const std::string &message) {
    bool rejected=false; try { action(); } catch(const Error &) { rejected=true; }
    expect(rejected,message);
}
void sql(const std::filesystem::path &path, const char *query) {
    sqlite3 *db=nullptr;
    expect(sqlite3_open(path.u8string().c_str(),&db)==SQLITE_OK,"fixture open");
    const auto rc=sqlite3_exec(db,query,nullptr,nullptr,nullptr);
    const std::string message=sqlite3_errmsg(db); sqlite3_close(db);
    expect(rc==SQLITE_OK,"fixture SQL: "+message);
}
std::string contents(const std::filesystem::path &p) {
    std::ifstream input(p,std::ios::binary); std::ostringstream out; out<<input.rdbuf(); return out.str();
}
void equal(const Plot &left,const Plot &right) {
    expect(left.records_scanned==right.records_scanned && left.samples_decoded==right.samples_decoded &&
           left.quality_skipped==right.quality_skipped && left.estimated_time==right.estimated_time &&
           left.points.size()==right.points.size(),"multi plot statistics differ from single-channel query");
    for(std::size_t n=0;n<left.points.size();++n) {
        const auto &a=left.points[n]; const auto &b=right.points[n];
        expect(a.timestamp==b.timestamp && a.value==b.value && a.gap==b.gap,"multi plot changed raw channel semantics");
    }
}
}
int main(int argc,char **argv) {
    try {
        expect(argc==2,"requires workspace-local output directory");
        const auto root=std::filesystem::u8path(argv[1]); std::filesystem::create_directories(root);
        const auto input=root/"browsing.db"; std::filesystem::remove(input); create_demo(input,1000);
        sql(input,
            "INSERT INTO records(id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version) VALUES"
            "('tiny','probe','wave','count',1793145600000000,2,NULL,X'00007B00F4FF',0,3000,1000,'s16le','synthetic','','',1),"
            "('invalid','probe','wave','count',1793145600003000,2,NULL,X'FF7F',1,1000,1000,'s16le','synthetic','','',1),"
            "('probe-left','probe','scalar','count',1793145600000000,1,11,NULL,0,0,NULL,'','','','',1),"
            "('probe-bad','probe','scalar','count',1793145600000500,1,999,NULL,1,0,NULL,'','','','',1),"
            "('probe-right','probe','scalar','count',1793145600001000,1,22,NULL,0,0,NULL,'','','','',1),"
            "('probe-same','probe','scalar','count',1793145600001000,1,33,NULL,0,0,NULL,'','','','',1),"
            "('probe-empty','empty','scalar','count',1793145600000000,0,NULL,NULL,2,0,NULL,'','','','',1);");
        const auto original=contents(input);
        constexpr Time t=1793145600000000LL;
        const Filter xyz{"demo-vibration","vibration_iio_block","count"};
        const Filter tiny{"probe","wave","count"};
        {
            Database db(input);
            const auto multi=db.plots(xyz,{t,t+1000000},{"x","y","z"},30);
            expect(multi.size()==3,"three XYZ curves");
            for(std::size_t n=0;n<3;++n) equal(multi[n],db.plot(xyz,{t,t+1000000},std::vector<std::string>{"x","y","z"}[n],30));
            expect(multi[0].samples_decoded==1000 && multi[0].records_scanned==1,"XYZ counts or shared row scan");
            auto clipped=db.plots(xyz,{t+500000,t+501000},{"x","y","z"},10);
            expect(clipped[0].points.size()==1 && clipped[0].points[0].timestamp==t+500000,"left overlap or half-open clipping");
            const auto p=db.probe(xyz,{t+500000,t+501000},"x",t+500500);
            expect(p && p->timestamp==t+500000,"probe overlapping block");
            const auto envelope=db.plot(tiny,{t,t+3000},"value",1);
            expect(envelope.points.size()==2 && envelope.points[0].value==123 && envelope.points[1].value==-12,"tiny min/max fixture");
            const auto exact=db.probe(tiny,{t,t+3000},"value",t);
            expect(exact && exact->timestamp==t && exact->value==0,"probe must read original sample absent from envelope");
            auto tie=db.probe(tiny,{t,t+3000},"value",t+500);
            expect(tie && tie->timestamp==t,"equidistant sample ties choose earlier timestamp");
            auto last=db.probe(tiny,{t,t+3000},"value",t+2999);
            expect(last && last->timestamp==t+2000 && last->value==-12,"nearest actual sample, no interpolation");
            expect(!db.probe(tiny,{t+1,t+1000},"value",t+500),"half-open empty sample interval");
            expect(!db.probe(tiny,{t+3000,t+4000},"value",t+3500),"non-good waveform must not produce exact readout");
            const auto scalar=db.probe({"probe","scalar","count"},{t,t+1001},"value",t+500);
            expect(scalar && scalar->timestamp==t && scalar->value==11,"quality skip and nearest scalar tie");
            const auto same=db.probe({"probe","scalar","count"},{t,t+1001},"value",t+1000);
            expect(same && same->value==22,"same-time cursor sequence deterministic");
            const auto feature=db.probe({"demo-vibration","feature_iio_scan","count"},{t,t+1},"temperature",t);
            expect(feature && feature->value==2500,"feature probe with no rate remains raw count");
            expect(!db.probe({"empty","scalar","count"},{t,t+1},"value",t),"missing data should stay missing");
            fails([&]{db.probe(tiny,{t,t+3000},"value",t+3000);},"probe exclusive endpoint");
            fails([&]{db.probe(tiny,{t,t+3000},"no-such-channel",t);},"probe bad channel");
            fails([&]{db.plots(xyz,{t,t+1},{"x","x"});},"duplicate channels");
            fails([&]{db.plots(xyz,{t,t+1},{});},"empty channels");
            fails([&]{db.plots(xyz,{t,t+1},{"x","bad"});},"one bad channel fails entire multi request");
        }
        {
            Limits limits; limits.scan_records=1; limits.page_records=1;
            Database db(input,limits);
            expect(db.plots(xyz,{t,t+1000000},{"x","y","z"}).size()==3,"multi request must count source rows once");
        }
        {
            Limits limits; limits.decode_samples=2000;
            Database db(input,limits);
            expect(db.plot(xyz,{t,t+1000000},"x").samples_decoded==1000,"single channel decode limit fixture");
            fails([&]{db.plots(xyz,{t,t+1000000},{"x","y","z"});},"combined channel decode budget");
            limits.decode_samples=2;
            Database small(input,limits);
            fails([&]{small.probe(tiny,{t,t+3000},"value",t);},"probe bounded sample decode");
        }
        {
            Limits limits; limits.page_bytes=900;
            Database db(input,limits);
            fails([&]{db.plot({"demo-vibration","acceleration_rms","count"},{t,t+1000000},"value",100);},"plot output is byte bounded");
        }
        {
            const auto cancel=std::make_shared<std::atomic_bool>(false);
            Database db(input,{},cancel); cancel->store(true);
            fails([&]{db.probe(tiny,{t,t+3000},"value",t);},"probe cancellation");
            fails([&]{db.plots(xyz,{t,t+1},{"x","y","z"});},"multi cancellation");
        }
        expect(contents(input)==original,"browsing changed source bytes");
        for(const auto *tail:{"-wal","-shm","-journal"}) expect(!std::filesystem::exists(std::filesystem::u8path(input.u8string()+tail)),"browsing created source sidecar");
        std::cout<<"Shared XYZ scan, precise original sample probe, gaps, bounds and cancellation passed\n";
        return 0;
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n';return 1; }
}
