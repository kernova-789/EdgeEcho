#include "edgeecho/core.hpp"
#include "sqlite3.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace edgeecho;
namespace {
void expect(bool c, const std::string &m) { if (!c) throw std::runtime_error(m); }
template<class F> void fails(F f, const std::string &m) { bool caught=false; try { f(); } catch(const Error &) { caught=true; } expect(caught,m); }
void sql(const std::filesystem::path &p, const std::string &s) {
    sqlite3 *db=nullptr;
    expect(sqlite3_open(p.u8string().c_str(),&db)==SQLITE_OK,"fixture open");
    const auto rc=sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr);
    const std::string error=sqlite3_errmsg(db); sqlite3_close(db);
    expect(rc==SQLITE_OK,"fixture SQL: "+error);
}
std::string contents(const std::filesystem::path &p) {
    std::ifstream file(p,std::ios::binary); std::ostringstream s; s<<file.rdbuf(); return s.str();
}
}
int main(int argc,char **argv) {
    try {
        expect(argc==2,"test needs a workspace-local output directory");
        const auto root=std::filesystem::u8path(argv[1]); std::filesystem::create_directories(root);
        const auto p=root/std::filesystem::u8path("数据 #% replay.db"); std::filesystem::remove(p);
        create_demo(p,2000);
        // Duplicate times exercise the lexicographic seq tie breaker.
        sql(p,"UPDATE records SET ts=1793145600000000 WHERE id IN ('scalar-1','scalar-2');");
        const auto original=contents(p);
        const Time t=1793145600000000LL;
        const Filter scalar{"demo-vibration","acceleration_rms","count"};
        Limits limits; limits.page_records=7;
        {
            Database db(p,limits); auto o=db.overview(); expect(o.series.size()==4 && o.records==2060,"overview");
            Cursor cursor; std::set<std::string> ids; std::int64_t last_seq=0; Time last_time=-1; bool more=true;
            while(more) {
                auto page=db.page(scalar,{t,t+30000000},cursor); expect(!page.records.empty(),"page progress");
                for(const auto &r:page.records) {
                    expect(ids.insert(r.id).second,"duplicate record from keyset pagination");
                    expect(r.timestamp>last_time || (r.timestamp==last_time && r.sequence>last_seq),"record sort");
                    last_time=r.timestamp; last_seq=r.sequence;
                }
                cursor=page.next; more=page.more;
            }
            expect(ids.size()==2000,"missing records from pagination");
            auto exact=db.page(scalar,{t,t+1}); expect(exact.records.size()==3,"half-open timestamp boundary");
            auto events=db.events(scalar,{t+4000000,t+5000000}); expect(events.size()==1 && events[0].revision==1,"overlap event lookup");
            auto plot=db.plot(scalar,{t,t+20000000},"value",80);
            expect(plot.points.size()<300 && plot.quality_skipped>0,"bounded envelope and quality");
            expect(std::any_of(plot.points.begin(),plot.points.end(),[](Point point){return point.gap;}),"quality break must survive downsampling");
            expect(std::any_of(plot.points.begin(),plot.points.end(),[](Point point){return !point.gap && point.value>35;}),"minmax retained spike");
            Filter raw{"demo-vibration","vibration_iio_block","count"};
            auto overlap=db.page(raw,{t+500000,t+501000},{},true); expect(overlap.records.size()==1,"block crossing left boundary");
            expect(db.page(raw,{t+500000,t+501000}).records.empty(),"normal page excludes overlap");
            auto waveform=db.plot(raw,{t+500000,t+501000},"x",10); expect(waveform.points.size()==1 && waveform.estimated_time,"waveform time clipping");
            const auto out=root/"out.csv"; std::filesystem::remove(out);
            expect(db.export_csv(out,scalar,{t,t+1})==3,"CSV count");
            expect(contents(out).find("sample_rate_hz")!=std::string::npos,"CSV columns");
            fails([&]{db.export_csv(p,scalar,{t,t+1});},"cannot export over source");
            fails([&]{db.export_csv(out,scalar,{t,t+1});},"cannot overwrite existing export");
            fails([&]{db.page(scalar,{t,t});},"invalid range");
        }
        expect(contents(p)==original,"read-only source bytes changed");
        expect(!std::filesystem::exists(p.u8string()+"-shm") && !std::filesystem::exists(p.u8string()+"-wal"),"source sidecars created");
        { Database db(p.parent_path()/p.filename()); expect(db.path()==std::filesystem::canonical(p),"Unicode URI path"); }
        { auto l=limits; l.page_bytes=1; Database db(p,l); fails([&]{db.page(scalar,{t,t+1});},"oversize first item must fail"); }
        { auto l=limits; l.record_bytes=300; Database db(p,l); fails([&]{db.page({"demo-vibration","vibration_iio_block","count"},{t,t+1});},"oversize block must fail"); }
        { auto l=limits; l.scan_records=10; Database db(p,l); fails([&]{db.plot(scalar,{t,t+1000000},"value");},"scan budget"); }
        { auto l=limits; l.decode_samples=10; Database db(p,l); fails([&]{db.plot({"demo-vibration","vibration_iio_block","count"},{t,t+1000000},"x");},"decode budget"); }
        { auto cancel=std::make_shared<std::atomic_bool>(true); fails([&]{Database db(p,{},cancel);},"initial cancellation"); }
        { auto cancel=std::make_shared<std::atomic_bool>(false); Database db(p,{},cancel); cancel->store(true); fails([&]{db.overview();},"query cancellation"); }
        { std::ofstream wal(p.u8string()+"-wal"); wal<<"nonempty"; wal.close(); fails([&]{Database db(p);},"must not ignore WAL"); std::filesystem::remove(p.u8string()+"-wal"); }
        { std::ofstream journal(p.u8string()+"-journal"); journal<<"unrecovered"; journal.close(); fails([&]{Database db(p);},"must not ignore rollback journal"); std::filesystem::remove(p.u8string()+"-journal"); }
        { Database db(p); sql(p,"UPDATE records SET source='changed' WHERE seq=1;"); fails([&]{db.overview();},"source mutation detected"); }
        // Revision outside the requested range must not resurrect the old warning.
        sql(p,"INSERT INTO event_history SELECT NULL,id,sensor,type,start+100000000,end+100000000,assessment,severity,event_type,description,source,model,confidence,version,2,1,1,'revise',operated_at,actor,reason,0,0,NULL,'',note,request FROM event_history WHERE revision=1;");
        { Database db(p); expect(db.events(scalar,{t,t+10000000}).empty(),"revision before filter"); expect(db.events(scalar,{t+103000000,t+107000000}).size()==1,"latest revision"); }
        sql(p,"UPDATE event_history SET revoked=1 WHERE revision=2;");
        { Database db(p); expect(db.events(scalar,{t,t+200000000}).empty(),"revoked event must not reappear"); }
        // Total page bytes produce progress even before count is full.
        { auto l=limits; l.page_bytes=900; Database db(p,l); auto page=db.page(scalar,{t,t+100000}); expect(page.more && page.records.size()<7 && !page.records.empty(),"cumulative page budget"); }
        // Feature records represent sparse scalar observations, not one-microsecond
        // waveform bursts: their nominal duration must not manufacture gaps.
        {
            Database db(p);
            const auto feature=db.plot({"demo-vibration","feature_iio_scan","count"},
                                       {t,t+20000000},"temperature",100);
            expect(feature.points.size()==20 && !feature.estimated_time,"feature observation count/time basis");
            expect(std::none_of(feature.points.begin(),feature.points.end(),[](Point point){return point.gap;}),
                   "sparse feature observations must not have artificial waveform gaps");
        }
        // Two legitimate events each fit the per-record budget, but their total
        // must still obey the result budget rather than return partial findings.
        sql(p,"UPDATE event_history SET revoked=0 WHERE revision=2;"
              "INSERT INTO events SELECT NULL,'budget-event',sensor,type,start,end,assessment,severity,event_type,description,source,model,confidence,version FROM events WHERE id='demo-event';"
              "INSERT INTO event_history SELECT NULL,'budget-event',sensor,type,start,end,assessment,severity,event_type,description,source,model,confidence,version,revision,previous_revision,action,'budget-create',operated_at,actor,reason,revoked,confirmed,confirmed_at,confirmed_by,note,request FROM event_history WHERE id='demo-event' AND revision=1;");
        std::size_t event_bytes=0;
        {
            Database db(p); const auto events=db.events(scalar,{t,t+200000000});
            expect(events.size()==2,"event budget fixture");
            for(const auto &event:events)
                event_bytes+=256+event.id.size()+event.sensor.size()+event.type.size()+event.event_type.size()+event.description.size();
        }
        { auto l=limits; l.page_bytes=event_bytes-1; Database db(p,l); fails([&]{db.events(scalar,{t,t+200000000});},"cumulative event byte budget must fail explicitly"); }
        { auto l=limits; l.record_bytes=256; Database db(p,l); fails([&]{db.events(scalar,{t,t+200000000});},"single event byte budget must fail explicitly"); }
        // Dedicated same-timestamp fixture: exactly one small record precedes
        // the oversized second record. Ordinary demo data has three scalars at
        // this timestamp, which would only test a small second look-ahead row.
        sql(p,"INSERT INTO records(id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version) SELECT 'lookahead-scalar','lookahead','mixed_payload',unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version FROM records WHERE id='scalar-0';"
              "INSERT INTO records(id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version) SELECT 'lookahead-block','lookahead','mixed_payload',unit,ts,kind,scalar,block,quality,duration,rate,encoding,source,batch,metadata,version FROM records WHERE id='block-0-0';");
        {
            auto l=limits; l.page_records=1; l.record_bytes=1024; Database db(p,l);
            const Filter selected{"lookahead","mixed_payload","count"};
            const auto first=db.page(selected,{t,t+1});
            expect(first.records.size()==1 && first.more && first.records.front().id=="lookahead-scalar",
                   "full page look-ahead does not reject oversized next block");
            fails([&]{db.page(selected,{t,t+1},first.next);},"oversized look-ahead block fails on its own page");
        }
        sql(p,"PRAGMA user_version=99;"); fails([&]{Database db(p);},"future schema rejected");
        sql(p,"PRAGMA user_version=3; PRAGMA application_id=0;"); fails([&]{Database db(p);},"unrelated database rejected");
        std::cout<<"Database compatibility, budgets, codecs, keyset, source safety, events and CSV checks passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
