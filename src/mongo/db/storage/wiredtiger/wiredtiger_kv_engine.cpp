// wiredtiger_kv_engine.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    namespace {
        int mdb_handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session,
                             int errorCode, const char *message) {
            error() << "WiredTiger (" << errorCode << ") " << message;
            return 0;
        }

        int mdb_handle_message( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                                const char *message) {
            log() << "WiredTiger " << message;
            return 0;
        }

        int mdb_handle_progress( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                                 const char *operation, uint64_t progress) {
            log() << "WiredTiger progress " << operation << " " << progress;
            return 0;
        }

        int mdb_handle_close( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                              WT_CURSOR *cursor) {
            return 0;
        }

    }

    WiredTigerKVEngine::WiredTigerKVEngine( const std::string& path,
                                            const std::string& extraOpenOptions,
                                            bool durable )
        : _durable( durable ),
          _epoch( 0 ),
          _sizeStorerSyncTracker( 100000, 60 * 1000 ) {

        _eventHandler.handle_error = mdb_handle_error;
        _eventHandler.handle_message = mdb_handle_message;
        _eventHandler.handle_progress = mdb_handle_progress;
        _eventHandler.handle_close = mdb_handle_close;

        int cacheSizeGB = 1;

        {
            ProcessInfo pi;
            BSONObjBuilder b;
            pi.appendSystemDetails( b );
            BSONObj obj = b.obj();
            BSONObj extra = obj["extra"].Obj();
            double pageSize = extra["pageSize"].number();
            double numPages = extra["numPages"].number();
            if ( pageSize > 0 && numPages > 0 ) {
                double totalBytes = numPages * pageSize;
                double cacheBytes = totalBytes / 10;
                cacheSizeGB = static_cast<int>( cacheBytes / ( 1024 * 1024 * 1024 ) );
                if ( cacheSizeGB < 1 )
                    cacheSizeGB = 1;
            }
        }

        if ( _durable ) {
            boost::filesystem::path journalPath = path;
            journalPath /= "journal";
            if ( !boost::filesystem::exists( journalPath ) ) {
                try {
                    boost::filesystem::create_directory( journalPath );
                }
                catch( std::exception& e) {
                    log() << "error creating journal dir " << journalPath.string() << ' ' << e.what();
                    throw;
                }
            }
        }

        std::stringstream ss;
        ss << "create,";
        ss << "cache_size=" << cacheSizeGB << "G,";
        ss << "session_max=20000,";
        ss << "extensions=[local=(entry=index_collator_extension)],";
        ss << "statistics=(all),";
        if ( _durable ) {
            ss << "log=(enabled=true,archive=true,path=journal),";
        }
        ss << "checkpoint=(wait=60,log_size=2GB),";
        ss << extraOpenOptions;
        string config = ss.str();
        log() << "wiredtiger_open config: " << config;
        invariantWTOK(wiredtiger_open(path.c_str(), &_eventHandler, config.c_str(), &_conn));
        _sessionCache.reset( new WiredTigerSessionCache( this ) );

        _sizeStorerUri = "table:sizeStorer";
        {
            WiredTigerSession session( _conn, -1 );
            WiredTigerSizeStorer* ss = new WiredTigerSizeStorer();
            ss->loadFrom( &session, _sizeStorerUri );
            _sizeStorer.reset( ss );
        }
    }


    WiredTigerKVEngine::~WiredTigerKVEngine() {
        log() << "WiredTigerKVEngine shutting down";
        syncSizeInfo();
        _sizeStorer.reset( NULL );

        _sessionCache.reset( NULL );

        if ( _conn ) {
            invariantWTOK( _conn->close(_conn, NULL) );
            _conn = NULL;
        }

    }

    Status WiredTigerKVEngine::okToRename( OperationContext* opCtx,
                                           const StringData& fromNS,
                                           const StringData& toNS,
                                           const StringData& ident,
                                           const RecordStore* originalRecordStore ) const {
        _sizeStorer->store( _uri( ident ),
                            originalRecordStore->numRecords( opCtx ),
                            originalRecordStore->dataSize( opCtx ) );
        syncSizeInfo();
        return Status::OK();
    }

    int64_t WiredTigerKVEngine::getIdentSize( OperationContext* opCtx,
                                              const StringData& ident ) {
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
        return WiredTigerUtil::getIdentSize(session->getSession(), _uri(ident) );
    }

    Status WiredTigerKVEngine::repairIdent( OperationContext* opCtx,
                                            const StringData& ident ) {
        WiredTigerSession session( _conn, -1 );
        WT_SESSION* s = session.getSession();
        string uri = _uri(ident);
        return wtRCToStatus( s->compact(s, uri.c_str(), NULL ) );
    }

    int WiredTigerKVEngine::flushAllFiles( bool sync ) {
        LOG(1) << "WiredTigerKVEngine::flushAllFiles";
        syncSizeInfo();

        WiredTigerSession session( _conn, -1 );
        WT_SESSION* s = session.getSession();
        invariantWTOK( s->checkpoint(s, NULL ) );

        return 1;
    }

    void WiredTigerKVEngine::syncSizeInfo() const {
        if ( !_sizeStorer )
            return;

        try {
            WiredTigerSession session( _conn, -1 );
            WT_SESSION* s = session.getSession();
            invariantWTOK( s->begin_transaction( s, "sync=true" ) );
            _sizeStorer->storeInto( &session, _sizeStorerUri );
            invariantWTOK( s->commit_transaction( s, NULL ) );
        }
        catch ( const WriteConflictException& de ) {
            // ignore, it means someone else is doing it
        }
    }

    RecoveryUnit* WiredTigerKVEngine::newRecoveryUnit() {
        return new WiredTigerRecoveryUnit( _sessionCache.get() );
    }

    void WiredTigerKVEngine::setRecordStoreExtraOptions( const std::string& options ) {
        _rsOptions = options;
    }

    void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions( const std::string& options ) {
        _indexOptions = options;
    }

    Status WiredTigerKVEngine::createRecordStore( OperationContext* opCtx,
                                                  const StringData& ns,
                                                  const StringData& ident,
                                                  const CollectionOptions& options ) {
        WiredTigerSession session( _conn, -1 );

        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(ns, options, _rsOptions);
        if (!result.isOK()) {
            return result.getStatus();
        }
        std::string config = result.getValue();

        string uri = _uri( ident );
        WT_SESSION* s = session.getSession();
        LOG(1) << "WiredTigerKVEngine::createRecordStore uri: " << uri << " config: " << config;
        return wtRCToStatus( s->create( s, uri.c_str(), config.c_str() ) );
    }

    RecordStore* WiredTigerKVEngine::getRecordStore( OperationContext* opCtx,
                                                     const StringData& ns,
                                                     const StringData& ident,
                                                     const CollectionOptions& options ) {

        if (options.capped) {
            return new WiredTigerRecordStore(opCtx, ns, _uri(ident), options.capped,
                                             options.cappedSize ? options.cappedSize : 4096,
                                             options.cappedMaxDocs ? options.cappedMaxDocs : -1,
                                             NULL,
                                             _sizeStorer.get() );
        }
        else {
            return new WiredTigerRecordStore(opCtx, ns, _uri(ident),
                                             false, -1, -1, NULL, _sizeStorer.get() );
        }
    }

    Status WiredTigerKVEngine::dropRecordStore( OperationContext* opCtx,
                                                const StringData& ident ) {
        _drop( ident );
        return Status::OK();
    }

    string WiredTigerKVEngine::_uri( const StringData& ident ) const {
        return string("table:") + ident.toString();
    }

    Status WiredTigerKVEngine::createSortedDataInterface( OperationContext* opCtx,
                                                          const StringData& ident,
                                                          const IndexDescriptor* desc ) {
        return wtRCToStatus( WiredTigerIndex::Create( opCtx, _uri( ident ), _indexOptions, desc ) );
    }

    SortedDataInterface* WiredTigerKVEngine::getSortedDataInterface( OperationContext* opCtx,
                                                                     const StringData& ident,
                                                                     const IndexDescriptor* desc ) {
        if ( desc->unique() )
            return new WiredTigerIndexUnique( _uri( ident ) );
        return new WiredTigerIndexStandard( _uri( ident ) );
    }

    Status WiredTigerKVEngine::dropSortedDataInterface( OperationContext* opCtx,
                                                        const StringData& ident ) {
        _drop( ident );
        return Status::OK();
    }

    bool WiredTigerKVEngine::_drop( const StringData& ident ) {
        string uri = _uri( ident );

        WiredTigerSession session( _conn, -1 );

        int ret = session.getSession()->drop( session.getSession(), uri.c_str(), "force" );
        LOG(1) << "WT drop of  " << uri << " res " << ret;

        if ( ret == 0 ) {
            // yay, it worked
            return true;
        }

        if ( ret == EBUSY ) {
            // this is expected, queue it up
            {
                boost::mutex::scoped_lock lk( _identToDropMutex );
                _identToDrop.insert( uri );
                _epoch++;
            }
            _sessionCache->closeAll();
            return false;
        }

        invariantWTOK( ret );
        return false;
    }

    bool WiredTigerKVEngine::haveDropsQueued() const {
        if ( _sizeStorerSyncTracker.intervalHasElapsed() ) {
            _sizeStorerSyncTracker.resetLastTime();
            syncSizeInfo();
        }
        boost::mutex::scoped_lock lk( _identToDropMutex );
        return !_identToDrop.empty();
    }

    void WiredTigerKVEngine::dropAllQueued() {
        set<string> mine;
        {
            boost::mutex::scoped_lock lk( _identToDropMutex );
            mine = _identToDrop;
        }

        set<string> deleted;

        {
            WiredTigerSession session( _conn, -1 );
            for ( set<string>::const_iterator it = mine.begin(); it != mine.end(); ++it ) {
                string uri = *it;
                int ret = session.getSession()->drop( session.getSession(), uri.c_str(), "force" );
                LOG(1) << "WT queued drop of  " << uri << " res " << ret;

                if ( ret == 0 ) {
                    deleted.insert( uri );
                    continue;
                }

                if ( ret == EBUSY ) {
                    // leave in qeuue
                    continue;
                }

                invariantWTOK( ret );
            }
        }

        {
            boost::mutex::scoped_lock lk( _identToDropMutex );
            for ( set<string>::const_iterator it = deleted.begin(); it != deleted.end(); ++it ) {
                _identToDrop.erase( *it );
            }
        }
    }

    bool WiredTigerKVEngine::supportsDocLocking() const {
        return true;
    }

}
