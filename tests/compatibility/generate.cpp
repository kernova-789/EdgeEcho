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
        constexpr int scalar_count=5000;
        constexpr auto end=start+scalar_count*1000;
        std::vector<r::Record> scalars;
        scalars.reserve(scalar_count);
        for(int n=0;n<scalar_count;++n) {
            r::Record record; record.id="writer-"+std::to_string(n); record.sensor_id="real-writer";
            record.data_type="temperature"; record.unit="degC"; record.timestamp=start+n*1000;
            record.value=20.0+static_cast<double>(n%100)/10; record.source="EdgeEcho compatibility fixture";
            record.metadata={{"input_origin","simulated"},{"中文","UTF-8"}};
            scalars.push_back(std::move(record));
        }
        store->append(scalars);

        r::Record feature;
        feature.id="xyz-feature"; feature.sensor_id="real-writer"; feature.data_type="feature";
        feature.unit="count"; feature.timestamp=start; feature.duration_us=1;
        feature.encoding="iio-le-48-v1";
        r::Bytes fields(48);
        fields[0]=0x9c; fields[1]=0xff; // Signed temperature at byte 0.
        fields[32]=0x34; fields[33]=0x12; // X displacement at byte 32.
        feature.value=std::move(fields);

        r::Record waveform;
        waveform.id="xyz-waveform"; waveform.sensor_id="real-writer"; waveform.data_type="waveform";
        waveform.unit="count"; waveform.timestamp=start; waveform.duration_us=8000;
        waveform.encoding="s16le"; waveform.sample_rate_hz=1000;
        waveform.metadata={{"time_anchor","first_sample"}};
        waveform.value=r::Bytes{0,0, 1,0, 2,0, 3,0, 4,0, 5,0, 6,0, 7,0};
        store->append({feature,waveform});

        r::Event event; event.id="writer-warning"; event.sensor_id="real-writer"; event.data_type="temperature";
        event.start=start+1000000; event.end=start+2000000; event.assessment=r::Assessment::warning;
        event.severity=r::Severity::warning; event.event_type="fixture"; event.description="Writer-generated test event";
        event.source="compatibility-test"; event.model_version="fixture-v1";
        store->append_events({event});
        store->set_event_confirmation(event.id,1,true,"Verified",
                                      {"confirm-warning",start+3000000,"compatibility-test","Reviewed"});
        store.reset();
        edgeecho::Limits limits;
        limits.page_records=37;
        edgeecho::Database reader(config.directory,limits);
        const auto overview=reader.overview();
        if(overview.records!=scalar_count+2 || overview.series.size()!=3) throw std::runtime_error("writer overview mismatch");
        const edgeecho::Filter selected{"real-writer","temperature","degC"};
        auto events=reader.events(selected,{start,start+3000000});
        if(events.size()!=1 || events[0].description!="Writer-generated test event" ||
           !events[0].confirmed || events[0].revision!=2) throw std::runtime_error("writer event mismatch");
        std::size_t received=0; edgeecho::Cursor cursor;
        for(;;) {
            auto page=reader.page(selected,{start,end},cursor);
            for(const auto &record:page.records) {
                if(!record.scalar || record.id!="writer-"+std::to_string(received) ||
                   record.timestamp!=start+static_cast<edgeecho::Time>(received)*1000) throw std::runtime_error("writer payload mismatch");
                auto meta=edgeecho::unpack_metadata(record.metadata);
                if(meta.size()!=2) throw std::runtime_error("writer metadata mismatch");
                ++received;
            }
            cursor=page.next; if(!page.more) break;
        }
        if(received!=scalar_count) throw std::runtime_error("writer pagination mismatch");
        const auto plot=reader.plot(selected,{start,end},"value",200);
        if(plot.samples_decoded!=scalar_count || plot.points.size()>400) throw std::runtime_error("writer plot mismatch");

        const edgeecho::Filter features{"real-writer","feature","count"};
        const auto channels=edgeecho::channels(feature.encoding);
        const auto feature_plots=reader.plots(features,{start,start+1},
            {"temperature","x_displacement_peak_to_peak"},1);
        if(channels.size()!=19 || edgeecho::is_waveform_encoding(feature.encoding) ||
           feature_plots[0].estimated_time || feature_plots[0].points.size()!=1 ||
           feature_plots[0].points[0].value!=-100 || feature_plots[1].points.size()!=1 ||
           feature_plots[1].points[0].value!=4660)
            throw std::runtime_error("XYZ feature decoding mismatch");

        const edgeecho::Filter wave{"real-writer","waveform","count"};
        const edgeecho::Range window{start+2500,start+5000};
        const auto wave_plot=reader.plot(wave,window,"value",10);
        const auto probe=reader.probe(wave,window,"value",start+4000);
        if(wave_plot.records_scanned!=1 || wave_plot.samples_decoded!=2 ||
           !wave_plot.estimated_time || !reader.page(wave,window).records.empty() ||
           !probe || probe->timestamp!=start+4000 || probe->value!=4)
            throw std::runtime_error("half-open waveform window mismatch");
        std::cout << "actual storage writer fixture complete\n"; return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
