// Included inside MainWindow: real-widget regression driver. It is enabled only
// by --interaction-test and only mutates the dedicated copy under --test-output.
    static void sendKey(QWidget *widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                        const QString &text = {}) {
        widget->setFocus();
        QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(widget, &press);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
        QApplication::sendEvent(widget, &release);
    }
    static void sendMouse(QWidget *widget, QEvent::Type type, QPointF local,
                          Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = widget->mapToGlobal(local.toPoint());
        QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
    static void clickWidget(QWidget *widget, QPointF local = {}) {
        if (local.isNull()) local = widget->rect().center();
        sendMouse(widget, QEvent::MouseButtonPress, local, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
    }
    bool interactionCheck(bool condition, const QString &label) {
        if (!condition) { finishInteraction(false, label); return false; }
        interactionChecks_.append(label); return true;
    }
    bool timelineAgrees() const {
        const auto range = clock_.range();
        const Time value = range.start + static_cast<Time>(static_cast<long double>(range.end - range.start) * timeline_->value() / 1000000.0L);
        const Time tolerance = (range.end - range.start) / 1000000 + 2;
        return std::abs(clock_.position() - value) <= tolerance;
    }
    void finishInteraction(bool passed, const QString &error = {}) {
        if (interactionDone_) return;
        interactionDone_ = true; interactionTimer_.stop();
        QJsonObject result{{QStringLiteral("passed"), passed}, {QStringLiteral("error"), error},
                           {QStringLiteral("checks"), interactionChecks_},
                           {QStringLiteral("version"), qs(version())}, {QStringLiteral("build_id"), qs(build_id())}};
        QFile file(pathToQt(testOutput_ / "interaction-result.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0) {
            std::cerr << "GUI interaction evidence write failed\n"; QApplication::exit(4); return;
        }
        file.close();
        if (passed && !screenshot_.isEmpty() && !grab().save(screenshot_)) {
            std::cerr << "GUI interaction screenshot failed\n"; QApplication::exit(4); return;
        }
        if (passed) std::cout << "GUI interaction passed: checks=" << interactionChecks_.size() << '\n';
        else std::cerr << "GUI interaction failed: " << utf8(error) << '\n';
        QApplication::exit(passed ? 0 : 3);
    }
    void interactionStep() {
        if (interactionLoggedPhase_ != interactionPhase_) {
            interactionLoggedPhase_ = interactionPhase_;
            std::cerr << "GUI interaction stage " << interactionPhase_ << '\n';
        }
        if (interactionDone_ || active_ || pending_ || requested_) return;
        try {
            switch (interactionPhase_) {
            case 0:
                if (!validSelection()) return;
                for (std::size_t n = 0; n < overview_.series.size(); ++n)
                    if (overview_.series[n].kind == 1 && overview_.series[n].records > 512)
                        interactionScalar_ = static_cast<int>(n);
                if (!interactionCheck(interactionScalar_ >= 0, QStringLiteral("fixture has paginated scalar series"))) return;
                interactionPhase_ = 1; seriesList_->setCurrentRow(interactionScalar_); return;
            case 1:
                if (!interactionCheck(validSelection() && loaded_.has_value(), QStringLiteral("source identity bound to first window"))) return;
                sendKey(timeline_, Qt::Key_Home);
                if (!interactionCheck(clock_.position() == clock_.range().start, QStringLiteral("timeline Home seeks"))) return;
                sendKey(timeline_, Qt::Key_Right);
                if (!interactionCheck(clock_.position() > clock_.range().start && timelineAgrees(), QStringLiteral("timeline Right seeks"))) return;
                sendKey(timeline_, Qt::Key_PageUp);
                if (!interactionCheck(timelineAgrees(), QStringLiteral("timeline PageUp seeks"))) return;
                sendKey(timeline_, Qt::Key_End);
                if (!interactionCheck(clock_.position() == clock_.range().end && clock_.state() == PlaybackState::finished,
                                      QStringLiteral("timeline End reaches finished"))) return;
                interactionPhase_ = 2; return;
            case 2:
                clickWidget(play_);
                interactionWait_ = std::chrono::steady_clock::now(); interactionPhase_ = 3; return;
            case 3:
                if (std::chrono::steady_clock::now() - interactionWait_ < std::chrono::milliseconds(120)) return;
                if (!interactionCheck(clock_.state() == PlaybackState::playing && clock_.position() < clock_.range().end,
                                      QStringLiteral("Play button replays after end"))) return;
                clickWidget(pause_); interactionPosition_ = clock_.position();
                interactionWait_ = std::chrono::steady_clock::now(); interactionPhase_ = 4; return;
            case 4: {
                if (std::chrono::steady_clock::now() - interactionWait_ < std::chrono::milliseconds(150)) return;
                if (!interactionCheck(clock_.state() == PlaybackState::paused && clock_.position() == interactionPosition_,
                                      QStringLiteral("Pause button freezes monotonic position"))) return;
                sendKey(speed_, Qt::Key_A, Qt::ControlModifier);
                sendKey(speed_, Qt::Key_8, Qt::NoModifier, QStringLiteral("8")); sendKey(speed_, Qt::Key_Return);
                if (!interactionCheck(speed_->value() == 8 && clock_.speed() == 8, QStringLiteral("speed editor keyboard changes clock speed"))) return;
                clickWidget(stop_);
                if (!interactionCheck(clock_.state() == PlaybackState::stopped && clock_.position() == clock_.range().start,
                                      QStringLiteral("Stop button resets source range"))) return;
                clickWidget(timeline_, QPointF(timeline_->width() * .62, timeline_->height() / 2.0));
                if (!interactionCheck(clock_.position() > clock_.range().start && timelineAgrees(), QStringLiteral("timeline track click seeks"))) return;
                QStyleOptionSlider option; option.initFrom(timeline_); option.orientation = Qt::Horizontal;
                option.minimum = timeline_->minimum(); option.maximum = timeline_->maximum();
                option.sliderPosition = timeline_->sliderPosition(); option.sliderValue = timeline_->value();
                const QRect handle = timeline_->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, timeline_);
                sendMouse(timeline_, QEvent::MouseButtonPress, handle.center(), Qt::LeftButton, Qt::LeftButton);
                const QPointF end(timeline_->width() * .82, timeline_->height() / 2.0);
                sendMouse(timeline_, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
                sendMouse(timeline_, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
                if (!interactionCheck(!timeline_->isSliderDown() && timelineAgrees() && timeline_->value() > 500000,
                                      QStringLiteral("timeline mouse drag commits seek"))) return;
                interactionPhase_ = 5; return;
            }
            case 5:
                sendKey(seriesList_, Qt::Key_End); sendKey(seriesList_, Qt::Key_Home); sendKey(seriesList_, Qt::Key_End);
                interactionPhase_ = 6; return;
            case 6:
                if (!interactionCheck(selected_ == seriesList_->count() - 1 && !page_.records.empty() &&
                                      page_.records.front().sensor == filter().sensor && page_.records.front().type == filter().type,
                                      QStringLiteral("rapid series changes discard stale windows"))) return;
                loadWindow(true); clickWidget(cancelButton_); interactionPhase_ = 7; return;
            case 7:
                if (!interactionCheck(validSelection() && clock_.state() != PlaybackState::playing,
                                      QStringLiteral("Cancel button settles without invalidating unchanged source"))) return;
                interactionPhase_ = 8; seriesList_->setCurrentRow(interactionScalar_); return;
            case 8:
                if (!interactionCheck(page_.more && nextPage_->isEnabled() && !page_.records.empty(), QStringLiteral("record table offers a second page"))) return;
                interactionLastId_ = page_.records.front().id; clickWidget(nextPage_); interactionPhase_ = 9; return;
            case 9:
                if (!interactionCheck(!page_.records.empty() && page_.records.front().id != interactionLastId_,
                                      QStringLiteral("Next page button advances records"))) return;
                clickWidget(firstPage_); interactionPhase_ = 10; return;
            case 10: {
                if (!interactionCheck(page_.more && nextPage_->isEnabled(), QStringLiteral("First page button resets cursor"))) return;
                // Replace only our private fixture, preserving SQLite ID, size and
                // mtime. A native file identity is required to detect this case.
                const auto next = testOutput_ / "replacement.next.db";
                const auto previous = testOutput_ / "interaction-source" / "previous.db";
                std::filesystem::copy_file(interactionSource_, next, std::filesystem::copy_options::overwrite_existing);
                std::filesystem::last_write_time(next, std::filesystem::last_write_time(interactionSource_));
                std::filesystem::remove(previous);
                std::filesystem::rename(interactionSource_, previous); std::filesystem::rename(next, interactionSource_);
                expectSourceRejection_ = true; interactionPhase_ = 11; clickWidget(nextPage_); return;
            }
            case 11:
                if (!interactionCheck(expectedSourceErrorSeen_ && !validSelection() && !source_ && records_->rowCount() == 0 &&
                                      !exportAction_->isEnabled() && cache_.empty() && !pendingProbe_, QStringLiteral("changed source clears records/cache and requires reopen"))) return;
                interactionPhase_ = 12; openPath(interactionSource_); return;
            case 12:
                if (!interactionCheck(validSelection() && loaded_ && !page_.records.empty(), QStringLiteral("explicit reopen recovers changed source"))) return;
                // Opening immediately invalidates the old controls. An old list
                // action cannot overwrite the pending new source request.
                openPath(interactionSource_); sendKey(seriesList_, Qt::Key_End);
                if (!interactionCheck(opening_ && !validSelection() && !seriesList_->isEnabled() && !exportAction_->isEnabled(),
                                      QStringLiteral("pending open disables stale source actions"))) return;
                interactionPhase_ = 13; return;
            case 13:
                if (!interactionCheck(validSelection() && selected_ == 0 && loaded_.has_value(), QStringLiteral("new source open owns latest result"))) return;
                interactionPhase_=14;seriesList_->setCurrentRow(interactionScalar_);return;
            case 14: {
                const auto original=clock_.range();
                const Range precise{original.start+123,std::min(original.end,original.start+5000987)};
                setRangeFields(precise);timeEditor_->setLocalDisplay(true);timeEditor_->setLocalDisplay(false);
                if(!interactionCheck(timeEditor_->range().start==precise.start && timeEditor_->range().end==precise.end &&
                                     fromEdit_->text().toLongLong()==precise.start && toEdit_->text().toLongLong()==precise.end,
                                     QStringLiteral("calendar timezone switches preserve exact UTC microseconds")))return;
                timeEditor_->setRange({max_time-1001,max_time});timeEditor_->setLocalDisplay(true);timeEditor_->setLocalDisplay(false);
                if(!interactionCheck(timeEditor_->range().end==max_time,QStringLiteral("calendar preserves exclusive maximum UTC boundary")))return;
                setRangeFields(precise);
                const auto residues=timeEditor_->findChildren<QSpinBox*>();
                if(!interactionCheck(residues.size()==2,QStringLiteral("calendar exposes sub-millisecond remainder")))return;
                residues.front()->setValue(333);QMetaObject::invokeMethod(residues.front(),"editingFinished",Qt::DirectConnection);
                if(!interactionCheck(timeEditor_->valid() && fromEdit_->text().toLongLong()==original.start+333,
                                     QStringLiteral("calendar editing writes precise UTC field")))return;
                setRangeFields(original);applyRange();interactionPhase_=15;return;
            }
            case 15: {
                int raw=-1;
                for(std::size_t n=0;n<overview_.series.size();++n)if(overview_.series[n].encoding=="iio-le-24-v1")raw=static_cast<int>(n);
                if(!interactionCheck(raw>=0,QStringLiteral("fixture has XYZ waveform series")))return;
                interactionPhase_=16;seriesList_->setCurrentRow(raw);return;
            }
            case 16: {
                const int all=channelBox_->findData(QStringLiteral("xyz"));
                if(all<0){finishInteraction(false,QStringLiteral("XYZ channel option missing"));return;}
                if(channelBox_->currentIndex()!=all){channelBox_->setCurrentIndex(all);return;}
                if(!interactionCheck(plot_->traceCount()==3 && plot_->traces()[0].label==QStringLiteral("x") &&
                                     plot_->traces()[1].label==QStringLiteral("y") && plot_->traces()[2].label==QStringLiteral("z"),
                                     QStringLiteral("XYZ renders three channels on one time axis")))return;
                interactionPosition_=loaded_->end-loaded_->start;
                const auto local=plot_->plotArea().center();const QPointF global=plot_->mapToGlobal(local.toPoint());
                QWheelEvent wheel(local,global,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
                QApplication::sendEvent(plot_,&wheel);interactionPhase_=17;return;
            }
            case 17: {
                if(!interactionCheck(loaded_ && loaded_->end-loaded_->start<interactionPosition_ &&
                                     plot_->plotRange().start==loaded_->start && plot_->plotRange().end==loaded_->end,
                                     QStringLiteral("wheel zoom reloads an exact bounded source window")))return;
                interactionPosition_=loaded_->start;const auto center=plot_->plotArea().center();
                sendMouse(plot_,QEvent::MouseButtonPress,center,Qt::MiddleButton,Qt::MiddleButton);
                sendMouse(plot_,QEvent::MouseMove,center+QPointF(60,0),Qt::NoButton,Qt::MiddleButton);
                sendMouse(plot_,QEvent::MouseButtonRelease,center+QPointF(60,0),Qt::MiddleButton,Qt::NoButton);
                interactionPhase_=18;return;
            }
            case 18:
                if(!interactionCheck(loaded_ && loaded_->start!=interactionPosition_,QStringLiteral("plot pan requests a different source window")))return;
                sendKey(plot_,Qt::Key_Escape);interactionPhase_=19;return;
            case 19: {
                if(!interactionCheck(loaded_->start==clock_.range().start && loaded_->end==clock_.range().end,
                                     QStringLiteral("plot reset restores full selected UTC range")))return;
                lastExactReadout_.clear();sendMouse(plot_,QEvent::MouseMove,plot_->plotArea().center(),Qt::NoButton,Qt::NoButton);
                interactionWait_=std::chrono::steady_clock::now();interactionPhase_=20;return;
            }
            case 20:
                if(lastExactReadout_.isEmpty() && std::chrono::steady_clock::now()-interactionWait_<std::chrono::seconds(5))return;
                if(!interactionCheck(lastExactReadout_.startsWith(QStringLiteral("原始样本，不插值")) &&
                                     lastExactReadout_.contains(QStringLiteral("x=")) && lastExactReadout_.contains(QStringLiteral("Δ=")),
                                     QStringLiteral("hover readout queries original samples with timestamp delta")))return;
                interactionExpectedCacheHits_=cacheHits_;loadWindow(true);interactionPhase_=21;return;
            case 21:
                if(!interactionCheck(cacheHits_>interactionExpectedCacheHits_ && cache_.size()<=3 && cacheBytes_<=32*1024*1024,
                                     QStringLiteral("repeated window uses bounded source-validated cache")))return;
                interactionPhase_=22;seriesList_->setCurrentRow(interactionScalar_);interactionWait_=std::chrono::steady_clock::now();return;
            case 22: {
                const auto expectedNext=windowAt(loaded_->end);
                bool next=false;for(const auto &entry:cache_) {
                    if(!interactionCheck(entry.filter.type==filter().type && entry.source==*source_,QStringLiteral("series switch clears incompatible cache entries")))return;
                    if(entry.range.start==expectedNext.start && entry.range.end==expectedNext.end)next=true;
                }
                if(!next && std::chrono::steady_clock::now()-interactionWait_<std::chrono::seconds(5))return;
                if(!interactionCheck(next,QStringLiteral("next window prefetched without changing displayed data")))return;
                interactionExpectedCacheHits_=cacheHits_;seekTo(loaded_->end);interactionPhase_=23;return;
            }
            case 23: {
                if(!interactionCheck(cacheHits_>interactionExpectedCacheHits_,QStringLiteral("seek consumes prefetched window")))return;
                int soundRow=-1;
                for(std::size_t n=0;n<overview_.series.size();++n)if(overview_.series[n].encoding=="iio-le-16-v1")soundRow=static_cast<int>(n);
                if(!interactionCheck(soundRow>=0,QStringLiteral("fixture has sound PCM series")))return;
                interactionPhase_=24;seriesList_->setCurrentRow(soundRow);return;
            }
            case 24: {
                if(!interactionCheck(audioPlayer_.has_clip() && audioPlayer_.sample_rate_hz()==1000,
                                     QStringLiteral("sound series synthesizes playable audio clip")))return;
                volumeSlider_->setValue(50);
                if(!interactionCheck(std::abs(audioPlayer_.volume()-0.5)<0.01,QStringLiteral("volume slider adjusts audio player gain")))return;
                audioEnable_->setChecked(false);
                if(!interactionCheck(audioPlayer_.muted(),QStringLiteral("audio checkbox toggles mute state")))return;
                audioEnable_->setChecked(true);
                const int specIdx=viewModeBox_->findData(QStringLiteral("spectrum"));
                if(specIdx<0){finishInteraction(false,QStringLiteral("spectrum mode option missing"));return;}
                viewModeBox_->setCurrentIndex(specIdx);
                if(!interactionCheck(plot_->isSpectrumMode() && std::abs(plot_->spectrum().dominant_frequency_hz-80.0)<2.0,
                                     QStringLiteral("FFT spectrum view identifies 80 Hz sound tone")))return;
                viewModeBox_->setCurrentIndex(0);
                if(!interactionCheck(!plot_->isSpectrumMode(),QStringLiteral("switching back restores time-domain waveform")))return;
                const int darkIdx=themeBox_->findData(QStringLiteral("dark"));
                const int lightIdx=themeBox_->findData(QStringLiteral("light"));
                const int sysIdx=themeBox_->findData(QStringLiteral("system"));
                if(darkIdx<0||lightIdx<0||sysIdx<0){finishInteraction(false,QStringLiteral("theme selector options missing"));return;}
                themeBox_->setCurrentIndex(darkIdx);
                if(!interactionCheck(themeToSpec_.dark && themePref_==ThemePreference::Dark,
                                     QStringLiteral("theme selector switches to animated dark mode")))return;
                setPlatformFlavor(PlatformFlavor::Windows11Fluent);
                if(!interactionCheck(platformFlavor_==PlatformFlavor::Windows11Fluent &&
                                     themeToSpec_.flavor==PlatformFlavor::Windows11Fluent,
                                     QStringLiteral("Windows 11 Fluent official component style applies")))return;
                setPlatformFlavor(defaultPlatformFlavor());
                themeBox_->setCurrentIndex(lightIdx);
                if(!interactionCheck(!themeToSpec_.dark && themePref_==ThemePreference::Light,
                                     QStringLiteral("theme selector switches to animated light mode")))return;
                themeBox_->setCurrentIndex(sysIdx);
                if(!interactionCheck(themePref_==ThemePreference::FollowSystem,
                                     QStringLiteral("theme selector supports follow-system mode")))return;
                finishInteraction(true);return;
            }
            default: finishInteraction(false, QStringLiteral("invalid interaction stage")); return;
            }
        } catch (const std::exception &error) { finishInteraction(false, qs(error.what())); }
    }
