// Schema snapshot from EdgeSense Replay v3, 2026-10-28. Used only to create demo/test databases.
#pragma once
namespace edgeecho {
inline constexpr const char *demo_schema = R"SCHEMA(
-- Frozen v2 schema for Phase 3 migration tests.
-- Frozen original v1 schema for migration tests; do not generate from v2.
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
INSERT OR IGNORE INTO meta VALUES('generation','0');
INSERT OR IGNORE INTO meta VALUES('database_id','0123456789abcdef0123456789abcdef');
CREATE TABLE IF NOT EXISTS records(
 seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL UNIQUE,sensor TEXT NOT NULL,type TEXT NOT NULL,unit TEXT NOT NULL,
 ts INTEGER NOT NULL CHECK(ts>=0),kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 2),scalar REAL,block BLOB,
 quality INTEGER NOT NULL CHECK(quality BETWEEN 0 AND 15),duration INTEGER NOT NULL CHECK(duration>=0),rate REAL,
 encoding TEXT NOT NULL,source TEXT NOT NULL,batch TEXT NOT NULL,metadata TEXT NOT NULL,version INTEGER NOT NULL CHECK(version=1),
 CHECK((kind=0 AND scalar IS NULL AND block IS NULL AND quality!=0) OR (kind=1 AND scalar IS NOT NULL AND block IS NULL) OR (kind=2 AND scalar IS NULL AND block IS NOT NULL AND duration>0)));
CREATE INDEX IF NOT EXISTS records_series_time ON records(sensor,type,ts,seq);
CREATE INDEX IF NOT EXISTS records_time ON records(ts,seq);
CREATE TABLE IF NOT EXISTS events(
 seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL UNIQUE,sensor TEXT NOT NULL,type TEXT NOT NULL,
 start INTEGER NOT NULL CHECK(start>=0),end INTEGER NOT NULL CHECK(end>start),
 assessment INTEGER NOT NULL CHECK(assessment BETWEEN 0 AND 4),severity INTEGER NOT NULL CHECK(severity BETWEEN 0 AND 2),
 event_type TEXT NOT NULL,description TEXT NOT NULL,source TEXT NOT NULL,model TEXT NOT NULL,confidence REAL,
 version INTEGER NOT NULL CHECK(version=1),CHECK(confidence IS NULL OR (confidence>=0 AND confidence<=1)));
CREATE INDEX IF NOT EXISTS events_sensor_time ON events(sensor,start,seq);
CREATE INDEX IF NOT EXISTS events_time ON events(start,seq);
CREATE INDEX IF NOT EXISTS events_end ON events(end,seq);
PRAGMA application_id=1163088464;
PRAGMA user_version=1;
CREATE TABLE event_history(
 seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
 sensor TEXT NOT NULL,type TEXT NOT NULL,start INTEGER NOT NULL CHECK(start>=0),end INTEGER NOT NULL CHECK(end>start),
 assessment INTEGER NOT NULL CHECK(assessment BETWEEN 0 AND 4),severity INTEGER NOT NULL CHECK(severity BETWEEN 0 AND 2),
 event_type TEXT NOT NULL,description TEXT NOT NULL,source TEXT NOT NULL,model TEXT NOT NULL,confidence REAL,
 version INTEGER NOT NULL CHECK(version=1),revision INTEGER NOT NULL CHECK(revision>0),
 previous_revision INTEGER NOT NULL CHECK(previous_revision=revision-1),action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 4),
 operation_id TEXT UNIQUE,operated_at INTEGER NOT NULL CHECK(operated_at>=0),actor TEXT NOT NULL,reason TEXT NOT NULL,
 revoked INTEGER NOT NULL CHECK(revoked IN (0,1)),confirmed INTEGER NOT NULL CHECK(confirmed IN (0,1)),
 confirmed_at INTEGER,confirmed_by TEXT NOT NULL,note TEXT NOT NULL,request TEXT NOT NULL,
 UNIQUE(id,revision),CHECK(confidence IS NULL OR (confidence>=0 AND confidence<=1)),
 CHECK((revision=1 AND action=0) OR (revision>1 AND action!=0)),
 CHECK((confirmed=0 AND confirmed_at IS NULL AND confirmed_by='') OR
       (confirmed=1 AND confirmed_at>=0 AND confirmed_by!='')));
CREATE INDEX event_history_identity ON event_history(id,seq);
CREATE INDEX event_history_sensor_time ON event_history(sensor,start,seq);
CREATE INDEX event_history_time ON event_history(start,seq);
CREATE INDEX event_history_end ON event_history(end,seq);

PRAGMA user_version=2;

CREATE TABLE aggregate_cells(
 level INTEGER NOT NULL CHECK(level BETWEEN 0 AND 2),sensor TEXT NOT NULL,type TEXT NOT NULL,unit TEXT NOT NULL,
 bucket INTEGER NOT NULL CHECK(bucket>=0),valid INTEGER NOT NULL CHECK(valid>=0),excluded INTEGER NOT NULL CHECK(excluded>=0),
 blocks INTEGER NOT NULL CHECK(blocks>=0),sum_value TEXT NOT NULL,minimum REAL,maximum REAL,
 first_good INTEGER,last_good INTEGER,max_gap INTEGER NOT NULL CHECK(max_gap>=0),positive_count INTEGER NOT NULL CHECK(positive_count>=0),
 max_seq INTEGER NOT NULL CHECK(max_seq>0),gap_remainder INTEGER NOT NULL CHECK(gap_remainder>=0),
 gap_detail INTEGER NOT NULL CHECK(gap_detail IN (0,1)),gaps TEXT NOT NULL CHECK(length(gaps)<=2048),PRIMARY KEY(level,sensor,type,unit,bucket),
 CHECK((valid=0 AND minimum IS NULL AND maximum IS NULL) OR (valid>0 AND minimum IS NOT NULL AND maximum IS NOT NULL)),
 CHECK((first_good IS NULL AND last_good IS NULL) OR (first_good>=0 AND last_good>=first_good))) WITHOUT ROWID;
CREATE TABLE aggregate_state(id INTEGER PRIMARY KEY CHECK(id=1),backfill_end INTEGER NOT NULL CHECK(backfill_end>=0),
 backfill_cursor INTEGER NOT NULL CHECK(backfill_cursor>=0 AND backfill_cursor<=backfill_end),algorithm INTEGER NOT NULL CHECK(algorithm=1));
INSERT INTO aggregate_state VALUES(1,coalesce((SELECT seq FROM sqlite_sequence WHERE name='records'),0),0,1);
CREATE INDEX records_coverage_end ON records(sensor,type,unit,ts+duration) WHERE duration>0 AND quality=0 AND kind!=0;
PRAGMA user_version=3;

)SCHEMA";
}
