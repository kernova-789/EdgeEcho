// Native Linux-only validation bridge: uses the unmodified sibling storage library
// to generate an actual writer-produced database entirely inside EdgeEcho.
#include <edgesense_replay/store.hpp>
#include <edgeecho/core.hpp>
#include <filesystem>
#include <iostream>
int main(int argc, char **argv) {
    try {
        if(argc!=2) return 2;
        namespace r=edgesense::replay;
        r::Config config; config.directory=std::filesystem::u8path(argv[1]); config.min_free_bytes=0;
        auto store=std::make_shared<r::Store>(config);
        constexpr r::Timestamp start=1793145600000000LL;
        for(int n=0;n<3000;++n) {
            r::Record record; record.id="writer-"+std::to_string(n); record.sensor_id="real-writer";
            record.data_type="temperature"; record.unit="degC"; record.timestamp=start+n*1000;
            record.value=20.0+static_cast<double>(n%100)/10; record.source="EdgeEcho compatibility fixture";
            record.metadata={{"input_origin","simulated"},{"中文","UTF-8"}};
            store->append({record});
        }
        r::Event event; event.id="writer-warning"; event.sensor_id="real-writer"; event.data_type="temperature";
        event.start=start+1000000; event.end=start+2000000; event.assessment=r::Assessment::warning;
        event.severity=r::Severity::warning; event.event_type="fixture"; event.description="Writer-generated test event";
        event.source="compatibility-test"; event.model_version="fixture-v1";
        store->append_events({event});
        store.reset();
        edgeecho::Database reader(config.directory);
        const auto overview=reader.overview();
        if(overview.records!=3000 || overview.series.size()!=1) throw std::runtime_error("writer overview mismatch");
        const edgeecho::Filter selected{"real-writer","temperature","degC"};
        auto events=reader.events(selected,{start,start+3000000});
        if(events.size()!=1 || events[0].description!="Writer-generated test event") throw std::runtime_error("writer event mismatch");
        std::size_t received=0; edgeecho::Cursor cursor;
        for(;;) {
            auto page=reader.page(selected,{start,start+3000000},cursor);
            for(const auto &record:page.records) {
                if(!record.scalar || record.id!="writer-"+std::to_string(received) ||
                   record.timestamp!=start+static_cast<edgeecho::Time>(received)*1000) throw std::runtime_error("writer payload mismatch");
                auto meta=edgeecho::unpack_metadata(record.metadata);
                if(meta.size()!=2) throw std::runtime_error("writer metadata mismatch");
                ++received;
            }
            cursor=page.next; if(!page.more) break;
        }
        if(received!=3000) throw std::runtime_error("writer pagination mismatch");
        const auto plot=reader.plot(selected,{start,start+3000000},"value",200);
        if(plot.samples_decoded!=3000 || plot.points.size()>400) throw std::runtime_error("writer plot mismatch");
        std::cout << "actual storage writer fixture complete\n"; return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
