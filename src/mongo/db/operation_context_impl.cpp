/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/random.h"
#include "mongo/util/log.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {
    // Dispenses unique OperationContext identifiers
    AtomicUInt64 idCounter(0);
}

    OperationContextImpl::OperationContextImpl() : _client(currentClient.get()) {
        invariant(_client);

        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        invariant(storageEngine);
        _recovery.reset(storageEngine->newRecoveryUnit(this));

        if (storageEngine->isMmapV1()) {
            _locker.reset(new MMAPV1LockerImpl(idCounter.addAndFetch(1)));
        }
        else {
            _locker.reset(new LockerImpl<false>(idCounter.addAndFetch(1)));
        }

        getGlobalEnvironment()->registerOperationContext(this);
    }

    OperationContextImpl::~OperationContextImpl() {
        getGlobalEnvironment()->unregisterOperationContext(this);
    }

    RecoveryUnit* OperationContextImpl::recoveryUnit() const {
        return _recovery.get();
    }

    RecoveryUnit* OperationContextImpl::releaseRecoveryUnit() {
        if ( _recovery.get() )
            _recovery->beingReleasedFromOperationContext();
        return _recovery.release();
    }

    void OperationContextImpl::setRecoveryUnit(RecoveryUnit* unit) {
        _recovery.reset(unit);
        if ( unit )
            unit->beingSetOnOperationContext();
    }

    Locker* OperationContextImpl::lockState() const {
        return _locker.get();
    }

    ProgressMeter* OperationContextImpl::setMessage(const char * msg,
                                                    const std::string &name,
                                                    unsigned long long progressMeterTotal,
                                                    int secondsBetween) {
        return &getCurOp()->setMessage(msg, name, progressMeterTotal, secondsBetween);
    }

    string OperationContextImpl::getNS() const {
        return getCurOp()->getNS();
    }

    bool OperationContextImpl::isGod() const {
        return getClient()->isGod();
    }

    Client* OperationContextImpl::getClient() const {
        return _client;
    }

    CurOp* OperationContextImpl::getCurOp() const {
        return getClient()->curop();
    }

    unsigned int OperationContextImpl::getOpID() const {
        return getCurOp()->opNum();
    }

    // Enabling the checkForInterruptFail fail point will start a game of random chance on the
    // connection specified in the fail point data, generating an interrupt with a given fixed
    // probability.  Example invocation:
    //
    // {configureFailPoint: "checkForInterruptFail",
    //  mode: "alwaysOn",
    //  data: {conn: 17, chance: .01, allowNested: true}}
    //
    // All three data fields must be specified.  In the above example, all interrupt points on
    // connection 17 will generate a kill on the current operation with probability p(.01),
    // including interrupt points of nested operations.  If "allowNested" is false, nested
    // operations are not targeted.  "chance" must be a double between 0 and 1, inclusive.
    MONGO_FP_DECLARE(checkForInterruptFail);

    namespace {

        // Global state for checkForInterrupt fail point.
        PseudoRandom checkForInterruptPRNG(static_cast<int64_t>(time(NULL)));

        // Helper function for checkForInterrupt fail point.  Decides whether the operation currently
        // being run by the given Client meet the (probabilistic) conditions for interruption as
        // specified in the fail point info.
        bool opShouldFail(const Client& c, const BSONObj& failPointInfo) {
            // Only target the client with the specified connection number.
            if (c.getConnectionId() != failPointInfo["conn"].safeNumberLong()) {
                return false;
            }

            // Only target nested operations if requested.
            if (!failPointInfo["allowNested"].trueValue() && c.curop()->parent() != NULL) {
                return false;
            }

            // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
            double next = static_cast<double>(std::abs(checkForInterruptPRNG.nextInt64()));
            double upperBound =
                std::numeric_limits<int64_t>::max() * failPointInfo["chance"].numberDouble();
            if (next > upperBound) {
                return false;
            }
            return true;
        }

    } // namespace

    void OperationContextImpl::checkForInterrupt(bool heedMutex) const {
        Client* c = getClient();

        if (heedMutex && lockState()->isWriteLocked() && c->hasWrittenSinceCheckpoint()) {
            return;
        }

        if (getGlobalEnvironment()->getKillAllOperations()) {
            uasserted(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
        }

        if (c->curop()->maxTimeHasExpired()) {
            c->curop()->kill();
            uasserted(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
        }

        MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
            if (opShouldFail(*c, scopedFailPoint.getData())) {
                log() << "set pending kill on " << (c->curop()->parent() ? "nested" : "top-level")
                      << " op " << c->curop()->opNum() << ", for checkForInterruptFail";
                c->curop()->kill();
            }
        }

        if (c->curop()->killPending()) {
            uasserted(ErrorCodes::Interrupted, "operation was interrupted");
        }
    }

    Status OperationContextImpl::checkForInterruptNoAssert() const {
        // TODO(spencer): Unify error codes and implementation with checkForInterrupt()
        Client* c = getClient();

        if (getGlobalEnvironment()->getKillAllOperations()) {
            return Status(ErrorCodes::Interrupted, "interrupted at shutdown");
        }

        if (c->curop()->maxTimeHasExpired()) {
            c->curop()->kill();
            return Status(ErrorCodes::Interrupted, "exceeded time limit");
        }

        MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
            if (opShouldFail(*c, scopedFailPoint.getData())) {
                log() << "set pending kill on " << (c->curop()->parent() ? "nested" : "top-level")
                      << " op " << c->curop()->opNum() << ", for checkForInterruptFail";
                c->curop()->kill();
            }
        }

        if (c->curop()->killPending()) {
            return Status(ErrorCodes::Interrupted, "interrupted");
        }

        return Status::OK();
    }

    bool OperationContextImpl::isPrimaryFor( const StringData& ns ) {
        return repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                NamespaceString(ns).db());
    }

}  // namespace mongo
